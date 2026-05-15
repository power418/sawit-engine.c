#include "platform_cocoa.h"

#include "diagnostics.h"
#include "gl_headers.h"

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>

#include <math.h>
#include <string.h>

enum
{
  PLATFORM_KEYCODE_A = 0,
  PLATFORM_KEYCODE_S = 1,
  PLATFORM_KEYCODE_D = 2,
  PLATFORM_KEYCODE_G = 5,
  PLATFORM_KEYCODE_Q = 12,
  PLATFORM_KEYCODE_W = 13,
  PLATFORM_KEYCODE_R = 15,
  PLATFORM_KEYCODE_1 = 18,
  PLATFORM_KEYCODE_2 = 19,
  PLATFORM_KEYCODE_3 = 20,
  PLATFORM_KEYCODE_4 = 21,
  PLATFORM_KEYCODE_P = 35,
  PLATFORM_KEYCODE_SPACE = 49,
  PLATFORM_KEYCODE_ESCAPE = 53,
  PLATFORM_KEYCODE_LEFT_COMMAND = 55,
  PLATFORM_KEYCODE_LEFT_SHIFT = 56,
  PLATFORM_KEYCODE_LEFT_OPTION = 58,
  PLATFORM_KEYCODE_LEFT_CONTROL = 59,
  PLATFORM_KEYCODE_RIGHT_SHIFT = 60,
  PLATFORM_KEYCODE_RIGHT_OPTION = 61,
  PLATFORM_KEYCODE_RIGHT_CONTROL = 62,
  PLATFORM_KEYCODE_F11 = 103,
  PLATFORM_KEYCODE_LEFT_ARROW = 123,
  PLATFORM_KEYCODE_RIGHT_ARROW = 124,
  PLATFORM_KEYCODE_DOWN_ARROW = 125,
  PLATFORM_KEYCODE_UP_ARROW = 126,
  PLATFORM_SETTINGS_TOGGLE_TAG_BASE = 2000,
  PLATFORM_SETTINGS_SLIDER_TAG_BASE = 3000,
  PLATFORM_SETTINGS_SLIDER_VALUE_TAG_BASE = 4000,
  PLATFORM_SETTINGS_GPU_SEGMENTED_TAG = 5000,
  PLATFORM_SETTINGS_GPU_STATUS_TAG = 5001,
  PLATFORM_SETTINGS_TITLE_TAG = 5002,
  PLATFORM_SETTINGS_HINT_TAG = 5003,
  PLATFORM_SETTINGS_QUALITY_SEGMENTED_TAG = 5004,
  PLATFORM_SETTINGS_QUALITY_STATUS_TAG = 5005
};

@interface PlatformOpenGLView : NSOpenGLView
@end

@implementation PlatformOpenGLView

- (BOOL)acceptsFirstResponder
{
  return YES;
}

- (BOOL)isOpaque
{
  return YES;
}

@end

@interface PlatformWindowDelegate : NSObject <NSWindowDelegate>
{
@public
  PlatformApp* app;
}
@end

@interface PlatformSettingsActionTarget : NSObject
{
@public
  PlatformApp* app;
}
- (void)handleSliderChanged:(id)sender;
- (void)handleToggleChanged:(id)sender;
- (void)handleGpuModeChanged:(id)sender;
- (void)handleRenderQualityChanged:(id)sender;
@end

static void platform_set_slider_setting_value(SceneSettings* settings, OverlaySliderId slider_id, float value);
static void platform_toggle_value(PlatformApp* app, OverlayToggleId toggle_id);
static void platform_sync_native_settings_controls(PlatformApp* app);
static const char* platform_get_render_quality_status_text(RendererQualityPreset preset);
static void platform_update_native_overlays(PlatformApp* app);

@implementation PlatformWindowDelegate

- (BOOL)windowShouldClose:(id)sender
{
  (void)sender;
  if (app != NULL)
  {
    app->running = 0;
  }
  return YES;
}

@end

@implementation PlatformSettingsActionTarget

- (void)handleSliderChanged:(id)sender
{
  NSSlider* slider = (NSSlider*)sender;
  OverlaySliderId slider_id = OVERLAY_SLIDER_NONE;

  if (app == NULL || slider == nil)
  {
    return;
  }

  slider_id = (OverlaySliderId)([slider tag] - PLATFORM_SETTINGS_SLIDER_TAG_BASE);
  if (slider_id <= OVERLAY_SLIDER_NONE || slider_id >= OVERLAY_SLIDER_COUNT)
  {
    return;
  }

  platform_set_slider_setting_value(&app->overlay.settings, slider_id, (float)[slider doubleValue]);
  if (slider_id == OVERLAY_SLIDER_SUN_ORBIT)
  {
    app->overlay.freeze_time_enabled = 1;
  }
  platform_sync_native_settings_controls(app);
}

- (void)handleToggleChanged:(id)sender
{
  NSButton* button = (NSButton*)sender;
  OverlayToggleId toggle_id = OVERLAY_TOGGLE_NONE;

  if (app == NULL || button == nil)
  {
    return;
  }

  toggle_id = (OverlayToggleId)([button tag] - PLATFORM_SETTINGS_TOGGLE_TAG_BASE);
  platform_toggle_value(app, toggle_id);
  platform_sync_native_settings_controls(app);
}

- (void)handleGpuModeChanged:(id)sender
{
  NSSegmentedControl* segmented = (NSSegmentedControl*)sender;
  NSInteger selected_segment = 0;

  if (app == NULL || segmented == nil)
  {
    return;
  }

  selected_segment = [segmented selectedSegment];
  if (selected_segment < GPU_PREFERENCE_MODE_AUTO || selected_segment >= GPU_PREFERENCE_MODE_COUNT)
  {
    return;
  }

  app->gpu_switch_requested = 1;
  app->requested_gpu_preference = (GpuPreferenceMode)selected_segment;
  app->overlay.gpu_info.selected_mode = app->requested_gpu_preference;
  platform_sync_native_settings_controls(app);
}

- (void)handleRenderQualityChanged:(id)sender
{
  NSSegmentedControl* segmented = (NSSegmentedControl*)sender;
  NSInteger selected_segment = 0;

  if (app == NULL || segmented == nil)
  {
    return;
  }

  selected_segment = [segmented selectedSegment];
  if (selected_segment < RENDER_QUALITY_PRESET_HIGH || selected_segment >= RENDER_QUALITY_PRESET_COUNT)
  {
    return;
  }

  app->render_quality_change_requested = 1;
  app->requested_render_quality_preset = (RendererQualityPreset)selected_segment;
  app->overlay.render_quality_preset = app->requested_render_quality_preset;
  platform_sync_native_settings_controls(app);
  platform_update_native_overlays(app);
}

@end

static int platform_window_has_focus(const PlatformApp* app);
static int platform_is_key_down(const PlatformApp* app, unsigned short key_code);
static void platform_sync_modifier_keys(PlatformApp* app, NSEventModifierFlags modifier_flags);
static void platform_set_mouse_capture(PlatformApp* app, int enabled);
static void platform_toggle_fullscreen(PlatformApp* app);
static void platform_update_dimensions(PlatformApp* app);
static NSTextView* platform_create_overlay_text_view(NSFont* font, NSColor* text_color, NSColor* background_color);
static NSTextField* platform_create_overlay_label(NSString* text, NSFont* font, NSColor* text_color, NSTextAlignment alignment);
static NSFont* platform_font_named_or(NSString* name, CGFloat size, NSFont* fallback);
static NSView* platform_create_settings_overlay(PlatformApp* app);
static void platform_layout_native_overlays(PlatformApp* app);
static void platform_update_native_overlays(PlatformApp* app);
static void platform_sync_native_overlay_visibility(PlatformApp* app);
static void platform_sync_native_settings_controls(PlatformApp* app);
static float platform_get_slider_setting_value(const SceneSettings* settings, OverlaySliderId slider_id);
static void platform_set_slider_setting_value(SceneSettings* settings, OverlaySliderId slider_id, float value);
static void platform_format_slider_setting_value(char* buffer, size_t buffer_size, OverlaySliderId slider_id, float value);
static void platform_toggle_value(PlatformApp* app, OverlayToggleId toggle_id);
static int platform_point_in_view(NSView* view, NSPoint point_in_window);
static int platform_try_enter_world_from_mouse_down(PlatformApp* app, NSEvent* event);

static int platform_window_has_focus(const PlatformApp* app)
{
  NSWindow* window = (NSWindow*)app->window;
  return app != NULL && window != nil && [NSApp isActive] && [window isKeyWindow];
}

static int platform_is_key_down(const PlatformApp* app, unsigned short key_code)
{
  return app != NULL && key_code < sizeof(app->key_down) && app->key_down[key_code] != 0;
}

static void platform_sync_modifier_keys(PlatformApp* app, NSEventModifierFlags modifier_flags)
{
  if (app == NULL)
  {
    return;
  }

  app->key_down[PLATFORM_KEYCODE_LEFT_SHIFT] = (modifier_flags & NSEventModifierFlagShift) ? 1U : 0U;
  app->key_down[PLATFORM_KEYCODE_RIGHT_SHIFT] = app->key_down[PLATFORM_KEYCODE_LEFT_SHIFT];
  app->key_down[PLATFORM_KEYCODE_LEFT_CONTROL] = (modifier_flags & NSEventModifierFlagControl) ? 1U : 0U;
  app->key_down[PLATFORM_KEYCODE_RIGHT_CONTROL] = app->key_down[PLATFORM_KEYCODE_LEFT_CONTROL];
  app->key_down[PLATFORM_KEYCODE_LEFT_OPTION] = (modifier_flags & NSEventModifierFlagOption) ? 1U : 0U;
  app->key_down[PLATFORM_KEYCODE_RIGHT_OPTION] = app->key_down[PLATFORM_KEYCODE_LEFT_OPTION];
  app->key_down[PLATFORM_KEYCODE_LEFT_COMMAND] = (modifier_flags & NSEventModifierFlagCommand) ? 1U : 0U;
}

static void platform_set_mouse_capture(PlatformApp* app, int enabled)
{
  if (app == NULL)
  {
    return;
  }

  if (enabled != 0)
  {
    if (app->mouse_captured == 0)
    {
      app->mouse_dx = 0;
      app->mouse_dy = 0;
      app->suppress_next_mouse_delta = 1;
      (void)CGAssociateMouseAndMouseCursorPosition(false);
      if (app->cursor_hidden == 0)
      {
        [NSCursor hide];
        app->cursor_hidden = 1;
      }
      app->mouse_captured = 1;
    }
    return;
  }

  if (app->mouse_captured != 0)
  {
    (void)CGAssociateMouseAndMouseCursorPosition(true);
    if (app->cursor_hidden != 0)
    {
      [NSCursor unhide];
      app->cursor_hidden = 0;
    }
    app->mouse_captured = 0;
  }
}

static void platform_toggle_fullscreen(PlatformApp* app)
{
  NSWindow* window = (NSWindow*)app->window;

  if (window == nil)
  {
    return;
  }

  [window toggleFullScreen:nil];
  app->resized = 1;
}

static void platform_update_dimensions(PlatformApp* app)
{
  NSView* view = (NSView*)app->view;

  if (app == NULL || view == nil)
  {
    return;
  }

  {
    NSRect backing_bounds = [view bounds];
    int new_width = 0;
    int new_height = 0;

    if ([view respondsToSelector:@selector(convertRectToBacking:)])
    {
      backing_bounds = [view convertRectToBacking:backing_bounds];
    }

    new_width = (int)lround(backing_bounds.size.width);
    new_height = (int)lround(backing_bounds.size.height);
    if (new_width < 0)
    {
      new_width = 0;
    }
    if (new_height < 0)
    {
      new_height = 0;
    }

    if (new_width != app->width || new_height != app->height)
    {
      app->width = new_width;
      app->height = new_height;
      app->resized = 1;
      if (app->gl_context != NULL)
      {
        [(NSOpenGLContext*)app->gl_context update];
      }
    }
  }

  platform_layout_native_overlays(app);
}

static NSTextView* platform_create_overlay_text_view(NSFont* font, NSColor* text_color, NSColor* background_color)
{
  NSTextView* text_view = [[NSTextView alloc] initWithFrame:NSZeroRect];

  if (text_view == nil)
  {
    return nil;
  }

  [text_view setEditable:NO];
  [text_view setSelectable:NO];
  [text_view setRichText:NO];
  [text_view setImportsGraphics:NO];
  [text_view setAutomaticQuoteSubstitutionEnabled:NO];
  [text_view setAutomaticDashSubstitutionEnabled:NO];
  [text_view setAutomaticDataDetectionEnabled:NO];
  [text_view setDrawsBackground:YES];
  [text_view setBackgroundColor:background_color];
  [text_view setTextColor:text_color];
  [text_view setFont:font];
  [text_view setVerticallyResizable:NO];
  [text_view setHorizontallyResizable:NO];
  [text_view setTextContainerInset:NSMakeSize(12.0, 10.0)];
  [[text_view textContainer] setLineFragmentPadding:0.0];
  [[text_view textContainer] setWidthTracksTextView:YES];
  [[text_view textContainer] setHeightTracksTextView:YES];
  if ([text_view respondsToSelector:@selector(setUsesAdaptiveColorMappingForDarkAppearance:)])
  {
    [text_view setUsesAdaptiveColorMappingForDarkAppearance:NO];
  }

  return text_view;
}

static NSTextField* platform_create_overlay_label(NSString* text, NSFont* font, NSColor* text_color, NSTextAlignment alignment)
{
  NSTextField* label = [NSTextField labelWithString:(text != nil) ? text : @""];

  if (label == nil)
  {
    return nil;
  }

  [label setFont:font];
  [label setTextColor:text_color];
  [label setAlignment:alignment];
  [label setLineBreakMode:NSLineBreakByTruncatingTail];
  [label setEditable:NO];
  [label setSelectable:NO];
  [label setBezeled:NO];
  [label setBordered:NO];
  [label setDrawsBackground:NO];
  return label;
}

static NSFont* platform_font_named_or(NSString* name, CGFloat size, NSFont* fallback)
{
  NSFont* font = [NSFont fontWithName:name size:size];
  return (font != nil) ? font : fallback;
}

static const char* platform_get_render_quality_status_text(RendererQualityPreset preset)
{
  switch (preset)
  {
    case RENDER_QUALITY_PRESET_HIGH:
      return "High: maksimum visual dan semua efek berat aktif.";
    case RENDER_QUALITY_PRESET_LOW:
      return "Low: balanced untuk iGPU kelas Intel Iris Xe, objek tetap jelas.";
    case RENDER_QUALITY_PRESET_ULTRA_LOW:
      return "Ultra Low: untuk iGPU lama seperti Intel UHD 617.";
    case RENDER_QUALITY_PRESET_COUNT:
    default:
      return "High: maksimum visual dan semua efek berat aktif.";
  }
}

static float platform_get_slider_setting_value(const SceneSettings* settings, OverlaySliderId slider_id)
{
  if (settings == NULL)
  {
    return 0.0f;
  }

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

static void platform_set_slider_setting_value(SceneSettings* settings, OverlaySliderId slider_id, float value)
{
  if (settings == NULL)
  {
    return;
  }

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

static void platform_format_slider_setting_value(char* buffer, size_t buffer_size, OverlaySliderId slider_id, float value)
{
  if (buffer == NULL || buffer_size == 0U)
  {
    return;
  }

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

static NSView* platform_create_settings_overlay(PlatformApp* app)
{
  static const OverlayToggleId k_toggle_ids[] = {
    OVERLAY_TOGGLE_GOD_MODE,
    OVERLAY_TOGGLE_FREEZE_TIME,
    OVERLAY_TOGGLE_SOUND,
    OVERLAY_TOGGLE_CLOUDS
  };
  static const OverlaySliderId k_slider_ids[] = {
    OVERLAY_SLIDER_SUN_DISTANCE,
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
    OVERLAY_SLIDER_PALM_RENDER_RADIUS
  };
  NSVisualEffectView* panel = nil;
  NSScrollView* scroll_view = nil;
  NSView* form_view = nil;
  NSTextField* title_label = nil;
  NSTextField* hint_label = nil;
  NSTextField* quality_status_label = nil;
  NSTextField* gpu_status_label = nil;
  NSSegmentedControl* quality_segmented = nil;
  NSSegmentedControl* gpu_segmented = nil;
  PlatformSettingsActionTarget* target = nil;
  CGFloat content_width = 312.0;
  CGFloat content_height = 0.0;
  CGFloat y = 0.0;
  size_t index = 0U;

  if (app == NULL)
  {
    return nil;
  }

  panel = [[NSVisualEffectView alloc] initWithFrame:NSZeroRect];
  if (panel == nil)
  {
    return nil;
  }

  [panel setMaterial:NSVisualEffectMaterialHUDWindow];
  [panel setBlendingMode:NSVisualEffectBlendingModeWithinWindow];
  [panel setState:NSVisualEffectStateActive];
  [panel setWantsLayer:YES];
  [[panel layer] setCornerRadius:16.0];
  [[panel layer] setMasksToBounds:YES];
  [panel setHidden:YES];

  title_label = platform_create_overlay_label(
    @"Settings",
    platform_font_named_or(@"Menlo Bold", 15.0, [NSFont boldSystemFontOfSize:15.0]),
    [NSColor colorWithCalibratedRed:0.96 green:0.97 blue:0.99 alpha:1.0],
    NSTextAlignmentLeft);
  [title_label setTag:PLATFORM_SETTINGS_TITLE_TAG];
  [panel addSubview:title_label];

  hint_label = platform_create_overlay_label(
    @"Alt untuk buka settings, klik scene untuk kembali ke world",
    platform_font_named_or(@"Menlo", 11.0, [NSFont systemFontOfSize:11.0]),
    [NSColor colorWithCalibratedRed:0.74 green:0.78 blue:0.84 alpha:0.96],
    NSTextAlignmentLeft);
  [hint_label setTag:PLATFORM_SETTINGS_HINT_TAG];
  [hint_label setLineBreakMode:NSLineBreakByWordWrapping];
  [hint_label setUsesSingleLineMode:NO];
  [panel addSubview:hint_label];

  scroll_view = [[NSScrollView alloc] initWithFrame:NSZeroRect];
  [scroll_view setHasVerticalScroller:YES];
  [scroll_view setBorderType:NSNoBorder];
  [scroll_view setDrawsBackground:NO];
  [scroll_view setAutohidesScrollers:YES];
  [panel addSubview:scroll_view];

  target = [[PlatformSettingsActionTarget alloc] init];
  target->app = app;
  app->settings_action_target = target;

  content_height = 40.0f + 4.0f * 30.0f + 96.0f + 88.0f + (CGFloat)(sizeof(k_slider_ids) / sizeof(k_slider_ids[0])) * 60.0f + 36.0f;
  form_view = [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, content_width, content_height)];
  [form_view setAutoresizingMask:NSViewWidthSizable];
  [scroll_view setDocumentView:form_view];

  y = content_height - 24.0f;
  [form_view addSubview:platform_create_overlay_label(
    @"Gameplay",
    platform_font_named_or(@"Menlo Bold", 12.0, [NSFont boldSystemFontOfSize:12.0]),
    [NSColor colorWithCalibratedRed:0.96 green:0.86 blue:0.52 alpha:0.98],
    NSTextAlignmentLeft)];
  [[[form_view subviews] lastObject] setFrame:NSMakeRect(0.0, y, content_width, 18.0)];
  y -= 28.0f;

  for (index = 0U; index < sizeof(k_toggle_ids) / sizeof(k_toggle_ids[0]); ++index)
  {
    NSButton* toggle = [[NSButton alloc] initWithFrame:NSMakeRect(0.0, y, content_width, 22.0)];
    [toggle setButtonType:NSButtonTypeSwitch];
    [toggle setTitle:[NSString stringWithUTF8String:overlay_get_toggle_title(k_toggle_ids[index])]];
    [toggle setTarget:target];
    [toggle setAction:@selector(handleToggleChanged:)];
    [toggle setTag:PLATFORM_SETTINGS_TOGGLE_TAG_BASE + (NSInteger)k_toggle_ids[index]];
    [toggle setFont:[NSFont systemFontOfSize:12.0]];
    [form_view addSubview:toggle];
    y -= 28.0f;
  }

  y -= 8.0f;
  [form_view addSubview:platform_create_overlay_label(
    @"Render Quality",
    platform_font_named_or(@"Menlo Bold", 12.0, [NSFont boldSystemFontOfSize:12.0]),
    [NSColor colorWithCalibratedRed:0.96 green:0.90 blue:0.72 alpha:0.98],
    NSTextAlignmentLeft)];
  [[[form_view subviews] lastObject] setFrame:NSMakeRect(0.0, y, content_width, 18.0)];
  y -= 30.0f;

  quality_segmented = [[NSSegmentedControl alloc] initWithFrame:NSMakeRect(0.0, y, content_width, 28.0)];
  [quality_segmented setSegmentCount:3];
  [quality_segmented setLabel:[NSString stringWithUTF8String:render_quality_preset_get_label(RENDER_QUALITY_PRESET_HIGH)] forSegment:RENDER_QUALITY_PRESET_HIGH];
  [quality_segmented setLabel:[NSString stringWithUTF8String:render_quality_preset_get_label(RENDER_QUALITY_PRESET_LOW)] forSegment:RENDER_QUALITY_PRESET_LOW];
  [quality_segmented setLabel:[NSString stringWithUTF8String:render_quality_preset_get_label(RENDER_QUALITY_PRESET_ULTRA_LOW)] forSegment:RENDER_QUALITY_PRESET_ULTRA_LOW];
  [quality_segmented setTag:PLATFORM_SETTINGS_QUALITY_SEGMENTED_TAG];
  [quality_segmented setTarget:target];
  [quality_segmented setAction:@selector(handleRenderQualityChanged:)];
  [form_view addSubview:quality_segmented];
  y -= 40.0f;

  quality_status_label = platform_create_overlay_label(
    @"",
    platform_font_named_or(@"Menlo", 10.0, [NSFont systemFontOfSize:10.0]),
    [NSColor colorWithCalibratedRed:0.82 green:0.84 blue:0.88 alpha:0.96],
    NSTextAlignmentLeft);
  [quality_status_label setTag:PLATFORM_SETTINGS_QUALITY_STATUS_TAG];
  [quality_status_label setLineBreakMode:NSLineBreakByWordWrapping];
  [quality_status_label setUsesSingleLineMode:NO];
  [quality_status_label setFrame:NSMakeRect(0.0, y, content_width, 34.0)];
  [form_view addSubview:quality_status_label];
  y -= 48.0f;

  y -= 4.0f;
  [form_view addSubview:platform_create_overlay_label(
    @"GPU Preference",
    platform_font_named_or(@"Menlo Bold", 12.0, [NSFont boldSystemFontOfSize:12.0]),
    [NSColor colorWithCalibratedRed:0.82 green:0.90 blue:0.98 alpha:0.98],
    NSTextAlignmentLeft)];
  [[[form_view subviews] lastObject] setFrame:NSMakeRect(0.0, y, content_width, 18.0)];
  y -= 30.0f;

  gpu_segmented = [[NSSegmentedControl alloc] initWithFrame:NSMakeRect(0.0, y, content_width, 28.0)];
  [gpu_segmented setSegmentCount:3];
  [gpu_segmented setLabel:[NSString stringWithUTF8String:gpu_preferences_get_mode_short_label(GPU_PREFERENCE_MODE_AUTO)] forSegment:0];
  [gpu_segmented setLabel:[NSString stringWithUTF8String:gpu_preferences_get_mode_short_label(GPU_PREFERENCE_MODE_MINIMUM_POWER)] forSegment:1];
  [gpu_segmented setLabel:[NSString stringWithUTF8String:gpu_preferences_get_mode_short_label(GPU_PREFERENCE_MODE_HIGH_PERFORMANCE)] forSegment:2];
  [gpu_segmented setTag:PLATFORM_SETTINGS_GPU_SEGMENTED_TAG];
  [gpu_segmented setTarget:target];
  [gpu_segmented setAction:@selector(handleGpuModeChanged:)];
  [form_view addSubview:gpu_segmented];
  y -= 40.0f;

  gpu_status_label = platform_create_overlay_label(
    @"",
    platform_font_named_or(@"Menlo", 10.0, [NSFont systemFontOfSize:10.0]),
    [NSColor colorWithCalibratedRed:0.72 green:0.76 blue:0.82 alpha:0.96],
    NSTextAlignmentLeft);
  [gpu_status_label setTag:PLATFORM_SETTINGS_GPU_STATUS_TAG];
  [gpu_status_label setLineBreakMode:NSLineBreakByWordWrapping];
  [gpu_status_label setUsesSingleLineMode:NO];
  [gpu_status_label setFrame:NSMakeRect(0.0, y, content_width, 34.0)];
  [form_view addSubview:gpu_status_label];
  y -= 48.0f;

  [form_view addSubview:platform_create_overlay_label(
    @"World Settings",
    platform_font_named_or(@"Menlo Bold", 12.0, [NSFont boldSystemFontOfSize:12.0]),
    [NSColor colorWithCalibratedRed:0.86 green:0.94 blue:0.98 alpha:0.98],
    NSTextAlignmentLeft)];
  [[[form_view subviews] lastObject] setFrame:NSMakeRect(0.0, y, content_width, 18.0)];
  y -= 30.0f;

  for (index = 0U; index < sizeof(k_slider_ids) / sizeof(k_slider_ids[0]); ++index)
  {
    NSTextField* slider_label = platform_create_overlay_label(
      [NSString stringWithUTF8String:overlay_get_slider_title(k_slider_ids[index])],
      [NSFont systemFontOfSize:12.0],
      [NSColor colorWithCalibratedRed:0.96 green:0.97 blue:0.99 alpha:0.98],
      NSTextAlignmentLeft);
    NSTextField* value_label = platform_create_overlay_label(
      @"",
      platform_font_named_or(@"Menlo", 11.0, [NSFont systemFontOfSize:11.0]),
      [NSColor colorWithCalibratedRed:0.98 green:0.84 blue:0.58 alpha:0.98],
      NSTextAlignmentRight);
    NSSlider* slider = [[NSSlider alloc] initWithFrame:NSMakeRect(0.0, y - 24.0, content_width, 24.0)];
    float min_value = 0.0f;
    float max_value = 1.0f;

    overlay_get_slider_range(k_slider_ids[index], &min_value, &max_value);
    [slider_label setFrame:NSMakeRect(0.0, y, content_width - 92.0, 18.0)];
    [value_label setFrame:NSMakeRect(content_width - 88.0, y, 88.0, 18.0)];
    [value_label setTag:PLATFORM_SETTINGS_SLIDER_VALUE_TAG_BASE + (NSInteger)k_slider_ids[index]];
    [slider setMinValue:min_value];
    [slider setMaxValue:max_value];
    [slider setContinuous:YES];
    [slider setTarget:target];
    [slider setAction:@selector(handleSliderChanged:)];
    [slider setTag:PLATFORM_SETTINGS_SLIDER_TAG_BASE + (NSInteger)k_slider_ids[index]];
    [slider setAutoresizingMask:NSViewWidthSizable];
    [slider_label setAutoresizingMask:NSViewWidthSizable];
    [value_label setAutoresizingMask:NSViewMinXMargin];
    [form_view addSubview:slider_label];
    [form_view addSubview:value_label];
    [form_view addSubview:slider];
    y -= 56.0f;
  }

  platform_sync_native_settings_controls(app);
  return panel;
}

static void platform_sync_native_settings_controls(PlatformApp* app)
{
  static const OverlayToggleId k_toggle_ids[] = {
    OVERLAY_TOGGLE_GOD_MODE,
    OVERLAY_TOGGLE_FREEZE_TIME,
    OVERLAY_TOGGLE_SOUND,
    OVERLAY_TOGGLE_CLOUDS
  };
  static const OverlaySliderId k_slider_ids[] = {
    OVERLAY_SLIDER_SUN_DISTANCE,
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
    OVERLAY_SLIDER_PALM_RENDER_RADIUS
  };
  NSView* root = (NSView*)app->settings_overlay_view;
  size_t index = 0U;

  if (app == NULL || root == nil)
  {
    return;
  }

  for (index = 0U; index < sizeof(k_toggle_ids) / sizeof(k_toggle_ids[0]); ++index)
  {
    NSButton* toggle = (NSButton*)[root viewWithTag:PLATFORM_SETTINGS_TOGGLE_TAG_BASE + (NSInteger)k_toggle_ids[index]];
    if (toggle == nil)
    {
      continue;
    }

    switch (k_toggle_ids[index])
    {
      case OVERLAY_TOGGLE_GOD_MODE:
        [toggle setState:(app->overlay.god_mode_enabled != 0) ? NSControlStateValueOn : NSControlStateValueOff];
        break;
      case OVERLAY_TOGGLE_FREEZE_TIME:
        [toggle setState:(app->overlay.freeze_time_enabled != 0) ? NSControlStateValueOn : NSControlStateValueOff];
        break;
      case OVERLAY_TOGGLE_SOUND:
        [toggle setState:(app->overlay.sound_enabled != 0) ? NSControlStateValueOn : NSControlStateValueOff];
        break;
      case OVERLAY_TOGGLE_CLOUDS:
        [toggle setState:(app->overlay.settings.clouds_enabled != 0) ? NSControlStateValueOn : NSControlStateValueOff];
        break;
      case OVERLAY_TOGGLE_NONE:
      case OVERLAY_TOGGLE_COUNT:
      default:
        break;
    }
  }

  for (index = 0U; index < sizeof(k_slider_ids) / sizeof(k_slider_ids[0]); ++index)
  {
    NSSlider* slider = (NSSlider*)[root viewWithTag:PLATFORM_SETTINGS_SLIDER_TAG_BASE + (NSInteger)k_slider_ids[index]];
    NSTextField* value_label = (NSTextField*)[root viewWithTag:PLATFORM_SETTINGS_SLIDER_VALUE_TAG_BASE + (NSInteger)k_slider_ids[index]];
    char value_text[64] = { 0 };
    float value = platform_get_slider_setting_value(&app->overlay.settings, k_slider_ids[index]);

    if (slider != nil)
    {
      [slider setDoubleValue:value];
    }
    if (value_label != nil)
    {
      platform_format_slider_setting_value(value_text, sizeof(value_text), k_slider_ids[index], value);
      [value_label setStringValue:[NSString stringWithUTF8String:value_text]];
    }
  }

  {
    NSSegmentedControl* quality_segmented = (NSSegmentedControl*)[root viewWithTag:PLATFORM_SETTINGS_QUALITY_SEGMENTED_TAG];
    NSTextField* quality_status = (NSTextField*)[root viewWithTag:PLATFORM_SETTINGS_QUALITY_STATUS_TAG];
    NSSegmentedControl* gpu_segmented = (NSSegmentedControl*)[root viewWithTag:PLATFORM_SETTINGS_GPU_SEGMENTED_TAG];
    NSTextField* gpu_status = (NSTextField*)[root viewWithTag:PLATFORM_SETTINGS_GPU_STATUS_TAG];
    const char* status_text = (app->overlay.gpu_info.status_message[0] != '\0')
      ? app->overlay.gpu_info.status_message
      : "GPU routing selection is not available on this platform";

    if (quality_segmented != nil)
    {
      [quality_segmented setSelectedSegment:(NSInteger)app->overlay.render_quality_preset];
    }
    if (quality_status != nil)
    {
      [quality_status setStringValue:[NSString stringWithUTF8String:platform_get_render_quality_status_text(app->overlay.render_quality_preset)]];
    }
    if (gpu_segmented != nil)
    {
      [gpu_segmented setSelectedSegment:(NSInteger)app->overlay.gpu_info.selected_mode];
      [gpu_segmented setEnabled:app->overlay.gpu_info.available != 0];
    }
    if (gpu_status != nil)
    {
      [gpu_status setStringValue:[NSString stringWithUTF8String:status_text]];
    }
  }
}

static void platform_sync_native_overlay_visibility(PlatformApp* app)
{
  NSView* settings_view = nil;
  const int overlay_visible = (app != NULL && app->cursor_mode_enabled != 0);

  if (app == NULL)
  {
    return;
  }

  settings_view = (NSView*)app->settings_overlay_view;
  if (settings_view != nil)
  {
    [settings_view setHidden:(overlay_visible == 0)];
  }

  app->overlay.cursor_mode_enabled = app->cursor_mode_enabled;
  app->overlay.panel_width = overlay_visible ? overlay_get_panel_width_for_window(app->width) : 0;
  app->overlay.panel_collapsed = overlay_visible ? 0 : 1;
  app->overlay.scroll_offset = 0.0f;
  app->overlay.scroll_max = 0.0f;
  platform_layout_native_overlays(app);
}

static int platform_point_in_view(NSView* view, NSPoint point_in_window)
{
  NSPoint point_in_view = NSZeroPoint;

  if (view == nil || [view isHidden] || [view window] == nil)
  {
    return 0;
  }

  point_in_view = [view convertPoint:point_in_window fromView:nil];
  return NSPointInRect(point_in_view, [view bounds]) ? 1 : 0;
}

static int platform_try_enter_world_from_mouse_down(PlatformApp* app, NSEvent* event)
{
  NSWindow* window = nil;
  NSView* settings_view = nil;

  if (app == NULL || event == nil || app->cursor_mode_enabled == 0)
  {
    return 0;
  }

  settings_view = (NSView*)app->settings_overlay_view;
  if (platform_point_in_view(settings_view, [event locationInWindow]))
  {
    return 0;
  }

  window = (NSWindow*)app->window;
  app->cursor_mode_enabled = 0;
  app->overlay.cursor_mode_enabled = 0;
  app->overlay.active_slider = OVERLAY_SLIDER_NONE;
  app->overlay.hot_slider = OVERLAY_SLIDER_NONE;
  app->overlay.hot_toggle = OVERLAY_TOGGLE_NONE;
  app->overlay.hot_gpu_preference = -1;
  app->left_button_down = 0;
  app->right_button_down = 0;
  app->suppress_world_click_until_release = 1;
  platform_sync_native_overlay_visibility(app);
  if (window != nil && app->view != NULL)
  {
    [window makeFirstResponder:(NSView*)app->view];
  }
  platform_set_mouse_capture(app, 1);
  return 1;
}

static void platform_layout_native_overlays(PlatformApp* app)
{
  NSWindow* window = nil;
  NSView* content_view = nil;
  NSView* settings_view = nil;
  NSTextView* stats_view = nil;
  NSTextView* debug_view = nil;
  NSRect bounds = NSZeroRect;
  const CGFloat margin = 12.0;
  const CGFloat settings_width = (CGFloat)overlay_get_panel_width_for_window((app != NULL) ? app->width : 0);
  const CGFloat stats_width = 292.0;
  const CGFloat stats_height = 208.0;
  const CGFloat debug_width = 560.0;
  const CGFloat debug_height = 214.0;

  if (app == NULL)
  {
    return;
  }

  window = (NSWindow*)app->window;
  content_view = (window != nil) ? [window contentView] : nil;
  settings_view = (NSView*)app->settings_overlay_view;
  stats_view = (NSTextView*)app->stats_overlay_view;
  debug_view = (NSTextView*)app->debug_overlay_view;
  if (content_view == nil)
  {
    return;
  }

  bounds = [content_view bounds];
  if ((NSView*)app->view != nil)
  {
    [(NSView*)app->view setFrame:bounds];
  }

  if (settings_view != nil)
  {
    NSTextField* title_label = (NSTextField*)[settings_view viewWithTag:PLATFORM_SETTINGS_TITLE_TAG];
    NSTextField* hint_label = (NSTextField*)[settings_view viewWithTag:PLATFORM_SETTINGS_HINT_TAG];
    NSScrollView* scroll_view = nil;
    NSView* document_view = (scroll_view != nil) ? [scroll_view documentView] : nil;
    const CGFloat panel_width = fmin(settings_width, fmax(280.0, bounds.size.width - margin * 2.0));
    const CGFloat panel_height = fmax(220.0, bounds.size.height - margin * 2.0);

    for (NSView* subview in [settings_view subviews])
    {
      if ([subview isKindOfClass:[NSScrollView class]])
      {
        scroll_view = (NSScrollView*)subview;
        break;
      }
    }
    document_view = (scroll_view != nil) ? [scroll_view documentView] : nil;

    [settings_view setFrame:NSMakeRect(margin, margin, panel_width, panel_height)];
    if (title_label != nil)
    {
      [title_label setFrame:NSMakeRect(16.0, panel_height - 32.0, panel_width - 32.0, 20.0)];
    }
    if (hint_label != nil)
    {
      [hint_label setFrame:NSMakeRect(16.0, panel_height - 58.0, panel_width - 32.0, 32.0)];
    }
    if (scroll_view != nil)
    {
      [scroll_view setFrame:NSMakeRect(16.0, 14.0, panel_width - 32.0, panel_height - 84.0)];
    }
    if (document_view != nil)
    {
      NSRect document_frame = [document_view frame];
      document_frame.size.width = panel_width - 48.0;
      [document_view setFrame:document_frame];
    }
  }

  if (stats_view != nil)
  {
    [stats_view setFrame:NSMakeRect(
      fmax(margin, bounds.size.width - stats_width - margin),
      fmax(margin, bounds.size.height - stats_height - margin),
      fmin(stats_width, fmax(180.0, bounds.size.width - margin * 2.0)),
      fmin(stats_height, fmax(120.0, bounds.size.height - margin * 2.0))
    )];
  }

  if (debug_view != nil)
  {
    [debug_view setFrame:NSMakeRect(
      fmax(margin, bounds.size.width - debug_width - margin),
      margin,
      fmin(debug_width, fmax(220.0, bounds.size.width - margin * 2.0)),
      fmin(debug_height, fmax(120.0, bounds.size.height * 0.34))
    )];
  }
}

static void platform_update_native_overlays(PlatformApp* app)
{
  NSTextView* stats_view = nil;
  NSTextView* debug_view = nil;

  if (app == NULL)
  {
    return;
  }

  stats_view = (NSTextView*)app->stats_overlay_view;
  debug_view = (NSTextView*)app->debug_overlay_view;

  if (stats_view != nil)
  {
    char stats_text[1024] = { 0 };
    const OverlayMetrics* metrics = &app->overlay.metrics;
    const GpuPreferenceInfo* gpu_info = &app->overlay.gpu_info;
    const float fps = (metrics->frames_per_second > 0.0f) ? metrics->frames_per_second : 0.0f;
    const float frame_ms = (metrics->frame_time_ms > 0.0f) ? metrics->frame_time_ms : 0.0f;
    const char* renderer_name =
      (gpu_info->current_renderer[0] != '\0') ? gpu_info->current_renderer : "OpenGL";

	    (void)snprintf(
	      stats_text,
	      sizeof(stats_text),
	      "ENGINE\n"
	      "FPS %.0f\n"
      "Frame %.2f ms\n"
      "CPU %.0f%%\n"
      "GPU0 %.0f%%\n"
      "GPU1 %.0f%%\n"
      "Pos %.1f %.1f %.1f\n"
      "Mode %d\n"
	      "Block %d\n"
	      "Placed %d\n"
	      "Target %s\n"
	      "Quality %s\n"
	      "Renderer %s",
	      fps,
	      frame_ms,
      metrics->cpu_usage_percent,
      metrics->gpu0_usage_percent,
      metrics->gpu1_usage_percent,
      metrics->player_position_x,
      metrics->player_position_y,
      metrics->player_position_z,
      metrics->player_mode,
	      metrics->selected_block_type,
	      metrics->placed_block_count,
	      (metrics->target_active != 0) ? "ON" : "OFF",
	      render_quality_preset_get_label(app->overlay.render_quality_preset),
	      renderer_name
	    );

    [stats_view setString:[NSString stringWithUTF8String:stats_text]];
  }

  if (debug_view != nil)
  {
    char console_text[8192] = { 0 };
    size_t offset = 0U;
    const int recent_count = diagnostics_get_recent_message_count();
    const int first_index = (recent_count > 10) ? (recent_count - 10) : 0;
    int message_index = 0;

    for (message_index = first_index; message_index < recent_count; ++message_index)
    {
      const char* message = diagnostics_get_recent_message(message_index);
      const int written = snprintf(
        console_text + offset,
        sizeof(console_text) - offset,
        "%s%s",
        (offset > 0U) ? "\n" : "",
        (message != NULL) ? message : ""
      );

      if (written <= 0 || (size_t)written >= sizeof(console_text) - offset)
      {
        break;
      }
      offset += (size_t)written;
    }

    if (offset == 0U)
    {
      (void)snprintf(console_text, sizeof(console_text), "DEBUG CONSOLE\nMenunggu log runtime...");
    }

    [debug_view setString:[NSString stringWithUTF8String:console_text]];
  }
}

int platform_create(PlatformApp* app, const char* title, int width, int height)
{
  @autoreleasepool
  {
    NSApplication* application = nil;
    NSWindow* window = nil;
    NSView* content_view = nil;
    NSOpenGLPixelFormat* pixel_format = nil;
    PlatformOpenGLView* view = nil;
    NSOpenGLContext* context = nil;
    PlatformWindowDelegate* delegate = nil;
    NSView* settings_view = nil;
    NSTextView* stats_view = nil;
    NSTextView* debug_view = nil;
    NSFont* stats_font = nil;
    NSFont* debug_font = nil;
    NSString* window_title = nil;
    GLint swap_interval = 0;
    NSRect content_rect = NSMakeRect(0.0, 0.0, width, height);
    const NSOpenGLPixelFormatAttribute pixel_attributes[] = {
#if defined(NSOpenGLProfileVersion4_1Core)
      NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion4_1Core,
#else
      NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion3_2Core,
#endif
      NSOpenGLPFAColorSize, 24,
      NSOpenGLPFAAlphaSize, 8,
      NSOpenGLPFADepthSize, 24,
      NSOpenGLPFAAccelerated,
      NSOpenGLPFADoubleBuffer,
      0
    };

    if (app == NULL)
    {
      return 0;
    }

    memset(app, 0, sizeof(*app));
    diagnostics_log("platform_create: cocoa begin");

    application = [NSApplication sharedApplication];
    if (application == nil)
    {
      platform_show_error_message("Cocoa Error", "Failed to create the shared NSApplication instance.");
      return 0;
    }

    [application setActivationPolicy:NSApplicationActivationPolicyRegular];

    window_title = [NSString stringWithUTF8String:(title != NULL) ? title : "OpenGL Sky"];
    if (window_title == nil)
    {
      window_title = @"OpenGL Sky";
    }

    window = [[NSWindow alloc]
      initWithContentRect:content_rect
                styleMask:NSWindowStyleMaskTitled |
                          NSWindowStyleMaskClosable |
                          NSWindowStyleMaskResizable |
                          NSWindowStyleMaskMiniaturizable
                  backing:NSBackingStoreBuffered
                    defer:NO];
    if (window == nil)
    {
      platform_show_error_message("Cocoa Error", "Failed to create the macOS window.");
      return 0;
    }

    if ([window respondsToSelector:@selector(setTabbingMode:)])
    {
      [window setTabbingMode:NSWindowTabbingModeDisallowed];
    }

    pixel_format = [[NSOpenGLPixelFormat alloc] initWithAttributes:pixel_attributes];
    if (pixel_format == nil)
    {
      [window release];
      platform_show_error_message("OpenGL Error", "Failed to create the OpenGL pixel format for macOS.");
      return 0;
    }

    view = [[PlatformOpenGLView alloc] initWithFrame:content_rect pixelFormat:pixel_format];
    [pixel_format release];
    if (view == nil)
    {
      [window release];
      platform_show_error_message("OpenGL Error", "Failed to create the OpenGL view for macOS.");
      return 0;
    }

    if ([view respondsToSelector:@selector(setWantsBestResolutionOpenGLSurface:)])
    {
      [view setWantsBestResolutionOpenGLSurface:YES];
    }

    delegate = [[PlatformWindowDelegate alloc] init];
    delegate->app = app;
    [window setDelegate:delegate];
    content_view = [window contentView];
    [view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [content_view addSubview:view];
    [window makeFirstResponder:view];
    [window setAcceptsMouseMovedEvents:YES];
    [window setTitle:window_title];
    [window center];
    [window makeKeyAndOrderFront:nil];

    stats_font = [NSFont fontWithName:@"Menlo Bold" size:13.0];
    if (stats_font == nil)
    {
      stats_font = [NSFont monospacedSystemFontOfSize:13.0 weight:NSFontWeightSemibold];
    }
    debug_font = [NSFont fontWithName:@"Menlo" size:11.0];
    if (debug_font == nil)
    {
      debug_font = [NSFont monospacedSystemFontOfSize:11.0 weight:NSFontWeightRegular];
    }

    stats_view = platform_create_overlay_text_view(
      stats_font,
      [NSColor colorWithCalibratedRed:0.75 green:0.98 blue:0.86 alpha:0.98],
      [NSColor colorWithCalibratedRed:0.03 green:0.04 blue:0.05 alpha:0.82]
    );
    debug_view = platform_create_overlay_text_view(
      debug_font,
      [NSColor colorWithCalibratedRed:0.92 green:0.95 blue:0.99 alpha:0.98],
      [NSColor colorWithCalibratedRed:0.03 green:0.03 blue:0.04 alpha:0.82]
    );
    context = [view openGLContext];
    if (context == nil)
    {
      [delegate release];
      [view release];
      [window release];
      platform_show_error_message("OpenGL Error", "Failed to resolve the NSOpenGLContext from the view.");
      return 0;
    }

    [context makeCurrentContext];
    [context setValues:&swap_interval forParameter:NSOpenGLCPSwapInterval];

    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK)
    {
      [delegate release];
      [view release];
      [window release];
      platform_show_error_message("OpenGL Error", "Failed to initialize GLEW for the Cocoa OpenGL context.");
      return 0;
    }
    (void)glGetError();

    app->application = application;
    app->window = window;
    app->view = view;
    app->gl_context = context;
    app->window_delegate = delegate;
    app->timer_start = CFAbsoluteTimeGetCurrent();
    app->running = 1;
    app->resized = 1;
    app->requested_gpu_preference = GPU_PREFERENCE_MODE_AUTO;
    app->requested_render_quality_preset = RENDER_QUALITY_PRESET_HIGH;
    app->overlay.settings = scene_settings_default();
    app->overlay.hot_slider = OVERLAY_SLIDER_NONE;
    app->overlay.active_slider = OVERLAY_SLIDER_NONE;
    app->overlay.hot_toggle = OVERLAY_TOGGLE_NONE;
    app->overlay.hot_render_quality_preset = -1;
    app->overlay.hot_gpu_preference = -1;
    app->overlay.god_mode_enabled = 0;
    app->overlay.freeze_time_enabled = 0;
    app->overlay.render_quality_preset = RENDER_QUALITY_PRESET_HIGH;
    app->overlay.panel_width = 0;
    app->overlay.panel_collapsed = 1;
    app->overlay.scroll_offset = 0.0f;
    app->overlay.scroll_max = 0.0f;
    settings_view = platform_create_settings_overlay(app);
    if (settings_view != nil)
    {
      [content_view addSubview:settings_view];
    }
    if (stats_view != nil)
    {
      [content_view addSubview:stats_view];
    }
    if (debug_view != nil)
    {
      [content_view addSubview:debug_view];
    }
    app->settings_overlay_view = settings_view;
    app->stats_overlay_view = stats_view;
    app->debug_overlay_view = debug_view;

    platform_update_dimensions(app);
    platform_refresh_gpu_info(app);
    platform_sync_native_overlay_visibility(app);
    platform_sync_native_settings_controls(app);
    platform_update_native_overlays(app);

    [application finishLaunching];
    [application activateIgnoringOtherApps:YES];
    platform_set_mouse_capture(app, 1);
    diagnostics_logf(
      "platform_create: cocoa success width=%d height=%d gl_version=%s renderer=%s vendor=%s",
      app->width,
      app->height,
      (const char*)glGetString(GL_VERSION),
      (const char*)glGetString(GL_RENDERER),
      (const char*)glGetString(GL_VENDOR));
    return 1;
  }
}

void platform_destroy(PlatformApp* app)
{
  @autoreleasepool
  {
    if (app == NULL)
    {
      return;
    }

    diagnostics_log("platform_destroy: cocoa begin");
    app->running = 0;
    platform_set_mouse_capture(app, 0);

    if (app->stats_overlay_view != NULL)
    {
      [(NSView*)app->stats_overlay_view removeFromSuperview];
      [(NSView*)app->stats_overlay_view release];
      app->stats_overlay_view = NULL;
    }

    if (app->debug_overlay_view != NULL)
    {
      [(NSView*)app->debug_overlay_view removeFromSuperview];
      [(NSView*)app->debug_overlay_view release];
      app->debug_overlay_view = NULL;
    }

    if (app->settings_overlay_view != NULL)
    {
      [(NSView*)app->settings_overlay_view removeFromSuperview];
      [(NSView*)app->settings_overlay_view release];
      app->settings_overlay_view = NULL;
    }

    if (app->settings_action_target != NULL)
    {
      [(id)app->settings_action_target release];
      app->settings_action_target = NULL;
    }

    if (app->gl_context != NULL)
    {
      [NSOpenGLContext clearCurrentContext];
      app->gl_context = NULL;
    }

    if (app->window != NULL)
    {
      NSWindow* window = (NSWindow*)app->window;
      [window orderOut:nil];
      [window setDelegate:nil];
      [window release];
      app->window = NULL;
    }

    if (app->view != NULL)
    {
      [(NSView*)app->view release];
      app->view = NULL;
    }

    if (app->window_delegate != NULL)
    {
      [(id)app->window_delegate release];
      app->window_delegate = NULL;
    }

    diagnostics_log("platform_destroy: cocoa end");
  }
}

void platform_pump_messages(PlatformApp* app, PlatformInput* input)
{
  @autoreleasepool
  {
    NSEvent* event = nil;
    unsigned long pressed_mouse_buttons = 0UL;
    int has_focus = 0;
    int alt_down = 0;
    int fullscreen_down = 0;
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

    if (app == NULL || input == NULL)
    {
      return;
    }

    memset(input, 0, sizeof(*input));
    input->selected_block_slot = -1;

    while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                       untilDate:[NSDate distantPast]
                                          inMode:NSDefaultRunLoopMode
                                         dequeue:YES]) != nil)
    {
      switch ([event type])
      {
        case NSEventTypeKeyDown:
          if ([event keyCode] < sizeof(app->key_down))
          {
            app->key_down[[event keyCode]] = 1U;
          }
          platform_sync_modifier_keys(app, [event modifierFlags]);
          if ([event keyCode] == PLATFORM_KEYCODE_ESCAPE && [event isARepeat] == NO)
          {
            app->escape_requested = 1;
          }
          if (([event modifierFlags] & NSEventModifierFlagCommand) != 0 &&
            [event keyCode] == PLATFORM_KEYCODE_Q)
          {
            app->running = 0;
          }
          break;

        case NSEventTypeKeyUp:
          if ([event keyCode] < sizeof(app->key_down))
          {
            app->key_down[[event keyCode]] = 0U;
          }
          platform_sync_modifier_keys(app, [event modifierFlags]);
          break;

        case NSEventTypeFlagsChanged:
          platform_sync_modifier_keys(app, [event modifierFlags]);
          break;

        case NSEventTypeLeftMouseDown:
          if (platform_try_enter_world_from_mouse_down(app, event))
          {
            break;
          }
          app->left_button_down = 1;
          if (app->mouse_captured == 0)
          {
            [NSApp sendEvent:event];
          }
          break;

        case NSEventTypeLeftMouseUp:
          app->left_button_down = 0;
          if (app->mouse_captured == 0)
          {
            [NSApp sendEvent:event];
          }
          break;

        case NSEventTypeRightMouseDown:
          if (platform_try_enter_world_from_mouse_down(app, event))
          {
            break;
          }
          app->right_button_down = 1;
          if (app->mouse_captured == 0)
          {
            [NSApp sendEvent:event];
          }
          break;

        case NSEventTypeRightMouseUp:
          app->right_button_down = 0;
          if (app->mouse_captured == 0)
          {
            [NSApp sendEvent:event];
          }
          break;

        case NSEventTypeMouseMoved:
        case NSEventTypeLeftMouseDragged:
        case NSEventTypeRightMouseDragged:
        case NSEventTypeOtherMouseDragged:
          if (app->mouse_captured != 0 && app->cursor_mode_enabled == 0)
          {
            app->mouse_dx += (int)lround([event deltaX]);
            app->mouse_dy -= (int)lround([event deltaY]);
          }
          else
          {
            [NSApp sendEvent:event];
          }
          break;

        default:
          [NSApp sendEvent:event];
          break;
      }
    }

    platform_update_dimensions(app);
    platform_sync_modifier_keys(app, [NSEvent modifierFlags]);
    has_focus = platform_window_has_focus(app);
    alt_down = has_focus
      ? (platform_is_key_down(app, PLATFORM_KEYCODE_LEFT_OPTION) || platform_is_key_down(app, PLATFORM_KEYCODE_RIGHT_OPTION))
      : 0;
    fullscreen_down = has_focus ? platform_is_key_down(app, PLATFORM_KEYCODE_F11) : 0;
    player_mode_down = (has_focus && app->cursor_mode_enabled == 0) ? platform_is_key_down(app, PLATFORM_KEYCODE_G) : 0;
    toggle_cycle_down = (has_focus && app->cursor_mode_enabled == 0) ? platform_is_key_down(app, PLATFORM_KEYCODE_P) : 0;
    reset_cycle_down = (has_focus && app->cursor_mode_enabled == 0) ? platform_is_key_down(app, PLATFORM_KEYCODE_R) : 0;
    increase_speed_down = (has_focus && app->cursor_mode_enabled == 0) ? platform_is_key_down(app, PLATFORM_KEYCODE_UP_ARROW) : 0;
    decrease_speed_down = (has_focus && app->cursor_mode_enabled == 0) ? platform_is_key_down(app, PLATFORM_KEYCODE_DOWN_ARROW) : 0;
    move_forward_down = (has_focus && app->cursor_mode_enabled == 0) ? platform_is_key_down(app, PLATFORM_KEYCODE_W) : 0;
    move_backward_down = (has_focus && app->cursor_mode_enabled == 0) ? platform_is_key_down(app, PLATFORM_KEYCODE_S) : 0;
    move_left_down = (has_focus && app->cursor_mode_enabled == 0) ? platform_is_key_down(app, PLATFORM_KEYCODE_A) : 0;
    move_right_down = (has_focus && app->cursor_mode_enabled == 0) ? platform_is_key_down(app, PLATFORM_KEYCODE_D) : 0;
    jump_down = (has_focus && app->cursor_mode_enabled == 0) ? platform_is_key_down(app, PLATFORM_KEYCODE_SPACE) : 0;
    move_down_down = (has_focus && app->cursor_mode_enabled == 0)
      ? (platform_is_key_down(app, PLATFORM_KEYCODE_LEFT_SHIFT) || platform_is_key_down(app, PLATFORM_KEYCODE_RIGHT_SHIFT))
      : 0;
    fast_modifier_down = (has_focus && app->cursor_mode_enabled == 0)
      ? (platform_is_key_down(app, PLATFORM_KEYCODE_LEFT_CONTROL) || platform_is_key_down(app, PLATFORM_KEYCODE_RIGHT_CONTROL))
      : 0;
    pressed_mouse_buttons = [NSEvent pressedMouseButtons];
    app->left_button_down = has_focus ? ((pressed_mouse_buttons & 1UL) != 0UL) : 0;
    app->right_button_down = has_focus ? ((pressed_mouse_buttons & 2UL) != 0UL) : 0;
    app->overlay.cursor_mode_enabled = app->cursor_mode_enabled;
    app->overlay.hot_slider = OVERLAY_SLIDER_NONE;
    app->overlay.active_slider = OVERLAY_SLIDER_NONE;
    app->overlay.hot_toggle = OVERLAY_TOGGLE_NONE;
    app->overlay.hot_gpu_preference = -1;
    app->overlay.scroll_offset = 0.0f;
    app->overlay.scroll_max = 0.0f;
    if (app->suppress_world_click_until_release != 0)
    {
      if ((pressed_mouse_buttons & 3UL) == 0UL)
      {
        app->suppress_world_click_until_release = 0;
      }
      app->left_button_down = 0;
      app->right_button_down = 0;
    }

    if (has_focus && alt_down && !app->previous_alt_down)
    {
      app->cursor_mode_enabled = (app->cursor_mode_enabled == 0);
      if (app->cursor_mode_enabled != 0)
      {
        platform_set_mouse_capture(app, 0);
      }
      else
      {
        platform_set_mouse_capture(app, 1);
      }
    }

    if (!has_focus)
    {
      platform_set_mouse_capture(app, 0);
    }
    else if (app->cursor_mode_enabled == 0)
    {
      platform_set_mouse_capture(app, 1);
    }

    if (has_focus && fullscreen_down && !app->previous_fullscreen_down)
    {
      platform_toggle_fullscreen(app);
    }

    platform_sync_native_overlay_visibility(app);
    platform_sync_native_settings_controls(app);

    input->look_x = (float)app->mouse_dx;
    input->look_y = (float)app->mouse_dy;
    if (app->suppress_next_mouse_delta != 0)
    {
      input->look_x = 0.0f;
      input->look_y = 0.0f;
      app->suppress_next_mouse_delta = 0;
    }

    if (app->cursor_mode_enabled != 0)
    {
      input->look_x = 0.0f;
      input->look_y = 0.0f;
    }

    input->move_forward = (float)(move_forward_down - move_backward_down);
    input->move_right = (float)(move_right_down - move_left_down);
    input->escape_pressed = app->escape_requested;
    input->toggle_player_mode_pressed = player_mode_down && !app->previous_player_mode_down;
    input->toggle_cycle_pressed = toggle_cycle_down && !app->previous_toggle_cycle_down;
    input->reset_cycle_pressed = reset_cycle_down && !app->previous_reset_cycle_down;
    input->increase_cycle_speed_pressed = increase_speed_down && !app->previous_increase_speed_down;
    input->decrease_cycle_speed_pressed = decrease_speed_down && !app->previous_decrease_speed_down;
    input->scrub_backward_held = (has_focus && app->cursor_mode_enabled == 0) ? platform_is_key_down(app, PLATFORM_KEYCODE_LEFT_ARROW) : 0;
    input->scrub_forward_held = (has_focus && app->cursor_mode_enabled == 0) ? platform_is_key_down(app, PLATFORM_KEYCODE_RIGHT_ARROW) : 0;
    input->scrub_fast_held = fast_modifier_down;
    input->move_fast_held = (app->overlay.god_mode_enabled != 0) ? fast_modifier_down : move_down_down;
    input->crouch_held = (app->overlay.god_mode_enabled == 0) ? fast_modifier_down : 0;
    input->jump_pressed = jump_down && !app->previous_jump_down;
    input->jump_held = jump_down;
    input->move_down_held = (app->overlay.god_mode_enabled != 0) ? move_down_down : 0;
    input->remove_block_pressed = (has_focus && app->cursor_mode_enabled == 0) ?
      (app->left_button_down && !app->previous_world_left_button_down) : 0;
    input->place_block_pressed = (has_focus && app->cursor_mode_enabled == 0) ?
      (app->right_button_down && !app->previous_world_right_button_down) : 0;

    if (has_focus && app->cursor_mode_enabled == 0)
    {
      if (platform_is_key_down(app, PLATFORM_KEYCODE_1))
      {
        input->selected_block_slot = 0;
      }
      else if (platform_is_key_down(app, PLATFORM_KEYCODE_2))
      {
        input->selected_block_slot = 1;
      }
      else if (platform_is_key_down(app, PLATFORM_KEYCODE_3))
      {
        input->selected_block_slot = 2;
      }
      else if (platform_is_key_down(app, PLATFORM_KEYCODE_4))
      {
        input->selected_block_slot = 3;
      }
    }

    if (app->cursor_mode_enabled != 0)
    {
      input->move_forward = 0.0f;
      input->move_right = 0.0f;
      input->toggle_player_mode_pressed = 0;
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
      input->remove_block_pressed = 0;
      input->place_block_pressed = 0;
      input->selected_block_slot = -1;
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
    app->previous_fullscreen_down = fullscreen_down;
    app->previous_world_left_button_down = app->left_button_down;
    app->previous_world_right_button_down = app->right_button_down;
  }
}

void platform_request_close(PlatformApp* app)
{
  if (app != NULL)
  {
    app->running = 0;
  }
}

float platform_get_time_seconds(const PlatformApp* app)
{
  if (app == NULL)
  {
    return 0.0f;
  }

  return (float)(CFAbsoluteTimeGetCurrent() - app->timer_start);
}

void platform_swap_buffers(const PlatformApp* app)
{
  if (app != NULL && app->gl_context != NULL)
  {
    [(NSOpenGLContext*)app->gl_context flushBuffer];
  }
}

void platform_set_window_title(const PlatformApp* app, const char* title)
{
  @autoreleasepool
  {
    NSWindow* window = (NSWindow*)app->window;
    NSString* window_title = nil;

    if (window == nil)
    {
      return;
    }

    window_title = [NSString stringWithUTF8String:(title != NULL) ? title : "OpenGL Sky"];
    if (window_title == nil)
    {
      window_title = @"OpenGL Sky";
    }
    [window setTitle:window_title];
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
  platform_sync_native_settings_controls(app);
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
  platform_sync_native_settings_controls(app);
}

void platform_update_overlay_metrics(PlatformApp* app, const OverlayMetrics* metrics)
{
  if (app == NULL || metrics == NULL)
  {
    return;
  }

  app->overlay.metrics = *metrics;
  platform_update_native_overlays(app);
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
  platform_sync_native_settings_controls(app);
  platform_update_native_overlays(app);
}

void platform_refresh_gpu_info(PlatformApp* app)
{
  if (app == NULL)
  {
    return;
  }

  (void)gpu_preferences_query(&app->overlay.gpu_info);
  gpu_preferences_set_current_renderer(
    &app->overlay.gpu_info,
    (const char*)glGetString(GL_RENDERER),
    (const char*)glGetString(GL_VENDOR));
  platform_update_native_overlays(app);
}

void platform_show_error_message(const char* title, const char* message)
{
  @autoreleasepool
  {
    NSString* alert_title = [NSString stringWithUTF8String:(title != NULL) ? title : "Error"];
    NSString* alert_message = [NSString stringWithUTF8String:(message != NULL) ? message : "Unknown error"];

    diagnostics_logf("%s: %s", (title != NULL) ? title : "Error", (message != NULL) ? message : "Unknown error");

    if (alert_title == nil)
    {
      alert_title = @"Error";
    }
    if (alert_message == nil)
    {
      alert_message = @"Unknown error";
    }

    if ([NSApplication sharedApplication] != nil)
    {
      NSAlert* alert = [[NSAlert alloc] init];
      [alert setMessageText:alert_title];
      [alert setInformativeText:alert_message];
      [alert addButtonWithTitle:@"OK"];
      [alert runModal];
      [alert release];
    }
  }
}
