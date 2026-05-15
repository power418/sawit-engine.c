#include "stats_overlay.h"

#if !defined(_WIN32) && !defined(__linux__)

#include <string.h>

int stats_overlay_create(StatsOverlay* overlay)
{
  if (overlay == NULL)
  {
    return 0;
  }

  memset(overlay, 0, sizeof(*overlay));
  return 1;
}

void stats_overlay_destroy(StatsOverlay* overlay)
{
  if (overlay == NULL)
  {
    return;
  }

  memset(overlay, 0, sizeof(*overlay));
}

void stats_overlay_render(StatsOverlay* overlay, int width, int height, const OverlayState* state)
{
  (void)overlay;
  (void)width;
  (void)height;
  (void)state;
}

#else

#include "diagnostics.h"
#include "player_controller.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#if defined(__linux__)
#include <X11/Xlib.h>

extern Display* glXGetCurrentDisplay(void);
extern void glXUseXFont(Font font, int first, int count, int list_base);
#endif

static void stats_overlay_show_error(const char* title, const char* message);
static int stats_overlay_build_font(GLuint* out_font_base, int font_height);
static void stats_overlay_draw_rect(float left, float top, float right, float bottom, float r, float g, float b, float a);
static void stats_overlay_draw_outline(float left, float top, float right, float bottom, float r, float g, float b, float a);
static float stats_overlay_get_font_glyph_width(const StatsOverlay* overlay, GLuint font_base);
static void stats_overlay_draw_text(GLuint font_base, float x, float y, float r, float g, float b, float a, const char* text);
static void stats_overlay_draw_text_right(
  const StatsOverlay* overlay,
  GLuint font_base,
  float right,
  float y,
  float r,
  float g,
  float b,
  float a,
  const char* text
);
static void stats_overlay_draw_text_shadow(
  const StatsOverlay* overlay,
  GLuint font_base,
  float x,
  float y,
  float r,
  float g,
  float b,
  float a,
  const char* text
);
static void stats_overlay_copy_text_fit(char* buffer, size_t buffer_size, const char* text, int max_characters);
static void stats_overlay_push_frame_sample(StatsOverlay* overlay, float frame_time_ms);
static void stats_overlay_get_frame_extents(const StatsOverlay* overlay, float* out_min_ms, float* out_max_ms);
static void stats_overlay_draw_frame_graph(
  const StatsOverlay* overlay,
  float left,
  float top,
  float right,
  float bottom,
  float line_r,
  float line_g,
  float line_b
);
static void stats_overlay_draw_debug_console(const StatsOverlay* overlay, int width, int height, const OverlayState* state);

int stats_overlay_create(StatsOverlay* overlay)
{
  if (overlay == NULL)
  {
    return 0;
  }

  memset(overlay, 0, sizeof(*overlay));
  if (!stats_overlay_build_font(&overlay->small_font_base, -12) ||
    !stats_overlay_build_font(&overlay->large_font_base, -18) ||
    !stats_overlay_build_font(&overlay->hero_font_base, -28))
  {
    stats_overlay_destroy(overlay);
    return 0;
  }

  return 1;
}

void stats_overlay_destroy(StatsOverlay* overlay)
{
  if (overlay == NULL)
  {
    return;
  }

  if (overlay->small_font_base != 0U)
  {
    glDeleteLists(overlay->small_font_base, 96);
    overlay->small_font_base = 0U;
  }

  if (overlay->large_font_base != 0U)
  {
    glDeleteLists(overlay->large_font_base, 96);
    overlay->large_font_base = 0U;
  }

  if (overlay->hero_font_base != 0U)
  {
    glDeleteLists(overlay->hero_font_base, 96);
    overlay->hero_font_base = 0U;
  }

  overlay->frame_time_history_write_index = 0;
  overlay->frame_time_history_count = 0;
  overlay->last_frame_sample_index = 0U;
}

void stats_overlay_render(StatsOverlay* overlay, int width, int height, const OverlayState* state)
{
  const OverlayState fallback_overlay = {
    .settings = { 149.6f, 180.0f, 0.5f, 65.0f, 0.42f, 0.62f, 1.0f, -14.0f, 1.0f, 1.0f, 1.0f, 1 },
    .metrics = { 149.6f, 90.0f, 90.0f, 60.0f, 16.6f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1, 1, 0, 0, 0U },
    .panel_width = OVERLAY_UI_DEFAULT_WIDTH,
    .mouse_x = 0,
    .mouse_y = 0,
    .cursor_mode_enabled = 0,
    .hot_slider = OVERLAY_SLIDER_NONE,
    .active_slider = OVERLAY_SLIDER_NONE,
    .hot_toggle = OVERLAY_TOGGLE_NONE,
    .hot_render_quality_preset = -1,
    .god_mode_enabled = 0,
    .ui_time_seconds = 0.0f,
    .scroll_offset = 0.0f,
    .scroll_max = 0.0f
  };
  const OverlayState* active_overlay = (state != NULL) ? state : &fallback_overlay;
  const float current_frame_ms = (active_overlay->metrics.frame_time_ms > 0.0f)
    ? active_overlay->metrics.frame_time_ms
    : ((active_overlay->metrics.frames_per_second > 0.01f) ? (1000.0f / active_overlay->metrics.frames_per_second) : 0.0f);
  const float fps_value = (active_overlay->metrics.frames_per_second > 0.0f)
    ? active_overlay->metrics.frames_per_second
    : ((current_frame_ms > 0.01f) ? (1000.0f / current_frame_ms) : 0.0f);
  const float cpu_usage = fminf(fmaxf(active_overlay->metrics.cpu_usage_percent, 0.0f), 100.0f);
  const float gpu0_usage = fminf(fmaxf(active_overlay->metrics.gpu0_usage_percent, 0.0f), 100.0f);
  const float gpu1_usage = fminf(fmaxf(active_overlay->metrics.gpu1_usage_percent, 0.0f), 100.0f);
  const float system_memory_usage = fminf(fmaxf(active_overlay->metrics.system_memory_percent, 0.0f), 100.0f);
  const int thermal_available =
    active_overlay->metrics.gpu_temperature_available != 0 ||
    active_overlay->metrics.thermal_zone_temperature_available != 0;
  const float display_temperature =
    (active_overlay->metrics.gpu_temperature_available != 0)
      ? active_overlay->metrics.gpu_temperature_c
      : active_overlay->metrics.thermal_zone_temperature_c;
  const char* temperature_label =
    (active_overlay->metrics.gpu_temperature_available != 0)
      ? "GPU"
      : ((active_overlay->metrics.thermal_zone_temperature_available != 0) ? "ZONE" : "N/A");
  const char* health_label = "UNKNOWN";
  const float hud_width = 308.0f;
  const float hud_height = 432.0f;
  const float hud_margin = 12.0f;
  const float hud_left = (float)width - hud_width - hud_margin;
  const float hud_top = 12.0f;
  const float hud_right = hud_left + hud_width;
  const float hud_bottom = hud_top + hud_height;
  const float graph_left = hud_left + 14.0f;
  const float graph_top = hud_top + 284.0f;
  const float graph_right = hud_right - 14.0f;
  const float graph_bottom = hud_top + 324.0f;
  const float info_left = hud_left + 14.0f;
  const float info_right = hud_left + 170.0f;
  const char* mode_label = player_controller_get_mode_label((PlayerMode)active_overlay->metrics.player_mode);
  float fps_r = 0.42f;
  float fps_g = 0.96f;
  float fps_b = 0.38f;
  float frame_min_ms = current_frame_ms;
  float frame_max_ms = current_frame_ms;
  float health_r = 0.82f;
  float health_g = 0.86f;
  float health_b = 0.90f;
  char line_buffer[96] = { 0 };
  char value_buffer[64] = { 0 };
  char active_gpu_buffer[OVERLAY_METRICS_ACTIVE_GPU_NAME_LENGTH] = { 0 };
  char gpu0_name_buffer[OVERLAY_METRICS_ACTIVE_GPU_NAME_LENGTH] = { 0 };
  char gpu1_name_buffer[OVERLAY_METRICS_ACTIVE_GPU_NAME_LENGTH] = { 0 };

  if (overlay == NULL ||
    overlay->small_font_base == 0U ||
    overlay->large_font_base == 0U ||
    overlay->hero_font_base == 0U ||
    width <= 0 ||
    height <= 0)
  {
    return;
  }

  if (current_frame_ms > 0.01f &&
    active_overlay->metrics.stats_sample_index != overlay->last_frame_sample_index)
  {
    stats_overlay_push_frame_sample(overlay, current_frame_ms);
    overlay->last_frame_sample_index = active_overlay->metrics.stats_sample_index;
  }
  stats_overlay_get_frame_extents(overlay, &frame_min_ms, &frame_max_ms);
  if (frame_min_ms <= 0.0f)
  {
    frame_min_ms = current_frame_ms;
  }
  if (frame_max_ms <= 0.0f)
  {
    frame_max_ms = current_frame_ms;
  }

  if (fps_value < 30.0f)
  {
    fps_r = 1.0f;
    fps_g = 0.36f;
    fps_b = 0.28f;
  }
  else if (fps_value < 55.0f)
  {
    fps_r = 0.98f;
    fps_g = 0.78f;
    fps_b = 0.24f;
  }

  switch ((OverlayHealthStatus)active_overlay->metrics.health_status)
  {
    case OVERLAY_HEALTH_STATUS_STABLE:
      health_label = "STABLE";
      health_r = 0.38f;
      health_g = 0.96f;
      health_b = 0.58f;
      break;
    case OVERLAY_HEALTH_STATUS_WARM:
      health_label = "WARM";
      health_r = 0.98f;
      health_g = 0.82f;
      health_b = 0.28f;
      break;
    case OVERLAY_HEALTH_STATUS_STRESSED:
      health_label = "STRESSED";
      health_r = 0.98f;
      health_g = 0.52f;
      health_b = 0.26f;
      break;
    case OVERLAY_HEALTH_STATUS_CRITICAL:
      health_label = "CRITICAL";
      health_r = 1.0f;
      health_g = 0.26f;
      health_b = 0.22f;
      break;
    case OVERLAY_HEALTH_STATUS_UNKNOWN:
    default:
      health_label = "UNKNOWN";
      break;
  }

  if (active_overlay->metrics.active_gpu_task_manager_index >= 0)
  {
    (void)snprintf(
      active_gpu_buffer,
      sizeof(active_gpu_buffer),
      "ACTIVE GPU %d | %s",
      active_overlay->metrics.active_gpu_task_manager_index,
      active_overlay->metrics.active_gpu_name[0] != '\0' ? active_overlay->metrics.active_gpu_name : "OpenGL renderer");
  }
  else
  {
    (void)snprintf(
      active_gpu_buffer,
      sizeof(active_gpu_buffer),
      "%s",
      active_overlay->metrics.active_gpu_name[0] != '\0' ? active_overlay->metrics.active_gpu_name : "OpenGL renderer");
  }
  stats_overlay_copy_text_fit(active_gpu_buffer, sizeof(active_gpu_buffer), active_gpu_buffer, 34);
  stats_overlay_copy_text_fit(
    gpu0_name_buffer,
    sizeof(gpu0_name_buffer),
    active_overlay->metrics.gpu0_name[0] != '\0' ? active_overlay->metrics.gpu0_name : "not mapped",
    18);
  stats_overlay_copy_text_fit(
    gpu1_name_buffer,
    sizeof(gpu1_name_buffer),
    active_overlay->metrics.gpu1_name[0] != '\0' ? active_overlay->metrics.gpu1_name : "not mapped",
    18);

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

  stats_overlay_draw_rect(hud_left, hud_top, hud_right, hud_bottom, 0.03f, 0.04f, 0.05f, 0.76f);
  stats_overlay_draw_rect(hud_left, hud_top, hud_right, hud_top + 38.0f, 0.18f, 0.19f, 0.20f, 0.18f);
  stats_overlay_draw_rect(hud_left, hud_top, hud_right, hud_top + 1.0f, 0.72f, 0.74f, 0.76f, 0.16f);
  stats_overlay_draw_rect(hud_right - 1.0f, hud_top, hud_right, hud_bottom, 0.70f, 0.72f, 0.76f, 0.14f);
  stats_overlay_draw_outline(hud_left + 0.5f, hud_top + 0.5f, hud_right - 0.5f, hud_bottom - 0.5f, 0.62f, 0.66f, 0.72f, 0.10f);

  stats_overlay_draw_text_shadow(overlay, overlay->large_font_base, hud_left + 12.0f, hud_top + 28.0f, 0.18f, 0.90f, 0.54f, 1.0f, "ENGINE");
  stats_overlay_draw_text_shadow(overlay, overlay->small_font_base, hud_right - 100.0f, hud_top + 26.0f, 0.94f, 0.95f, 0.97f, 0.96f, "REALTIME");
  stats_overlay_draw_text_shadow(overlay, overlay->small_font_base, hud_left + 12.0f, hud_top + 44.0f, 0.86f, 0.90f, 0.94f, 0.90f, active_gpu_buffer);

  stats_overlay_draw_text_shadow(overlay, overlay->large_font_base, hud_left + 12.0f, hud_top + 72.0f, 0.18f, 0.90f, 0.48f, 1.0f, "GPU 0");
  (void)snprintf(value_buffer, sizeof(value_buffer), "%.0f%%", gpu0_usage);
  stats_overlay_draw_text_right(overlay, overlay->large_font_base, hud_right - 12.0f, hud_top + 70.0f, 0.97f, 0.98f, 0.99f, 1.0f, value_buffer);
  stats_overlay_draw_text(overlay->small_font_base, hud_left + 86.0f, hud_top + 70.0f, 0.74f, 0.80f, 0.86f, 0.88f, gpu0_name_buffer);

  stats_overlay_draw_text_shadow(overlay, overlay->large_font_base, hud_left + 12.0f, hud_top + 98.0f, 0.20f, 0.78f, 0.94f, 1.0f, "GPU 1");
  (void)snprintf(value_buffer, sizeof(value_buffer), "%.0f%%", gpu1_usage);
  stats_overlay_draw_text_right(overlay, overlay->large_font_base, hud_right - 12.0f, hud_top + 96.0f, 0.97f, 0.98f, 0.99f, 1.0f, value_buffer);
  stats_overlay_draw_text(overlay->small_font_base, hud_left + 86.0f, hud_top + 96.0f, 0.74f, 0.80f, 0.86f, 0.88f, gpu1_name_buffer);

  stats_overlay_draw_text_shadow(overlay, overlay->large_font_base, hud_left + 12.0f, hud_top + 124.0f, 0.36f, 0.58f, 0.98f, 1.0f, "CPU");
  (void)snprintf(value_buffer, sizeof(value_buffer), "%.0f%%", cpu_usage);
  stats_overlay_draw_text_right(overlay, overlay->large_font_base, hud_right - 12.0f, hud_top + 122.0f, 0.97f, 0.98f, 0.99f, 1.0f, value_buffer);

  stats_overlay_draw_text_shadow(overlay, overlay->large_font_base, hud_left + 12.0f, hud_top + 150.0f, 0.84f, 0.72f, 0.26f, 1.0f, "RAM");
  (void)snprintf(value_buffer, sizeof(value_buffer), "%.0f%%", system_memory_usage);
  stats_overlay_draw_text_right(overlay, overlay->large_font_base, hud_right - 12.0f, hud_top + 148.0f, 0.97f, 0.98f, 0.99f, 1.0f, value_buffer);

  stats_overlay_draw_text_shadow(overlay, overlay->large_font_base, hud_left + 12.0f, hud_top + 176.0f, 0.96f, 0.44f, 0.30f, 1.0f, "TEMP");
  if (thermal_available != 0)
  {
    (void)snprintf(value_buffer, sizeof(value_buffer), "%s %.1fC", temperature_label, display_temperature);
  }
  else
  {
    (void)snprintf(value_buffer, sizeof(value_buffer), "N/A");
  }
  stats_overlay_draw_text_right(overlay, overlay->small_font_base, hud_right - 12.0f, hud_top + 174.0f, 0.97f, 0.98f, 0.99f, 1.0f, value_buffer);

  stats_overlay_draw_text_shadow(overlay, overlay->large_font_base, hud_left + 12.0f, hud_top + 202.0f, health_r, health_g, health_b, 1.0f, "HEALTH");
  stats_overlay_draw_text_right(overlay, overlay->large_font_base, hud_right - 12.0f, hud_top + 200.0f, health_r, health_g, health_b, 1.0f, health_label);

  stats_overlay_draw_text_shadow(overlay, overlay->large_font_base, hud_left + 12.0f, hud_top + 230.0f, 0.96f, 0.36f, 0.32f, 1.0f, "OPENGL");
  (void)snprintf(value_buffer, sizeof(value_buffer), "%.0f", fps_value);
  stats_overlay_draw_text_right(overlay, overlay->hero_font_base, hud_right - 108.0f, hud_top + 228.0f, 0.97f, 0.98f, 0.99f, 1.0f, value_buffer);
  stats_overlay_draw_text_shadow(overlay, overlay->small_font_base, hud_right - 96.0f, hud_top + 226.0f, fps_r, fps_g, fps_b, 1.0f, "FPS");

  stats_overlay_draw_text_shadow(overlay, overlay->large_font_base, hud_left + 12.0f, hud_top + 258.0f, 0.96f, 0.44f, 0.48f, 1.0f, "Frametime");
  (void)snprintf(value_buffer, sizeof(value_buffer), "%.1f", current_frame_ms);
  stats_overlay_draw_text_right(overlay, overlay->large_font_base, hud_right - 38.0f, hud_top + 256.0f, 0.98f, 0.98f, 0.99f, 1.0f, value_buffer);
  stats_overlay_draw_text_shadow(overlay, overlay->small_font_base, hud_right - 28.0f, hud_top + 254.0f, 0.92f, 0.94f, 0.98f, 0.98f, "MS");
  (void)snprintf(line_buffer, sizeof(line_buffer), "min: %.1fms, max: %.1fms", frame_min_ms, frame_max_ms);
  stats_overlay_draw_text_right(overlay, overlay->small_font_base, hud_right - 12.0f, hud_top + 272.0f, 0.90f, 0.92f, 0.95f, 0.90f, line_buffer);

  stats_overlay_draw_frame_graph(overlay, graph_left, graph_top, graph_right, graph_bottom, fps_r, fps_g, fps_b);

  stats_overlay_draw_rect(hud_left + 14.0f, hud_top + 334.0f, hud_right - 14.0f, hud_top + 335.0f, 0.18f, 0.22f, 0.25f, 0.84f);

  (void)snprintf(line_buffer, sizeof(line_buffer), "RES %dx%d", width, height);
  stats_overlay_draw_text(overlay->small_font_base, info_left, hud_top + 350.0f, 0.78f, 0.82f, 0.88f, 0.94f, line_buffer);
  (void)snprintf(line_buffer, sizeof(line_buffer), "FOV %.0f", active_overlay->settings.camera_fov_degrees);
  stats_overlay_draw_text(overlay->small_font_base, info_right, hud_top + 350.0f, 0.78f, 0.82f, 0.88f, 0.94f, line_buffer);

  (void)snprintf(line_buffer, sizeof(line_buffer), "TM0 %s", gpu0_name_buffer);
  stats_overlay_draw_text(overlay->small_font_base, info_left, hud_top + 364.0f, 0.78f, 0.82f, 0.88f, 0.94f, line_buffer);
  (void)snprintf(line_buffer, sizeof(line_buffer), "TM1 %s", gpu1_name_buffer);
  stats_overlay_draw_text(overlay->small_font_base, info_left, hud_top + 378.0f, 0.78f, 0.82f, 0.88f, 0.94f, line_buffer);

  (void)snprintf(
    line_buffer,
    sizeof(line_buffer),
    "MEM %.0f/%.0f",
    active_overlay->metrics.system_memory_used_mb,
    active_overlay->metrics.system_memory_total_mb);
  stats_overlay_draw_text(overlay->small_font_base, info_left, hud_top + 392.0f, 0.78f, 0.82f, 0.88f, 0.94f, line_buffer);
  (void)snprintf(
    line_buffer,
    sizeof(line_buffer),
    "VRAM %.0f/%.0f",
    active_overlay->metrics.gpu0_memory_usage_mb,
    active_overlay->metrics.gpu1_memory_usage_mb);
  stats_overlay_draw_text(overlay->small_font_base, info_right, hud_top + 392.0f, 0.78f, 0.82f, 0.88f, 0.94f, line_buffer);

  if (active_overlay->metrics.active_gpu_task_manager_index >= 0)
  {
    (void)snprintf(line_buffer, sizeof(line_buffer), "ACTIVE GPU %d", active_overlay->metrics.active_gpu_task_manager_index);
  }
  else
  {
    (void)snprintf(line_buffer, sizeof(line_buffer), "ACTIVE GPU ?");
  }
  stats_overlay_draw_text(overlay->small_font_base, info_left, hud_top + 406.0f, health_r, health_g, health_b, 0.96f, line_buffer);
  (void)snprintf(line_buffer, sizeof(line_buffer), "MODE %s", mode_label);
  stats_overlay_draw_text(overlay->small_font_base, info_right, hud_top + 406.0f, 0.78f, 0.82f, 0.88f, 0.94f, line_buffer);

  if (active_overlay->metrics.net_enabled != 0)
  {
    (void)snprintf(
      line_buffer,
      sizeof(line_buffer),
      "NET %s/%s P%llu R%d %.0fms",
      active_overlay->metrics.net_connected != 0 ? "ON" : "JOIN",
      active_overlay->metrics.net_control_joined != 0 ? "TCP" : "UDP",
      active_overlay->metrics.net_player_id,
      active_overlay->metrics.net_remote_player_count,
      active_overlay->metrics.net_ping_ms);
  }
  else
  {
    (void)snprintf(line_buffer, sizeof(line_buffer), "NET OFF");
  }
  stats_overlay_draw_text(overlay->small_font_base, info_left, hud_top + 420.0f, 0.58f, 0.92f, 1.0f, 0.94f, line_buffer);

  stats_overlay_draw_debug_console(overlay, width, height, active_overlay);

  if (active_overlay->cursor_mode_enabled == 0)
  {
    const float world_left = (float)active_overlay->panel_width;
    const float center_x = world_left + ((float)width - world_left) * 0.5f;
    const float center_y = (float)height * 0.5f;
    const float cross_r = active_overlay->metrics.target_active != 0 ? 0.36f : 0.92f;
    const float cross_g = active_overlay->metrics.target_active != 0 ? 0.92f : 0.96f;
    const float cross_b = active_overlay->metrics.target_active != 0 ? 0.42f : 0.98f;

    stats_overlay_draw_rect(center_x - 8.0f, center_y - 1.0f, center_x + 8.0f, center_y + 1.0f, cross_r, cross_g, cross_b, 0.85f);
    stats_overlay_draw_rect(center_x - 1.0f, center_y - 8.0f, center_x + 1.0f, center_y + 8.0f, cross_r, cross_g, cross_b, 0.85f);
  }

  glPopMatrix();
  glMatrixMode(GL_PROJECTION);
  glPopMatrix();
  glMatrixMode(GL_MODELVIEW);
  glDisable(GL_BLEND);
}

static void stats_overlay_show_error(const char* title, const char* message)
{
  diagnostics_logf("%s: %s", title, message);
#if defined(_WIN32)
  (void)MessageBoxA(NULL, message, title, MB_ICONERROR | MB_OK);
#else
  (void)title;
  (void)message;
#endif
}

static int stats_overlay_build_font(GLuint* out_font_base, int font_height)
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
      stats_overlay_show_error("OpenGL Error", "Failed to resolve the device context for the stats overlay font.");
      return 0;
    }

    font = CreateFontA(
      font_height,
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
      stats_overlay_show_error("Win32 Error", "Failed to create the stats overlay font.");
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
      stats_overlay_show_error("Win32 Error", "Failed to build glyphs for the stats overlay.");
      return 0;
    }

    (void)SelectObject(device_context, previous_font);
    (void)DeleteObject(font);
  }
#elif defined(__linux__)
  {
    const char* font_name = "fixed";
    Display* display = glXGetCurrentDisplay();
    Display* owned_display = NULL;
    XFontStruct* font = NULL;

    if (font_height <= -24)
    {
      font_name = "10x20";
    }
    else if (font_height <= -16)
    {
      font_name = "9x15";
    }

    if (display == NULL)
    {
      owned_display = XOpenDisplay(NULL);
      display = owned_display;
    }
    if (display == NULL)
    {
      stats_overlay_show_error("X11 Error", "Failed to open an X11 display for the stats overlay font.");
      return 0;
    }

    font = XLoadQueryFont(display, font_name);
    if (font == NULL)
    {
      font = XLoadQueryFont(display, "fixed");
    }
    if (font == NULL)
    {
      if (owned_display != NULL)
      {
        XCloseDisplay(owned_display);
      }
      stats_overlay_show_error("X11 Error", "Failed to load a fixed-width X11 font for the stats overlay.");
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
      stats_overlay_show_error("OpenGL Error", "Failed to allocate stats overlay glyph lists.");
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
  return 1;
}

static void stats_overlay_draw_rect(float left, float top, float right, float bottom, float r, float g, float b, float a)
{
  glColor4f(r, g, b, a);
  glBegin(GL_QUADS);
  glVertex2f(left, top);
  glVertex2f(right, top);
  glVertex2f(right, bottom);
  glVertex2f(left, bottom);
  glEnd();
}

static void stats_overlay_draw_outline(float left, float top, float right, float bottom, float r, float g, float b, float a)
{
  glColor4f(r, g, b, a);
  glBegin(GL_LINE_LOOP);
  glVertex2f(left, top);
  glVertex2f(right, top);
  glVertex2f(right, bottom);
  glVertex2f(left, bottom);
  glEnd();
}

static float stats_overlay_get_font_glyph_width(const StatsOverlay* overlay, GLuint font_base)
{
  if (overlay == NULL)
  {
    return 8.0f;
  }

  if (font_base == overlay->hero_font_base)
  {
    return 14.0f;
  }
  if (font_base == overlay->large_font_base)
  {
    return 10.0f;
  }
  return 7.0f;
}

static void stats_overlay_draw_text(GLuint font_base, float x, float y, float r, float g, float b, float a, const char* text)
{
  unsigned char glyphs[256] = { 0 };
  size_t length = 0U;
  size_t i = 0U;

  if (font_base == 0U || text == NULL || text[0] == '\0')
  {
    return;
  }

  length = strlen(text);
  if (length > sizeof(glyphs))
  {
    length = sizeof(glyphs);
  }

  for (i = 0U; i < length; ++i)
  {
    unsigned char character = (unsigned char)text[i];
    if (character < 32U || character > 127U)
    {
      character = (unsigned char)'?';
    }
    glyphs[i] = (unsigned char)(character - 32U);
  }

  glColor4f(r, g, b, a);
  glRasterPos2f(x, y);
  glListBase(font_base);
  glCallLists((GLsizei)length, GL_UNSIGNED_BYTE, glyphs);
}

static void stats_overlay_draw_text_right(
  const StatsOverlay* overlay,
  GLuint font_base,
  float right,
  float y,
  float r,
  float g,
  float b,
  float a,
  const char* text
)
{
  const float glyph_width = stats_overlay_get_font_glyph_width(overlay, font_base);
  const size_t length = (text != NULL) ? strlen(text) : 0U;
  const float x = right - (float)length * glyph_width;
  stats_overlay_draw_text(font_base, x, y, r, g, b, a, text);
}

static void stats_overlay_draw_text_shadow(
  const StatsOverlay* overlay,
  GLuint font_base,
  float x,
  float y,
  float r,
  float g,
  float b,
  float a,
  const char* text
)
{
  (void)overlay;
  stats_overlay_draw_text(font_base, x + 1.0f, y + 1.0f, 0.0f, 0.0f, 0.0f, a * 0.55f, text);
  stats_overlay_draw_text(font_base, x, y, r, g, b, a, text);
}

static void stats_overlay_copy_text_fit(char* buffer, size_t buffer_size, const char* text, int max_characters)
{
  size_t length = 0U;
  size_t copy_length = 0U;

  if (buffer == NULL || buffer_size == 0U)
  {
    return;
  }

  buffer[0] = '\0';
  if (text == NULL || text[0] == '\0' || max_characters <= 0)
  {
    return;
  }

  length = strlen(text);
  if ((int)length <= max_characters)
  {
    (void)snprintf(buffer, buffer_size, "%s", text);
    return;
  }

  if (max_characters <= 3)
  {
    return;
  }

  copy_length = (size_t)(max_characters - 3);
  if (copy_length > buffer_size - 4U)
  {
    copy_length = buffer_size - 4U;
  }
  memcpy(buffer, text, copy_length);
  memcpy(buffer + copy_length, "...", 4U);
}

static void stats_overlay_push_frame_sample(StatsOverlay* overlay, float frame_time_ms)
{
  if (overlay == NULL || frame_time_ms <= 0.0f)
  {
    return;
  }

  overlay->frame_time_history[overlay->frame_time_history_write_index] = frame_time_ms;
  overlay->frame_time_history_write_index =
    (overlay->frame_time_history_write_index + 1) % (int)(sizeof(overlay->frame_time_history) / sizeof(overlay->frame_time_history[0]));

  if (overlay->frame_time_history_count < (int)(sizeof(overlay->frame_time_history) / sizeof(overlay->frame_time_history[0])))
  {
    overlay->frame_time_history_count += 1;
  }
}

static void stats_overlay_get_frame_extents(const StatsOverlay* overlay, float* out_min_ms, float* out_max_ms)
{
  int index = 0;
  float min_ms = 0.0f;
  float max_ms = 0.0f;

  if (out_min_ms != NULL)
  {
    *out_min_ms = 0.0f;
  }
  if (out_max_ms != NULL)
  {
    *out_max_ms = 0.0f;
  }
  if (overlay == NULL || overlay->frame_time_history_count <= 0)
  {
    return;
  }

  min_ms = overlay->frame_time_history[0];
  max_ms = overlay->frame_time_history[0];
  for (index = 1; index < overlay->frame_time_history_count; ++index)
  {
    const float value = overlay->frame_time_history[index];
    if (value < min_ms)
    {
      min_ms = value;
    }
    if (value > max_ms)
    {
      max_ms = value;
    }
  }

  if (out_min_ms != NULL)
  {
    *out_min_ms = min_ms;
  }
  if (out_max_ms != NULL)
  {
    *out_max_ms = max_ms;
  }
}

static void stats_overlay_draw_frame_graph(
  const StatsOverlay* overlay,
  float left,
  float top,
  float right,
  float bottom,
  float line_r,
  float line_g,
  float line_b
)
{
  const float inner_left = left + 4.0f;
  const float inner_top = top + 4.0f;
  const float inner_right = right - 4.0f;
  const float inner_bottom = bottom - 4.0f;
  const float graph_width = inner_right - inner_left;
  const float graph_height = inner_bottom - inner_top;
  const int history_capacity = (int)(sizeof(overlay->frame_time_history) / sizeof(overlay->frame_time_history[0]));
  const int sample_count = (overlay != NULL) ? overlay->frame_time_history_count : 0;
  float min_ms = 0.0f;
  float max_ms = 0.0f;
  float display_max_ms = 18.0f;
  int grid_index = 0;
  int sample_index = 0;

  stats_overlay_draw_rect(left, top, right, bottom, 0.02f, 0.03f, 0.03f, 0.68f);
  stats_overlay_draw_rect(left, top, right, bottom, 0.06f, 0.12f, 0.08f, 0.08f);
  stats_overlay_draw_outline(left + 0.5f, top + 0.5f, right - 0.5f, bottom - 0.5f, 0.20f, 0.28f, 0.22f, 0.32f);

  for (grid_index = 0; grid_index < 3; ++grid_index)
  {
    const float y = inner_top + ((float)(grid_index + 1) / 4.0f) * graph_height;
    stats_overlay_draw_rect(inner_left, y, inner_right, y + 1.0f, 0.18f, 0.26f, 0.18f, 0.22f);
  }

  if (overlay == NULL || sample_count <= 0 || graph_width <= 1.0f || graph_height <= 1.0f)
  {
    return;
  }

  stats_overlay_get_frame_extents(overlay, &min_ms, &max_ms);
  if (max_ms > 0.01f)
  {
    display_max_ms = max_ms * 1.16f;
  }
  if (display_max_ms < 18.0f)
  {
    display_max_ms = 18.0f;
  }
  if (display_max_ms > 66.0f)
  {
    display_max_ms = 66.0f;
  }

  glLineWidth(3.0f);
  glColor4f(line_r * 0.55f, line_g * 0.80f, line_b * 0.55f, 0.18f);
  glBegin(GL_LINE_STRIP);
  for (sample_index = 0; sample_index < sample_count; ++sample_index)
  {
    const int history_index = (overlay->frame_time_history_write_index - sample_count + sample_index + history_capacity) % history_capacity;
    const float sample_value = overlay->frame_time_history[history_index];
    const float normalized = (sample_value > 0.0f) ? (sample_value / display_max_ms) : 0.0f;
    const float x = inner_left + ((sample_count > 1) ? ((float)sample_index / (float)(sample_count - 1)) : 0.0f) * graph_width;
    const float y = inner_bottom - fminf(normalized, 1.0f) * graph_height;
    glVertex2f(x, y);
  }
  glEnd();

  glLineWidth(1.5f);
  glColor4f(0.12f, 0.98f, 0.22f, 0.95f);
  glBegin(GL_LINE_STRIP);
  for (sample_index = 0; sample_index < sample_count; ++sample_index)
  {
    const int history_index = (overlay->frame_time_history_write_index - sample_count + sample_index + history_capacity) % history_capacity;
    const float sample_value = overlay->frame_time_history[history_index];
    const float normalized = (sample_value > 0.0f) ? (sample_value / display_max_ms) : 0.0f;
    const float x = inner_left + ((sample_count > 1) ? ((float)sample_index / (float)(sample_count - 1)) : 0.0f) * graph_width;
    const float y = inner_bottom - fminf(normalized, 1.0f) * graph_height;
    glVertex2f(x, y);
  }
  glEnd();
  glLineWidth(1.0f);

  if (sample_count > 0)
  {
    const int last_history_index = (overlay->frame_time_history_write_index - 1 + history_capacity) % history_capacity;
    const float sample_value = overlay->frame_time_history[last_history_index];
    const float normalized = (sample_value > 0.0f) ? (sample_value / display_max_ms) : 0.0f;
    const float x = inner_right;
    const float y = inner_bottom - fminf(normalized, 1.0f) * graph_height;

    stats_overlay_draw_rect(x - 2.0f, y - 2.0f, x + 2.0f, y + 2.0f, 0.78f, 1.0f, 0.82f, 0.96f);
  }
}

static void stats_overlay_draw_debug_console(const StatsOverlay* overlay, int width, int height, const OverlayState* state)
{
  const int recent_count = diagnostics_get_recent_message_count();
  const int visible_count = (recent_count > 8) ? 8 : recent_count;
  const int coord_line_count = 2;
  const int visible_width = overlay_get_visible_width_for_state(state->panel_width, state->panel_collapsed);
  const float panel_left = (float)visible_width + 16.0f;
  const float panel_right_limit = (float)width - 334.0f;
  const float panel_width = fminf(560.0f, panel_right_limit - panel_left);
  const float line_height = 14.0f;
  const float panel_height = 48.0f + (float)(coord_line_count + ((visible_count > 0) ? visible_count : 1)) * line_height;
  const float panel_bottom = (float)height - 16.0f;
  const float panel_top = panel_bottom - panel_height;
  const float panel_right = panel_left + panel_width;
  const float content_left = panel_left + 12.0f;
  const float content_right = panel_right - 12.0f;
  const int max_characters = (int)((content_right - content_left) / 7.0f);
  const int player_cell_x = (int)floorf(state->metrics.player_position_x);
  const int player_cell_y = (int)floorf(state->metrics.player_position_y);
  const int player_cell_z = (int)floorf(state->metrics.player_position_z);
  const float diagnostics_top = panel_top + 42.0f + (float)coord_line_count * line_height;
  int message_index = 0;
  int first_visible_index = 0;
  char line_buffer[224] = { 0 };

  if (overlay == NULL || state == NULL || width <= 0 || height <= 0 || panel_width < 220.0f)
  {
    return;
  }

  stats_overlay_draw_rect(panel_left, panel_top, panel_right, panel_bottom, 0.03f, 0.03f, 0.04f, 0.82f);
  stats_overlay_draw_rect(panel_left, panel_top, panel_right, panel_top + 28.0f, 0.15f, 0.18f, 0.22f, 0.30f);
  stats_overlay_draw_outline(panel_left + 0.5f, panel_top + 0.5f, panel_right - 0.5f, panel_bottom - 0.5f, 0.48f, 0.56f, 0.66f, 0.28f);
  stats_overlay_draw_text_shadow(overlay, overlay->large_font_base, panel_left + 12.0f, panel_top + 20.0f, 0.92f, 0.94f, 0.98f, 0.96f, "DEBUG CONSOLE");

  (void)snprintf(
    line_buffer,
    sizeof(line_buffer),
    "POS  X %.2f  Y %.2f  Z %.2f",
    state->metrics.player_position_x,
    state->metrics.player_position_y,
    state->metrics.player_position_z);
  stats_overlay_draw_text_shadow(
    overlay,
    overlay->small_font_base,
    content_left,
    panel_top + 42.0f,
    0.80f,
    0.94f,
    0.86f,
    0.96f,
    line_buffer);

  (void)snprintf(
    line_buffer,
    sizeof(line_buffer),
    "CELL X %d  Y %d  Z %d",
    player_cell_x,
    player_cell_y,
    player_cell_z);
  stats_overlay_draw_text_shadow(
    overlay,
    overlay->small_font_base,
    content_left,
    panel_top + 42.0f + line_height,
    0.74f,
    0.82f,
    0.96f,
    0.94f,
    line_buffer);

  if (visible_count <= 0)
  {
    stats_overlay_draw_text_shadow(
      overlay,
      overlay->small_font_base,
      content_left,
      diagnostics_top,
      0.74f,
      0.80f,
      0.88f,
      0.90f,
      "Belum ada diagnostics message.");
    return;
  }

  first_visible_index = recent_count - visible_count;
  for (message_index = 0; message_index < visible_count; ++message_index)
  {
    const char* message = diagnostics_get_recent_message(first_visible_index + message_index);
    const float y = diagnostics_top + (float)message_index * line_height;
    const float fade = 0.72f + 0.28f * ((float)(message_index + 1) / (float)visible_count);

    stats_overlay_copy_text_fit(line_buffer, sizeof(line_buffer), message, max_characters);
    stats_overlay_draw_text_shadow(
      overlay,
      overlay->small_font_base,
      content_left,
      y,
      0.76f * fade,
      0.83f * fade,
      0.90f * fade,
      0.94f,
      line_buffer);
  }
}

#endif
