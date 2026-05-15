#include "platform_win32.h"

#include "diagnostics.h"
#include "gl_headers.h"
#include "keymap.h"
#include "resource.h"

#include <math.h>
#include <string.h>

static const wchar_t k_platform_window_class_name[] = L"OpenGLSkyWindowClass";

static LRESULT CALLBACK platform_window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param);
static int platform_register_window_class(HINSTANCE instance);
static int platform_bootstrap_wgl(HINSTANCE instance);
static HICON platform_load_icon_resource(HINSTANCE instance, int resource_id, int system_metric_x, int system_metric_y);
static int platform_set_legacy_pixel_format(HDC device_context);
static int platform_set_modern_pixel_format(HDC device_context);
static int platform_register_raw_mouse_device(HWND window);
static void platform_accumulate_raw_mouse(PlatformApp* app, HRAWINPUT input_handle);
static int platform_is_virtual_key_down(int virtual_key);
static int platform_is_action_down(KeymapAction action);
static int platform_window_has_focus(const PlatformApp* app);
static void platform_set_cursor_hidden(PlatformApp* app, int hidden);
static void platform_update_cursor_clip(const PlatformApp* app);
static void platform_center_cursor(const PlatformApp* app);
static void platform_set_mouse_capture(PlatformApp* app, int enabled);
static void platform_toggle_fullscreen(PlatformApp* app);
static int platform_get_overlay_width(const PlatformApp* app);
static int platform_get_overlay_visible_width(const PlatformApp* app);
static float platform_clamp_float(float value, float min_value, float max_value);
static int platform_point_in_client(const PlatformApp* app, int x, int y);
static int platform_point_in_overlay(const PlatformApp* app, int x, int y);
static int platform_point_in_overlay_scroll_view(const PlatformApp* app, int x, int y);
static int platform_get_overlay_button_rect(const PlatformApp* app, RECT* out_rect);
static int platform_get_overlay_toggle_rect(const PlatformApp* app, OverlayToggleId toggle_id, RECT* out_rect);
static int platform_get_overlay_slider_rect(const PlatformApp* app, OverlaySliderId slider_id, RECT* out_rect);
static int platform_get_overlay_render_quality_rect(const PlatformApp* app, RendererQualityPreset preset, RECT* out_rect);
static int platform_get_overlay_gpu_preference_rect(const PlatformApp* app, GpuPreferenceMode mode, RECT* out_rect);
static OverlayToggleId platform_get_hovered_toggle(const PlatformApp* app, int x, int y);
static OverlaySliderId platform_get_hovered_slider(const PlatformApp* app, int x, int y);
static RendererQualityPreset platform_get_hovered_render_quality(const PlatformApp* app, int x, int y);
static GpuPreferenceMode platform_get_hovered_gpu_preference(const PlatformApp* app, int x, int y);
static int platform_utf8_to_wide(const char* text, wchar_t* out_text, size_t out_text_count);
static void platform_toggle_value(PlatformApp* app, OverlayToggleId toggle_id);
static void platform_set_slider_value(SceneSettings* settings, OverlaySliderId slider_id, float value);
static void platform_adjust_overlay_scroll(PlatformApp* app, float delta);
static void platform_update_overlay_interaction(PlatformApp* app, int has_focus);

int platform_create(PlatformApp* app, const char* title, int width, int height)
{
  RECT window_rect = { 0, 0, width, height };
  RECT client_rect = { 0, 0, 0, 0 };
  wchar_t wide_title[256] = { 0 };
  HINSTANCE instance = GetModuleHandleW(NULL);

  memset(app, 0, sizeof(*app));
  app->instance = instance;
  diagnostics_log("platform_create: begin");

  if (instance == NULL)
  {
    platform_show_error_message("Win32 Error", "Failed to resolve the current module handle.");
    return 0;
  }

  if (!platform_utf8_to_wide((title != NULL) ? title : "OpenGL Sky", wide_title, sizeof(wide_title) / sizeof(wide_title[0])))
  {
    platform_show_error_message("Win32 Error", "Failed to convert the window title to UTF-16.");
    return 0;
  }

  if (!platform_register_window_class(instance))
  {
    platform_show_error_message("Win32 Error", "Failed to register the window class.");
    return 0;
  }

  if (!platform_bootstrap_wgl(instance))
  {
    platform_show_error_message("OpenGL Error", "Failed to initialize WGL extensions.");
    return 0;
  }

  if (!AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE))
  {
    platform_show_error_message("Win32 Error", "Failed to calculate the window bounds.");
    return 0;
  }

  app->window = CreateWindowExW(
    0U,
    k_platform_window_class_name,
    wide_title,
    WS_OVERLAPPEDWINDOW,
    CW_USEDEFAULT,
    CW_USEDEFAULT,
    window_rect.right - window_rect.left,
    window_rect.bottom - window_rect.top,
    NULL,
    NULL,
    instance,
    app
  );
  if (app->window == NULL)
  {
    platform_show_error_message("Win32 Error", "Failed to create the application window.");
    return 0;
  }

  app->device_context = GetDC(app->window);
  if (app->device_context == NULL)
  {
    platform_show_error_message("Win32 Error", "Failed to acquire the window device context.");
    platform_destroy(app);
    return 0;
  }

  if (!platform_set_modern_pixel_format(app->device_context))
  {
    platform_show_error_message("OpenGL Error", "Failed to set the pixel format for the OpenGL window.");
    platform_destroy(app);
    return 0;
  }

  if (wglCreateContextAttribsARB != NULL)
  {
    const int context_attributes[] = {
      WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
      WGL_CONTEXT_MINOR_VERSION_ARB, 3,
      WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB,
      0
    };
    app->gl_context = wglCreateContextAttribsARB(app->device_context, 0, context_attributes);
  }
  else
  {
    app->gl_context = wglCreateContext(app->device_context);
  }

  if (app->gl_context == NULL)
  {
    platform_show_error_message("OpenGL Error", "Failed to create the OpenGL rendering context.");
    platform_destroy(app);
    return 0;
  }

  if (!wglMakeCurrent(app->device_context, app->gl_context))
  {
    platform_show_error_message("OpenGL Error", "Failed to activate the OpenGL rendering context.");
    platform_destroy(app);
    return 0;
  }

  glewExperimental = GL_TRUE;
  if (glewInit() != GLEW_OK)
  {
    platform_show_error_message("OpenGL Error", "Failed to load OpenGL entry points through GLEW.");
    platform_destroy(app);
    return 0;
  }
  (void)glGetError();

  if ((wglSwapIntervalEXT != NULL) && !wglSwapIntervalEXT(0))
  {
    (void)glGetError();
  }

  if (!platform_register_raw_mouse_device(app->window))
  {
    platform_show_error_message("Win32 Error", "Failed to register raw mouse input.");
    platform_destroy(app);
    return 0;
  }

  if (!QueryPerformanceFrequency(&app->timer_frequency) || !QueryPerformanceCounter(&app->timer_start))
  {
    platform_show_error_message("Win32 Error", "Failed to initialize the high resolution timer.");
    platform_destroy(app);
    return 0;
  }

  if (!GetClientRect(app->window, &client_rect))
  {
    platform_show_error_message("Win32 Error", "Failed to read the client size.");
    platform_destroy(app);
    return 0;
  }

  app->width = (int)(client_rect.right - client_rect.left);
  app->height = (int)(client_rect.bottom - client_rect.top);
  app->windowed_style = GetWindowLongPtrW(app->window, GWL_STYLE);
  app->windowed_ex_style = GetWindowLongPtrW(app->window, GWL_EXSTYLE);
  app->fullscreen_enabled = 0;
  app->running = 1;
  app->resized = 1;
  app->overlay.settings = scene_settings_default();
  app->overlay.hot_slider = OVERLAY_SLIDER_NONE;
  app->overlay.active_slider = OVERLAY_SLIDER_NONE;
  app->overlay.hot_toggle = OVERLAY_TOGGLE_NONE;
  app->overlay.hot_render_quality_preset = -1;
  app->overlay.hot_gpu_preference = -1;
  app->overlay.god_mode_enabled = 0;
  app->overlay.freeze_time_enabled = 0;
  app->overlay.panel_width = platform_get_overlay_width(app);
  app->overlay.panel_collapsed = 0;
  app->overlay.scroll_offset = 0.0f;
  app->overlay.scroll_max = overlay_get_scroll_max_for_window(app->height);
  app->overlay.render_quality_preset = RENDER_QUALITY_PRESET_HIGH;
  app->requested_gpu_preference = GPU_PREFERENCE_MODE_AUTO;
  app->requested_render_quality_preset = RENDER_QUALITY_PRESET_HIGH;
  platform_refresh_gpu_info(app);

  diagnostics_logf(
    "platform_create: success width=%d height=%d gl_version=%s renderer=%s vendor=%s",
    app->width,
    app->height,
    (const char*)glGetString(GL_VERSION),
    (const char*)glGetString(GL_RENDERER),
    (const char*)glGetString(GL_VENDOR)
  );
  gpu_preferences_set_current_renderer(
    &app->overlay.gpu_info,
    (const char*)glGetString(GL_RENDERER),
    (const char*)glGetString(GL_VENDOR));

  glViewport(0, 0, (app->width > 0) ? app->width : 1, (app->height > 0) ? app->height : 1);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  SwapBuffers(app->device_context);

  ShowWindow(app->window, SW_SHOWDEFAULT);
  UpdateWindow(app->window);
  (void)SetForegroundWindow(app->window);
  (void)SetActiveWindow(app->window);
  (void)SetFocus(app->window);
  platform_set_mouse_capture(app, 1);
  return 1;
}

void platform_destroy(PlatformApp* app)
{
  diagnostics_log("platform_destroy: begin");
  app->running = 0;
  platform_set_mouse_capture(app, 0);

  if (app->gl_context != NULL)
  {
    (void)wglMakeCurrent(NULL, NULL);
    (void)wglDeleteContext(app->gl_context);
    app->gl_context = NULL;
  }

  if (app->device_context != NULL && app->window != NULL)
  {
    (void)ReleaseDC(app->window, app->device_context);
    app->device_context = NULL;
  }

  if (app->window != NULL)
  {
    DestroyWindow(app->window);
    app->window = NULL;
  }

  diagnostics_log("platform_destroy: end");
}

void platform_pump_messages(PlatformApp* app, PlatformInput* input)
{
  MSG message = { 0 };
  int has_focus = 0;
  int alt_down = 0;
  int player_mode_down = 0;
  int toggle_cycle_down = 0;
  int reset_cycle_down = 0;
  int increase_speed_down = 0;
  int decrease_speed_down = 0;
  int move_forward_down = 0;
  int move_backward_down = 0;
  int move_left_down = 0;
  int move_right_down = 0;
  int jump_down = 0;
  int move_down_down = 0;
  int fast_modifier_down = 0;
  int left_button_down = 0;
  int right_button_down = 0;

  input->look_x = 0.0f;
  input->look_y = 0.0f;
  input->move_forward = 0.0f;
  input->move_right = 0.0f;
  input->escape_pressed = 0;
  input->toggle_cycle_pressed = 0;
  input->reset_cycle_pressed = 0;
  input->increase_cycle_speed_pressed = 0;
  input->decrease_cycle_speed_pressed = 0;
  input->scrub_backward_held = 0;
  input->scrub_forward_held = 0;
  input->scrub_fast_held = 0;
  input->move_fast_held = 0;
  input->crouch_held = 0;
  input->jump_pressed = 0;
  input->jump_held = 0;
  input->move_down_held = 0;
  input->toggle_player_mode_pressed = 0;
  input->remove_block_pressed = 0;
  input->place_block_pressed = 0;
  input->selected_block_slot = -1;

  while (PeekMessageW(&message, NULL, 0U, 0U, PM_REMOVE) != FALSE)
  {
    if (message.message == WM_QUIT)
    {
      app->running = 0;
      break;
    }

    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  has_focus = platform_window_has_focus(app);
  alt_down = has_focus ? platform_is_action_down(KEYMAP_ACTION_TOGGLE_CURSOR_MODE) : 0;
  if (has_focus && alt_down && !app->previous_alt_down)
  {
    app->cursor_mode_enabled = 1;
    platform_set_mouse_capture(app, 0);
  }
  if (!has_focus)
  {
    platform_set_mouse_capture(app, 0);
  }
  else if (app->cursor_mode_enabled == 0 && app->width > 0 && app->height > 0)
  {
    platform_set_mouse_capture(app, 1);
  }

  platform_update_overlay_interaction(app, has_focus);
  player_mode_down = has_focus ? platform_is_action_down(KEYMAP_ACTION_TOGGLE_PLAYER_MODE) : 0;
  toggle_cycle_down = has_focus ? platform_is_action_down(KEYMAP_ACTION_TOGGLE_CYCLE) : 0;
  reset_cycle_down = has_focus ? platform_is_action_down(KEYMAP_ACTION_RESET_CYCLE) : 0;
  increase_speed_down = has_focus ? platform_is_action_down(KEYMAP_ACTION_CYCLE_FASTER) : 0;
  decrease_speed_down = has_focus ? platform_is_action_down(KEYMAP_ACTION_CYCLE_SLOWER) : 0;
  move_forward_down = has_focus ? platform_is_action_down(KEYMAP_ACTION_MOVE_FORWARD) : 0;
  move_backward_down = has_focus ? platform_is_action_down(KEYMAP_ACTION_MOVE_BACKWARD) : 0;
  move_left_down = has_focus ? platform_is_action_down(KEYMAP_ACTION_MOVE_LEFT) : 0;
  move_right_down = has_focus ? platform_is_action_down(KEYMAP_ACTION_MOVE_RIGHT) : 0;
  jump_down = has_focus ? platform_is_action_down(KEYMAP_ACTION_JUMP) : 0;
  move_down_down = has_focus ? platform_is_action_down(KEYMAP_ACTION_MOVE_DOWN) : 0;
  fast_modifier_down = has_focus ? platform_is_action_down(KEYMAP_ACTION_MOVE_FAST) : 0;
  left_button_down = has_focus ? platform_is_action_down(KEYMAP_ACTION_BREAK_BLOCK) : 0;
  right_button_down = has_focus ? platform_is_action_down(KEYMAP_ACTION_PLACE_BLOCK) : 0;

  if (app->cursor_mode_enabled != 0)
  {
    player_mode_down = 0;
    toggle_cycle_down = 0;
    reset_cycle_down = 0;
    increase_speed_down = 0;
    decrease_speed_down = 0;
    move_forward_down = 0;
    move_backward_down = 0;
    move_left_down = 0;
    move_right_down = 0;
    jump_down = 0;
    move_down_down = 0;
    fast_modifier_down = 0;
    left_button_down = 0;
    right_button_down = 0;
    app->mouse_dx = 0;
    app->mouse_dy = 0;
  }

  if (app->suppress_world_click_until_release != 0)
  {
    left_button_down = 0;
    right_button_down = 0;
    if (!platform_is_action_down(KEYMAP_ACTION_BREAK_BLOCK) && !platform_is_action_down(KEYMAP_ACTION_PLACE_BLOCK))
    {
      app->suppress_world_click_until_release = 0;
    }
  }

  input->look_x = (float)app->mouse_dx;
  input->look_y = (float)app->mouse_dy;
  if (app->suppress_next_mouse_delta != 0)
  {
    input->look_x = 0.0f;
    input->look_y = 0.0f;
    app->suppress_next_mouse_delta = 0;
  }
  input->move_forward = (float)(move_forward_down - move_backward_down);
  input->move_right = (float)(move_right_down - move_left_down);
  input->escape_pressed = app->escape_requested;
  input->toggle_player_mode_pressed = player_mode_down && !app->previous_player_mode_down;
  input->toggle_cycle_pressed = toggle_cycle_down && !app->previous_toggle_cycle_down;
  input->reset_cycle_pressed = reset_cycle_down && !app->previous_reset_cycle_down;
  input->increase_cycle_speed_pressed = increase_speed_down && !app->previous_increase_speed_down;
  input->decrease_cycle_speed_pressed = decrease_speed_down && !app->previous_decrease_speed_down;
  input->scrub_backward_held = (has_focus && app->cursor_mode_enabled == 0) ? platform_is_action_down(KEYMAP_ACTION_SCRUB_BACKWARD) : 0;
  input->scrub_forward_held = (has_focus && app->cursor_mode_enabled == 0) ? platform_is_action_down(KEYMAP_ACTION_SCRUB_FORWARD) : 0;
  input->scrub_fast_held = fast_modifier_down;
  input->move_fast_held = (app->overlay.god_mode_enabled != 0) ? fast_modifier_down : move_down_down;
  input->crouch_held = (app->overlay.god_mode_enabled == 0) ? fast_modifier_down : 0;
  input->jump_pressed = jump_down && !app->previous_jump_down;
  input->jump_held = jump_down;
  input->move_down_held = (app->overlay.god_mode_enabled != 0) ? move_down_down : 0;
  input->remove_block_pressed = left_button_down && !app->previous_world_left_button_down;
  input->place_block_pressed = right_button_down && !app->previous_world_right_button_down;
  if (has_focus && app->cursor_mode_enabled == 0)
  {
    if (platform_is_action_down(KEYMAP_ACTION_BLOCK_SLOT_1))
    {
      input->selected_block_slot = keymap_get_block_slot_index(KEYMAP_ACTION_BLOCK_SLOT_1);
    }
    else if (platform_is_action_down(KEYMAP_ACTION_BLOCK_SLOT_2))
    {
      input->selected_block_slot = keymap_get_block_slot_index(KEYMAP_ACTION_BLOCK_SLOT_2);
    }
    else if (platform_is_action_down(KEYMAP_ACTION_BLOCK_SLOT_3))
    {
      input->selected_block_slot = keymap_get_block_slot_index(KEYMAP_ACTION_BLOCK_SLOT_3);
    }
    else if (platform_is_action_down(KEYMAP_ACTION_BLOCK_SLOT_4))
    {
      input->selected_block_slot = keymap_get_block_slot_index(KEYMAP_ACTION_BLOCK_SLOT_4);
    }
  }

  app->mouse_dx = 0;
  app->mouse_dy = 0;
  app->escape_requested = 0;
  app->previous_player_mode_down = player_mode_down;
  app->previous_toggle_cycle_down = toggle_cycle_down;
  app->previous_reset_cycle_down = reset_cycle_down;
  app->previous_increase_speed_down = increase_speed_down;
  app->previous_decrease_speed_down = decrease_speed_down;
  app->previous_jump_down = jump_down;
  app->previous_alt_down = alt_down;
  app->previous_world_left_button_down = left_button_down;
  app->previous_world_right_button_down = right_button_down;
}

void platform_request_close(PlatformApp* app)
{
  app->running = 0;
}

float platform_get_time_seconds(const PlatformApp* app)
{
  LARGE_INTEGER current = { 0 };
  QueryPerformanceCounter(&current);
  return (float)((double)(current.QuadPart - app->timer_start.QuadPart) / (double)app->timer_frequency.QuadPart);
}

void platform_swap_buffers(const PlatformApp* app)
{
  (void)SwapBuffers(app->device_context);
}

void platform_set_window_title(const PlatformApp* app, const char* title)
{
  if (app->window != NULL)
  {
    (void)SetWindowTextA(app->window, title);
  }
}

void platform_get_scene_settings(const PlatformApp* app, SceneSettings* out_settings)
{
  if (out_settings == NULL)
  {
    return;
  }

  if (app == NULL)
  {
    *out_settings = scene_settings_default();
    return;
  }

  *out_settings = app->overlay.settings;
}

void platform_set_scene_settings(PlatformApp* app, const SceneSettings* settings)
{
  if (app == NULL || settings == NULL)
  {
    return;
  }

  app->overlay.settings = *settings;
}

int platform_get_god_mode_enabled(const PlatformApp* app)
{
  return (app != NULL && app->overlay.god_mode_enabled != 0);
}

void platform_set_god_mode_enabled(PlatformApp* app, int enabled)
{
  if (app == NULL)
  {
    return;
  }

  app->overlay.god_mode_enabled = (enabled != 0);
}

void platform_update_overlay_metrics(PlatformApp* app, const OverlayMetrics* metrics)
{
  if (app == NULL || metrics == NULL)
  {
    return;
  }

  app->overlay.metrics = *metrics;
}

int platform_consume_gpu_switch_request(PlatformApp* app, GpuPreferenceMode* out_mode)
{
  if (app == NULL || app->gpu_switch_requested == 0)
  {
    return 0;
  }

  if (out_mode != NULL)
  {
    *out_mode = app->requested_gpu_preference;
  }

  app->gpu_switch_requested = 0;
  return 1;
}

int platform_consume_render_quality_request(PlatformApp* app, RendererQualityPreset* out_preset)
{
  if (app == NULL || app->render_quality_change_requested == 0)
  {
    return 0;
  }

  if (out_preset != NULL)
  {
    *out_preset = app->requested_render_quality_preset;
  }

  app->render_quality_change_requested = 0;
  return 1;
}

void platform_set_render_quality_preset(PlatformApp* app, RendererQualityPreset preset)
{
  if (app == NULL)
  {
    return;
  }

  app->overlay.render_quality_preset = preset;
  app->requested_render_quality_preset = preset;
}

void platform_refresh_gpu_info(PlatformApp* app)
{
  if (app == NULL)
  {
    return;
  }

  (void)gpu_preferences_query(&app->overlay.gpu_info);
  if (app->gl_context != NULL)
  {
    gpu_preferences_set_current_renderer(
      &app->overlay.gpu_info,
      (const char*)glGetString(GL_RENDERER),
      (const char*)glGetString(GL_VENDOR));
  }
}

void platform_show_error_message(const char* title, const char* message)
{
  diagnostics_logf("%s: %s", title, message);
  (void)MessageBoxA(NULL, message, title, MB_ICONERROR | MB_OK);
}

static LRESULT CALLBACK platform_window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param)
{
  PlatformApp* app = (PlatformApp*)GetWindowLongPtrW(window, GWLP_USERDATA);

  if (message == WM_NCCREATE)
  {
    const CREATESTRUCTW* create_struct = (const CREATESTRUCTW*)l_param;
    SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)create_struct->lpCreateParams);
    return TRUE;
  }

  switch (message)
  {
    case WM_CLOSE:
      if (app != NULL)
      {
        diagnostics_log("platform_window_proc: WM_CLOSE");
        app->running = 0;
      }
      return 0;

    case WM_DESTROY:
      diagnostics_log("platform_window_proc: WM_DESTROY");
      if (app != NULL)
      {
        PostQuitMessage(0);
      }
      return 0;

    case WM_SIZE:
      if (app != NULL)
      {
        app->width = (int)LOWORD((DWORD_PTR)l_param);
        app->height = (int)HIWORD((DWORD_PTR)l_param);
        app->resized = 1;
        app->overlay.panel_width = platform_get_overlay_width(app);
        app->overlay.scroll_max = overlay_get_scroll_max_for_window(app->height);
        if (app->overlay.scroll_offset > app->overlay.scroll_max)
        {
          app->overlay.scroll_offset = app->overlay.scroll_max;
        }
        if (app->mouse_captured != 0)
        {
          platform_update_cursor_clip(app);
        }
      }
      return 0;

    case WM_ACTIVATE:
      if (app != NULL)
      {
        if (LOWORD(w_param) == WA_INACTIVE)
        {
          platform_set_mouse_capture(app, 0);
        }
        else if (app->cursor_mode_enabled == 0)
        {
          platform_set_mouse_capture(app, 1);
        }
      }
      return 0;

    case WM_SETFOCUS:
      if (app != NULL)
      {
        if (app->cursor_mode_enabled == 0)
        {
          platform_set_mouse_capture(app, 1);
        }
      }
      return 0;

    case WM_KILLFOCUS:
      if (app != NULL)
      {
        platform_set_mouse_capture(app, 0);
      }
      return 0;

    case WM_LBUTTONDOWN:
      if (app != NULL && app->cursor_mode_enabled != 0)
      {
        (void)SetFocus(window);
        return 0;
      }
      break;

    case WM_KEYDOWN:
      if (app != NULL && w_param == (WPARAM)keymap_get_primary_virtual_key(KEYMAP_ACTION_TOGGLE_FULLSCREEN))
      {
        if ((((DWORD_PTR)l_param >> 30U) & 0x1U) == 0U)
        {
          platform_toggle_fullscreen(app);
        }
        return 0;
      }
      if (app != NULL && w_param == (WPARAM)keymap_get_primary_virtual_key(KEYMAP_ACTION_CLOSE_APP))
      {
        app->escape_requested = 1;
      }
      return 0;

    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
      if (w_param == (WPARAM)keymap_get_primary_virtual_key(KEYMAP_ACTION_TOGGLE_CURSOR_MODE))
      {
        return 0;
      }
      break;

    case WM_SYSCOMMAND:
      if ((w_param & 0xFFF0U) == SC_KEYMENU)
      {
        return 0;
      }
      break;

    case WM_SETCURSOR:
      if (app != NULL && app->mouse_captured != 0 && LOWORD((DWORD_PTR)l_param) == HTCLIENT)
      {
        (void)SetCursor(NULL);
        return TRUE;
      }
      break;

    case WM_ERASEBKGND:
      return 1;

    case WM_INPUT:
      if (app != NULL)
      {
        platform_accumulate_raw_mouse(app, (HRAWINPUT)l_param);
      }
      return 0;

    case WM_MOUSEWHEEL:
      if (app != NULL && app->cursor_mode_enabled != 0)
      {
        POINT cursor = { (int)(short)LOWORD((DWORD_PTR)l_param), (int)(short)HIWORD((DWORD_PTR)l_param) };
        if (ScreenToClient(window, &cursor) && platform_point_in_overlay(app, cursor.x, cursor.y))
        {
          platform_adjust_overlay_scroll(app, -(float)GET_WHEEL_DELTA_WPARAM(w_param) / 120.0f * 28.0f);
          return 0;
        }
      }
      break;

    default:
      break;
  }

  return DefWindowProcW(window, message, w_param, l_param);
}

static int platform_register_window_class(HINSTANCE instance)
{
  static int is_registered = 0;
  WNDCLASSEXW window_class = { 0 };

  if (is_registered != 0)
  {
    return 1;
  }

  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
  window_class.lpfnWndProc = platform_window_proc;
  window_class.hInstance = instance;
  window_class.hbrBackground = NULL;
  window_class.hCursor = LoadCursorA(NULL, IDC_ARROW);
  window_class.hIcon = platform_load_icon_resource(instance, IDI_APP_ICON, SM_CXICON, SM_CYICON);
  window_class.hIconSm = platform_load_icon_resource(instance, IDI_APP_ICON, SM_CXSMICON, SM_CYSMICON);
  window_class.lpszClassName = k_platform_window_class_name;

  if (RegisterClassExW(&window_class) == 0U && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
  {
    return 0;
  }

  is_registered = 1;
  return 1;
}

static HICON platform_load_icon_resource(HINSTANCE instance, int resource_id, int system_metric_x, int system_metric_y)
{
  const int width = GetSystemMetrics(system_metric_x);
  const int height = GetSystemMetrics(system_metric_y);

  return (HICON)LoadImageW(
    instance,
    MAKEINTRESOURCEW(resource_id),
    IMAGE_ICON,
    (width > 0) ? width : 0,
    (height > 0) ? height : 0,
    LR_DEFAULTCOLOR | LR_SHARED);
}

static int platform_bootstrap_wgl(HINSTANCE instance)
{
  HDC dummy_device_context = NULL;
  HGLRC dummy_context = NULL;
  HWND dummy_window = NULL;
  int success = 0;

  dummy_window = CreateWindowExW(
    0U,
    k_platform_window_class_name,
    L"OpenGLSkyDummy",
    WS_OVERLAPPEDWINDOW,
    0,
    0,
    1,
    1,
    NULL,
    NULL,
    instance,
    NULL
  );
  if (dummy_window == NULL)
  {
    return 0;
  }

  dummy_device_context = GetDC(dummy_window);
  if (dummy_device_context == NULL)
  {
    goto cleanup;
  }

  if (!platform_set_legacy_pixel_format(dummy_device_context))
  {
    goto cleanup;
  }

  dummy_context = wglCreateContext(dummy_device_context);
  if (dummy_context == NULL)
  {
    goto cleanup;
  }

  if (!wglMakeCurrent(dummy_device_context, dummy_context))
  {
    goto cleanup;
  }

  glewExperimental = GL_TRUE;
  if (glewInit() != GLEW_OK)
  {
    goto cleanup;
  }
  (void)glGetError();

  success = 1;

cleanup:
  (void)wglMakeCurrent(NULL, NULL);

  if (dummy_context != NULL)
  {
    (void)wglDeleteContext(dummy_context);
  }

  if (dummy_device_context != NULL)
  {
    (void)ReleaseDC(dummy_window, dummy_device_context);
  }

  if (dummy_window != NULL)
  {
    DestroyWindow(dummy_window);
  }

  return success;
}

static int platform_set_legacy_pixel_format(HDC device_context)
{
  PIXELFORMATDESCRIPTOR descriptor = { 0 };
  int pixel_format = 0;

  descriptor.nSize = sizeof(descriptor);
  descriptor.nVersion = 1;
  descriptor.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
  descriptor.iPixelType = PFD_TYPE_RGBA;
  descriptor.cColorBits = 32;
  descriptor.cAlphaBits = 8;
  descriptor.cDepthBits = 24;
  descriptor.cStencilBits = 8;
  descriptor.iLayerType = PFD_MAIN_PLANE;

  pixel_format = ChoosePixelFormat(device_context, &descriptor);
  if (pixel_format == 0)
  {
    return 0;
  }

  return SetPixelFormat(device_context, pixel_format, &descriptor) != FALSE;
}

static int platform_set_modern_pixel_format(HDC device_context)
{
  if (wglChoosePixelFormatARB != NULL)
  {
    const int pixel_attributes[] = {
      WGL_DRAW_TO_WINDOW_ARB, GL_TRUE,
      WGL_SUPPORT_OPENGL_ARB, GL_TRUE,
      WGL_DOUBLE_BUFFER_ARB, GL_TRUE,
      WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB,
      WGL_COLOR_BITS_ARB, 32,
      WGL_ALPHA_BITS_ARB, 8,
      WGL_DEPTH_BITS_ARB, 24,
      WGL_STENCIL_BITS_ARB, 8,
      0
    };
    PIXELFORMATDESCRIPTOR descriptor = { 0 };
    UINT format_count = 0;
    int pixel_format = 0;

    if (wglChoosePixelFormatARB(device_context, pixel_attributes, NULL, 1, &pixel_format, &format_count) == TRUE &&
      format_count > 0U &&
      DescribePixelFormat(device_context, pixel_format, sizeof(descriptor), &descriptor) != 0)
    {
      return SetPixelFormat(device_context, pixel_format, &descriptor) != FALSE;
    }
  }

  return platform_set_legacy_pixel_format(device_context);
}

static int platform_register_raw_mouse_device(HWND window)
{
  RAWINPUTDEVICE device = { 0 };
  device.usUsagePage = 0x01;
  device.usUsage = 0x02;
  device.dwFlags = 0U;
  device.hwndTarget = window;
  return RegisterRawInputDevices(&device, 1U, sizeof(device)) != FALSE;
}

static void platform_accumulate_raw_mouse(PlatformApp* app, HRAWINPUT input_handle)
{
  RAWINPUT raw_input = { 0 };
  UINT raw_input_size = sizeof(raw_input);
  UINT result = GetRawInputData(input_handle, RID_INPUT, &raw_input, &raw_input_size, sizeof(raw_input.header));

  if (result == raw_input_size && raw_input.header.dwType == RIM_TYPEMOUSE)
  {
    app->mouse_dx += raw_input.data.mouse.lLastX;
    app->mouse_dy += raw_input.data.mouse.lLastY;
  }
}

static int platform_is_virtual_key_down(int virtual_key)
{
  return (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
}

static int platform_is_action_down(KeymapAction action)
{
  const int primary_key = keymap_get_primary_virtual_key(action);
  const int secondary_key = keymap_get_secondary_virtual_key(action);

  if (primary_key != 0 && platform_is_virtual_key_down(primary_key))
  {
    return 1;
  }
  if (secondary_key != 0 && platform_is_virtual_key_down(secondary_key))
  {
    return 1;
  }

  return 0;
}

static int platform_window_has_focus(const PlatformApp* app)
{
  const HWND foreground = GetForegroundWindow();
  const HWND focus = GetFocus();

  if (app == NULL || app->window == NULL || foreground != app->window)
  {
    return 0;
  }

  return (focus == NULL) || (focus == app->window) || (IsChild(app->window, focus) != FALSE);
}

static int platform_get_overlay_width(const PlatformApp* app)
{
  if (app == NULL)
  {
    return OVERLAY_UI_DEFAULT_WIDTH;
  }

  return overlay_get_panel_width_for_window(app->width);
}

static int platform_get_overlay_visible_width(const PlatformApp* app)
{
  if (app == NULL)
  {
    return OVERLAY_UI_DEFAULT_WIDTH;
  }

  return overlay_get_visible_width_for_state(platform_get_overlay_width(app), app->overlay.panel_collapsed);
}

static float platform_clamp_float(float value, float min_value, float max_value)
{
  if (value < min_value)
  {
    return min_value;
  }
  if (value > max_value)
  {
    return max_value;
  }
  return value;
}

static int platform_point_in_client(const PlatformApp* app, int x, int y)
{
  return app != NULL && x >= 0 && y >= 0 && x < app->width && y < app->height;
}

static int platform_point_in_overlay(const PlatformApp* app, int x, int y)
{
  RECT button_rect = { 0, 0, 0, 0 };

  if (!platform_point_in_client(app, x, y))
  {
    return 0;
  }

  if (platform_get_overlay_button_rect(app, &button_rect) &&
    x >= button_rect.left && x <= button_rect.right &&
    y >= button_rect.top && y <= button_rect.bottom)
  {
    return 1;
  }

  return app != NULL && app->overlay.panel_collapsed == 0 && x < platform_get_overlay_width(app);
}

static int platform_point_in_overlay_scroll_view(const PlatformApp* app, int x, int y)
{
  return app != NULL &&
    app->overlay.panel_collapsed == 0 &&
    platform_point_in_overlay(app, x, y) &&
    y >= overlay_get_scroll_view_top() &&
    y < app->height - OVERLAY_UI_MARGIN;
}

static int platform_get_overlay_button_rect(const PlatformApp* app, RECT* out_rect)
{
  RECT rect = { 0, 0, 0, 0 };
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  if (app == NULL || out_rect == NULL)
  {
    return 0;
  }

  (void)overlay_get_panel_toggle_button_rect(
    platform_get_overlay_width(app),
    app->overlay.panel_collapsed,
    &left,
    &top,
    &right,
    &bottom);
  rect.left = left;
  rect.top = top;
  rect.right = right;
  rect.bottom = bottom;
  *out_rect = rect;
  return 1;
}

static int platform_get_overlay_slider_rect(const PlatformApp* app, OverlaySliderId slider_id, RECT* out_rect)
{
  RECT rect = { 0, 0, 0, 0 };
  int y = 0;
  const int panel_width = platform_get_overlay_width(app);
  const int left = OVERLAY_UI_MARGIN;
  const int right = panel_width - OVERLAY_UI_MARGIN;
  int index = 0;

  if (app == NULL || out_rect == NULL)
  {
    return 0;
  }

  y = overlay_get_scroll_view_top() - (int)app->overlay.scroll_offset;

  for (index = 0; index < OVERLAY_SLIDER_COUNT; ++index)
  {
    if (overlay_has_gameplay_toggle_before_slider((OverlaySliderId)index))
    {
      y += overlay_get_gameplay_toggle_block_height();
    }

    if (overlay_has_cloud_toggle_before_slider((OverlaySliderId)index))
    {
      y += overlay_get_cloud_toggle_block_height();
    }

    if (overlay_has_quality_selector_before_slider((OverlaySliderId)index))
    {
      y += overlay_get_quality_selector_block_height();
    }

    if (overlay_has_gpu_selector_before_slider((OverlaySliderId)index))
    {
      y += overlay_get_gpu_selector_block_height();
    }

    if (overlay_has_metric_card_before_slider((OverlaySliderId)index))
    {
      y += OVERLAY_UI_METRIC_HEIGHT + OVERLAY_UI_SECTION_SPACING;
    }

    y += OVERLAY_UI_LABEL_HEIGHT + OVERLAY_UI_ITEM_SPACING;
    rect.left = left;
    rect.top = y;
    rect.right = right;
    rect.bottom = y + OVERLAY_UI_SLIDER_HEIGHT;
    if (index == slider_id)
    {
      *out_rect = rect;
      return 1;
    }
    y += OVERLAY_UI_SLIDER_HEIGHT + OVERLAY_UI_SECTION_SPACING;
  }

  return 0;
}

static int platform_get_overlay_toggle_rect(const PlatformApp* app, OverlayToggleId toggle_id, RECT* out_rect)
{
  RECT rect = { 0, 0, 0, 0 };
  int y = 0;
  const int panel_width = platform_get_overlay_width(app);
  int index = 0;

  if (app == NULL || out_rect == NULL)
  {
    return 0;
  }

  y = overlay_get_scroll_view_top() - (int)app->overlay.scroll_offset;

  for (index = 0; index < OVERLAY_SLIDER_COUNT; ++index)
  {
    if (overlay_has_gameplay_toggle_before_slider((OverlaySliderId)index))
    {
      rect.left = OVERLAY_UI_MARGIN;
      rect.top = y + OVERLAY_UI_LABEL_HEIGHT + OVERLAY_UI_ITEM_SPACING;
      rect.right = panel_width - OVERLAY_UI_MARGIN;
      rect.bottom = rect.top + OVERLAY_UI_CHECKBOX_HEIGHT;
      if (toggle_id == OVERLAY_TOGGLE_GOD_MODE)
      {
        *out_rect = rect;
        return 1;
      }
      y = rect.bottom + OVERLAY_UI_SECTION_SPACING;

      rect.left = OVERLAY_UI_MARGIN;
      rect.top = y + OVERLAY_UI_LABEL_HEIGHT + OVERLAY_UI_ITEM_SPACING;
      rect.right = panel_width - OVERLAY_UI_MARGIN;
      rect.bottom = rect.top + OVERLAY_UI_CHECKBOX_HEIGHT;
      if (toggle_id == OVERLAY_TOGGLE_FREEZE_TIME)
      {
        *out_rect = rect;
        return 1;
      }
      y = rect.bottom + OVERLAY_UI_SECTION_SPACING;

      rect.left = OVERLAY_UI_MARGIN;
      rect.top = y + OVERLAY_UI_LABEL_HEIGHT + OVERLAY_UI_ITEM_SPACING;
      rect.right = panel_width - OVERLAY_UI_MARGIN;
      rect.bottom = rect.top + OVERLAY_UI_CHECKBOX_HEIGHT;
      if (toggle_id == OVERLAY_TOGGLE_SOUND)
      {
        *out_rect = rect;
        return 1;
      }
      y = rect.bottom + OVERLAY_UI_SECTION_SPACING;
    }

    if (overlay_has_cloud_toggle_before_slider((OverlaySliderId)index))
    {
      rect.left = OVERLAY_UI_MARGIN;
      rect.top = y + OVERLAY_UI_LABEL_HEIGHT + OVERLAY_UI_ITEM_SPACING;
      rect.right = panel_width - OVERLAY_UI_MARGIN;
      rect.bottom = rect.top + OVERLAY_UI_CHECKBOX_HEIGHT;
      if (toggle_id == OVERLAY_TOGGLE_CLOUDS)
      {
        *out_rect = rect;
        return 1;
      }
      y = rect.bottom + OVERLAY_UI_SECTION_SPACING;
    }

    if (overlay_has_quality_selector_before_slider((OverlaySliderId)index))
    {
      y += overlay_get_quality_selector_block_height();
    }

    if (overlay_has_gpu_selector_before_slider((OverlaySliderId)index))
    {
      y += overlay_get_gpu_selector_block_height();
    }

    if (overlay_has_metric_card_before_slider((OverlaySliderId)index))
    {
      y += OVERLAY_UI_METRIC_HEIGHT + OVERLAY_UI_SECTION_SPACING;
    }

    y += OVERLAY_UI_LABEL_HEIGHT + OVERLAY_UI_ITEM_SPACING + OVERLAY_UI_SLIDER_HEIGHT + OVERLAY_UI_SECTION_SPACING;
  }

  return 0;
}

static int platform_get_overlay_render_quality_rect(const PlatformApp* app, RendererQualityPreset preset, RECT* out_rect)
{
  RECT rect = { 0, 0, 0, 0 };
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  if (app == NULL || out_rect == NULL)
  {
    return 0;
  }

  if (!overlay_get_render_quality_button_rect(
    platform_get_overlay_width(app),
    app->overlay.scroll_offset,
    preset,
    &left,
    &top,
    &right,
    &bottom))
  {
    return 0;
  }

  rect.left = left;
  rect.top = top;
  rect.right = right;
  rect.bottom = bottom;
  *out_rect = rect;
  return 1;
}

static int platform_get_overlay_gpu_preference_rect(const PlatformApp* app, GpuPreferenceMode mode, RECT* out_rect)
{
  RECT rect = { 0, 0, 0, 0 };
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  if (app == NULL || out_rect == NULL)
  {
    return 0;
  }

  if (!overlay_get_gpu_preference_button_rect(
    platform_get_overlay_width(app),
    app->overlay.scroll_offset,
    mode,
    &left,
    &top,
    &right,
    &bottom))
  {
    return 0;
  }

  rect.left = left;
  rect.top = top;
  rect.right = right;
  rect.bottom = bottom;
  *out_rect = rect;
  return 1;
}

static OverlayToggleId platform_get_hovered_toggle(const PlatformApp* app, int x, int y)
{
  RECT rect = { 0 };

  if (platform_get_overlay_toggle_rect(app, OVERLAY_TOGGLE_GOD_MODE, &rect) &&
    x >= rect.left && x <= rect.right &&
    y >= rect.top && y <= rect.bottom)
  {
    return OVERLAY_TOGGLE_GOD_MODE;
  }

  if (platform_get_overlay_toggle_rect(app, OVERLAY_TOGGLE_FREEZE_TIME, &rect) &&
    x >= rect.left && x <= rect.right &&
    y >= rect.top && y <= rect.bottom)
  {
    return OVERLAY_TOGGLE_FREEZE_TIME;
  }

  if (platform_get_overlay_toggle_rect(app, OVERLAY_TOGGLE_SOUND, &rect) &&
    x >= rect.left && x <= rect.right &&
    y >= rect.top && y <= rect.bottom)
  {
    return OVERLAY_TOGGLE_SOUND;
  }

  if (platform_get_overlay_toggle_rect(app, OVERLAY_TOGGLE_CLOUDS, &rect) &&
    x >= rect.left && x <= rect.right &&
    y >= rect.top && y <= rect.bottom)
  {
    return OVERLAY_TOGGLE_CLOUDS;
  }

  return OVERLAY_TOGGLE_NONE;
}

static OverlaySliderId platform_get_hovered_slider(const PlatformApp* app, int x, int y)
{
  OverlaySliderId slider_id = OVERLAY_SLIDER_SUN_DISTANCE;

  for (slider_id = OVERLAY_SLIDER_SUN_DISTANCE; slider_id < OVERLAY_SLIDER_COUNT; ++slider_id)
  {
    RECT rect = { 0 };
    if (platform_get_overlay_slider_rect(app, slider_id, &rect) &&
      x >= rect.left && x <= rect.right &&
      y >= rect.top && y <= rect.bottom)
    {
      return slider_id;
    }
  }

  return OVERLAY_SLIDER_NONE;
}

static RendererQualityPreset platform_get_hovered_render_quality(const PlatformApp* app, int x, int y)
{
  RendererQualityPreset preset = RENDER_QUALITY_PRESET_HIGH;

  for (preset = RENDER_QUALITY_PRESET_HIGH; preset < RENDER_QUALITY_PRESET_COUNT; ++preset)
  {
    RECT rect = { 0 };
    if (platform_get_overlay_render_quality_rect(app, preset, &rect) &&
      x >= rect.left && x <= rect.right &&
      y >= rect.top && y <= rect.bottom)
    {
      return preset;
    }
  }

  return (RendererQualityPreset)-1;
}

static GpuPreferenceMode platform_get_hovered_gpu_preference(const PlatformApp* app, int x, int y)
{
  GpuPreferenceMode mode = GPU_PREFERENCE_MODE_AUTO;

  for (mode = GPU_PREFERENCE_MODE_AUTO; mode < GPU_PREFERENCE_MODE_COUNT; ++mode)
  {
    RECT rect = { 0 };
    if (platform_get_overlay_gpu_preference_rect(app, mode, &rect) &&
      x >= rect.left && x <= rect.right &&
      y >= rect.top && y <= rect.bottom)
    {
      return mode;
    }
  }

  return (GpuPreferenceMode)-1;
}

static int platform_utf8_to_wide(const char* text, wchar_t* out_text, size_t out_text_count)
{
  const int converted_length = MultiByteToWideChar(
    CP_UTF8,
    0,
    (text != NULL) ? text : "",
    -1,
    out_text,
    (int)out_text_count);

  return converted_length > 0;
}

static void platform_toggle_value(PlatformApp* app, OverlayToggleId toggle_id)
{
  if (app == NULL)
  {
    return;
  }

  switch (toggle_id)
  {
    case OVERLAY_TOGGLE_GOD_MODE:
      app->overlay.god_mode_enabled = (app->overlay.god_mode_enabled == 0);
      break;
    case OVERLAY_TOGGLE_FREEZE_TIME:
      app->overlay.freeze_time_enabled = (app->overlay.freeze_time_enabled == 0);
      break;
    case OVERLAY_TOGGLE_SOUND:
      app->overlay.sound_enabled = (app->overlay.sound_enabled == 0);
      break;
    case OVERLAY_TOGGLE_CLOUDS:
      app->overlay.settings.clouds_enabled = (app->overlay.settings.clouds_enabled == 0);
      break;
    case OVERLAY_TOGGLE_NONE:
    case OVERLAY_TOGGLE_COUNT:
    default:
      break;
  }
}

static void platform_set_slider_value(SceneSettings* settings, OverlaySliderId slider_id, float value)
{
  switch (slider_id)
  {
    case OVERLAY_SLIDER_SUN_DISTANCE:
      settings->sun_distance_mkm = value;
      break;
    case OVERLAY_SLIDER_SUN_ORBIT:
      settings->sun_orbit_degrees = value;
      break;
    case OVERLAY_SLIDER_CYCLE_DURATION:
      settings->cycle_duration_seconds = value;
      break;
    case OVERLAY_SLIDER_DAYLIGHT_FRACTION:
      settings->daylight_fraction = value;
      break;
    case OVERLAY_SLIDER_CAMERA_FOV:
      settings->camera_fov_degrees = value;
      break;
    case OVERLAY_SLIDER_FOG_DENSITY:
      settings->fog_density = value;
      break;
    case OVERLAY_SLIDER_CLOUD_AMOUNT:
      settings->cloud_amount = value;
      break;
    case OVERLAY_SLIDER_CLOUD_SPACING:
      settings->cloud_spacing = value;
      break;
    case OVERLAY_SLIDER_TERRAIN_BASE:
      settings->terrain_base_height = value;
      break;
    case OVERLAY_SLIDER_TERRAIN_HEIGHT:
      settings->terrain_height_scale = value;
      break;
    case OVERLAY_SLIDER_TERRAIN_ROUGHNESS:
      settings->terrain_roughness = value;
      break;
    case OVERLAY_SLIDER_TERRAIN_RIDGE:
      settings->terrain_ridge_strength = value;
      break;
    case OVERLAY_SLIDER_PALM_SIZE:
      settings->palm_size = value;
      break;
    case OVERLAY_SLIDER_PALM_COUNT:
      settings->palm_count = value;
      break;
    case OVERLAY_SLIDER_PALM_FRUIT_DENSITY:
      settings->palm_fruit_density = value;
      break;
    case OVERLAY_SLIDER_PALM_RENDER_RADIUS:
      settings->palm_render_radius = value;
      break;
    case OVERLAY_SLIDER_NONE:
    case OVERLAY_SLIDER_COUNT:
    default:
      break;
  }
}

static void platform_adjust_overlay_scroll(PlatformApp* app, float delta)
{
  if (app == NULL)
  {
    return;
  }

  app->overlay.scroll_max = overlay_get_scroll_max_for_window(app->height);
  app->overlay.scroll_offset += delta;
  if (app->overlay.scroll_offset < 0.0f)
  {
    app->overlay.scroll_offset = 0.0f;
  }
  if (app->overlay.scroll_offset > app->overlay.scroll_max)
  {
    app->overlay.scroll_offset = app->overlay.scroll_max;
  }
}

static void platform_update_overlay_interaction(PlatformApp* app, int has_focus)
{
  POINT cursor = { 0, 0 };
  int left_button_down = 0;
  RECT button_rect = { 0, 0, 0, 0 };
  int button_hovered = 0;

  if (app == NULL)
  {
    return;
  }

  app->overlay.panel_width = platform_get_overlay_width(app);
  app->overlay.cursor_mode_enabled = app->cursor_mode_enabled;
  app->overlay.hot_slider = OVERLAY_SLIDER_NONE;
  app->overlay.hot_toggle = OVERLAY_TOGGLE_NONE;
  app->overlay.hot_render_quality_preset = -1;
  app->overlay.hot_gpu_preference = -1;
  app->overlay.scroll_max = overlay_get_scroll_max_for_window(app->height);
  if (app->overlay.scroll_offset > app->overlay.scroll_max)
  {
    app->overlay.scroll_offset = app->overlay.scroll_max;
  }

  if (has_focus && GetCursorPos(&cursor) && ScreenToClient(app->window, &cursor))
  {
    app->overlay.mouse_x = cursor.x;
    app->overlay.mouse_y = cursor.y;
  }

  if (app->cursor_mode_enabled == 0 || !has_focus)
  {
    app->overlay.active_slider = OVERLAY_SLIDER_NONE;
    app->previous_left_button_down = 0;
    return;
  }

  left_button_down = platform_is_virtual_key_down(VK_LBUTTON);
  if (platform_get_overlay_button_rect(app, &button_rect) &&
    app->overlay.mouse_x >= button_rect.left && app->overlay.mouse_x <= button_rect.right &&
    app->overlay.mouse_y >= button_rect.top && app->overlay.mouse_y <= button_rect.bottom)
  {
    button_hovered = 1;
  }

  if (platform_point_in_overlay_scroll_view(app, app->overlay.mouse_x, app->overlay.mouse_y))
  {
    app->overlay.hot_toggle = platform_get_hovered_toggle(app, app->overlay.mouse_x, app->overlay.mouse_y);
    app->overlay.hot_slider = platform_get_hovered_slider(app, app->overlay.mouse_x, app->overlay.mouse_y);
    app->overlay.hot_render_quality_preset = (int)platform_get_hovered_render_quality(app, app->overlay.mouse_x, app->overlay.mouse_y);
    app->overlay.hot_gpu_preference = (int)platform_get_hovered_gpu_preference(app, app->overlay.mouse_x, app->overlay.mouse_y);
  }

  if (left_button_down && !app->previous_left_button_down)
  {
    if (button_hovered)
    {
      app->overlay.panel_collapsed = (app->overlay.panel_collapsed == 0);
      app->overlay.active_slider = OVERLAY_SLIDER_NONE;
    }
    else if (app->overlay.hot_toggle != OVERLAY_TOGGLE_NONE)
    {
      platform_toggle_value(app, (OverlayToggleId)app->overlay.hot_toggle);
      app->overlay.active_slider = OVERLAY_SLIDER_NONE;
    }
    else if (app->overlay.hot_render_quality_preset >= (int)RENDER_QUALITY_PRESET_HIGH &&
      app->overlay.hot_render_quality_preset < (int)RENDER_QUALITY_PRESET_COUNT)
    {
      app->render_quality_change_requested = 1;
      app->requested_render_quality_preset = (RendererQualityPreset)app->overlay.hot_render_quality_preset;
      app->overlay.render_quality_preset = app->requested_render_quality_preset;
      app->overlay.active_slider = OVERLAY_SLIDER_NONE;
    }
    else if (app->overlay.hot_gpu_preference >= (int)GPU_PREFERENCE_MODE_AUTO &&
      app->overlay.hot_gpu_preference < (int)GPU_PREFERENCE_MODE_COUNT)
    {
      app->gpu_switch_requested = 1;
      app->requested_gpu_preference = (GpuPreferenceMode)app->overlay.hot_gpu_preference;
      app->overlay.active_slider = OVERLAY_SLIDER_NONE;
    }
    else if (app->overlay.hot_slider != OVERLAY_SLIDER_NONE)
    {
      app->overlay.active_slider = app->overlay.hot_slider;
      if (app->overlay.active_slider == OVERLAY_SLIDER_SUN_ORBIT)
      {
        app->overlay.freeze_time_enabled = 1;
      }
    }
    else if (platform_point_in_client(app, app->overlay.mouse_x, app->overlay.mouse_y) && !platform_point_in_overlay(app, app->overlay.mouse_x, app->overlay.mouse_y))
    {
      app->overlay.active_slider = OVERLAY_SLIDER_NONE;
      app->cursor_mode_enabled = 0;
      app->overlay.cursor_mode_enabled = 0;
      app->suppress_world_click_until_release = 1;
      (void)SetFocus(app->window);
      platform_set_mouse_capture(app, 1);
      left_button_down = 0;
    }
  }

  if (!left_button_down)
  {
    app->overlay.active_slider = OVERLAY_SLIDER_NONE;
  }
  else if (app->overlay.active_slider != OVERLAY_SLIDER_NONE)
  {
    RECT slider_rect = { 0 };
    float min_value = 0.0f;
    float max_value = 1.0f;
    if (platform_get_overlay_slider_rect(app, app->overlay.active_slider, &slider_rect))
    {
      const float span = (float)((slider_rect.right - slider_rect.left > 1) ? (slider_rect.right - slider_rect.left) : 1);
      const float normalized = platform_clamp_float((float)(app->overlay.mouse_x - slider_rect.left) / span, 0.0f, 1.0f);
      overlay_get_slider_range(app->overlay.active_slider, &min_value, &max_value);
      platform_set_slider_value(&app->overlay.settings, app->overlay.active_slider, min_value + (max_value - min_value) * normalized);
      if (app->overlay.active_slider == OVERLAY_SLIDER_SUN_ORBIT)
      {
        app->overlay.freeze_time_enabled = 1;
      }
    }
  }

  app->previous_left_button_down = left_button_down;
}

static void platform_set_cursor_hidden(PlatformApp* app, int hidden)
{
  if (hidden != 0)
  {
    if (app->cursor_hidden == 0)
    {
      while (ShowCursor(FALSE) >= 0)
      {
      }
      app->cursor_hidden = 1;
    }
    return;
  }

  if (app->cursor_hidden != 0)
  {
    while (ShowCursor(TRUE) < 0)
    {
    }
    app->cursor_hidden = 0;
  }
}

static void platform_update_cursor_clip(const PlatformApp* app)
{
  RECT client_rect = { 0 };
  POINT top_left = { 0, 0 };
  POINT bottom_right = { 0, 0 };
  const int overlay_width = platform_get_overlay_visible_width(app);

  if (app->window == NULL || app->width <= 0 || app->height <= 0)
  {
    return;
  }

  if (!GetClientRect(app->window, &client_rect))
  {
    return;
  }

  if (client_rect.right - client_rect.left > overlay_width + 80)
  {
    client_rect.left += overlay_width;
  }

  top_left.x = client_rect.left;
  top_left.y = client_rect.top;
  bottom_right.x = client_rect.right;
  bottom_right.y = client_rect.bottom;
  if (!ClientToScreen(app->window, &top_left) || !ClientToScreen(app->window, &bottom_right))
  {
    return;
  }

  client_rect.left = top_left.x;
  client_rect.top = top_left.y;
  client_rect.right = bottom_right.x;
  client_rect.bottom = bottom_right.y;
  (void)ClipCursor(&client_rect);
}

static void platform_center_cursor(const PlatformApp* app)
{
  RECT client_rect = { 0 };
  POINT center = { 0, 0 };
  const int overlay_width = platform_get_overlay_visible_width(app);

  if (app->window == NULL || app->width <= 0 || app->height <= 0)
  {
    return;
  }

  if (!GetClientRect(app->window, &client_rect))
  {
    return;
  }

  if (client_rect.right - client_rect.left > overlay_width + 80)
  {
    client_rect.left += overlay_width;
  }

  center.x = (client_rect.left + client_rect.right) / 2;
  center.y = (client_rect.top + client_rect.bottom) / 2;
  if (!ClientToScreen(app->window, &center))
  {
    return;
  }

  (void)SetCursorPos(center.x, center.y);
}

static void platform_set_mouse_capture(PlatformApp* app, int enabled)
{
  if (enabled != 0)
  {
    if (app->mouse_captured == 0 && app->window != NULL)
    {
      (void)SetCapture(app->window);
      platform_update_cursor_clip(app);
      platform_center_cursor(app);
      app->mouse_dx = 0;
      app->mouse_dy = 0;
      app->suppress_next_mouse_delta = 1;
      platform_set_cursor_hidden(app, 1);
      app->mouse_captured = 1;
    }
    return;
  }

  if (app->mouse_captured != 0)
  {
    (void)ClipCursor(NULL);
    (void)ReleaseCapture();
    platform_set_cursor_hidden(app, 0);
    app->mouse_captured = 0;
  }
}

static void platform_toggle_fullscreen(PlatformApp* app)
{
  MONITORINFO monitor_info = { 0 };
  HMONITOR monitor = NULL;

  if (app == NULL || app->window == NULL)
  {
    return;
  }

  monitor = MonitorFromWindow(app->window, MONITOR_DEFAULTTONEAREST);
  monitor_info.cbSize = sizeof(monitor_info);
  if (monitor == NULL || !GetMonitorInfoW(monitor, &monitor_info))
  {
    return;
  }

  if (app->fullscreen_enabled == 0)
  {
    LONG_PTR style = GetWindowLongPtrW(app->window, GWL_STYLE);
    LONG_PTR ex_style = GetWindowLongPtrW(app->window, GWL_EXSTYLE);
    const RECT* monitor_rect = &monitor_info.rcMonitor;

    app->windowed_style = style;
    app->windowed_ex_style = ex_style;
    (void)SetWindowLongPtrW(app->window, GWL_STYLE, (style & ~((LONG_PTR)WS_OVERLAPPEDWINDOW)) | WS_VISIBLE | WS_POPUP);
    (void)SetWindowLongPtrW(app->window, GWL_EXSTYLE, ex_style & ~((LONG_PTR)WS_EX_WINDOWEDGE | (LONG_PTR)WS_EX_CLIENTEDGE));
    (void)SetWindowPos(
      app->window,
      HWND_TOP,
      monitor_rect->left,
      monitor_rect->top,
      monitor_rect->right - monitor_rect->left,
      monitor_rect->bottom - monitor_rect->top,
      SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    app->fullscreen_enabled = 1;
  }
  else
  {
    LONG_PTR style = (app->windowed_style != 0) ? app->windowed_style : (LONG_PTR)(WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    LONG_PTR ex_style = app->windowed_ex_style;
    const RECT* work_rect = &monitor_info.rcWork;

    (void)SetWindowLongPtrW(app->window, GWL_STYLE, style | WS_VISIBLE);
    (void)SetWindowLongPtrW(app->window, GWL_EXSTYLE, ex_style);
    (void)SetWindowPos(
      app->window,
      HWND_NOTOPMOST,
      work_rect->left,
      work_rect->top,
      work_rect->right - work_rect->left,
      work_rect->bottom - work_rect->top,
      SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    (void)ShowWindow(app->window, SW_MAXIMIZE);
    app->fullscreen_enabled = 0;
  }

  app->resized = 1;
  app->overlay.panel_width = platform_get_overlay_width(app);
  app->overlay.scroll_max = overlay_get_scroll_max_for_window(app->height);
  if (app->overlay.scroll_offset > app->overlay.scroll_max)
  {
    app->overlay.scroll_offset = app->overlay.scroll_max;
  }

  if (app->mouse_captured != 0)
  {
    platform_update_cursor_clip(app);
    platform_center_cursor(app);
    app->mouse_dx = 0;
    app->mouse_dy = 0;
    app->suppress_next_mouse_delta = 1;
  }
}
