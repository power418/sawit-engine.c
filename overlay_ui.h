#ifndef OVERLAY_UI_H
#define OVERLAY_UI_H

#include "gpu_preferences.h"
#include "render_quality.h"
#include "scene_settings.h"

#include <stddef.h>

typedef enum OverlaySliderId
{
  OVERLAY_SLIDER_NONE = -1,
  OVERLAY_SLIDER_SUN_DISTANCE = 0,
  OVERLAY_SLIDER_SUN_ORBIT,
  OVERLAY_SLIDER_CYCLE_DURATION,
  OVERLAY_SLIDER_DAYLIGHT_FRACTION,
  OVERLAY_SLIDER_CAMERA_FOV,
  OVERLAY_SLIDER_FOG_DENSITY,
  OVERLAY_SLIDER_CLOUD_AMOUNT,
  OVERLAY_SLIDER_CLOUD_SPACING,
  OVERLAY_SLIDER_TERRAIN_BASE,
  OVERLAY_SLIDER_TERRAIN_HEIGHT,
  OVERLAY_SLIDER_TERRAIN_ROUGHNESS,
  OVERLAY_SLIDER_TERRAIN_RIDGE,
  OVERLAY_SLIDER_PALM_SIZE,
  OVERLAY_SLIDER_PALM_COUNT,
  OVERLAY_SLIDER_PALM_FRUIT_DENSITY,
  OVERLAY_SLIDER_PALM_RENDER_RADIUS,
  OVERLAY_SLIDER_COUNT
} OverlaySliderId;

typedef enum OverlayToggleId
{
  OVERLAY_TOGGLE_NONE = -1,
  OVERLAY_TOGGLE_GOD_MODE = 0,
  OVERLAY_TOGGLE_FREEZE_TIME,
  OVERLAY_TOGGLE_SOUND,
  OVERLAY_TOGGLE_CLOUDS,
  OVERLAY_TOGGLE_COUNT
} OverlayToggleId;

typedef struct OverlayState
{
  SceneSettings settings;
  OverlayMetrics metrics;
  GpuPreferenceInfo gpu_info;
  RendererQualityPreset render_quality_preset;
  int panel_width;
  int panel_collapsed;
  int mouse_x;
  int mouse_y;
  int cursor_mode_enabled;
  int hot_slider;
  int active_slider;
  int hot_toggle;
  int hot_render_quality_preset;
  int hot_gpu_preference;
  int god_mode_enabled;
  int freeze_time_enabled;
  int sound_enabled;
  float ui_time_seconds;
  float scroll_offset;
  float scroll_max;
  float debug_console_scroll_offset;
} OverlayState;

enum
{
  OVERLAY_UI_DEFAULT_WIDTH = 360,
  OVERLAY_UI_MIN_WIDTH = 260,
  OVERLAY_UI_WORLD_MIN_WIDTH = 560,
  OVERLAY_UI_MARGIN = 18,
  OVERLAY_UI_TITLE_HEIGHT = 22,
  OVERLAY_UI_HINT_HEIGHT = 34,
  OVERLAY_UI_SECTION_SPACING = 16,
  OVERLAY_UI_LABEL_HEIGHT = 16,
  OVERLAY_UI_CHECKBOX_HEIGHT = 22,
  OVERLAY_UI_CHECKBOX_SIZE = 14,
  OVERLAY_UI_TOGGLE_BUTTON_SIZE = 40,
  OVERLAY_UI_SLIDER_HEIGHT = 26,
  OVERLAY_UI_SLIDER_TRACK_HEIGHT = 6,
  OVERLAY_UI_QUALITY_CARD_HEIGHT = 74,
  OVERLAY_UI_QUALITY_BUTTON_HEIGHT = 24,
  OVERLAY_UI_QUALITY_BUTTON_GAP = 8,
  OVERLAY_UI_QUALITY_CARD_PADDING = 10,
  OVERLAY_UI_GPU_CARD_HEIGHT = 96,
  OVERLAY_UI_GPU_BUTTON_HEIGHT = 24,
  OVERLAY_UI_GPU_BUTTON_GAP = 8,
  OVERLAY_UI_GPU_CARD_PADDING = 10,
  OVERLAY_UI_ITEM_SPACING = 8,
  OVERLAY_UI_METRIC_HEIGHT = 52,
  OVERLAY_DEBUG_CONSOLE_MARGIN = 16,
  OVERLAY_DEBUG_CONSOLE_MAX_WIDTH = 560,
  OVERLAY_DEBUG_CONSOLE_MIN_WIDTH = 220,
  OVERLAY_DEBUG_CONSOLE_MIN_HEIGHT = 180,
  OVERLAY_DEBUG_CONSOLE_MAX_HEIGHT = 280,
  OVERLAY_DEBUG_CONSOLE_RESERVED_RIGHT = 334,
  OVERLAY_DEBUG_CONSOLE_HEADER_HEIGHT = 28,
  OVERLAY_DEBUG_CONSOLE_ROW_HEIGHT = 18,
  OVERLAY_DEBUG_CONSOLE_PADDING_X = 12,
  OVERLAY_DEBUG_CONSOLE_PADDING_Y = 12,
  OVERLAY_DEBUG_CONSOLE_PINNED_ROW_COUNT = 2,
  OVERLAY_DEBUG_CONSOLE_SECTION_GAP = 8,
  OVERLAY_DEBUG_CONSOLE_SCROLLBAR_WIDTH = 10
};

static inline int overlay_clamp_int(int value, int min_value, int max_value)
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

static inline int overlay_get_panel_toggle_button_rect(
  int panel_width,
  int panel_collapsed,
  int* out_left,
  int* out_top,
  int* out_right,
  int* out_bottom)
{
  const int safe_panel_width = (panel_width > 0) ? panel_width : OVERLAY_UI_DEFAULT_WIDTH;
  const int left = (panel_collapsed != 0)
    ? OVERLAY_UI_MARGIN
    : (safe_panel_width - OVERLAY_UI_MARGIN - OVERLAY_UI_TOGGLE_BUTTON_SIZE);
  const int top = OVERLAY_UI_MARGIN;

  if (out_left != NULL)
  {
    *out_left = left;
  }
  if (out_top != NULL)
  {
    *out_top = top;
  }
  if (out_right != NULL)
  {
    *out_right = left + OVERLAY_UI_TOGGLE_BUTTON_SIZE;
  }
  if (out_bottom != NULL)
  {
    *out_bottom = top + OVERLAY_UI_TOGGLE_BUTTON_SIZE;
  }

  return 1;
}

static inline int overlay_get_visible_width_for_state(int panel_width, int panel_collapsed)
{
  int button_right = 0;

  if (panel_collapsed == 0)
  {
    return (panel_width > 0) ? panel_width : OVERLAY_UI_DEFAULT_WIDTH;
  }

  (void)overlay_get_panel_toggle_button_rect(panel_width, panel_collapsed, NULL, NULL, &button_right, NULL);
  return button_right + OVERLAY_UI_MARGIN;
}

static inline int overlay_has_gameplay_toggle_before_slider(OverlaySliderId slider_id)
{
  return slider_id == OVERLAY_SLIDER_SUN_DISTANCE;
}

static inline int overlay_has_cloud_toggle_before_slider(OverlaySliderId slider_id)
{
  return slider_id == OVERLAY_SLIDER_CLOUD_AMOUNT;
}

static inline int overlay_has_gpu_selector_before_slider(OverlaySliderId slider_id)
{
  return slider_id == OVERLAY_SLIDER_SUN_DISTANCE;
}

static inline int overlay_has_quality_selector_before_slider(OverlaySliderId slider_id)
{
  return slider_id == OVERLAY_SLIDER_SUN_DISTANCE;
}

static inline int overlay_has_metric_card_before_slider(OverlaySliderId slider_id)
{
  return slider_id == OVERLAY_SLIDER_TERRAIN_BASE;
}

static inline int overlay_get_gameplay_toggle_block_height(void)
{
  return (OVERLAY_UI_LABEL_HEIGHT + OVERLAY_UI_ITEM_SPACING + OVERLAY_UI_CHECKBOX_HEIGHT + OVERLAY_UI_SECTION_SPACING) * 3;
}

static inline int overlay_get_cloud_toggle_block_height(void)
{
  return OVERLAY_UI_LABEL_HEIGHT + OVERLAY_UI_ITEM_SPACING + OVERLAY_UI_CHECKBOX_HEIGHT + OVERLAY_UI_SECTION_SPACING;
}

static inline int overlay_get_gpu_selector_block_height(void)
{
  return OVERLAY_UI_LABEL_HEIGHT + OVERLAY_UI_ITEM_SPACING + OVERLAY_UI_GPU_CARD_HEIGHT + OVERLAY_UI_SECTION_SPACING;
}

static inline int overlay_get_quality_selector_block_height(void)
{
  return OVERLAY_UI_LABEL_HEIGHT + OVERLAY_UI_ITEM_SPACING + OVERLAY_UI_QUALITY_CARD_HEIGHT + OVERLAY_UI_SECTION_SPACING;
}

static inline int overlay_get_scroll_view_top(void)
{
  return OVERLAY_UI_MARGIN + OVERLAY_UI_TITLE_HEIGHT + OVERLAY_UI_ITEM_SPACING + OVERLAY_UI_HINT_HEIGHT + OVERLAY_UI_SECTION_SPACING;
}

static inline int overlay_get_scroll_content_height(void)
{
  int content_height = 0;
  int index = 0;

  for (index = 0; index < OVERLAY_SLIDER_COUNT; ++index)
  {
    if (overlay_has_gameplay_toggle_before_slider((OverlaySliderId)index))
    {
      content_height += overlay_get_gameplay_toggle_block_height();
    }

    if (overlay_has_cloud_toggle_before_slider((OverlaySliderId)index))
    {
      content_height += overlay_get_cloud_toggle_block_height();
    }

    if (overlay_has_quality_selector_before_slider((OverlaySliderId)index))
    {
      content_height += overlay_get_quality_selector_block_height();
    }

    if (overlay_has_gpu_selector_before_slider((OverlaySliderId)index))
    {
      content_height += overlay_get_gpu_selector_block_height();
    }

    if (overlay_has_metric_card_before_slider((OverlaySliderId)index))
    {
      content_height += OVERLAY_UI_METRIC_HEIGHT + OVERLAY_UI_SECTION_SPACING;
    }

    content_height += OVERLAY_UI_LABEL_HEIGHT + OVERLAY_UI_ITEM_SPACING + OVERLAY_UI_SLIDER_HEIGHT + OVERLAY_UI_SECTION_SPACING;
  }

  return content_height;
}

static inline float overlay_get_scroll_max_for_window(int window_height)
{
  const int scroll_view_height = window_height - overlay_get_scroll_view_top() - OVERLAY_UI_MARGIN;
  const int overflow = overlay_get_scroll_content_height() - ((scroll_view_height > 0) ? scroll_view_height : 0);
  return (overflow > 0) ? (float)overflow : 0.0f;
}

static inline int overlay_get_panel_width_for_window(int window_width)
{
  int overlay_width = OVERLAY_UI_DEFAULT_WIDTH;

  if (window_width <= 0)
  {
    return overlay_width;
  }

  if (window_width - overlay_width < OVERLAY_UI_WORLD_MIN_WIDTH)
  {
    overlay_width = window_width - OVERLAY_UI_WORLD_MIN_WIDTH;
  }
  if (overlay_width < OVERLAY_UI_MIN_WIDTH)
  {
    overlay_width = OVERLAY_UI_MIN_WIDTH;
  }

  return overlay_width;
}

static inline int overlay_get_debug_console_rect(
  int window_width,
  int window_height,
  int overlay_visible_width,
  int* out_left,
  int* out_top,
  int* out_right,
  int* out_bottom)
{
  const int left = overlay_visible_width + OVERLAY_DEBUG_CONSOLE_MARGIN;
  const int right_limit = window_width - OVERLAY_DEBUG_CONSOLE_RESERVED_RIGHT;
  int panel_width = right_limit - left;
  const int panel_height = overlay_clamp_int(window_height / 3, OVERLAY_DEBUG_CONSOLE_MIN_HEIGHT, OVERLAY_DEBUG_CONSOLE_MAX_HEIGHT);
  const int bottom = window_height - OVERLAY_DEBUG_CONSOLE_MARGIN;
  const int top = bottom - panel_height;

  if (window_width <= 0 || window_height <= 0 || right_limit <= left)
  {
    return 0;
  }

  if (panel_width > OVERLAY_DEBUG_CONSOLE_MAX_WIDTH)
  {
    panel_width = OVERLAY_DEBUG_CONSOLE_MAX_WIDTH;
  }
  if (panel_width < OVERLAY_DEBUG_CONSOLE_MIN_WIDTH)
  {
    return 0;
  }

  if (out_left != NULL)
  {
    *out_left = left;
  }
  if (out_top != NULL)
  {
    *out_top = top;
  }
  if (out_right != NULL)
  {
    *out_right = left + panel_width;
  }
  if (out_bottom != NULL)
  {
    *out_bottom = bottom;
  }
  return 1;
}

static inline int overlay_get_debug_console_log_view_rect(
  int window_width,
  int window_height,
  int overlay_visible_width,
  int* out_left,
  int* out_top,
  int* out_right,
  int* out_bottom)
{
  int panel_left = 0;
  int panel_top = 0;
  int panel_right = 0;
  int panel_bottom = 0;
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  if (!overlay_get_debug_console_rect(
    window_width,
    window_height,
    overlay_visible_width,
    &panel_left,
    &panel_top,
    &panel_right,
      &panel_bottom))
  {
    return 0;
  }

  left = panel_left + OVERLAY_DEBUG_CONSOLE_PADDING_X;
  top =
    panel_top +
    OVERLAY_DEBUG_CONSOLE_HEADER_HEIGHT +
    OVERLAY_DEBUG_CONSOLE_PADDING_Y +
    OVERLAY_DEBUG_CONSOLE_PINNED_ROW_COUNT * OVERLAY_DEBUG_CONSOLE_ROW_HEIGHT +
    OVERLAY_DEBUG_CONSOLE_SECTION_GAP;
  right = panel_right - OVERLAY_DEBUG_CONSOLE_PADDING_X - OVERLAY_DEBUG_CONSOLE_SCROLLBAR_WIDTH - 6;
  bottom = panel_bottom - OVERLAY_DEBUG_CONSOLE_PADDING_Y;

  if (right <= left || bottom <= top)
  {
    return 0;
  }

  if (out_left != NULL)
  {
    *out_left = left;
  }
  if (out_top != NULL)
  {
    *out_top = top;
  }
  if (out_right != NULL)
  {
    *out_right = right;
  }
  if (out_bottom != NULL)
  {
    *out_bottom = bottom;
  }
  return 1;
}

static inline float overlay_get_debug_console_log_scroll_max(
  int window_width,
  int window_height,
  int overlay_visible_width,
  int log_row_count)
{
  int view_top = 0;
  int view_bottom = 0;
  int visible_row_capacity = 0;
  int overflow_row_count = 0;

  if (log_row_count <= 0 ||
    !overlay_get_debug_console_log_view_rect(
      window_width,
      window_height,
      overlay_visible_width,
      NULL,
      &view_top,
      NULL,
      &view_bottom))
  {
    return 0.0f;
  }

  visible_row_capacity = (view_bottom - view_top) / OVERLAY_DEBUG_CONSOLE_ROW_HEIGHT;
  overflow_row_count = log_row_count - visible_row_capacity;
  return (overflow_row_count > 0) ? (float)(overflow_row_count * OVERLAY_DEBUG_CONSOLE_ROW_HEIGHT) : 0.0f;
}

static inline int overlay_get_gpu_selector_rect(
  int panel_width,
  float scroll_offset,
  int* out_left,
  int* out_top,
  int* out_right,
  int* out_bottom)
{
  const int y =
    overlay_get_scroll_view_top() -
    (int)scroll_offset +
    overlay_get_gameplay_toggle_block_height() +
    overlay_get_quality_selector_block_height();
  const int left = OVERLAY_UI_MARGIN;
  const int top = y + OVERLAY_UI_LABEL_HEIGHT + OVERLAY_UI_ITEM_SPACING;
  const int right = panel_width - OVERLAY_UI_MARGIN;
  const int bottom = top + OVERLAY_UI_GPU_CARD_HEIGHT;

  if (out_left != NULL)
  {
    *out_left = left;
  }
  if (out_top != NULL)
  {
    *out_top = top;
  }
  if (out_right != NULL)
  {
    *out_right = right;
  }
  if (out_bottom != NULL)
  {
    *out_bottom = bottom;
  }

  return 1;
}

static inline int overlay_get_quality_selector_rect(
  int panel_width,
  float scroll_offset,
  int* out_left,
  int* out_top,
  int* out_right,
  int* out_bottom)
{
  const int y = overlay_get_scroll_view_top() - (int)scroll_offset + overlay_get_gameplay_toggle_block_height();
  const int left = OVERLAY_UI_MARGIN;
  const int top = y + OVERLAY_UI_LABEL_HEIGHT + OVERLAY_UI_ITEM_SPACING;
  const int right = panel_width - OVERLAY_UI_MARGIN;
  const int bottom = top + OVERLAY_UI_QUALITY_CARD_HEIGHT;

  if (out_left != NULL)
  {
    *out_left = left;
  }
  if (out_top != NULL)
  {
    *out_top = top;
  }
  if (out_right != NULL)
  {
    *out_right = right;
  }
  if (out_bottom != NULL)
  {
    *out_bottom = bottom;
  }

  return 1;
}

static inline int overlay_get_render_quality_button_rect(
  int panel_width,
  float scroll_offset,
  RendererQualityPreset preset,
  int* out_left,
  int* out_top,
  int* out_right,
  int* out_bottom)
{
  int selector_left = 0;
  int selector_top = 0;
  int selector_right = 0;
  int selector_bottom = 0;
  const int inner_left_padding = OVERLAY_UI_QUALITY_CARD_PADDING;
  const int button_gap = OVERLAY_UI_QUALITY_BUTTON_GAP;
  int left = 0;
  int top = 0;

  (void)selector_bottom;
  if (preset < RENDER_QUALITY_PRESET_HIGH || preset >= RENDER_QUALITY_PRESET_COUNT)
  {
    return 0;
  }

  (void)overlay_get_quality_selector_rect(panel_width, scroll_offset, &selector_left, &selector_top, &selector_right, &selector_bottom);
  left = selector_left + inner_left_padding;
  top = selector_top + OVERLAY_UI_QUALITY_CARD_PADDING;
  if (selector_right - selector_left <= inner_left_padding * 2)
  {
    return 0;
  }

  {
    const int available_width = (selector_right - selector_left) - inner_left_padding * 2 - button_gap * 2;
    const int computed_button_width = (available_width > 0) ? (available_width / 3) : 0;
    const int preset_index = (int)preset;

    if (computed_button_width <= 0)
    {
      return 0;
    }

    left += preset_index * (computed_button_width + button_gap);
    if (out_left != NULL)
    {
      *out_left = left;
    }
    if (out_top != NULL)
    {
      *out_top = top;
    }
    if (out_right != NULL)
    {
      *out_right = left + computed_button_width;
    }
    if (out_bottom != NULL)
    {
      *out_bottom = top + OVERLAY_UI_QUALITY_BUTTON_HEIGHT;
    }
  }

  return 1;
}

static inline int overlay_get_gpu_preference_button_rect(
  int panel_width,
  float scroll_offset,
  GpuPreferenceMode mode,
  int* out_left,
  int* out_top,
  int* out_right,
  int* out_bottom)
{
  int selector_left = 0;
  int selector_top = 0;
  int selector_right = 0;
  int selector_bottom = 0;
  const int inner_left_padding = OVERLAY_UI_GPU_CARD_PADDING;
  const int button_gap = OVERLAY_UI_GPU_BUTTON_GAP;
  int left = 0;
  int top = 0;

  (void)selector_bottom;
  if (mode < GPU_PREFERENCE_MODE_AUTO || mode >= GPU_PREFERENCE_MODE_COUNT)
  {
    return 0;
  }

  (void)overlay_get_gpu_selector_rect(panel_width, scroll_offset, &selector_left, &selector_top, &selector_right, &selector_bottom);
  left = selector_left + inner_left_padding;
  top = selector_top + OVERLAY_UI_GPU_CARD_PADDING;
  if (selector_right - selector_left <= inner_left_padding * 2)
  {
    return 0;
  }

  {
    const int available_width = (selector_right - selector_left) - inner_left_padding * 2 - button_gap * 2;
    const int computed_button_width = (available_width > 0) ? (available_width / 3) : 0;
    const int mode_index = (int)mode;

    if (computed_button_width <= 0)
    {
      return 0;
    }

    left += mode_index * (computed_button_width + button_gap);
    if (out_left != NULL)
    {
      *out_left = left;
    }
    if (out_top != NULL)
    {
      *out_top = top;
    }
    if (out_right != NULL)
    {
      *out_right = left + computed_button_width;
    }
    if (out_bottom != NULL)
    {
      *out_bottom = top + OVERLAY_UI_GPU_BUTTON_HEIGHT;
    }
  }

  return 1;
}

static inline void overlay_get_slider_range(OverlaySliderId slider_id, float* out_min_value, float* out_max_value)
{
  float min_value = 0.0f;
  float max_value = 1.0f;

  switch (slider_id)
  {
    case OVERLAY_SLIDER_SUN_DISTANCE:
      min_value = 60.0f;
      max_value = 280.0f;
      break;

    case OVERLAY_SLIDER_SUN_ORBIT:
      min_value = 0.0f;
      max_value = 359.9f;
      break;

    case OVERLAY_SLIDER_CYCLE_DURATION:
      min_value = 60.0f;
      max_value = 900.0f;
      break;

    case OVERLAY_SLIDER_DAYLIGHT_FRACTION:
      min_value = 0.30f;
      max_value = 0.75f;
      break;

    case OVERLAY_SLIDER_CAMERA_FOV:
      min_value = 40.0f;
      max_value = 100.0f;
      break;

    case OVERLAY_SLIDER_FOG_DENSITY:
      min_value = 0.00f;
      max_value = 1.00f;
      break;

    case OVERLAY_SLIDER_CLOUD_AMOUNT:
      min_value = 0.10f;
      max_value = 1.00f;
      break;

    case OVERLAY_SLIDER_CLOUD_SPACING:
      min_value = 0.45f;
      max_value = 2.20f;
      break;

    case OVERLAY_SLIDER_TERRAIN_BASE:
      min_value = -40.0f;
      max_value = 20.0f;
      break;

    case OVERLAY_SLIDER_TERRAIN_HEIGHT:
      min_value = 0.35f;
      max_value = 3.00f;
      break;

    case OVERLAY_SLIDER_TERRAIN_ROUGHNESS:
      min_value = 0.55f;
      max_value = 2.50f;
      break;

    case OVERLAY_SLIDER_TERRAIN_RIDGE:
      min_value = 0.00f;
      max_value = 4.00f;
      break;

    case OVERLAY_SLIDER_PALM_SIZE:
      min_value = 0.55f;
      max_value = 2.40f;
      break;

    case OVERLAY_SLIDER_PALM_COUNT:
      min_value = 0.0f;
      max_value = 1200.0f;
      break;

    case OVERLAY_SLIDER_PALM_FRUIT_DENSITY:
      min_value = 0.0f;
      max_value = 1.0f;
      break;

    case OVERLAY_SLIDER_PALM_RENDER_RADIUS:
      min_value = 120.0f;
      max_value = 520.0f;
      break;

    case OVERLAY_SLIDER_NONE:
    case OVERLAY_SLIDER_COUNT:
    default:
      break;
  }

  if (out_min_value != NULL)
  {
    *out_min_value = min_value;
  }
  if (out_max_value != NULL)
  {
    *out_max_value = max_value;
  }
}

static inline const char* overlay_get_slider_title(OverlaySliderId slider_id)
{
  switch (slider_id)
  {
    case OVERLAY_SLIDER_SUN_DISTANCE:
      return "Sun distance";
    case OVERLAY_SLIDER_SUN_ORBIT:
      return "Sun orbit";
    case OVERLAY_SLIDER_CYCLE_DURATION:
      return "Full cycle";
    case OVERLAY_SLIDER_DAYLIGHT_FRACTION:
      return "Sunrise -> sunset";
    case OVERLAY_SLIDER_CAMERA_FOV:
      return "Camera FOV";
    case OVERLAY_SLIDER_FOG_DENSITY:
      return "Fog density";
    case OVERLAY_SLIDER_CLOUD_AMOUNT:
      return "Cloud amount";
    case OVERLAY_SLIDER_CLOUD_SPACING:
      return "Cloud spacing";
    case OVERLAY_SLIDER_TERRAIN_BASE:
      return "Terrain base";
    case OVERLAY_SLIDER_TERRAIN_HEIGHT:
      return "Terrain relief";
    case OVERLAY_SLIDER_TERRAIN_ROUGHNESS:
      return "Terrain roughness";
    case OVERLAY_SLIDER_TERRAIN_RIDGE:
      return "Terrain ridges";
    case OVERLAY_SLIDER_PALM_SIZE:
      return "Palm size";
    case OVERLAY_SLIDER_PALM_COUNT:
      return "Palm amount";
    case OVERLAY_SLIDER_PALM_FRUIT_DENSITY:
      return "Palm fruit";
    case OVERLAY_SLIDER_PALM_RENDER_RADIUS:
      return "Palm distance";
    case OVERLAY_SLIDER_NONE:
    case OVERLAY_SLIDER_COUNT:
    default:
      return "";
  }
}

static inline const char* overlay_get_toggle_title(OverlayToggleId toggle_id)
{
  switch (toggle_id)
  {
    case OVERLAY_TOGGLE_GOD_MODE:
      return "God mode (no physics)";
    case OVERLAY_TOGGLE_FREEZE_TIME:
      return "Freeze time and clouds";
    case OVERLAY_TOGGLE_SOUND:
      return "Game music and effects";
    case OVERLAY_TOGGLE_CLOUDS:
      return "Raymarched clouds";
    case OVERLAY_TOGGLE_NONE:
    case OVERLAY_TOGGLE_COUNT:
    default:
      return "";
  }
}

#endif
