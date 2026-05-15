#include "console_overlay.h"

#if !defined(_WIN32) && !defined(__linux__)

#include <string.h>

int console_overlay_create(ConsoleOverlay* overlay)
{
  if (overlay == NULL)
  {
    return 0;
  }

  memset(overlay, 0, sizeof(*overlay));
  return 1;
}

void console_overlay_destroy(ConsoleOverlay* overlay)
{
  if (overlay == NULL)
  {
    return;
  }

  memset(overlay, 0, sizeof(*overlay));
}

void console_overlay_render(ConsoleOverlay* overlay, int width, int height, const OverlayState* state)
{
  (void)overlay;
  (void)width;
  (void)height;
  (void)state;
}

#else

#include "diagnostics.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define CONSOLE_OVERLAY_PATH_MAX MAX_PATH
#elif defined(__linux__)
#include <limits.h>
#include <unistd.h>
#include <X11/Xlib.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define CONSOLE_OVERLAY_PATH_MAX PATH_MAX

extern Display* glXGetCurrentDisplay(void);
extern void glXUseXFont(Font font, int first, int count, int list_base);
#endif

#ifndef MAX_PATH
#define MAX_PATH CONSOLE_OVERLAY_PATH_MAX
#endif

static void console_overlay_show_error(const char* title, const char* message);
static int console_overlay_build_font(GLuint* out_font_base);
static void console_overlay_draw_rect(float left, float top, float right, float bottom, float r, float g, float b, float a);
static void console_overlay_draw_outline(float left, float top, float right, float bottom, float r, float g, float b, float a);
static void console_overlay_draw_circle(float center_x, float center_y, float radius, float r, float g, float b, float a);
static void console_overlay_draw_circle_outline(float center_x, float center_y, float radius, float r, float g, float b, float a);
static void console_overlay_draw_hamburger_icon(float center_x, float center_y, float tone);
static void console_overlay_draw_chevron_icon(float center_x, float center_y, float tone);
static void console_overlay_draw_panel_toggle_button(const OverlayState* overlay, float left, float top, float right, float bottom, int hovered);
static int console_overlay_build_shader_path(const char* relative_path, char* out_path, size_t out_path_size);
static int console_overlay_load_text_file(const char* path, const char* label, char** out_source);
static int console_overlay_append_shader_source(char** buffer, size_t* length, size_t* capacity, const char* text, size_t text_length);
static int console_overlay_resolve_shader_include_path(const char* source_path, const char* include_path, char* out_path, size_t out_path_size);
static int console_overlay_load_shader_source(const char* path, const char* label, int include_depth, char** out_source);
static int console_overlay_check_shader(GLuint shader, const char* label);
static int console_overlay_check_program(GLuint program, const char* label);
static GLuint console_overlay_compile_shader(GLenum shader_type, const char* source, const char* label);
static GLuint console_overlay_create_program_from_files(const char* vertex_path, const char* fragment_path, const char* label);
static int console_overlay_create_backdrop_pipeline(ConsoleOverlay* overlay);
static void console_overlay_update_backdrop_geometry(ConsoleOverlay* overlay, float panel_width, float panel_height);
static int console_overlay_ensure_backdrop_texture(ConsoleOverlay* overlay, int width, int height);
static void console_overlay_capture_backdrop(ConsoleOverlay* overlay, int width, int height);
static void console_overlay_draw_backdrop(ConsoleOverlay* overlay, float viewport_width, float viewport_height, float panel_width, float panel_height);
static void console_overlay_draw_text(const ConsoleOverlay* overlay, float x, float y, float r, float g, float b, const char* text);
static void console_overlay_copy_text_fit(char* buffer, size_t buffer_size, const char* text, float max_width);
static int console_overlay_get_toggle_rect(const OverlayState* overlay, OverlayToggleId toggle_id, float* out_left, float* out_top, float* out_right, float* out_bottom);
static int console_overlay_get_slider_rect(const OverlayState* overlay, OverlaySliderId slider_id, float* out_left, float* out_top, float* out_right, float* out_bottom);
static int console_overlay_get_render_quality_rect(const OverlayState* overlay, RendererQualityPreset preset, float* out_left, float* out_top, float* out_right, float* out_bottom);
static int console_overlay_get_gpu_preference_rect(const OverlayState* overlay, GpuPreferenceMode mode, float* out_left, float* out_top, float* out_right, float* out_bottom);
static float console_overlay_get_slider_value(const SceneSettings* settings, OverlaySliderId slider_id);
static void console_overlay_format_slider_value(char* buffer, size_t buffer_size, OverlaySliderId slider_id, float value);

enum
{
  CONSOLE_OVERLAY_SHADER_INCLUDE_DEPTH_MAX = 8
};

int console_overlay_create(ConsoleOverlay* overlay)
{
  if (overlay == NULL)
  {
    return 0;
  }

  memset(overlay, 0, sizeof(*overlay));
  if (!console_overlay_build_font(&overlay->font_base) ||
    !console_overlay_create_backdrop_pipeline(overlay))
  {
    console_overlay_destroy(overlay);
    return 0;
  }
  return 1;
}

void console_overlay_destroy(ConsoleOverlay* overlay)
{
  if (overlay == NULL)
  {
    return;
  }

  if (overlay->font_base != 0U)
  {
    glDeleteLists(overlay->font_base, 96);
    overlay->font_base = 0U;
  }

  if (overlay->backdrop_texture != 0U)
  {
    glDeleteTextures(1, &overlay->backdrop_texture);
    overlay->backdrop_texture = 0U;
  }
  if (overlay->backdrop_vertex_buffer != 0U)
  {
    glDeleteBuffers(1, &overlay->backdrop_vertex_buffer);
    overlay->backdrop_vertex_buffer = 0U;
  }
  if (overlay->backdrop_vao != 0U)
  {
    glDeleteVertexArrays(1, &overlay->backdrop_vao);
    overlay->backdrop_vao = 0U;
  }
  if (overlay->backdrop_program != 0U)
  {
    glDeleteProgram(overlay->backdrop_program);
    overlay->backdrop_program = 0U;
  }
  overlay->backdrop_width = 0;
  overlay->backdrop_height = 0;
  overlay->backdrop_viewport_size_location = -1;
  overlay->backdrop_texel_size_location = -1;
  overlay->backdrop_blur_radius_location = -1;
  overlay->backdrop_sampler_location = -1;
}

void console_overlay_render(ConsoleOverlay* overlay, int width, int height, const OverlayState* state)
{
  const OverlayState fallback_overlay = {
    .settings = { 149.6f, 82.8f, 180.0f, 0.5f, 65.0f, 0.42f, 0.62f, 1.0f, -14.0f, 1.0f, 1.0f, 1.0f, 1.0f, 24.0f, 0.55f, 260.0f, 1 },
    .metrics = { 149.6f, 90.0f, 90.0f, 60.0f, 16.6f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1, 1, 0, 0, 0U },
    .panel_width = OVERLAY_UI_DEFAULT_WIDTH,
    .panel_collapsed = 0,
    .mouse_x = 0,
    .mouse_y = 0,
    .cursor_mode_enabled = 0,
    .hot_slider = OVERLAY_SLIDER_NONE,
    .active_slider = OVERLAY_SLIDER_NONE,
    .hot_toggle = OVERLAY_TOGGLE_NONE,
    .hot_render_quality_preset = -1,
    .hot_gpu_preference = -1,
    .god_mode_enabled = 0,
    .freeze_time_enabled = 0,
    .sound_enabled = 1,
    .ui_time_seconds = 0.0f,
    .scroll_offset = 0.0f,
    .scroll_max = 0.0f
  };
  const OverlayState* active_overlay = (state != NULL) ? state : &fallback_overlay;
  const float panel_width = (float)((active_overlay->panel_width > 0) ? active_overlay->panel_width : OVERLAY_UI_DEFAULT_WIDTH);
  const float panel_height = (float)height;
  const float scroll_view_top = (float)overlay_get_scroll_view_top();
  const float header_x = (float)OVERLAY_UI_MARGIN;
  const float content_width = panel_width - (float)(OVERLAY_UI_MARGIN * 2);
  const float glare_factor = powf(149.6f / ((active_overlay->metrics.sun_distance_mkm > 1.0f) ? active_overlay->metrics.sun_distance_mkm : 1.0f), 1.35f);
  int toggle_button_left_i = 0;
  int toggle_button_top_i = 0;
  int toggle_button_right_i = 0;
  int toggle_button_bottom_i = 0;
  float toggle_button_left = 0.0f;
  float toggle_button_top = 0.0f;
  float toggle_button_right = 0.0f;
  float toggle_button_bottom = 0.0f;
  const int panel_collapsed = active_overlay->panel_collapsed != 0;
  int button_hovered = 0;
  char text_buffer[128] = { 0 };
  char value_buffer[64] = { 0 };
  char hint_buffer[128] = { 0 };
  OverlaySliderId slider_id = OVERLAY_SLIDER_SUN_DISTANCE;

  if (overlay == NULL || overlay->font_base == 0U || width <= 0 || height <= 0)
  {
    return;
  }

  (void)overlay_get_panel_toggle_button_rect(
    active_overlay->panel_width,
    active_overlay->panel_collapsed,
    &toggle_button_left_i,
    &toggle_button_top_i,
    &toggle_button_right_i,
    &toggle_button_bottom_i);
  toggle_button_left = (float)toggle_button_left_i;
  toggle_button_top = (float)toggle_button_top_i;
  toggle_button_right = (float)toggle_button_right_i;
  toggle_button_bottom = (float)toggle_button_bottom_i;
  button_hovered =
    active_overlay->cursor_mode_enabled != 0 &&
    active_overlay->mouse_x >= toggle_button_left_i &&
    active_overlay->mouse_x <= toggle_button_right_i &&
    active_overlay->mouse_y >= toggle_button_top_i &&
    active_overlay->mouse_y <= toggle_button_bottom_i;

  glUseProgram(0);
  glBindVertexArray(0);
  glBindTexture(GL_TEXTURE_2D, 0);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glLoadIdentity();
  glOrtho(0.0, (GLdouble)width, (GLdouble)height, 0.0, -1.0, 1.0);
  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  glLoadIdentity();

  if (!panel_collapsed)
  {
    console_overlay_capture_backdrop(overlay, (int)panel_width, height);
    console_overlay_draw_backdrop(overlay, (float)width, (float)height, panel_width, panel_height);
    console_overlay_draw_rect(0.0f, 0.0f, panel_width, panel_height, 0.05f, 0.07f, 0.10f, active_overlay->cursor_mode_enabled ? 0.30f : 0.24f);
    console_overlay_draw_rect(0.0f, 0.0f, panel_width, 74.0f, 0.25f, 0.30f, 0.38f, active_overlay->cursor_mode_enabled ? 0.16f : 0.11f);
    console_overlay_draw_rect(0.0f, 0.0f, panel_width, panel_height * 0.34f, 0.60f, 0.68f, 0.82f, active_overlay->cursor_mode_enabled ? 0.08f : 0.05f);
    console_overlay_draw_rect(panel_width - 1.0f, 0.0f, panel_width, panel_height, 0.68f, 0.76f, 0.90f, 0.34f);
    console_overlay_draw_rect(0.0f, 0.0f, panel_width, 1.0f, 0.88f, 0.92f, 0.99f, 0.24f);
    console_overlay_draw_outline(0.5f, 0.5f, panel_width - 0.5f, panel_height - 0.5f, 0.74f, 0.82f, 0.94f, 0.16f);
    console_overlay_draw_text(overlay, header_x, (float)(OVERLAY_UI_MARGIN + 14), 0.95f, 0.96f, 0.98f, "Scene Overlay");

    console_overlay_copy_text_fit(
      hint_buffer,
      sizeof(hint_buffer),
      active_overlay->cursor_mode_enabled ? "Cursor active" : "Overlay locked",
      content_width
    );
    console_overlay_draw_text(
      overlay,
      header_x,
      (float)(OVERLAY_UI_MARGIN + OVERLAY_UI_TITLE_HEIGHT + 12),
      active_overlay->cursor_mode_enabled ? 0.90f : 0.68f,
      active_overlay->cursor_mode_enabled ? 0.82f : 0.76f,
      active_overlay->cursor_mode_enabled ? 0.48f : 0.82f,
      hint_buffer
    );

    console_overlay_copy_text_fit(
      hint_buffer,
      sizeof(hint_buffer),
      active_overlay->cursor_mode_enabled ? "Drag sliders or click world" : "Press Alt to edit scene",
      content_width
    );
    console_overlay_draw_text(
      overlay,
      header_x,
      (float)(OVERLAY_UI_MARGIN + OVERLAY_UI_TITLE_HEIGHT + 28),
      0.62f,
      0.68f,
      0.76f,
      hint_buffer
    );

    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, (GLsizei)panel_width, height);
    glScissor(0, 0, (GLsizei)panel_width, (GLsizei)((height > (int)scroll_view_top) ? (height - (int)scroll_view_top) : 0));

    for (slider_id = OVERLAY_SLIDER_SUN_DISTANCE; slider_id < OVERLAY_SLIDER_COUNT; ++slider_id)
    {
      float min_value = 0.0f;
      float max_value = 1.0f;
      float slider_left = 0.0f;
      float slider_top = 0.0f;
      float slider_right = 0.0f;
      float slider_bottom = 0.0f;
      float track_left = 0.0f;
      float track_right = 0.0f;
      float track_top = 0.0f;
      float track_bottom = 0.0f;
      float knob_center_x = 0.0f;
      float normalized = 0.0f;
      float tone = 1.0f;
      const int is_hot = active_overlay->hot_slider == slider_id;
      const int is_active = active_overlay->active_slider == slider_id;
      const float value = console_overlay_get_slider_value(&active_overlay->settings, slider_id);

      if ((slider_id == OVERLAY_SLIDER_CLOUD_AMOUNT || slider_id == OVERLAY_SLIDER_CLOUD_SPACING) && active_overlay->settings.clouds_enabled == 0)
      {
        tone = 0.55f;
      }

      overlay_get_slider_range(slider_id, &min_value, &max_value);
      if (!console_overlay_get_slider_rect(active_overlay, slider_id, &slider_left, &slider_top, &slider_right, &slider_bottom))
      {
        continue;
      }

      normalized = (max_value > min_value) ? (value - min_value) / (max_value - min_value) : 0.0f;
      if (normalized < 0.0f)
      {
        normalized = 0.0f;
      }
      else if (normalized > 1.0f)
      {
        normalized = 1.0f;
      }

      console_overlay_format_slider_value(value_buffer, sizeof(value_buffer), slider_id, value);
      console_overlay_copy_text_fit(
        text_buffer,
        sizeof(text_buffer),
        overlay_get_slider_title(slider_id),
        (slider_right - slider_left) - ((float)strlen(value_buffer) * 8.0f) - 18.0f
      );
      console_overlay_draw_text(overlay, slider_left, slider_top - 8.0f, 0.86f * tone, 0.88f * tone, 0.92f * tone, text_buffer);
      console_overlay_draw_text(
        overlay,
        slider_right - (float)strlen(value_buffer) * 8.0f,
        slider_top - 8.0f,
        0.66f * tone,
        0.72f * tone,
        0.80f * tone,
        value_buffer
      );
      track_left = slider_left;
      track_right = slider_right;
      track_top = slider_top + (float)((OVERLAY_UI_SLIDER_HEIGHT - OVERLAY_UI_SLIDER_TRACK_HEIGHT) / 2);
      track_bottom = track_top + (float)OVERLAY_UI_SLIDER_TRACK_HEIGHT;
      knob_center_x = track_left + (track_right - track_left) * normalized;

      console_overlay_draw_rect(slider_left, slider_top, slider_right, slider_bottom, 0.10f * tone, 0.11f * tone, 0.14f * tone, 0.96f);
      console_overlay_draw_outline(slider_left, slider_top, slider_right, slider_bottom, 0.23f * tone, 0.26f * tone, 0.32f * tone, 1.0f);
      console_overlay_draw_rect(track_left, track_top, track_right, track_bottom, 0.15f * tone, 0.16f * tone, 0.19f * tone, 1.0f);
      console_overlay_draw_rect(
        track_left,
        track_top,
        knob_center_x,
        track_bottom,
        (is_active ? 0.92f : 0.74f) * tone,
        (is_active ? 0.64f : 0.46f) * tone,
        (is_active ? 0.25f : 0.31f) * tone,
        1.0f
      );
      console_overlay_draw_rect(
        knob_center_x - 6.0f,
        slider_top + 4.0f,
        knob_center_x + 6.0f,
        slider_bottom - 4.0f,
        (is_active ? 0.98f : (is_hot ? 0.91f : 0.82f)) * tone,
        (is_active ? 0.77f : (is_hot ? 0.71f : 0.74f)) * tone,
        (is_active ? 0.32f : (is_hot ? 0.40f : 0.78f)) * tone,
        1.0f
      );
    }

    {
    float toggle_left = 0.0f;
      float toggle_top = 0.0f;
      float toggle_right = 0.0f;
      float toggle_bottom = 0.0f;
      const int enabled = active_overlay->god_mode_enabled != 0;
      const int hot = active_overlay->hot_toggle == OVERLAY_TOGGLE_GOD_MODE;
      const float box_left = (float)OVERLAY_UI_MARGIN;
      float box_top = 0.0f;
      const float pulse = enabled ? (0.80f + 0.20f * (0.5f + 0.5f * sinf(active_overlay->ui_time_seconds * 5.0f))) : 1.0f;

      if (console_overlay_get_toggle_rect(active_overlay, OVERLAY_TOGGLE_GOD_MODE, &toggle_left, &toggle_top, &toggle_right, &toggle_bottom))
      {
        box_top = toggle_top + (float)((OVERLAY_UI_CHECKBOX_HEIGHT - OVERLAY_UI_CHECKBOX_SIZE) / 2);
        console_overlay_draw_text(overlay, toggle_left, toggle_top - 8.0f, 0.86f, 0.88f, 0.92f, "Player mode");
        if (enabled || hot)
        {
          console_overlay_draw_rect(
            box_left - 3.0f,
            box_top - 3.0f,
            box_left + (float)OVERLAY_UI_CHECKBOX_SIZE + 3.0f,
            box_top + (float)OVERLAY_UI_CHECKBOX_SIZE + 3.0f,
            0.18f * pulse,
            0.28f * pulse,
            0.36f * pulse,
            hot ? 0.30f : 0.18f
          );
        }
        console_overlay_draw_rect(box_left, box_top, box_left + (float)OVERLAY_UI_CHECKBOX_SIZE, box_top + (float)OVERLAY_UI_CHECKBOX_SIZE, 0.10f, 0.11f, 0.14f, 0.96f);
        console_overlay_draw_outline(
          box_left,
          box_top,
          box_left + (float)OVERLAY_UI_CHECKBOX_SIZE,
          box_top + (float)OVERLAY_UI_CHECKBOX_SIZE,
          hot ? 0.58f : 0.23f * pulse,
          hot ? 0.84f : 0.26f * pulse,
          hot ? 0.94f : 0.32f * pulse,
          1.0f
        );
        if (enabled)
        {
          console_overlay_draw_rect(
            box_left + 3.0f,
            box_top + 3.0f,
            box_left + (float)OVERLAY_UI_CHECKBOX_SIZE - 3.0f,
            box_top + (float)OVERLAY_UI_CHECKBOX_SIZE - 3.0f,
            0.38f * pulse,
            0.82f * pulse,
            0.96f * pulse,
            1.0f
          );
        }
        console_overlay_draw_text(
          overlay,
          box_left + (float)OVERLAY_UI_CHECKBOX_SIZE + 10.0f,
          toggle_top + 15.0f,
          enabled ? 0.70f * pulse : 0.86f,
          enabled ? 0.92f * pulse : 0.76f,
          enabled ? 0.98f * pulse : 0.82f,
          enabled ? "God" : "Survival"
        );
        console_overlay_draw_text(
          overlay,
          box_left + (float)OVERLAY_UI_CHECKBOX_SIZE + 70.0f,
          toggle_top + 15.0f,
          0.64f,
          0.70f,
          0.78f,
          overlay_get_toggle_title(OVERLAY_TOGGLE_GOD_MODE)
      );
    }
  }

  {
    float toggle_left = 0.0f;
    float toggle_top = 0.0f;
    float toggle_right = 0.0f;
    float toggle_bottom = 0.0f;
    const int enabled = active_overlay->freeze_time_enabled != 0;
    const int hot = active_overlay->hot_toggle == OVERLAY_TOGGLE_FREEZE_TIME;
    const float box_left = (float)OVERLAY_UI_MARGIN;
    float box_top = 0.0f;
    const float pulse = enabled ? (0.82f + 0.18f * (0.5f + 0.5f * sinf(active_overlay->ui_time_seconds * 4.2f))) : 1.0f;

    if (console_overlay_get_toggle_rect(active_overlay, OVERLAY_TOGGLE_FREEZE_TIME, &toggle_left, &toggle_top, &toggle_right, &toggle_bottom))
    {
      box_top = toggle_top + (float)((OVERLAY_UI_CHECKBOX_HEIGHT - OVERLAY_UI_CHECKBOX_SIZE) / 2);
      console_overlay_draw_text(overlay, toggle_left, toggle_top - 8.0f, 0.86f, 0.88f, 0.92f, "Time controls");
      if (enabled || hot)
      {
        console_overlay_draw_rect(
          box_left - 3.0f,
          box_top - 3.0f,
          box_left + (float)OVERLAY_UI_CHECKBOX_SIZE + 3.0f,
          box_top + (float)OVERLAY_UI_CHECKBOX_SIZE + 3.0f,
          0.16f * pulse,
          0.34f * pulse,
          0.44f * pulse,
          hot ? 0.30f : 0.18f
        );
      }
      console_overlay_draw_rect(box_left, box_top, box_left + (float)OVERLAY_UI_CHECKBOX_SIZE, box_top + (float)OVERLAY_UI_CHECKBOX_SIZE, 0.10f, 0.11f, 0.14f, 0.96f);
      console_overlay_draw_outline(
        box_left,
        box_top,
        box_left + (float)OVERLAY_UI_CHECKBOX_SIZE,
        box_top + (float)OVERLAY_UI_CHECKBOX_SIZE,
        hot ? 0.56f : 0.24f * pulse,
        hot ? 0.88f : 0.28f * pulse,
        hot ? 0.98f : 0.34f * pulse,
        1.0f
      );
      if (enabled)
      {
        console_overlay_draw_rect(
          box_left + 3.0f,
          box_top + 3.0f,
          box_left + (float)OVERLAY_UI_CHECKBOX_SIZE - 3.0f,
          box_top + (float)OVERLAY_UI_CHECKBOX_SIZE - 3.0f,
          0.44f * pulse,
          0.86f * pulse,
          0.96f * pulse,
          1.0f
        );
      }
      console_overlay_draw_text(
        overlay,
        box_left + (float)OVERLAY_UI_CHECKBOX_SIZE + 10.0f,
        toggle_top + 15.0f,
        enabled ? 0.74f * pulse : 0.84f,
        enabled ? 0.94f * pulse : 0.78f,
        enabled ? 0.98f * pulse : 0.84f,
        enabled ? "Frozen" : "Running"
      );
      console_overlay_draw_text(
        overlay,
        box_left + (float)OVERLAY_UI_CHECKBOX_SIZE + 76.0f,
        toggle_top + 15.0f,
        0.64f,
        0.70f,
        0.78f,
        overlay_get_toggle_title(OVERLAY_TOGGLE_FREEZE_TIME)
      );
    }
  }

  {
    float toggle_left = 0.0f;
    float toggle_top = 0.0f;
    float toggle_right = 0.0f;
    float toggle_bottom = 0.0f;
    const int enabled = active_overlay->sound_enabled != 0;
    const int hot = active_overlay->hot_toggle == OVERLAY_TOGGLE_SOUND;
    const float box_left = (float)OVERLAY_UI_MARGIN;
    float box_top = 0.0f;
    const float pulse = enabled ? (0.82f + 0.18f * (0.5f + 0.5f * sinf(active_overlay->ui_time_seconds * 3.8f))) : 1.0f;

    if (console_overlay_get_toggle_rect(active_overlay, OVERLAY_TOGGLE_SOUND, &toggle_left, &toggle_top, &toggle_right, &toggle_bottom))
    {
      box_top = toggle_top + (float)((OVERLAY_UI_CHECKBOX_HEIGHT - OVERLAY_UI_CHECKBOX_SIZE) / 2);
      console_overlay_draw_text(overlay, toggle_left, toggle_top - 8.0f, 0.86f, 0.88f, 0.92f, "Audio output");
      if (enabled || hot)
      {
        console_overlay_draw_rect(
          box_left - 3.0f,
          box_top - 3.0f,
          box_left + (float)OVERLAY_UI_CHECKBOX_SIZE + 3.0f,
          box_top + (float)OVERLAY_UI_CHECKBOX_SIZE + 3.0f,
          0.16f * pulse,
          0.34f * pulse,
          0.44f * pulse,
          hot ? 0.30f : 0.18f
        );
      }
      console_overlay_draw_rect(box_left, box_top, box_left + (float)OVERLAY_UI_CHECKBOX_SIZE, box_top + (float)OVERLAY_UI_CHECKBOX_SIZE, 0.10f, 0.11f, 0.14f, 0.96f);
      console_overlay_draw_outline(
        box_left,
        box_top,
        box_left + (float)OVERLAY_UI_CHECKBOX_SIZE,
        box_top + (float)OVERLAY_UI_CHECKBOX_SIZE,
        hot ? 0.56f : 0.24f * pulse,
        hot ? 0.88f : 0.28f * pulse,
        hot ? 0.98f : 0.34f * pulse,
        1.0f
      );
      if (enabled)
      {
        console_overlay_draw_rect(
          box_left + 3.0f,
          box_top + 3.0f,
          box_left + (float)OVERLAY_UI_CHECKBOX_SIZE - 3.0f,
          box_top + (float)OVERLAY_UI_CHECKBOX_SIZE - 3.0f,
          0.44f * pulse,
          0.86f * pulse,
          0.96f * pulse,
          1.0f
        );
      }
      console_overlay_draw_text(
        overlay,
        box_left + (float)OVERLAY_UI_CHECKBOX_SIZE + 10.0f,
        toggle_top + 15.0f,
        enabled ? 0.74f * pulse : 0.84f,
        enabled ? 0.94f * pulse : 0.78f,
        enabled ? 0.98f * pulse : 0.84f,
        enabled ? "Unmuted" : "Muted   "
      );
      console_overlay_draw_text(
        overlay,
        box_left + (float)OVERLAY_UI_CHECKBOX_SIZE + 76.0f,
        toggle_top + 15.0f,
        0.64f,
        0.70f,
        0.78f,
        overlay_get_toggle_title(OVERLAY_TOGGLE_SOUND)
      );
    }
  }

  {
    float toggle_left = 0.0f;
      float toggle_top = 0.0f;
      float toggle_right = 0.0f;
      float toggle_bottom = 0.0f;
      const int enabled = active_overlay->settings.clouds_enabled != 0;
      const int hot = active_overlay->hot_toggle == OVERLAY_TOGGLE_CLOUDS;
      const float box_left = (float)OVERLAY_UI_MARGIN;
      float box_top = 0.0f;
      const float pulse = enabled ? (0.78f + 0.22f * (0.5f + 0.5f * sinf(active_overlay->ui_time_seconds * 6.0f))) : 1.0f;

      if (console_overlay_get_toggle_rect(active_overlay, OVERLAY_TOGGLE_CLOUDS, &toggle_left, &toggle_top, &toggle_right, &toggle_bottom))
      {
        box_top = toggle_top + (float)((OVERLAY_UI_CHECKBOX_HEIGHT - OVERLAY_UI_CHECKBOX_SIZE) / 2);
        console_overlay_draw_text(overlay, toggle_left, toggle_top - 8.0f, 0.86f, 0.88f, 0.92f, "Cloud controls");
        if (enabled || hot)
        {
          console_overlay_draw_rect(
            box_left - 3.0f,
            box_top - 3.0f,
            box_left + (float)OVERLAY_UI_CHECKBOX_SIZE + 3.0f,
            box_top + (float)OVERLAY_UI_CHECKBOX_SIZE + 3.0f,
            0.34f * pulse,
            0.24f * pulse,
            0.12f * pulse,
            hot ? 0.28f : 0.18f
          );
        }
        console_overlay_draw_rect(box_left, box_top, box_left + (float)OVERLAY_UI_CHECKBOX_SIZE, box_top + (float)OVERLAY_UI_CHECKBOX_SIZE, 0.10f, 0.11f, 0.14f, 0.96f);
        console_overlay_draw_outline(
          box_left,
          box_top,
          box_left + (float)OVERLAY_UI_CHECKBOX_SIZE,
          box_top + (float)OVERLAY_UI_CHECKBOX_SIZE,
          hot ? 0.86f : 0.23f * pulse,
          hot ? 0.70f : 0.26f * pulse,
          hot ? 0.35f : 0.32f * pulse,
          1.0f
        );
        if (enabled)
        {
          console_overlay_draw_rect(
            box_left + 3.0f,
            box_top + 3.0f,
            box_left + (float)OVERLAY_UI_CHECKBOX_SIZE - 3.0f,
            box_top + (float)OVERLAY_UI_CHECKBOX_SIZE - 3.0f,
            0.84f * pulse,
            0.62f * pulse,
            0.28f * pulse,
            1.0f
          );
        }
        console_overlay_draw_text(
          overlay,
          box_left + (float)OVERLAY_UI_CHECKBOX_SIZE + 10.0f,
          toggle_top + 15.0f,
          enabled ? 0.90f * pulse : 0.68f,
          enabled ? 0.82f * pulse : 0.74f,
          enabled ? 0.54f * pulse : 0.78f,
          enabled ? "Enabled" : "Disabled"
        );
        console_overlay_draw_text(
          overlay,
          box_left + (float)OVERLAY_UI_CHECKBOX_SIZE + 96.0f,
          toggle_top + 15.0f,
          0.64f,
          0.70f,
          0.78f,
          overlay_get_toggle_title(OVERLAY_TOGGLE_CLOUDS)
        );
      }
    }

    {
      int quality_card_left_i = 0;
      int quality_card_top_i = 0;
      int quality_card_right_i = 0;
      int quality_card_bottom_i = 0;
      float quality_card_left = 0.0f;
      float quality_card_top = 0.0f;
      float quality_card_right = 0.0f;
      float quality_card_bottom = 0.0f;

      (void)overlay_get_quality_selector_rect(
        active_overlay->panel_width,
        active_overlay->scroll_offset,
        &quality_card_left_i,
        &quality_card_top_i,
        &quality_card_right_i,
        &quality_card_bottom_i);
      quality_card_left = (float)quality_card_left_i;
      quality_card_top = (float)quality_card_top_i;
      quality_card_right = (float)quality_card_right_i;
      quality_card_bottom = (float)quality_card_bottom_i;

      console_overlay_draw_text(overlay, quality_card_left, quality_card_top - 8.0f, 0.86f, 0.88f, 0.92f, "Render quality");
      console_overlay_draw_rect(quality_card_left, quality_card_top, quality_card_right, quality_card_bottom, 0.09f, 0.10f, 0.13f, 0.98f);
      console_overlay_draw_outline(quality_card_left, quality_card_top, quality_card_right, quality_card_bottom, 0.23f, 0.26f, 0.32f, 1.0f);

      {
        RendererQualityPreset preset = RENDER_QUALITY_PRESET_HIGH;
        for (preset = RENDER_QUALITY_PRESET_HIGH; preset < RENDER_QUALITY_PRESET_COUNT; ++preset)
        {
          float button_left = 0.0f;
          float button_top = 0.0f;
          float button_right = 0.0f;
          float button_bottom = 0.0f;
          const int selected = active_overlay->render_quality_preset == preset;
          const int hot = active_overlay->hot_render_quality_preset == (int)preset;
          const float pulse = selected ? (0.88f + 0.12f * (0.5f + 0.5f * sinf(active_overlay->ui_time_seconds * 4.1f))) : 1.0f;
          const char* label = render_quality_preset_get_label(preset);

          if (!console_overlay_get_render_quality_rect(active_overlay, preset, &button_left, &button_top, &button_right, &button_bottom))
          {
            continue;
          }

          console_overlay_draw_rect(
            button_left,
            button_top,
            button_right,
            button_bottom,
            selected ? (0.40f * pulse) : (hot ? 0.20f : 0.11f),
            selected ? (0.34f * pulse) : (hot ? 0.22f : 0.12f),
            selected ? (0.20f * pulse) : (hot ? 0.16f : 0.15f),
            0.98f);
          console_overlay_draw_outline(
            button_left,
            button_top,
            button_right,
            button_bottom,
            selected ? 0.92f * pulse : (hot ? 0.90f : 0.24f),
            selected ? 0.66f * pulse : (hot ? 0.78f : 0.28f),
            selected ? 0.34f * pulse : (hot ? 0.48f : 0.35f),
            1.0f);
          console_overlay_draw_text(
            overlay,
            button_left + 10.0f,
            button_top + 16.0f,
            selected ? 0.98f : 0.84f,
            selected ? 0.90f : 0.86f,
            selected ? 0.72f : 0.91f,
            label);
        }
      }

      console_overlay_copy_text_fit(
        text_buffer,
        sizeof(text_buffer),
        render_quality_preset_get_description(active_overlay->render_quality_preset),
        (quality_card_right - quality_card_left) - 20.0f);
      console_overlay_draw_text(overlay, quality_card_left + 10.0f, quality_card_top + 52.0f, 0.78f, 0.82f, 0.88f, text_buffer);
    }

    {
      int gpu_card_left_i = 0;
      int gpu_card_top_i = 0;
      int gpu_card_right_i = 0;
      int gpu_card_bottom_i = 0;
      float gpu_card_left = 0.0f;
      float gpu_card_top = 0.0f;
      float gpu_card_right = 0.0f;
      float gpu_card_bottom = 0.0f;
      const GpuPreferenceInfo* gpu_info = &active_overlay->gpu_info;
      const char* power_name = "Unavailable";
      const char* high_name = "Unavailable";

      (void)overlay_get_gpu_selector_rect(
        active_overlay->panel_width,
        active_overlay->scroll_offset,
        &gpu_card_left_i,
        &gpu_card_top_i,
        &gpu_card_right_i,
        &gpu_card_bottom_i);
      gpu_card_left = (float)gpu_card_left_i;
      gpu_card_top = (float)gpu_card_top_i;
      gpu_card_right = (float)gpu_card_right_i;
      gpu_card_bottom = (float)gpu_card_bottom_i;

      if (gpu_info->minimum_power_index >= 0 && gpu_info->minimum_power_index < gpu_info->adapter_count)
      {
        power_name = gpu_info->adapters[gpu_info->minimum_power_index].name;
      }
      if (gpu_info->high_performance_index >= 0 && gpu_info->high_performance_index < gpu_info->adapter_count)
      {
        high_name = gpu_info->adapters[gpu_info->high_performance_index].name;
      }

      console_overlay_draw_text(overlay, gpu_card_left, gpu_card_top - 8.0f, 0.86f, 0.88f, 0.92f, "GPU routing");
      console_overlay_draw_rect(gpu_card_left, gpu_card_top, gpu_card_right, gpu_card_bottom, 0.09f, 0.10f, 0.13f, 0.98f);
      console_overlay_draw_outline(gpu_card_left, gpu_card_top, gpu_card_right, gpu_card_bottom, 0.23f, 0.26f, 0.32f, 1.0f);

      {
        GpuPreferenceMode mode = GPU_PREFERENCE_MODE_AUTO;
        for (mode = GPU_PREFERENCE_MODE_AUTO; mode < GPU_PREFERENCE_MODE_COUNT; ++mode)
        {
          float button_left = 0.0f;
          float button_top = 0.0f;
          float button_right = 0.0f;
          float button_bottom = 0.0f;
          const int selected = gpu_info->selected_mode == mode;
          const int hot = active_overlay->hot_gpu_preference == (int)mode;
          const float pulse = selected ? (0.88f + 0.12f * (0.5f + 0.5f * sinf(active_overlay->ui_time_seconds * 4.4f))) : 1.0f;
          const char* short_label = gpu_preferences_get_mode_short_label(mode);

          if (!console_overlay_get_gpu_preference_rect(active_overlay, mode, &button_left, &button_top, &button_right, &button_bottom))
          {
            continue;
          }

          console_overlay_draw_rect(
            button_left,
            button_top,
            button_right,
            button_bottom,
            selected ? (0.30f * pulse) : (hot ? 0.16f : 0.11f),
            selected ? (0.44f * pulse) : (hot ? 0.20f : 0.12f),
            selected ? (0.56f * pulse) : (hot ? 0.30f : 0.15f),
            0.98f);
          console_overlay_draw_outline(
            button_left,
            button_top,
            button_right,
            button_bottom,
            selected ? 0.54f * pulse : (hot ? 0.70f : 0.24f),
            selected ? 0.76f * pulse : (hot ? 0.84f : 0.28f),
            selected ? 0.94f * pulse : (hot ? 0.96f : 0.35f),
            1.0f);
          console_overlay_draw_text(
            overlay,
            button_left + 12.0f,
            button_top + 16.0f,
            selected ? 0.92f : 0.82f,
            selected ? 0.96f : 0.86f,
            selected ? 0.99f : 0.91f,
            short_label);
        }
      }

      if (gpu_info->available == 0)
      {
        console_overlay_copy_text_fit(text_buffer, sizeof(text_buffer), gpu_info->status_message, (gpu_card_right - gpu_card_left) - 20.0f);
        console_overlay_draw_text(overlay, gpu_card_left + 10.0f, gpu_card_top + 52.0f, 0.86f, 0.70f, 0.48f, text_buffer);
      }
      else
      {
        (void)snprintf(value_buffer, sizeof(value_buffer), "Power: %s", power_name);
        console_overlay_copy_text_fit(text_buffer, sizeof(text_buffer), value_buffer, (gpu_card_right - gpu_card_left) - 20.0f);
        console_overlay_draw_text(overlay, gpu_card_left + 10.0f, gpu_card_top + 52.0f, 0.72f, 0.88f, 0.70f, text_buffer);

        (void)snprintf(value_buffer, sizeof(value_buffer), "High: %s", high_name);
        console_overlay_copy_text_fit(text_buffer, sizeof(text_buffer), value_buffer, (gpu_card_right - gpu_card_left) - 20.0f);
        console_overlay_draw_text(overlay, gpu_card_left + 10.0f, gpu_card_top + 66.0f, 0.76f, 0.82f, 0.92f, text_buffer);
      }

      if (gpu_info->current_renderer[0] != '\0')
      {
        (void)snprintf(value_buffer, sizeof(value_buffer), "OpenGL: %s", gpu_info->current_renderer);
      }
      else
      {
        (void)snprintf(value_buffer, sizeof(value_buffer), "%s", gpu_info->status_message);
      }
      console_overlay_copy_text_fit(text_buffer, sizeof(text_buffer), value_buffer, (gpu_card_right - gpu_card_left) - 20.0f);
      console_overlay_draw_text(overlay, gpu_card_left + 10.0f, gpu_card_top + 80.0f, 0.78f, 0.82f, 0.88f, text_buffer);
    }

    {
      float card_top = 0.0f;
      float card_bottom = 0.0f;
      const float card_left = (float)OVERLAY_UI_MARGIN;
      const float card_right = panel_width - (float)OVERLAY_UI_MARGIN;
      float dummy = 0.0f;

      (void)console_overlay_get_slider_rect(active_overlay, OVERLAY_SLIDER_CLOUD_SPACING, &dummy, &card_top, &dummy, &card_bottom);
      card_top = card_bottom + (float)OVERLAY_UI_SECTION_SPACING;
      card_bottom = card_top + (float)OVERLAY_UI_METRIC_HEIGHT;

      console_overlay_draw_rect(card_left, card_top, card_right, card_bottom, 0.09f, 0.10f, 0.13f, 0.98f);
      console_overlay_draw_outline(card_left, card_top, card_right, card_bottom, 0.21f, 0.24f, 0.30f, 1.0f);
      (void)snprintf(value_buffer, sizeof(value_buffer), "Glare proxy %.2fx", glare_factor);
      console_overlay_copy_text_fit(text_buffer, sizeof(text_buffer), value_buffer, (card_right - card_left) - 24.0f);
      console_overlay_draw_text(overlay, card_left + 12.0f, card_top + 16.0f, 0.96f, 0.86f, 0.52f, text_buffer);
      (void)snprintf(value_buffer, sizeof(value_buffer), "Day %.0fs  |  Night %.0fs", active_overlay->metrics.daylight_duration_seconds, active_overlay->metrics.night_duration_seconds);
      console_overlay_copy_text_fit(text_buffer, sizeof(text_buffer), value_buffer, (card_right - card_left) - 24.0f);
      console_overlay_draw_text(overlay, card_left + 12.0f, card_top + 33.0f, 0.78f, 0.82f, 0.88f, text_buffer);
    }

    if (active_overlay->scroll_max > 0.5f)
    {
      const float track_left = panel_width - 10.0f;
      const float track_right = panel_width - 4.0f;
      const float track_top = scroll_view_top + 6.0f;
      const float track_bottom = panel_height - 10.0f;
      const float track_height = track_bottom - track_top;
      const float thumb_height = fmaxf(28.0f, track_height * ((track_height) / (track_height + active_overlay->scroll_max)));
      const float thumb_travel = fmaxf(track_height - thumb_height, 1.0f);
      const float thumb_top = track_top + (active_overlay->scroll_offset / active_overlay->scroll_max) * thumb_travel;

      glScissor(0, 0, (GLsizei)panel_width, height);
      console_overlay_draw_rect(track_left, track_top, track_right, track_bottom, 0.10f, 0.11f, 0.14f, 0.88f);
      console_overlay_draw_rect(track_left, thumb_top, track_right, thumb_top + thumb_height, 0.74f, 0.54f, 0.26f, 0.94f);
    }

    glDisable(GL_SCISSOR_TEST);
  }

  console_overlay_draw_panel_toggle_button(
    active_overlay,
    toggle_button_left,
    toggle_button_top,
    toggle_button_right,
    toggle_button_bottom,
    button_hovered);

  glPopMatrix();
  glMatrixMode(GL_PROJECTION);
  glPopMatrix();
  glMatrixMode(GL_MODELVIEW);
  glDisable(GL_SCISSOR_TEST);

  glDisable(GL_BLEND);
}

static void console_overlay_show_error(const char* title, const char* message)
{
  diagnostics_logf("%s: %s", title, message);
#if defined(_WIN32)
  (void)MessageBoxA(NULL, message, title, MB_ICONERROR | MB_OK);
#else
  (void)title;
  (void)message;
#endif
}

static int console_overlay_build_font(GLuint* out_font_base)
{
  GLuint font_base = 0U;

  if (out_font_base == NULL)
  {
    return 0;
  }

  *out_font_base = 0U;

#if defined(_WIN32)
  {
    HDC device_context = wglGetCurrentDC();
    HFONT font = NULL;
    HGDIOBJ previous_font = NULL;

    if (device_context == NULL)
    {
      console_overlay_show_error("OpenGL Error", "Failed to resolve the device context for the console overlay font.");
      return 0;
    }

    font = CreateFontA(
      -16,
      0,
      0,
      0,
      FW_NORMAL,
      FALSE,
      FALSE,
      FALSE,
      ANSI_CHARSET,
      OUT_DEFAULT_PRECIS,
      CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY,
      FIXED_PITCH | FF_MODERN,
      "Consolas"
    );
    if (font == NULL)
    {
      console_overlay_show_error("Win32 Error", "Failed to create the console overlay font.");
      return 0;
    }

    previous_font = SelectObject(device_context, font);
    font_base = glGenLists(96);
    if (font_base == 0U || !wglUseFontBitmapsA(device_context, 32, 96, font_base))
    {
      if (font_base != 0U)
      {
        glDeleteLists(font_base, 96);
      }
      (void)SelectObject(device_context, previous_font);
      (void)DeleteObject(font);
      console_overlay_show_error("Win32 Error", "Failed to build OpenGL bitmap glyphs for the console overlay.");
      return 0;
    }

    (void)SelectObject(device_context, previous_font);
    (void)DeleteObject(font);
  }
#elif defined(__linux__)
  {
    static const char* k_font_names[] = {
      "-misc-fixed-medium-r-normal--14-130-75-75-c-70-iso8859-1",
      "9x15",
      "fixed"
    };
    Display* display = glXGetCurrentDisplay();
    Display* owned_display = NULL;
    XFontStruct* font = NULL;
    size_t font_index = 0U;

    if (display == NULL)
    {
      owned_display = XOpenDisplay(NULL);
      display = owned_display;
    }
    if (display == NULL)
    {
      console_overlay_show_error("X11 Error", "Failed to open an X11 display for the console overlay font.");
      return 0;
    }

    for (font_index = 0U; font_index < sizeof(k_font_names) / sizeof(k_font_names[0]); ++font_index)
    {
      font = XLoadQueryFont(display, k_font_names[font_index]);
      if (font != NULL)
      {
        break;
      }
    }
    if (font == NULL)
    {
      if (owned_display != NULL)
      {
        XCloseDisplay(owned_display);
      }
      console_overlay_show_error("X11 Error", "Failed to load a fixed-width X11 font for the console overlay.");
      return 0;
    }

    font_base = glGenLists(96);
    if (font_base == 0U)
    {
      XFreeFont(display, font);
      if (owned_display != NULL)
      {
        XCloseDisplay(owned_display);
      }
      console_overlay_show_error("OpenGL Error", "Failed to allocate console overlay glyph lists.");
      return 0;
    }

    glXUseXFont(font->fid, 32, 96, (int)font_base);
    XFreeFont(display, font);
    if (owned_display != NULL)
    {
      XCloseDisplay(owned_display);
    }
  }
#endif

  *out_font_base = font_base;
  return font_base != 0U;
}

static void console_overlay_draw_rect(float left, float top, float right, float bottom, float r, float g, float b, float a)
{
  glColor4f(r, g, b, a);
  glBegin(GL_QUADS);
  glVertex2f(left, top);
  glVertex2f(right, top);
  glVertex2f(right, bottom);
  glVertex2f(left, bottom);
  glEnd();
}

static void console_overlay_draw_outline(float left, float top, float right, float bottom, float r, float g, float b, float a)
{
  glColor4f(r, g, b, a);
  glBegin(GL_LINE_LOOP);
  glVertex2f(left, top);
  glVertex2f(right, top);
  glVertex2f(right, bottom);
  glVertex2f(left, bottom);
  glEnd();
}

static void console_overlay_draw_circle(float center_x, float center_y, float radius, float r, float g, float b, float a)
{
  int segment = 0;

  glColor4f(r, g, b, a);
  glBegin(GL_TRIANGLE_FAN);
  glVertex2f(center_x, center_y);
  for (segment = 0; segment <= 32; ++segment)
  {
    const float angle = ((float)segment / 32.0f) * 6.2831853f;
    glVertex2f(center_x + cosf(angle) * radius, center_y + sinf(angle) * radius);
  }
  glEnd();
}

static void console_overlay_draw_circle_outline(float center_x, float center_y, float radius, float r, float g, float b, float a)
{
  int segment = 0;

  glColor4f(r, g, b, a);
  glBegin(GL_LINE_LOOP);
  for (segment = 0; segment < 32; ++segment)
  {
    const float angle = ((float)segment / 32.0f) * 6.2831853f;
    glVertex2f(center_x + cosf(angle) * radius, center_y + sinf(angle) * radius);
  }
  glEnd();
}

static void console_overlay_draw_hamburger_icon(float center_x, float center_y, float tone)
{
  console_overlay_draw_rect(center_x - 7.0f, center_y - 6.5f, center_x + 7.0f, center_y - 4.0f, 0.88f * tone, 0.92f * tone, 0.98f * tone, 1.0f);
  console_overlay_draw_rect(center_x - 7.0f, center_y - 1.2f, center_x + 7.0f, center_y + 1.3f, 0.88f * tone, 0.92f * tone, 0.98f * tone, 1.0f);
  console_overlay_draw_rect(center_x - 7.0f, center_y + 4.0f, center_x + 7.0f, center_y + 6.5f, 0.88f * tone, 0.92f * tone, 0.98f * tone, 1.0f);
}

static void console_overlay_draw_chevron_icon(float center_x, float center_y, float tone)
{
  glColor4f(0.88f * tone, 0.92f * tone, 0.98f * tone, 1.0f);
  glLineWidth(2.5f);
  glBegin(GL_LINES);
  glVertex2f(center_x + 4.0f, center_y - 7.0f);
  glVertex2f(center_x - 3.5f, center_y);
  glVertex2f(center_x - 3.5f, center_y);
  glVertex2f(center_x + 4.0f, center_y + 7.0f);
  glEnd();
  glLineWidth(1.0f);
}

static void console_overlay_draw_panel_toggle_button(const OverlayState* overlay, float left, float top, float right, float bottom, int hovered)
{
  const float center_x = (left + right) * 0.5f;
  const float center_y = (top + bottom) * 0.5f;
  const float radius = (right - left) * 0.5f;
  const int collapsed = (overlay != NULL && overlay->panel_collapsed != 0);
  const float active_mix = hovered ? 1.0f : 0.0f;
  const float pulse = 0.94f + 0.06f * sinf(((overlay != NULL) ? overlay->ui_time_seconds : 0.0f) * 4.0f);
  const float icon_tone = collapsed ? 1.0f : 0.96f;

  console_overlay_draw_circle(center_x + 1.0f, center_y + 2.0f, radius, 0.01f, 0.02f, 0.04f, 0.18f + active_mix * 0.10f);
  console_overlay_draw_circle(
    center_x,
    center_y,
    radius,
    (0.10f + active_mix * 0.08f) * pulse,
    (0.13f + active_mix * 0.14f) * pulse,
    (0.18f + active_mix * 0.20f) * pulse,
    0.94f);
  console_overlay_draw_circle(center_x, center_y, radius - 5.0f, 0.24f, 0.30f + active_mix * 0.14f, 0.40f + active_mix * 0.18f, 0.22f + active_mix * 0.08f);
  console_overlay_draw_circle_outline(center_x, center_y, radius - 0.5f, 0.72f + active_mix * 0.20f, 0.80f + active_mix * 0.12f, 0.92f + active_mix * 0.06f, 0.88f);

  if (collapsed)
  {
    console_overlay_draw_hamburger_icon(center_x, center_y, icon_tone + active_mix * 0.08f);
  }
  else
  {
    console_overlay_draw_chevron_icon(center_x, center_y, icon_tone + active_mix * 0.08f);
  }
}

static int console_overlay_build_shader_path(const char* relative_path, char* out_path, size_t out_path_size)
{
#if defined(_WIN32)
  static const char* k_shader_fallbacks[] = {
    "shaders\\",
    "..\\shaders\\",
    "..\\..\\shaders\\",
    "..\\..\\..\\shaders\\"
  };
  char executable_path[MAX_PATH] = { 0 };
  char executable_directory[MAX_PATH] = { 0 };
  const char* last_separator = NULL;
  size_t fallback_index = 0U;

  if (relative_path == NULL || out_path == NULL || out_path_size == 0U)
  {
    return 0;
  }

  if (GetModuleFileNameA(NULL, executable_path, (DWORD)sizeof(executable_path)) == 0U)
  {
    return 0;
  }

  last_separator = strrchr(executable_path, '\\');
  if (last_separator == NULL)
  {
    last_separator = strrchr(executable_path, '/');
  }
  if (last_separator == NULL)
  {
    return 0;
  }

  memcpy(executable_directory, executable_path, (size_t)(last_separator - executable_path));
  executable_directory[last_separator - executable_path] = '\0';

  for (fallback_index = 0U; fallback_index < sizeof(k_shader_fallbacks) / sizeof(k_shader_fallbacks[0]); ++fallback_index)
  {
    const int written = snprintf(
      out_path,
      out_path_size,
      "%s\\%s%s",
      executable_directory,
      k_shader_fallbacks[fallback_index],
      relative_path);
    if (written > 0 && (size_t)written < out_path_size && GetFileAttributesA(out_path) != INVALID_FILE_ATTRIBUTES)
    {
      return 1;
    }
  }

  return 0;
#elif defined(__linux__)
  static const char* k_shader_fallbacks[] = {
    "shaders/",
    "../shaders/",
    "../../shaders/",
    "../../../shaders/"
  };
  char executable_path[MAX_PATH] = { 0 };
  char executable_directory[MAX_PATH] = { 0 };
  char normalized_relative_path[MAX_PATH] = { 0 };
  ssize_t executable_length = 0;
  const char* last_separator = NULL;
  size_t relative_index = 0U;
  size_t fallback_index = 0U;

  if (relative_path == NULL || out_path == NULL || out_path_size == 0U)
  {
    return 0;
  }

  executable_length = readlink("/proc/self/exe", executable_path, sizeof(executable_path) - 1U);
  if (executable_length <= 0 || executable_length >= (ssize_t)sizeof(executable_path))
  {
    return 0;
  }
  executable_path[executable_length] = '\0';

  last_separator = strrchr(executable_path, '/');
  if (last_separator == NULL)
  {
    return 0;
  }

  memcpy(executable_directory, executable_path, (size_t)(last_separator - executable_path));
  executable_directory[last_separator - executable_path] = '\0';

  for (relative_index = 0U; relative_path[relative_index] != '\0' && relative_index < sizeof(normalized_relative_path) - 1U; ++relative_index)
  {
    normalized_relative_path[relative_index] = (relative_path[relative_index] == '\\') ? '/' : relative_path[relative_index];
  }
  normalized_relative_path[relative_index] = '\0';

  for (fallback_index = 0U; fallback_index < sizeof(k_shader_fallbacks) / sizeof(k_shader_fallbacks[0]); ++fallback_index)
  {
    const int written = snprintf(
      out_path,
      out_path_size,
      "%s/%s%s",
      executable_directory,
      k_shader_fallbacks[fallback_index],
      normalized_relative_path);
    if (written > 0 && (size_t)written < out_path_size && access(out_path, R_OK) == 0)
    {
      return 1;
    }
  }

  return 0;
#else
  (void)relative_path;
  (void)out_path;
  (void)out_path_size;
  return 0;
#endif
}

static int console_overlay_load_text_file(const char* path, const char* label, char** out_source)
{
  FILE* file = NULL;
  long file_size = 0L;
  char* buffer = NULL;
  size_t bytes_read = 0U;

  if (path == NULL || label == NULL || out_source == NULL)
  {
    return 0;
  }

  *out_source = NULL;
#if defined(_WIN32)
  if (fopen_s(&file, path, "rb") != 0 || file == NULL)
#else
  file = fopen(path, "rb");
  if (file == NULL)
#endif
  {
    char message[512] = { 0 };
    (void)snprintf(message, sizeof(message), "Failed to open shader file: %s\n%s", label, path);
    console_overlay_show_error("Shader Error", message);
    return 0;
  }

  if (fseek(file, 0L, SEEK_END) != 0)
  {
    fclose(file);
    console_overlay_show_error("Shader Error", "Failed to seek UI shader file.");
    return 0;
  }

  file_size = ftell(file);
  if (file_size < 0L)
  {
    fclose(file);
    console_overlay_show_error("Shader Error", "Failed to determine UI shader file size.");
    return 0;
  }
  if (fseek(file, 0L, SEEK_SET) != 0)
  {
    fclose(file);
    console_overlay_show_error("Shader Error", "Failed to rewind UI shader file.");
    return 0;
  }

  buffer = (char*)malloc((size_t)file_size + 1U);
  if (buffer == NULL)
  {
    fclose(file);
    console_overlay_show_error("Memory Error", "Failed to allocate UI shader source buffer.");
    return 0;
  }

  bytes_read = fread(buffer, 1U, (size_t)file_size, file);
  fclose(file);
  if (bytes_read != (size_t)file_size)
  {
    free(buffer);
    console_overlay_show_error("Shader Error", "Failed to read full UI shader source.");
    return 0;
  }

  buffer[file_size] = '\0';
  *out_source = buffer;
  return 1;
}

static int console_overlay_append_shader_source(char** buffer, size_t* length, size_t* capacity, const char* text, size_t text_length)
{
  char* resized_buffer = NULL;
  size_t required_capacity = 0U;
  size_t new_capacity = 0U;

  if (buffer == NULL || length == NULL || capacity == NULL || text == NULL)
  {
    return 0;
  }

  required_capacity = *length + text_length + 1U;
  if (required_capacity > *capacity)
  {
    new_capacity = (*capacity > 0U) ? *capacity : 256U;
    while (new_capacity < required_capacity)
    {
      const size_t previous_capacity = new_capacity;
      new_capacity *= 2U;
      if (new_capacity < previous_capacity)
      {
        new_capacity = required_capacity;
        break;
      }
    }

    resized_buffer = (char*)realloc(*buffer, new_capacity);
    if (resized_buffer == NULL)
    {
      console_overlay_show_error("Memory Error", "Failed to grow the UI shader source buffer.");
      return 0;
    }

    *buffer = resized_buffer;
    *capacity = new_capacity;
  }

  if (text_length > 0U)
  {
    memcpy(*buffer + *length, text, text_length);
    *length += text_length;
  }
  (*buffer)[*length] = '\0';
  return 1;
}

static int console_overlay_resolve_shader_include_path(const char* source_path, const char* include_path, char* out_path, size_t out_path_size)
{
  const char* last_backslash = NULL;
  const char* last_slash = NULL;
  const char* separator = NULL;
  size_t prefix_length = 0U;
  int written = 0;

  if (source_path == NULL || include_path == NULL || out_path == NULL || out_path_size == 0U)
  {
    return 0;
  }

  if (include_path[0] == '\\' || include_path[0] == '/' || strchr(include_path, ':') != NULL)
  {
    written = snprintf(out_path, out_path_size, "%s", include_path);
    return written > 0 && (size_t)written < out_path_size;
  }

  last_backslash = strrchr(source_path, '\\');
  last_slash = strrchr(source_path, '/');
  separator = last_backslash;
  if (separator == NULL || (last_slash != NULL && last_slash > separator))
  {
    separator = last_slash;
  }

  if (separator == NULL)
  {
    written = snprintf(out_path, out_path_size, "%s", include_path);
    return written > 0 && (size_t)written < out_path_size;
  }

  prefix_length = (size_t)(separator - source_path + 1);
  written = snprintf(out_path, out_path_size, "%.*s%s", (int)prefix_length, source_path, include_path);
  return written > 0 && (size_t)written < out_path_size;
}

static int console_overlay_load_shader_source(const char* path, const char* label, int include_depth, char** out_source)
{
  char* raw_source = NULL;
  char* expanded_source = NULL;
  size_t expanded_length = 0U;
  size_t expanded_capacity = 0U;
  const char* cursor = NULL;

  if (path == NULL || label == NULL || out_source == NULL)
  {
    return 0;
  }

  *out_source = NULL;
  if (include_depth > CONSOLE_OVERLAY_SHADER_INCLUDE_DEPTH_MAX)
  {
    console_overlay_show_error("Shader Error", "UI shader include depth exceeded the safe limit.");
    return 0;
  }

  if (!console_overlay_load_text_file(path, label, &raw_source))
  {
    return 0;
  }

  cursor = raw_source;
  while (*cursor != '\0')
  {
    const char* line_end = strchr(cursor, '\n');
    const size_t line_length = (line_end != NULL) ? (size_t)(line_end - cursor + 1) : strlen(cursor);
    const char* trimmed = cursor;
    size_t trimmed_length = 0U;

    while (*trimmed == ' ' || *trimmed == '\t')
    {
      ++trimmed;
    }
    trimmed_length = line_length - (size_t)(trimmed - cursor);

    if (trimmed_length >= 8U && strncmp(trimmed, "#include", 8) == 0)
    {
      const char* open_quote = NULL;
      const char* close_quote = NULL;
      char include_name[MAX_PATH] = { 0 };
      char include_path[MAX_PATH] = { 0 };
      char* include_source = NULL;
      size_t include_name_length = 0U;
      size_t scan_index = 8U;

      while (scan_index < trimmed_length && (trimmed[scan_index] == ' ' || trimmed[scan_index] == '\t'))
      {
        ++scan_index;
      }
      if (scan_index < trimmed_length && trimmed[scan_index] == '"')
      {
        size_t close_index = scan_index + 1U;
        open_quote = trimmed + scan_index;
        while (close_index < trimmed_length && trimmed[close_index] != '"')
        {
          ++close_index;
        }
        if (close_index < trimmed_length && trimmed[close_index] == '"')
        {
          close_quote = trimmed + close_index;
        }
      }

      if (open_quote == NULL || close_quote == NULL || close_quote <= open_quote + 1)
      {
        free(raw_source);
        free(expanded_source);
        console_overlay_show_error("Shader Error", "Malformed UI shader include directive.");
        return 0;
      }

      include_name_length = (size_t)(close_quote - open_quote - 1);
      if (include_name_length >= sizeof(include_name))
      {
        free(raw_source);
        free(expanded_source);
        console_overlay_show_error("Shader Error", "UI shader include path is too long.");
        return 0;
      }

      memcpy(include_name, open_quote + 1, include_name_length);
      include_name[include_name_length] = '\0';
      if (!console_overlay_resolve_shader_include_path(path, include_name, include_path, sizeof(include_path)) ||
        !console_overlay_load_shader_source(include_path, include_name, include_depth + 1, &include_source))
      {
        free(raw_source);
        free(expanded_source);
        return 0;
      }

      if (!console_overlay_append_shader_source(&expanded_source, &expanded_length, &expanded_capacity, include_source, strlen(include_source)))
      {
        free(include_source);
        free(raw_source);
        free(expanded_source);
        return 0;
      }
      free(include_source);

      if (expanded_length == 0U || expanded_source[expanded_length - 1] != '\n')
      {
        if (!console_overlay_append_shader_source(&expanded_source, &expanded_length, &expanded_capacity, "\n", 1U))
        {
          free(raw_source);
          free(expanded_source);
          return 0;
        }
      }
    }
    else
    {
      if (!console_overlay_append_shader_source(&expanded_source, &expanded_length, &expanded_capacity, cursor, line_length))
      {
        free(raw_source);
        free(expanded_source);
        return 0;
      }
    }

    cursor += line_length;
  }

  free(raw_source);
  *out_source = expanded_source;
  return 1;
}

static int console_overlay_check_shader(GLuint shader, const char* label)
{
  GLint status = GL_FALSE;
  GLint log_length = 0;
  char* log = NULL;

  glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
  if (status == GL_TRUE)
  {
    return 1;
  }

  glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
  if (log_length < 1)
  {
    console_overlay_show_error("Shader Error", label);
    return 0;
  }

  log = (char*)malloc((size_t)log_length + 1U);
  if (log == NULL)
  {
    console_overlay_show_error("Memory Error", "Failed to allocate UI shader log buffer.");
    return 0;
  }

  glGetShaderInfoLog(shader, log_length, NULL, log);
  log[log_length] = '\0';
  console_overlay_show_error(label, log);
  free(log);
  return 0;
}

static int console_overlay_check_program(GLuint program, const char* label)
{
  GLint status = GL_FALSE;
  GLint log_length = 0;
  char* log = NULL;

  glGetProgramiv(program, GL_LINK_STATUS, &status);
  if (status == GL_TRUE)
  {
    return 1;
  }

  glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);
  if (log_length < 1)
  {
    console_overlay_show_error("Shader Link Error", label);
    return 0;
  }

  log = (char*)malloc((size_t)log_length + 1U);
  if (log == NULL)
  {
    console_overlay_show_error("Memory Error", "Failed to allocate UI program log buffer.");
    return 0;
  }

  glGetProgramInfoLog(program, log_length, NULL, log);
  log[log_length] = '\0';
  console_overlay_show_error(label, log);
  free(log);
  return 0;
}

static GLuint console_overlay_compile_shader(GLenum shader_type, const char* source, const char* label)
{
  GLuint shader = 0U;

  if (source == NULL || label == NULL)
  {
    return 0U;
  }

  shader = glCreateShader(shader_type);
  if (shader == 0U)
  {
    console_overlay_show_error("OpenGL Error", "Failed to create UI shader object.");
    return 0U;
  }

  glShaderSource(shader, 1, &source, NULL);
  glCompileShader(shader);
  if (!console_overlay_check_shader(shader, label))
  {
    glDeleteShader(shader);
    return 0U;
  }

  return shader;
}

static GLuint console_overlay_create_program_from_files(const char* vertex_path, const char* fragment_path, const char* label)
{
  char resolved_vertex_path[MAX_PATH] = { 0 };
  char resolved_fragment_path[MAX_PATH] = { 0 };
  char vertex_label[128] = { 0 };
  char fragment_label[128] = { 0 };
  char* vertex_source = NULL;
  char* fragment_source = NULL;
  GLuint vertex_shader = 0U;
  GLuint fragment_shader = 0U;
  GLuint program = 0U;

  if (!console_overlay_build_shader_path(vertex_path, resolved_vertex_path, sizeof(resolved_vertex_path)) ||
    !console_overlay_build_shader_path(fragment_path, resolved_fragment_path, sizeof(resolved_fragment_path)) ||
    !console_overlay_load_shader_source(resolved_vertex_path, vertex_path, 0, &vertex_source) ||
    !console_overlay_load_shader_source(resolved_fragment_path, fragment_path, 0, &fragment_source))
  {
    free(vertex_source);
    free(fragment_source);
    return 0U;
  }

  (void)snprintf(vertex_label, sizeof(vertex_label), "%s Vertex", label);
  (void)snprintf(fragment_label, sizeof(fragment_label), "%s Fragment", label);
  vertex_shader = console_overlay_compile_shader(GL_VERTEX_SHADER, vertex_source, vertex_label);
  fragment_shader = console_overlay_compile_shader(GL_FRAGMENT_SHADER, fragment_source, fragment_label);
  free(vertex_source);
  free(fragment_source);

  if (vertex_shader == 0U || fragment_shader == 0U)
  {
    if (vertex_shader != 0U)
    {
      glDeleteShader(vertex_shader);
    }
    if (fragment_shader != 0U)
    {
      glDeleteShader(fragment_shader);
    }
    return 0U;
  }

  program = glCreateProgram();
  if (program == 0U)
  {
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);
    console_overlay_show_error("OpenGL Error", "Failed to create UI shader program.");
    return 0U;
  }

  glAttachShader(program, vertex_shader);
  glAttachShader(program, fragment_shader);
  glLinkProgram(program);
  glDetachShader(program, vertex_shader);
  glDetachShader(program, fragment_shader);
  glDeleteShader(vertex_shader);
  glDeleteShader(fragment_shader);

  if (!console_overlay_check_program(program, label))
  {
    glDeleteProgram(program);
    return 0U;
  }

  return program;
}

static int console_overlay_create_backdrop_pipeline(ConsoleOverlay* overlay)
{
  static const float initial_vertices[16] = {
    0.0f, 0.0f, 0.0f, 1.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    1.0f, 0.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 0.0f
  };

  if (overlay == NULL)
  {
    return 0;
  }

  overlay->backdrop_program =
    console_overlay_create_program_from_files("ui\\overlay_blur.vert.glsl", "ui\\overlay_blur.frag.glsl", "Overlay Blur");
  if (overlay->backdrop_program == 0U)
  {
    return 0;
  }

  overlay->backdrop_viewport_size_location = glGetUniformLocation(overlay->backdrop_program, "viewport_size");
  overlay->backdrop_texel_size_location = glGetUniformLocation(overlay->backdrop_program, "texel_size");
  overlay->backdrop_blur_radius_location = glGetUniformLocation(overlay->backdrop_program, "blur_radius");
  overlay->backdrop_sampler_location = glGetUniformLocation(overlay->backdrop_program, "backdrop_texture");

  glGenVertexArrays(1, &overlay->backdrop_vao);
  glGenBuffers(1, &overlay->backdrop_vertex_buffer);
  if (overlay->backdrop_vao == 0U || overlay->backdrop_vertex_buffer == 0U)
  {
    console_overlay_show_error("OpenGL Error", "Failed to create UI backdrop geometry.");
    return 0;
  }

  glBindVertexArray(overlay->backdrop_vao);
  glBindBuffer(GL_ARRAY_BUFFER, overlay->backdrop_vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, sizeof(initial_vertices), initial_vertices, GL_DYNAMIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, (GLsizei)(sizeof(float) * 4), (const void*)0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, (GLsizei)(sizeof(float) * 4), (const void*)(sizeof(float) * 2));
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
  return 1;
}

static void console_overlay_update_backdrop_geometry(ConsoleOverlay* overlay, float panel_width, float panel_height)
{
  const float vertices[16] = {
    0.0f, 0.0f, 0.0f, 1.0f,
    0.0f, panel_height, 0.0f, 0.0f,
    panel_width, 0.0f, 1.0f, 1.0f,
    panel_width, panel_height, 1.0f, 0.0f
  };

  if (overlay == NULL || overlay->backdrop_vertex_buffer == 0U)
  {
    return;
  }

  glBindBuffer(GL_ARRAY_BUFFER, overlay->backdrop_vertex_buffer);
  glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static int console_overlay_ensure_backdrop_texture(ConsoleOverlay* overlay, int width, int height)
{
  if (overlay == NULL || width <= 0 || height <= 0)
  {
    return 0;
  }

  if (overlay->backdrop_texture == 0U)
  {
    glGenTextures(1, &overlay->backdrop_texture);
  }
  if (overlay->backdrop_texture == 0U)
  {
    return 0;
  }

  glBindTexture(GL_TEXTURE_2D, overlay->backdrop_texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  if (overlay->backdrop_width != width || overlay->backdrop_height != height)
  {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    overlay->backdrop_width = width;
    overlay->backdrop_height = height;
  }

  return 1;
}

static void console_overlay_capture_backdrop(ConsoleOverlay* overlay, int width, int height)
{
  if (!console_overlay_ensure_backdrop_texture(overlay, width, height))
  {
    return;
  }

  glBindTexture(GL_TEXTURE_2D, overlay->backdrop_texture);
  glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
}

static void console_overlay_draw_backdrop(ConsoleOverlay* overlay, float viewport_width, float viewport_height, float panel_width, float panel_height)
{
  if (overlay == NULL ||
    overlay->backdrop_texture == 0U ||
    overlay->backdrop_program == 0U ||
    overlay->backdrop_vao == 0U ||
    panel_width <= 0.0f ||
    panel_height <= 0.0f ||
    viewport_width <= 0.0f ||
    viewport_height <= 0.0f)
  {
    return;
  }

  console_overlay_update_backdrop_geometry(overlay, panel_width, panel_height);
  glUseProgram(overlay->backdrop_program);
  if (overlay->backdrop_viewport_size_location >= 0)
  {
    glUniform2f(overlay->backdrop_viewport_size_location, viewport_width, viewport_height);
  }
  if (overlay->backdrop_texel_size_location >= 0)
  {
    glUniform2f(
      overlay->backdrop_texel_size_location,
      (panel_width > 1.0f) ? (1.0f / panel_width) : 0.0f,
      (panel_height > 1.0f) ? (1.0f / panel_height) : 0.0f);
  }
  if (overlay->backdrop_blur_radius_location >= 0)
  {
    glUniform1f(overlay->backdrop_blur_radius_location, 2.35f);
  }
  if (overlay->backdrop_sampler_location >= 0)
  {
    glUniform1i(overlay->backdrop_sampler_location, 0);
  }

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, overlay->backdrop_texture);
  glBindVertexArray(overlay->backdrop_vao);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindVertexArray(0);
  glBindTexture(GL_TEXTURE_2D, 0);
  glUseProgram(0);
}

static void console_overlay_draw_text(const ConsoleOverlay* overlay, float x, float y, float r, float g, float b, const char* text)
{
  unsigned char glyphs[256] = { 0 };
  size_t length = 0U;
  size_t i = 0U;

  if (overlay == NULL || overlay->font_base == 0U || text == NULL || text[0] == '\0')
  {
    return;
  }

  length = strlen(text);
  if (length > sizeof(glyphs))
  {
    length = sizeof(glyphs);
  }
  for (i = 0; i < length; ++i)
  {
    unsigned char character = (unsigned char)text[i];
    if (character < 32U || character > 127U)
    {
      character = (unsigned char)'?';
    }
    glyphs[i] = (unsigned char)(character - 32U);
  }

  glColor3f(r, g, b);
  glRasterPos2f(x, y);
  glListBase(overlay->font_base);
  glCallLists((GLsizei)length, GL_UNSIGNED_BYTE, glyphs);
}

static void console_overlay_copy_text_fit(char* buffer, size_t buffer_size, const char* text, float max_width)
{
  const size_t text_length = (text != NULL) ? strlen(text) : 0U;
  int max_chars = (int)(max_width / 8.0f);
  size_t copy_length = 0U;

  if (buffer == NULL || buffer_size == 0U)
  {
    return;
  }

  buffer[0] = '\0';
  if (text == NULL || max_chars <= 0)
  {
    return;
  }

  if ((size_t)max_chars >= text_length)
  {
    (void)snprintf(buffer, buffer_size, "%s", text);
    return;
  }

  if (max_chars <= 3)
  {
    return;
  }

  copy_length = (size_t)(max_chars - 3);
  if (copy_length > buffer_size - 4U)
  {
    copy_length = buffer_size - 4U;
  }
  if (copy_length > text_length)
  {
    copy_length = text_length;
  }

  memcpy(buffer, text, copy_length);
  memcpy(buffer + copy_length, "...", 4U);
}

static int console_overlay_get_slider_rect(const OverlayState* overlay, OverlaySliderId slider_id, float* out_left, float* out_top, float* out_right, float* out_bottom)
{
  float y = 0.0f;
  int index = 0;

  if (overlay == NULL)
  {
    return 0;
  }

  y = (float)overlay_get_scroll_view_top() - overlay->scroll_offset;

  for (index = 0; index < OVERLAY_SLIDER_COUNT; ++index)
  {
    if (overlay_has_gameplay_toggle_before_slider((OverlaySliderId)index))
    {
      y += (float)overlay_get_gameplay_toggle_block_height();
    }

    if (overlay_has_cloud_toggle_before_slider((OverlaySliderId)index))
    {
      y += (float)overlay_get_cloud_toggle_block_height();
    }

    if (overlay_has_quality_selector_before_slider((OverlaySliderId)index))
    {
      y += (float)overlay_get_quality_selector_block_height();
    }

    if (overlay_has_gpu_selector_before_slider((OverlaySliderId)index))
    {
      y += (float)overlay_get_gpu_selector_block_height();
    }

    if (overlay_has_metric_card_before_slider((OverlaySliderId)index))
    {
      y += (float)(OVERLAY_UI_METRIC_HEIGHT + OVERLAY_UI_SECTION_SPACING);
    }

    y += (float)(OVERLAY_UI_LABEL_HEIGHT + OVERLAY_UI_ITEM_SPACING);
    if (index == slider_id)
    {
      if (out_left != NULL)
      {
        *out_left = (float)OVERLAY_UI_MARGIN;
      }
      if (out_top != NULL)
      {
        *out_top = y;
      }
      if (out_right != NULL)
      {
        *out_right = (float)(overlay->panel_width - OVERLAY_UI_MARGIN);
      }
      if (out_bottom != NULL)
      {
        *out_bottom = y + (float)OVERLAY_UI_SLIDER_HEIGHT;
      }
      return 1;
    }
    y += (float)(OVERLAY_UI_SLIDER_HEIGHT + OVERLAY_UI_SECTION_SPACING);
  }

  return 0;
}

static int console_overlay_get_toggle_rect(const OverlayState* overlay, OverlayToggleId toggle_id, float* out_left, float* out_top, float* out_right, float* out_bottom)
{
  float y = 0.0f;
  int index = 0;

  if (overlay == NULL)
  {
    return 0;
  }

  y = (float)overlay_get_scroll_view_top() - overlay->scroll_offset;

  for (index = 0; index < OVERLAY_SLIDER_COUNT; ++index)
  {
    if (overlay_has_gameplay_toggle_before_slider((OverlaySliderId)index))
    {
      if (toggle_id == OVERLAY_TOGGLE_GOD_MODE)
      {
        if (out_left != NULL)
        {
          *out_left = (float)OVERLAY_UI_MARGIN;
        }
        if (out_top != NULL)
        {
          *out_top = y + (float)(OVERLAY_UI_LABEL_HEIGHT + OVERLAY_UI_ITEM_SPACING);
        }
        if (out_right != NULL)
        {
          *out_right = (float)(overlay->panel_width - OVERLAY_UI_MARGIN);
        }
        if (out_bottom != NULL)
        {
          *out_bottom = y + (float)(OVERLAY_UI_LABEL_HEIGHT + OVERLAY_UI_ITEM_SPACING + OVERLAY_UI_CHECKBOX_HEIGHT);
        }
        return 1;
      }

      y += (float)(OVERLAY_UI_LABEL_HEIGHT + OVERLAY_UI_ITEM_SPACING + OVERLAY_UI_CHECKBOX_HEIGHT + OVERLAY_UI_SECTION_SPACING);

      if (toggle_id == OVERLAY_TOGGLE_FREEZE_TIME)
      {
        if (out_left != NULL)
        {
          *out_left = (float)OVERLAY_UI_MARGIN;
        }
        if (out_top != NULL)
        {
          *out_top = y + (float)(OVERLAY_UI_LABEL_HEIGHT + OVERLAY_UI_ITEM_SPACING);
        }
        if (out_right != NULL)
        {
          *out_right = (float)(overlay->panel_width - OVERLAY_UI_MARGIN);
        }
        if (out_bottom != NULL)
        {
          *out_bottom = y + (float)(OVERLAY_UI_LABEL_HEIGHT + OVERLAY_UI_ITEM_SPACING + OVERLAY_UI_CHECKBOX_HEIGHT);
        }
        return 1;
      }

      y += (float)(OVERLAY_UI_LABEL_HEIGHT + OVERLAY_UI_ITEM_SPACING + OVERLAY_UI_CHECKBOX_HEIGHT + OVERLAY_UI_SECTION_SPACING);

      if (toggle_id == OVERLAY_TOGGLE_SOUND)
      {
        if (out_left != NULL)
        {
          *out_left = (float)OVERLAY_UI_MARGIN;
        }
        if (out_top != NULL)
        {
          *out_top = y + (float)(OVERLAY_UI_LABEL_HEIGHT + OVERLAY_UI_ITEM_SPACING);
        }
        if (out_right != NULL)
        {
          *out_right = (float)(overlay->panel_width - OVERLAY_UI_MARGIN);
        }
        if (out_bottom != NULL)
        {
          *out_bottom = y + (float)(OVERLAY_UI_LABEL_HEIGHT + OVERLAY_UI_ITEM_SPACING + OVERLAY_UI_CHECKBOX_HEIGHT);
        }
        return 1;
      }

      y += (float)(OVERLAY_UI_LABEL_HEIGHT + OVERLAY_UI_ITEM_SPACING + OVERLAY_UI_CHECKBOX_HEIGHT + OVERLAY_UI_SECTION_SPACING);
    }

    if (overlay_has_cloud_toggle_before_slider((OverlaySliderId)index))
    {
      if (toggle_id == OVERLAY_TOGGLE_CLOUDS)
      {
        if (out_left != NULL)
        {
          *out_left = (float)OVERLAY_UI_MARGIN;
        }
        if (out_top != NULL)
        {
          *out_top = y + (float)(OVERLAY_UI_LABEL_HEIGHT + OVERLAY_UI_ITEM_SPACING);
        }
        if (out_right != NULL)
        {
          *out_right = (float)(overlay->panel_width - OVERLAY_UI_MARGIN);
        }
        if (out_bottom != NULL)
        {
          *out_bottom = y + (float)(OVERLAY_UI_LABEL_HEIGHT + OVERLAY_UI_ITEM_SPACING + OVERLAY_UI_CHECKBOX_HEIGHT);
        }
        return 1;
      }

      y += (float)overlay_get_cloud_toggle_block_height();
    }

    if (overlay_has_quality_selector_before_slider((OverlaySliderId)index))
    {
      y += (float)overlay_get_quality_selector_block_height();
    }

    if (overlay_has_gpu_selector_before_slider((OverlaySliderId)index))
    {
      y += (float)overlay_get_gpu_selector_block_height();
    }

    if (overlay_has_metric_card_before_slider((OverlaySliderId)index))
    {
      y += (float)(OVERLAY_UI_METRIC_HEIGHT + OVERLAY_UI_SECTION_SPACING);
    }

    y += (float)(OVERLAY_UI_LABEL_HEIGHT + OVERLAY_UI_ITEM_SPACING + OVERLAY_UI_SLIDER_HEIGHT + OVERLAY_UI_SECTION_SPACING);
  }

  return 0;
}

static int console_overlay_get_render_quality_rect(const OverlayState* overlay, RendererQualityPreset preset, float* out_left, float* out_top, float* out_right, float* out_bottom)
{
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  if (overlay == NULL)
  {
    return 0;
  }

  if (!overlay_get_render_quality_button_rect(
    overlay->panel_width,
    overlay->scroll_offset,
    preset,
    &left,
    &top,
    &right,
    &bottom))
  {
    return 0;
  }

  if (out_left != NULL)
  {
    *out_left = (float)left;
  }
  if (out_top != NULL)
  {
    *out_top = (float)top;
  }
  if (out_right != NULL)
  {
    *out_right = (float)right;
  }
  if (out_bottom != NULL)
  {
    *out_bottom = (float)bottom;
  }

  return 1;
}

static int console_overlay_get_gpu_preference_rect(const OverlayState* overlay, GpuPreferenceMode mode, float* out_left, float* out_top, float* out_right, float* out_bottom)
{
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  if (overlay == NULL)
  {
    return 0;
  }

  if (!overlay_get_gpu_preference_button_rect(
    overlay->panel_width,
    overlay->scroll_offset,
    mode,
    &left,
    &top,
    &right,
    &bottom))
  {
    return 0;
  }

  if (out_left != NULL)
  {
    *out_left = (float)left;
  }
  if (out_top != NULL)
  {
    *out_top = (float)top;
  }
  if (out_right != NULL)
  {
    *out_right = (float)right;
  }
  if (out_bottom != NULL)
  {
    *out_bottom = (float)bottom;
  }

  return 1;
}

static float console_overlay_get_slider_value(const SceneSettings* settings, OverlaySliderId slider_id)
{
  switch (slider_id)
  {
    case OVERLAY_SLIDER_SUN_DISTANCE:
      return settings->sun_distance_mkm;
    case OVERLAY_SLIDER_SUN_ORBIT:
      return settings->sun_orbit_degrees;
    case OVERLAY_SLIDER_CYCLE_DURATION:
      return settings->cycle_duration_seconds;
    case OVERLAY_SLIDER_DAYLIGHT_FRACTION:
      return settings->daylight_fraction;
    case OVERLAY_SLIDER_CAMERA_FOV:
      return settings->camera_fov_degrees;
    case OVERLAY_SLIDER_FOG_DENSITY:
      return settings->fog_density;
    case OVERLAY_SLIDER_CLOUD_AMOUNT:
      return settings->cloud_amount;
    case OVERLAY_SLIDER_CLOUD_SPACING:
      return settings->cloud_spacing;
    case OVERLAY_SLIDER_TERRAIN_BASE:
      return settings->terrain_base_height;
    case OVERLAY_SLIDER_TERRAIN_HEIGHT:
      return settings->terrain_height_scale;
    case OVERLAY_SLIDER_TERRAIN_ROUGHNESS:
      return settings->terrain_roughness;
    case OVERLAY_SLIDER_TERRAIN_RIDGE:
      return settings->terrain_ridge_strength;
    case OVERLAY_SLIDER_PALM_SIZE:
      return settings->palm_size;
    case OVERLAY_SLIDER_PALM_COUNT:
      return settings->palm_count;
    case OVERLAY_SLIDER_PALM_FRUIT_DENSITY:
      return settings->palm_fruit_density;
    case OVERLAY_SLIDER_PALM_RENDER_RADIUS:
      return settings->palm_render_radius;
    case OVERLAY_SLIDER_NONE:
    case OVERLAY_SLIDER_COUNT:
    default:
      return 0.0f;
  }
}

static void console_overlay_format_slider_value(char* buffer, size_t buffer_size, OverlaySliderId slider_id, float value)
{
  switch (slider_id)
  {
    case OVERLAY_SLIDER_SUN_DISTANCE:
      (void)snprintf(buffer, buffer_size, "%.1f Mkm", value);
      break;
    case OVERLAY_SLIDER_SUN_ORBIT:
      (void)snprintf(buffer, buffer_size, "%.0f deg", value);
      break;
    case OVERLAY_SLIDER_CYCLE_DURATION:
      (void)snprintf(buffer, buffer_size, "%.0f s", value);
      break;
    case OVERLAY_SLIDER_DAYLIGHT_FRACTION:
      (void)snprintf(buffer, buffer_size, "%.0f%%", value * 100.0f);
      break;
    case OVERLAY_SLIDER_CAMERA_FOV:
      (void)snprintf(buffer, buffer_size, "%.0f deg", value);
      break;
    case OVERLAY_SLIDER_FOG_DENSITY:
      (void)snprintf(buffer, buffer_size, "%.0f%%", value * 100.0f);
      break;
    case OVERLAY_SLIDER_CLOUD_AMOUNT:
      (void)snprintf(buffer, buffer_size, "%.0f%%", value * 100.0f);
      break;
    case OVERLAY_SLIDER_CLOUD_SPACING:
      (void)snprintf(buffer, buffer_size, "%.2fx", value);
      break;
    case OVERLAY_SLIDER_TERRAIN_BASE:
      (void)snprintf(buffer, buffer_size, "%.1f m", value);
      break;
    case OVERLAY_SLIDER_TERRAIN_HEIGHT:
    case OVERLAY_SLIDER_TERRAIN_ROUGHNESS:
    case OVERLAY_SLIDER_TERRAIN_RIDGE:
      (void)snprintf(buffer, buffer_size, "%.2fx", value);
      break;
    case OVERLAY_SLIDER_PALM_SIZE:
      (void)snprintf(buffer, buffer_size, "%.2fx", value);
      break;
    case OVERLAY_SLIDER_PALM_COUNT:
      (void)snprintf(buffer, buffer_size, "%.0f trees", value);
      break;
    case OVERLAY_SLIDER_PALM_FRUIT_DENSITY:
      (void)snprintf(buffer, buffer_size, "%.0f%%", value * 100.0f);
      break;
    case OVERLAY_SLIDER_PALM_RENDER_RADIUS:
      (void)snprintf(buffer, buffer_size, "%.0f m", value);
      break;
    case OVERLAY_SLIDER_NONE:
    case OVERLAY_SLIDER_COUNT:
    default:
      buffer[0] = '\0';
      break;
  }
}

#endif
