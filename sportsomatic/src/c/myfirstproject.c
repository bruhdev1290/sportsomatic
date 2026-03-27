#include <pebble.h>
#include <math.h>
#include "message_keys.auto.h"

// Animation duration for smooth transitions (ms)
#define ANIMATION_DURATION 200
#define SLIDE_ANIMATION_DURATION 250
#define FADE_ANIMATION_DURATION 150

typedef enum {
  RUN_STATE_IDLE,
  RUN_STATE_RUNNING,
  RUN_STATE_PAUSED,
  RUN_STATE_SUMMARY
} RunState;

typedef enum {
  SPORT_RUN = 0,
  SPORT_SWIM = 1
} SportMode;

typedef enum {
  PAGE_MAIN,
  PAGE_LAP,
  PAGE_HR,
  PAGE_SPLITS,
  PAGE_COUNT
} Page;

// Animation states for page transitions
typedef enum {
  ANIM_STATE_NONE,
  ANIM_STATE_SLIDING_LEFT,
  ANIM_STATE_SLIDING_RIGHT
} AnimState;

// UI state colors based on run state
typedef enum {
  UI_STATE_RUNNING,
  UI_STATE_PAUSED,
  UI_STATE_FINISHED
} UIStateColor;

typedef enum {
  CMD_START = 1,
  CMD_PAUSE = 2,
  CMD_RESUME = 3,
  CMD_STOP = 4
} BridgeCommand;

typedef struct {
  int32_t elapsed_ms;
  double distance_m;
} Lap;

typedef struct {
  double lat;
  double lon;
  bool valid;
  uint16_t accuracy_m;
} Fix;

#define MAX_LAPS 50

static Window *s_window;
static TextLayer *s_title_layer;
static TextLayer *s_primary_layer;
static TextLayer *s_secondary_layer;
static TextLayer *s_tertiary_layer;
static TextLayer *s_footer_layer;
static Layer *s_divider_layer;
static Layer *s_dots_layer;
static Layer *s_bg_layer;
static Window *s_sport_window;
static MenuLayer *s_menu_layer;
static Window *s_confirm_window;
static TextLayer *s_confirm_title_layer;
static TextLayer *s_confirm_msg_layer;
static Layer *s_confirm_bg_layer;
static bool s_confirm_result = false;
static bool s_confirm_active = false;

// Animation state
static AnimState s_anim_state = ANIM_STATE_NONE;
static Page s_target_page = PAGE_MAIN;

// Lap flash effect
static bool s_lap_flash_active = false;
static AppTimer *s_lap_flash_timer = NULL;

// Current UI color state
static UIStateColor s_ui_state_color = UI_STATE_RUNNING;

// Message handling state
// static bool s_js_ready = false;  // Reserved for future use

static GFont s_font_title;
static GFont s_font_primary;
static GFont s_font_secondary;

static RunState s_state = RUN_STATE_IDLE;
static Page s_page = PAGE_MAIN;
static SportMode s_sport = SPORT_RUN;
static bool s_use_metric = true;
static bool s_distance_alerts = true;
static double s_next_alert_m = 1000.0;
static double s_alert_step_m = 1000; // updated when units change
static bool s_auto_run = true;
static bool s_auto_stop = true;
static int64_t s_last_movement_ms = 0;
static bool s_auto_lap = true;
static double s_auto_lap_distance_m = 1000.0;
static double s_next_auto_lap_m = 1000.0;
static int32_t s_pool_length_cm = 2500; // 25.00m default (or ~25yd in imperial when set)

static int64_t s_start_epoch_ms = 0;
static int64_t s_last_tick_ms = 0;
static int64_t s_pause_started_ms = 0;
static int32_t s_elapsed_ms = 0;
static int32_t s_paused_ms = 0;
static int32_t s_lap_anchor_ms = 0;
static double s_lap_anchor_distance_m = 0;

static double s_distance_m = 0;
static double s_inst_speed_mps = 0;
static Fix s_last_fix;

static Lap s_laps[MAX_LAPS];
static int s_lap_count = 0;
// Current heart rate (bpm), -1 if unavailable
static int32_t s_heart_rate_bpm = -1;

static char s_buf_primary[32];
static char s_buf_secondary[32];
static char s_buf_tertiary[32];
static char s_buf_title[24];
static char s_buf_footer[48];

static int64_t prv_now_ms(void);
static void prv_reset_thresholds(void);
static void prv_mark_chrome_dirty(void);
static void prv_update_layers(void);
static void prv_send_summary(void);
static void prv_show_sport_selector(void);
static void prv_show_confirmation(const char *title, const char *message, void (*callback)(bool));
static void prv_animate_page_transition(Page new_page, bool slide_left);
static void prv_end_run(void);
static void prv_end_run_confirmed(bool confirmed);
static void prv_trigger_lap_flash(void);
static void prv_update_ui_state_color(void);
static GColor prv_get_state_color(void);
static GColor prv_get_state_color_dim(void);

// Pebble libc lacks __errno symbol required by libm; provide a stub.
static int s_errno_stub = 0;
int *__errno(void) {
  return &s_errno_stub;
}

// ==================== Lap Flash Animation ====================

static void prv_lap_flash_timer_cb(void *data) {
  (void)data;
  s_lap_flash_active = false;
  s_lap_flash_timer = NULL;
  prv_update_layers();
}

static void prv_trigger_lap_flash(void) {
  s_lap_flash_active = true;
  if (s_lap_flash_timer) {
    app_timer_cancel(s_lap_flash_timer);
  }
  s_lap_flash_timer = app_timer_register(300, prv_lap_flash_timer_cb, NULL);
  prv_update_layers();
}

// ==================== State-Based Colors ====================

static void prv_update_ui_state_color(void) {
  UIStateColor new_state;
  switch (s_state) {
    case RUN_STATE_PAUSED:
      new_state = UI_STATE_PAUSED;
      break;
    case RUN_STATE_SUMMARY:
      new_state = UI_STATE_FINISHED;
      break;
    case RUN_STATE_RUNNING:
    case RUN_STATE_IDLE:
    default:
      new_state = UI_STATE_RUNNING;
      break;
  }
  if (new_state != s_ui_state_color) {
    s_ui_state_color = new_state;
    prv_mark_chrome_dirty();
  }
}

static GColor prv_get_state_color(void) {
#ifdef PBL_COLOR
  switch (s_ui_state_color) {
    case UI_STATE_PAUSED:
      return GColorChromeYellow;  // Yellow for paused
    case UI_STATE_FINISHED:
      return GColorVividCerulean; // Blue for finished
    case UI_STATE_RUNNING:
    default:
      return GColorJaegerGreen;   // Green for running
  }
#else
  // On B&W, use white for all states
  return GColorWhite;
#endif
}

static GColor prv_get_state_color_dim(void) {
#ifdef PBL_COLOR
  switch (s_ui_state_color) {
    case UI_STATE_PAUSED:
      return GColorYellow;
    case UI_STATE_FINISHED:
      return GColorBlue;
    case UI_STATE_RUNNING:
    default:
      return GColorGreen;
  }
#else
  return GColorWhite;
#endif
}

// ==================== Animation Helpers ====================

// Note: Animation callbacks defined below where needed

static void prv_animate_page_transition(Page new_page, bool slide_left) {
  // For now, simple fade effect by marking layers dirty
  // Full slide animation requires more complex multi-layer management
  // which can be added in a future iteration
  if (new_page == s_page) return;
  
  s_target_page = new_page;
  s_page = new_page;
  prv_update_layers();
  
  // Visual feedback via dots animation
  prv_mark_chrome_dirty();
  
  // Brief vibration feedback on page change
  static int64_t last_page_change = 0;
  int64_t now = prv_now_ms();
  if (now - last_page_change > 200) { // Debounce
    vibes_short_pulse();
    last_page_change = now;
  }
}

// ==================== Pebble Design System Colors ====================

#ifdef PBL_COLOR
  // Primary accent colors per Pebble Design System
  #define COLOR_PRIMARY GColorVividCerulean
  #define COLOR_SECONDARY GColorChromeYellow  
  #define COLOR_TERTIARY GColorJaegerGreen
  #define COLOR_BG_CARD GColorDarkGray
  #define COLOR_TEXT_PRIMARY GColorWhite
  #define COLOR_TEXT_SECONDARY GColorLightGray
  #define COLOR_STATUS_BAR GColorBlack
#else
  // Diorite (Pebble 2) B&W optimized - high contrast
  #define COLOR_PRIMARY GColorWhite
  #define COLOR_SECONDARY GColorWhite
  #define COLOR_TERTIARY GColorWhite
  #define COLOR_BG_CARD GColorBlack
  #define COLOR_TEXT_PRIMARY GColorWhite
  #define COLOR_TEXT_SECONDARY GColorWhite
  #define COLOR_STATUS_BAR GColorBlack
#endif

// ==================== Platform-Specific Layout ====================
// BIG NUMBERS DESIGN: Massive time, minimal everything else
// Pebble 2 (Diorite): 144x168, B&W, HR sensor
// Time 2 (Emery): 200x228, Color, HR sensor (larger screen)

#if defined(PBL_PLATFORM_EMERY)
  // Time 2 - huge screen, use biggest fonts
  #define LAYOUT_PADDING 12
  #define LAYOUT_TITLE_Y 2
  #define LAYOUT_PRIMARY_Y 28
  #define LAYOUT_SECONDARY_Y 96
  #define LAYOUT_TERTIARY_Y 134
  #define LAYOUT_FOOTER_Y 188
  #define FONT_PRIMARY FONT_KEY_ROBOTO_BOLD_SUBSET_49
  #define FONT_SECONDARY FONT_KEY_GOTHIC_24_BOLD
  #define FONT_TERTIARY FONT_KEY_GOTHIC_18_BOLD
  #define CARD_RADIUS 0
#elif defined(PBL_PLATFORM_DIORITE)
  // Pebble 2 - 144x168, maximize the time display
  #define LAYOUT_PADDING 4
  #define LAYOUT_TITLE_Y 0
  #define LAYOUT_PRIMARY_Y 20
  #define LAYOUT_SECONDARY_Y 78
  #define LAYOUT_TERTIARY_Y 108
  #define LAYOUT_FOOTER_Y 146
  #define FONT_PRIMARY FONT_KEY_BITHAM_42_BOLD
  #define FONT_SECONDARY FONT_KEY_GOTHIC_18_BOLD
  #define FONT_TERTIARY FONT_KEY_GOTHIC_14
  #define CARD_RADIUS 0
#elif defined(PBL_PLATFORM_CHALK)
  // Round display - center everything
  #define LAYOUT_PADDING 16
  #define LAYOUT_TITLE_Y 4
  #define LAYOUT_PRIMARY_Y 32
  #define LAYOUT_SECONDARY_Y 100
  #define LAYOUT_TERTIARY_Y 138
  #define LAYOUT_FOOTER_Y 188
  #define FONT_PRIMARY FONT_KEY_ROBOTO_BOLD_SUBSET_49
  #define FONT_SECONDARY FONT_KEY_GOTHIC_24_BOLD
  #define FONT_TERTIARY FONT_KEY_GOTHIC_18_BOLD
  #define CARD_RADIUS 0
#else
  // Basalt and others
  #define LAYOUT_PADDING 6
  #define LAYOUT_TITLE_Y 0
  #define LAYOUT_PRIMARY_Y 22
  #define LAYOUT_SECONDARY_Y 84
  #define LAYOUT_TERTIARY_Y 114
  #define LAYOUT_FOOTER_Y 150
  #define FONT_PRIMARY FONT_KEY_BITHAM_42_BOLD
  #define FONT_SECONDARY FONT_KEY_GOTHIC_18_BOLD
  #define FONT_TERTIARY FONT_KEY_GOTHIC_14
  #define CARD_RADIUS 0
#endif

static int64_t prv_now_ms(void) {
  time_t sec;
  uint16_t ms;
  time_ms(&sec, &ms);
  return ((int64_t)sec * 1000) + ms;
}

static double prv_deg2rad(double deg) {
  return deg * 3.14159265359 / 180.0;
}

static double prv_haversine(double lat1, double lon1, double lat2, double lon2) {
  const double R = 6371000.0;
  double dlat = prv_deg2rad(lat2 - lat1);
  double dlon = prv_deg2rad(lon2 - lon1);
  double a = sin(dlat / 2) * sin(dlat / 2) + cos(prv_deg2rad(lat1)) * cos(prv_deg2rad(lat2)) * sin(dlon / 2) * sin(dlon / 2);
  double c = 2 * atan2(sqrt(a), sqrt(1 - a));
  return R * c;
}

static void prv_send_cmd(BridgeCommand cmd) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
    return;
  }
  dict_write_int32(iter, MESSAGE_KEY_KEY_CMD, (int32_t)cmd);
  dict_write_end(iter);
  app_message_outbox_send();
}

static void prv_reset_session(void) {
  s_state = RUN_STATE_IDLE;
  s_elapsed_ms = 0;
  s_paused_ms = 0;
  s_lap_anchor_ms = 0;
  s_lap_anchor_distance_m = 0;
  s_distance_m = 0;
  s_inst_speed_mps = 0;
  s_lap_count = 0;
  s_last_fix.valid = false;
  s_auto_lap_distance_m = s_use_metric ? 1000.0 : 1609.34;
  prv_reset_thresholds();
  s_start_epoch_ms = 0;
  s_last_tick_ms = prv_now_ms();
  s_page = PAGE_MAIN;
  s_last_movement_ms = 0;
}

static void prv_format_time_ms(int32_t ms, char *out, size_t len) {
  int32_t total_sec = ms / 1000;
  int hours = total_sec / 3600;
  int minutes = (total_sec % 3600) / 60;
  int seconds = total_sec % 60;
  if (hours > 0) {
    snprintf(out, len, "%d:%02d:%02d", hours, minutes, seconds);
  } else {
    snprintf(out, len, "%02d:%02d", minutes, seconds);
  }
}

// Format meters to km/mi with two decimals using integer math (no %f)
static void prv_format_unit_distance_2dp(double meters, char *out, size_t len) {
  double units = s_use_metric ? (meters / 1000.0) : (meters / 1609.34);
  int hundredths = (int) ((units >= 0.0) ? (units * 100.0 + 0.5) : (units * 100.0 - 0.5));
  if (hundredths < 0) hundredths = -hundredths; // ensure non-negative display
  int whole = hundredths / 100;
  int frac = hundredths % 100;
  const char *label = s_use_metric ? "km" : "mi";
  snprintf(out, len, "%d.%02d %s", whole, frac, label);
}

static void prv_format_pace(double distance_m, int32_t elapsed_ms, char *out, size_t len) {
  if (distance_m <= 0.1) {
    snprintf(out, len, "--:--");
    return;
  }
  double unit_m = s_use_metric ? 1000.0 : 1609.34;
  double pace_sec = ((double)elapsed_ms / 1000.0) / (distance_m / unit_m);
  int total_sec = (int)pace_sec;
  int minutes = total_sec / 60;
  int seconds = total_sec % 60;
  snprintf(out, len, "%d:%02d /%s", minutes, seconds, s_use_metric ? "km" : "mi");
}

static void prv_reset_thresholds(void) {
  double unit_m = s_use_metric ? 1000.0 : 1609.34;
  s_alert_step_m = unit_m;
  s_next_alert_m = (floor(s_distance_m / unit_m) + 1) * unit_m;
  if (s_next_alert_m < unit_m) {
    s_next_alert_m = unit_m;
  }

  if (s_sport == SPORT_SWIM) {
    if (s_pool_length_cm <= 0) {
      s_pool_length_cm = s_use_metric ? 2500 : 2290; // 25m or ~25yd
    }
    s_auto_lap_distance_m = s_pool_length_cm / 100.0;
  } else {
    if (s_auto_lap_distance_m <= 0.0) {
      s_auto_lap_distance_m = unit_m;
    }
  }
  s_next_auto_lap_m = (floor(s_distance_m / s_auto_lap_distance_m) + 1) * s_auto_lap_distance_m;
  if (s_next_auto_lap_m < s_auto_lap_distance_m) {
    s_next_auto_lap_m = s_auto_lap_distance_m;
  }
}

static void prv_apply_settings(DictionaryIterator *iter) {
  Tuple *units_t = dict_find(iter, MESSAGE_KEY_KEY_UNITS);
  Tuple *auto_lap_en_t = dict_find(iter, MESSAGE_KEY_KEY_AUTO_LAP_ENABLED);
  Tuple *auto_lap_dist_t = dict_find(iter, MESSAGE_KEY_KEY_AUTO_LAP_DIST);
  Tuple *auto_run_t = dict_find(iter, MESSAGE_KEY_KEY_AUTO_RUN);
  Tuple *auto_stop_t = dict_find(iter, MESSAGE_KEY_KEY_AUTO_STOP);
  Tuple *dist_alerts_t = dict_find(iter, MESSAGE_KEY_KEY_DISTANCE_ALERTS);
  Tuple *sport_t = dict_find(iter, MESSAGE_KEY_KEY_SPORT);
  Tuple *pool_len_t = dict_find(iter, MESSAGE_KEY_KEY_POOL_LENGTH);

  if (units_t) {
    // Expect 1 = metric, 0 = imperial
    s_use_metric = (units_t->value->int32 != 0);
  }
  if (auto_lap_en_t) {
    s_auto_lap = auto_lap_en_t->value->int32 != 0;
  }
  if (auto_lap_dist_t) {
    int32_t val = auto_lap_dist_t->value->int32;
    if (val > 0) {
      s_auto_lap_distance_m = (double)val;
    }
  }
  if (auto_run_t) {
    s_auto_run = auto_run_t->value->int32 != 0;
  }
  if (auto_stop_t) {
    s_auto_stop = auto_stop_t->value->int32 != 0;
  }
  if (dist_alerts_t) {
    s_distance_alerts = dist_alerts_t->value->int32 != 0;
  }
  if (sport_t) {
    s_sport = (sport_t->value->int32 == 1) ? SPORT_SWIM : SPORT_RUN;
    if (s_sport == SPORT_SWIM) {
      s_auto_run = false;
      s_auto_stop = false;
    }
  }
  if (pool_len_t) {
    int32_t cm = pool_len_t->value->int32;
    if (cm > 0) {
      s_pool_length_cm = cm;
    }
  }

  prv_reset_thresholds();
  prv_update_layers();
}

static const char *prv_page_label(Page page) {
  switch (page) {
    case PAGE_MAIN:
      return "Main";
    case PAGE_LAP:
      return (s_sport == SPORT_SWIM) ? "Details" : "Lap";
    case PAGE_HR:
      return "HR";
    case PAGE_SPLITS:
      return "Splits";
    default:
      return "";
  }
}

static void prv_mark_chrome_dirty(void) {
  if (s_divider_layer) {
    layer_mark_dirty(s_divider_layer);
  }
  if (s_dots_layer) {
    layer_mark_dirty(s_dots_layer);
  }
}

static void prv_update_layers(void) {
  // Handle text colors during lap flash
  if (s_lap_flash_active) {
#ifdef PBL_COLOR
    text_layer_set_text_color(s_primary_layer, GColorBlack);
    text_layer_set_text_color(s_secondary_layer, GColorBlack);
    text_layer_set_text_color(s_tertiary_layer, GColorBlack);
    text_layer_set_text_color(s_title_layer, GColorBlack);
#else
    text_layer_set_text_color(s_primary_layer, GColorBlack);
    text_layer_set_text_color(s_secondary_layer, GColorBlack);
    text_layer_set_text_color(s_tertiary_layer, GColorBlack);
    text_layer_set_text_color(s_title_layer, GColorBlack);
#endif
  } else {
    // Normal text colors
#ifdef PBL_COLOR
    text_layer_set_text_color(s_primary_layer, COLOR_TEXT_PRIMARY);
    text_layer_set_text_color(s_secondary_layer, COLOR_SECONDARY);
    text_layer_set_text_color(s_tertiary_layer, COLOR_TERTIARY);
    text_layer_set_text_color(s_title_layer, COLOR_SECONDARY);
#else
    text_layer_set_text_color(s_primary_layer, GColorWhite);
    text_layer_set_text_color(s_secondary_layer, GColorWhite);
    text_layer_set_text_color(s_tertiary_layer, GColorWhite);
    text_layer_set_text_color(s_title_layer, GColorWhite);
#endif
  }

  const char *page_label = prv_page_label(s_page);
  switch (s_state) {
    case RUN_STATE_RUNNING:
      if (s_sport == SPORT_SWIM) {
        // Show lap count in title for swim mode
        snprintf(s_buf_title, sizeof(s_buf_title), "SWIM %d - %s", s_lap_count, page_label);
      } else {
        snprintf(s_buf_title, sizeof(s_buf_title), "RUN - %s", page_label);
      }
      break;
    case RUN_STATE_PAUSED:
      snprintf(s_buf_title, sizeof(s_buf_title), "PAUSED - %s", page_label);
      break;
    case RUN_STATE_SUMMARY:
      snprintf(s_buf_title, sizeof(s_buf_title), "SUMMARY");
      break;
    default:
      snprintf(s_buf_title, sizeof(s_buf_title), "%s", s_sport == SPORT_SWIM ? "SWIM" : "RUN");
      break;
  }
  text_layer_set_text(s_title_layer, s_buf_title);

  int32_t lap_elapsed_ms = s_elapsed_ms - s_lap_anchor_ms;
  double lap_distance_m = s_distance_m - s_lap_anchor_distance_m;

  if (s_sport == SPORT_SWIM) {
    // Two pages only: Main and Details (use PAGE_LAP slot)
    if (s_page == PAGE_MAIN) {
      // Main: Total time prominently, with lap count as secondary focus
      prv_format_time_ms(s_elapsed_ms, s_buf_primary, sizeof(s_buf_primary));
      // Show lap count more prominently when swimming
      if (s_lap_count == 0) {
        snprintf(s_buf_secondary, sizeof(s_buf_secondary), "Ready to swim");
      } else if (s_lap_count == 1) {
        snprintf(s_buf_secondary, sizeof(s_buf_secondary), "1 lap completed");
      } else {
        snprintf(s_buf_secondary, sizeof(s_buf_secondary), "%d laps completed", s_lap_count);
      }
      // Distance in pool-appropriate units
      if (s_use_metric) {
        int m = (int)(s_distance_m + 0.5);
        snprintf(s_buf_tertiary, sizeof(s_buf_tertiary), "%d m", m);
      } else {
        int yd = (int)((s_distance_m / 0.9144) + 0.5);
        snprintf(s_buf_tertiary, sizeof(s_buf_tertiary), "%d yd", yd);
      }
    } else { // Details (Lap page for swim)
      // Show last lap time and pace per 100m/yd
      if (s_lap_count > 0) {
        Lap last = s_laps[s_lap_count - 1];
        prv_format_time_ms(last.elapsed_ms, s_buf_primary, sizeof(s_buf_primary));
        // Pace per 100m/yd
        double segment = s_use_metric ? 100.0 : 91.44;
        if (last.distance_m > 0.0 && last.elapsed_ms > 0) {
          double pace_sec = ((double)last.elapsed_ms / 1000.0) / (last.distance_m / segment);
          int total_sec = (int)pace_sec;
          int minutes = total_sec / 60;
          int seconds = total_sec % 60;
          snprintf(s_buf_tertiary, sizeof(s_buf_tertiary), "%d:%02d /%s", minutes, seconds, s_use_metric ? "100m" : "100yd");
        } else {
          snprintf(s_buf_tertiary, sizeof(s_buf_tertiary), "--:--");
        }
        // Lap count in secondary (more useful than pool length here)
        snprintf(s_buf_secondary, sizeof(s_buf_secondary), "Lap %d of %d", s_lap_count, s_lap_count);
      } else {
        // Empty state: show pool length prominently, encouraging message
        if (s_use_metric) {
          int m = (int)((s_pool_length_cm + 50) / 100);
          snprintf(s_buf_primary, sizeof(s_buf_primary), "%d m", m);
        } else {
          int yd = (int)((s_pool_length_cm / 91.44) + 0.5);
          snprintf(s_buf_primary, sizeof(s_buf_primary), "%d yd", yd);
        }
        snprintf(s_buf_secondary, sizeof(s_buf_secondary), "Pool length");
        snprintf(s_buf_tertiary, sizeof(s_buf_tertiary), "Press UP for lap");
      }
    }
  } else {
    switch (s_page) {
      case PAGE_MAIN:
        prv_format_time_ms(s_elapsed_ms, s_buf_primary, sizeof(s_buf_primary));
        prv_format_unit_distance_2dp(s_distance_m, s_buf_secondary, sizeof(s_buf_secondary));
        prv_format_pace(s_distance_m, s_elapsed_ms, s_buf_tertiary, sizeof(s_buf_tertiary));
        break;
      case PAGE_LAP:
        prv_format_time_ms(lap_elapsed_ms, s_buf_primary, sizeof(s_buf_primary));
        prv_format_unit_distance_2dp(lap_distance_m, s_buf_secondary, sizeof(s_buf_secondary));
        prv_format_pace(lap_distance_m, lap_elapsed_ms, s_buf_tertiary, sizeof(s_buf_tertiary));
        break;
      case PAGE_HR:
        // Enhanced HR display for Pebble 2 and Time 2 (both have HR sensors)
        {
          int32_t hr = s_heart_rate_bpm;
          if (hr > 0) {
            snprintf(s_buf_primary, sizeof(s_buf_primary), "%ld", (long)hr);
            snprintf(s_buf_secondary, sizeof(s_buf_secondary), "bpm");
            
            // Simple HR zone indication based on common max HR formula (220 - age, assuming ~30yo => ~190 max)
            const char *zone;
            if (hr < 114) zone = "Recovery";
            else if (hr < 133) zone = "Fat Burn";
            else if (hr < 152) zone = "Cardio";
            else if (hr < 171) zone = "Peak";
            else zone = "Max";
            snprintf(s_buf_tertiary, sizeof(s_buf_tertiary), "%s", zone);
          } else {
            snprintf(s_buf_primary, sizeof(s_buf_primary), "--");
            snprintf(s_buf_secondary, sizeof(s_buf_secondary), "bpm");
            snprintf(s_buf_tertiary, sizeof(s_buf_tertiary), "No HR signal");
          }
        }
        break;
      case PAGE_SPLITS:
        if (s_lap_count > 0) {
          Lap last = s_laps[s_lap_count - 1];
          prv_format_time_ms(last.elapsed_ms, s_buf_primary, sizeof(s_buf_primary));
          snprintf(s_buf_secondary, sizeof(s_buf_secondary), "Lap %d", s_lap_count);
          prv_format_unit_distance_2dp(last.distance_m, s_buf_tertiary, sizeof(s_buf_tertiary));
        } else {
          // Better empty state for splits page
          snprintf(s_buf_primary, sizeof(s_buf_primary), "Ready");
          snprintf(s_buf_secondary, sizeof(s_buf_secondary), "No splits yet");
          snprintf(s_buf_tertiary, sizeof(s_buf_tertiary), "UP to mark lap");
        }
        break;
      default:
        break;
    }
  }

  text_layer_set_text(s_primary_layer, s_buf_primary);
  text_layer_set_text(s_secondary_layer, s_buf_secondary);
  text_layer_set_text(s_tertiary_layer, s_buf_tertiary);
  
  // Update footer hint based on state
  if (s_state == RUN_STATE_SUMMARY) {
    text_layer_set_text(s_footer_layer, "SELECT: New workout");
  }
  
  prv_mark_chrome_dirty();
}

static void prv_check_distance_alert(void) {
  if (!s_distance_alerts || s_state != RUN_STATE_RUNNING) {
    return;
  }
  if (s_distance_m >= s_next_alert_m) {
    vibes_short_pulse();
    s_next_alert_m += s_alert_step_m;
  }
}

static void prv_add_lap(void);

static void prv_check_auto_lap(void) {
  if (!s_auto_lap || s_state != RUN_STATE_RUNNING) {
    return;
  }
  while (s_distance_m >= s_next_auto_lap_m && s_lap_count < MAX_LAPS) {
    prv_add_lap();
    s_next_auto_lap_m += s_auto_lap_distance_m;
  }
}

static void prv_add_lap(void) {
  if (s_state != RUN_STATE_RUNNING) {
    return;
  }
  if (s_sport == SPORT_SWIM) {
    // For pool swims, increment distance by pool length on each lap.
    s_distance_m += s_pool_length_cm / 100.0;
  }
  if (s_lap_count < MAX_LAPS) {
    Lap lap = {
      .elapsed_ms = s_elapsed_ms - s_lap_anchor_ms,
      .distance_m = s_distance_m - s_lap_anchor_distance_m
    };
    s_laps[s_lap_count++] = lap;
  }
  s_lap_anchor_ms = s_elapsed_ms;
  s_lap_anchor_distance_m = s_distance_m;
  
  // Lap feedback - flash + vibration
  prv_trigger_lap_flash();
  vibes_short_pulse();
  text_layer_set_text(s_footer_layer, "Lap marked!");
}

static void prv_update_elapsed(void) {
  int64_t now = prv_now_ms();
  if (s_state == RUN_STATE_RUNNING) {
    int32_t delta = (int32_t)(now - s_last_tick_ms);
    if (delta > 0) {
      s_elapsed_ms += delta;
    }
  }
  s_last_tick_ms = now;
}

static void prv_tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  prv_update_elapsed();
  if (s_state == RUN_STATE_RUNNING && s_auto_stop && s_last_movement_ms > 0) {
    int64_t now = prv_now_ms();
    if ((now - s_last_movement_ms) > 15000) {
      // Auto-stop: end without confirmation since it's automatic
      prv_update_elapsed();
      s_state = RUN_STATE_SUMMARY;
      prv_send_cmd(CMD_STOP);
      prv_send_summary();
      prv_update_layers();
      vibes_double_pulse();
      text_layer_set_text(s_footer_layer, "Auto-stopped");
    }
  }
  // Update heart rate once per second (if available)
#ifdef PBL_HEALTH
  {
    HealthValue hr = health_service_peek_current_value(HealthMetricHeartRateBPM);
    s_heart_rate_bpm = (hr > 0) ? (int32_t)hr : -1;
  }
#endif
  prv_update_layers();
}

static void prv_start_run(void) {
  prv_reset_session();
  s_state = RUN_STATE_RUNNING;
  s_start_epoch_ms = prv_now_ms();
  s_last_tick_ms = s_start_epoch_ms;
  s_lap_anchor_ms = 0;
  s_lap_anchor_distance_m = 0;
  prv_send_cmd(CMD_START);
  prv_update_ui_state_color();
  prv_update_layers();
  
  // Success feedback
  vibes_short_pulse();
  text_layer_set_text(s_footer_layer, "Workout started!");
}

static void prv_pause_run(void) {
  if (s_state != RUN_STATE_RUNNING) {
    return;
  }
  prv_update_elapsed();
  s_state = RUN_STATE_PAUSED;
  s_pause_started_ms = prv_now_ms();
  prv_send_cmd(CMD_PAUSE);
  prv_update_ui_state_color();
  prv_update_layers();
  
  // Pause feedback - double short pulse
  vibes_double_pulse();
  text_layer_set_text(s_footer_layer, "Workout paused");
}

static void prv_resume_run(void) {
  if (s_state != RUN_STATE_PAUSED) {
    return;
  }
  int64_t now = prv_now_ms();
  s_paused_ms += (int32_t)(now - s_pause_started_ms);
  s_last_tick_ms = now;
  s_state = RUN_STATE_RUNNING;
  prv_send_cmd(CMD_RESUME);
  prv_update_ui_state_color();
  prv_update_layers();
  
  // Resume feedback
  vibes_short_pulse();
  text_layer_set_text(s_footer_layer, "Workout resumed!");
}

static void prv_send_summary(void) {
  if (s_elapsed_ms == 0) {
    return;
  }
  char summary[96];
  int32_t total_sec = s_elapsed_ms / 1000;
  char dist_str[24];
  prv_format_unit_distance_2dp(s_distance_m, dist_str, sizeof(dist_str));
  snprintf(summary, sizeof(summary), "time:%ld dist:%s laps:%d", (long)total_sec, dist_str, s_lap_count);

  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
    return;
  }
  dict_write_cstring(iter, MESSAGE_KEY_KEY_SUMMARY, summary);
  dict_write_end(iter);
  app_message_outbox_send();
}

static void prv_back_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_state == RUN_STATE_RUNNING || s_state == RUN_STATE_PAUSED) {
    prv_end_run();
  }
}

static void prv_back_single_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_state == RUN_STATE_SUMMARY) {
    // Exit app from summary screen
    window_stack_pop_all(true);
  } else if (s_state == RUN_STATE_IDLE) {
    text_layer_set_text(s_footer_layer, "Hold SELECT to change sport");
  } else {
    text_layer_set_text(s_footer_layer, "Hold back to end workout");
  }
}

static void prv_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_state == RUN_STATE_IDLE || s_state == RUN_STATE_SUMMARY) {
    prv_start_run();
  } else if (s_state == RUN_STATE_RUNNING) {
    prv_pause_run();
  } else if (s_state == RUN_STATE_PAUSED) {
    prv_resume_run();
  }
}

static void prv_select_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_state == RUN_STATE_RUNNING) {
    text_layer_set_text(s_footer_layer, "Pause to change mode");
    return;
  }
  prv_show_sport_selector();
}

static void prv_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  // Long press on UP navigates back a page
  // Short press marks a lap (or shows message in swim mode)
  if (s_sport == SPORT_SWIM) {
    text_layer_set_text(s_footer_layer, "Swim laps are automatic");
    // Brief vibration feedback
    vibes_short_pulse();
    return;
  }
  prv_add_lap();
  prv_update_layers();
}

static void prv_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_anim_state != ANIM_STATE_NONE) return;
  
  if (s_sport == SPORT_SWIM) {
    Page new_page = (s_page == PAGE_MAIN) ? PAGE_LAP : PAGE_MAIN;
    prv_animate_page_transition(new_page, true);
    s_page = new_page;
  } else {
    Page new_page = (Page)((s_page + 1) % PAGE_COUNT);
    prv_animate_page_transition(new_page, true);
    s_page = new_page;
  }
  prv_update_layers();
}

// Up click now goes to previous page (reverse navigation)
static void prv_up_page_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_anim_state != ANIM_STATE_NONE) return;
  
  if (s_sport == SPORT_SWIM) {
    Page new_page = (s_page == PAGE_MAIN) ? PAGE_LAP : PAGE_MAIN;
    prv_animate_page_transition(new_page, false);
    s_page = new_page;
  } else {
    Page new_page = (s_page == 0) ? (Page)(PAGE_COUNT - 1) : (Page)(s_page - 1);
    prv_animate_page_transition(new_page, false);
    s_page = new_page;
  }
  prv_update_layers();
}

// Button timing optimized for Pebble 2 and Time 2
// Pebble 2 has very clicky buttons, slightly longer press feels better
#if defined(PBL_PLATFORM_DIORITE)
  #define LONG_PRESS_MS 800
  #define PAGE_NAV_PRESS_MS 600
#else
  #define LONG_PRESS_MS 700
  #define PAGE_NAV_PRESS_MS 500
#endif

static void prv_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, LONG_PRESS_MS, prv_select_long_click_handler, NULL);
  window_single_click_subscribe(BUTTON_ID_UP, prv_up_click_handler);
  window_long_click_subscribe(BUTTON_ID_UP, PAGE_NAV_PRESS_MS, prv_up_page_handler, NULL);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_down_click_handler);
  window_single_click_subscribe(BUTTON_ID_BACK, prv_back_single_click_handler);
  window_long_click_subscribe(BUTTON_ID_BACK, LONG_PRESS_MS, prv_back_long_click_handler, NULL);
}

static void prv_process_fix(double lat, double lon, uint16_t accuracy_m, int32_t speed_cms) {
  if (s_sport == SPORT_SWIM) {
    // Ignore GPS for swim; distance is lap-based.
    return;
  }

  if (accuracy_m > 0 && accuracy_m > 50) {
    return;
  }

  if (s_last_fix.valid) {
    double delta_m = prv_haversine(s_last_fix.lat, s_last_fix.lon, lat, lon);
    bool moving = (delta_m > 1.0) || (speed_cms > 50);
    if (moving) {
      s_last_movement_ms = prv_now_ms();
    }

    if (s_state == RUN_STATE_IDLE && s_auto_run && moving) {
      prv_start_run();
    }

    if (s_state != RUN_STATE_IDLE && delta_m > 0 && delta_m < 200) {
      s_distance_m += delta_m;
    }
  }

  if (!s_last_fix.valid && speed_cms > 50) {
    s_last_movement_ms = prv_now_ms();
    if (s_state == RUN_STATE_IDLE && s_auto_run) {
      prv_start_run();
    }
  }

  s_last_fix.lat = lat;
  s_last_fix.lon = lon;
  s_last_fix.accuracy_m = accuracy_m;
  s_last_fix.valid = true;

  if (speed_cms > 0) {
    s_inst_speed_mps = speed_cms / 100.0;
  }

  prv_check_distance_alert();
  prv_check_auto_lap();
  prv_update_layers();
}

static void prv_inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *lat_t = dict_find(iter, MESSAGE_KEY_KEY_LAT);
  Tuple *lon_t = dict_find(iter, MESSAGE_KEY_KEY_LON);
  Tuple *acc_t = dict_find(iter, MESSAGE_KEY_KEY_ACCURACY);
  Tuple *speed_t = dict_find(iter, MESSAGE_KEY_KEY_SPEED);
  Tuple *status_t = dict_find(iter, MESSAGE_KEY_KEY_STATUS);
  Tuple *error_t = dict_find(iter, MESSAGE_KEY_KEY_ERROR);

  // Apply settings if present
  if (dict_find(iter, MESSAGE_KEY_KEY_UNITS) ||
      dict_find(iter, MESSAGE_KEY_KEY_AUTO_LAP_ENABLED) ||
      dict_find(iter, MESSAGE_KEY_KEY_AUTO_LAP_DIST) ||
      dict_find(iter, MESSAGE_KEY_KEY_AUTO_RUN) ||
      dict_find(iter, MESSAGE_KEY_KEY_AUTO_STOP) ||
      dict_find(iter, MESSAGE_KEY_KEY_DISTANCE_ALERTS) ||
      dict_find(iter, MESSAGE_KEY_KEY_SPORT) ||
      dict_find(iter, MESSAGE_KEY_KEY_POOL_LENGTH)) {
    prv_apply_settings(iter);
  }

  if (status_t) {
    snprintf(s_buf_footer, sizeof(s_buf_footer), "%s", status_t->value->cstring);
    text_layer_set_text(s_footer_layer, s_buf_footer);
  }
  if (error_t) {
    snprintf(s_buf_footer, sizeof(s_buf_footer), "ERR: %s", error_t->value->cstring);
    text_layer_set_text(s_footer_layer, s_buf_footer);
  }

  if (lat_t && lon_t) {
    double lat = lat_t->value->int32 / 1000000.0;
    double lon = lon_t->value->int32 / 1000000.0;
    uint16_t acc = acc_t ? (uint16_t)acc_t->value->int32 : 0;
    int32_t speed_cms = speed_t ? speed_t->value->int32 : 0;
    prv_process_fix(lat, lon, acc, speed_cms);
  }
}

static void prv_inbox_dropped(AppMessageResult reason, void *context) {
  snprintf(s_buf_footer, sizeof(s_buf_footer), "Inbox drop %d", reason);
  text_layer_set_text(s_footer_layer, s_buf_footer);
}

static void prv_outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  snprintf(s_buf_footer, sizeof(s_buf_footer), "Outbox fail %d", reason);
  text_layer_set_text(s_footer_layer, s_buf_footer);
}

static void prv_outbox_sent(DictionaryIterator *iter, void *context) {
  text_layer_set_text(s_footer_layer, "");
}

static void prv_draw_divider(Layer *layer, GContext *ctx) {
  // BIG NUMBERS: No divider needed, keeping function for compatibility
  (void)ctx;
  (void)layer;
}

static void prv_draw_dots(Layer *layer, GContext *ctx) {
  // BIG NUMBERS: Minimal dots - just small squares with state color
  GRect b = layer_get_bounds(layer);
  int count = (s_sport == SPORT_SWIM) ? 2 : PAGE_COUNT;
  int16_t spacing = 10;
  int16_t size = 3; // small squares
  int total = (count - 1) * spacing;
  int16_t start_x = (b.size.w - total) / 2;
  int16_t y = b.size.h / 2 - size / 2;
  
  for (int i = 0; i < count; i++) {
    int16_t x = start_x + i * spacing - size / 2;
    bool is_active = (i == s_page);
    
    GRect dot_rect = GRect(x, y, size, size);
    
#ifdef PBL_COLOR
    if (is_active) {
      graphics_context_set_fill_color(ctx, prv_get_state_color());
      graphics_fill_rect(ctx, dot_rect, 0, GCornerNone);
    } else {
      graphics_context_set_fill_color(ctx, GColorDarkGray);
      graphics_fill_rect(ctx, dot_rect, 0, GCornerNone);
    }
#else
    if (is_active) {
      graphics_context_set_fill_color(ctx, GColorWhite);
      graphics_fill_rect(ctx, dot_rect, 0, GCornerNone);
    } else {
      // Inactive barely visible on B&W
      graphics_context_set_stroke_color(ctx, GColorWhite);
      graphics_context_set_stroke_width(ctx, 1);
      graphics_draw_rect(ctx, dot_rect);
    }
#endif
  }
}

static void prv_draw_bg(Layer *layer, GContext *ctx) {
  // BIG NUMBERS DESIGN: Clean background with state colors
  GRect b = layer_get_bounds(layer);
  
  // Lap flash effect - invert the screen briefly
  if (s_lap_flash_active) {
#ifdef PBL_COLOR
    graphics_context_set_fill_color(ctx, prv_get_state_color());
    graphics_fill_rect(ctx, b, 0, GCornerNone);
#else
    // B&W: fill with white
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, b, 0, GCornerNone);
#endif
    return; // Don't draw anything else during flash
  }
  
  // Subtle separator line under the big time display - uses state color
  int16_t line_y = LAYOUT_SECONDARY_Y - 6;
  
#ifdef PBL_COLOR
  graphics_context_set_stroke_color(ctx, prv_get_state_color());
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_line(ctx, GPoint(20, line_y), GPoint(b.size.w - 20, line_y));
#else
  // B&W: even more minimal, short centered line
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_context_set_stroke_width(ctx, 1);
  int16_t center_x = b.size.w / 2;
  graphics_draw_line(ctx, GPoint(center_x - 25, line_y), GPoint(center_x + 25, line_y));
#endif
}

static void prv_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  
  // Use platform-specific layout defines
  int16_t pad = PBL_IF_ROUND_ELSE(18, LAYOUT_PADDING);
  int16_t w = bounds.size.w - pad * 2;

  // Platform-optimized font selection
  s_font_title = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  s_font_primary = fonts_get_system_font(FONT_PRIMARY);
  s_font_secondary = fonts_get_system_font(FONT_SECONDARY);

  // Title - minimal, small, top-left (or center on round)
  s_title_layer = text_layer_create(GRect(pad, LAYOUT_TITLE_Y, w, 20));
  text_layer_set_background_color(s_title_layer, GColorClear);
  text_layer_set_text_color(s_title_layer, COLOR_SECONDARY);
  text_layer_set_font(s_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_title_layer, PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentCenter));
  text_layer_set_text(s_title_layer, "READY");
  layer_add_child(window_layer, text_layer_get_layer(s_title_layer));

  // No divider in Big Numbers design
  s_divider_layer = layer_create(GRect(0, 0, 0, 0));
  layer_set_update_proc(s_divider_layer, prv_draw_divider);
  layer_add_child(window_layer, s_divider_layer);

  // Card background layer (drawn behind text)
  s_bg_layer = layer_create(bounds);
  layer_set_update_proc(s_bg_layer, prv_draw_bg);
  layer_add_child(window_layer, s_bg_layer);

  // Primary metric - BIG TIME (massive, centered)
  s_primary_layer = text_layer_create(GRect(pad, LAYOUT_PRIMARY_Y, w, 56));
  text_layer_set_background_color(s_primary_layer, GColorClear);
#ifdef PBL_COLOR
  text_layer_set_text_color(s_primary_layer, COLOR_TEXT_PRIMARY);
#else
  text_layer_set_text_color(s_primary_layer, GColorWhite);
#endif
  text_layer_set_font(s_primary_layer, s_font_primary);
  text_layer_set_text_alignment(s_primary_layer, GTextAlignmentCenter);
  text_layer_set_text(s_primary_layer, "00:00");
  layer_add_child(window_layer, text_layer_get_layer(s_primary_layer));

  // Secondary metric - Distance (smaller, below time)
  s_secondary_layer = text_layer_create(GRect(pad, LAYOUT_SECONDARY_Y, w, 26));
  text_layer_set_background_color(s_secondary_layer, GColorClear);
  text_layer_set_text_color(s_secondary_layer, COLOR_SECONDARY);
  text_layer_set_font(s_secondary_layer, s_font_secondary);
  text_layer_set_text_alignment(s_secondary_layer, GTextAlignmentCenter);
  text_layer_set_text(s_secondary_layer, "0.00 km");
  layer_add_child(window_layer, text_layer_get_layer(s_secondary_layer));

  // Tertiary metric - Pace/HR (smallest, bottom)
  s_tertiary_layer = text_layer_create(GRect(pad, LAYOUT_TERTIARY_Y, w, 20));
  text_layer_set_background_color(s_tertiary_layer, GColorClear);
  text_layer_set_text_color(s_tertiary_layer, COLOR_TERTIARY);
  text_layer_set_font(s_tertiary_layer, fonts_get_system_font(FONT_TERTIARY));
  text_layer_set_text_alignment(s_tertiary_layer, GTextAlignmentCenter);
  text_layer_set_text(s_tertiary_layer, "--:--");
  layer_add_child(window_layer, text_layer_get_layer(s_tertiary_layer));

  // Footer - minimal hints
  s_footer_layer = text_layer_create(GRect(pad, LAYOUT_FOOTER_Y, w, 16));
  text_layer_set_background_color(s_footer_layer, GColorClear);
  text_layer_set_text_color(s_footer_layer, COLOR_TEXT_SECONDARY);
  text_layer_set_font(s_footer_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_footer_layer, GTextAlignmentCenter);
  text_layer_set_text(s_footer_layer, "");
  layer_add_child(window_layer, text_layer_get_layer(s_footer_layer));

  // Page indicator dots - above footer
  s_dots_layer = layer_create(GRect(0, LAYOUT_FOOTER_Y - 12, bounds.size.w, 8));
  layer_set_update_proc(s_dots_layer, prv_draw_dots);
  layer_add_child(window_layer, s_dots_layer);
}

static void prv_window_unload(Window *window) {
  text_layer_destroy(s_title_layer);
  text_layer_destroy(s_primary_layer);
  text_layer_destroy(s_secondary_layer);
  text_layer_destroy(s_tertiary_layer);
  text_layer_destroy(s_footer_layer);
  layer_destroy(s_divider_layer);
  layer_destroy(s_dots_layer);
  layer_destroy(s_bg_layer);
}

// ==================== Confirmation Dialog ====================
// Pebble Design System: Modal confirmation for destructive actions

static void (*s_confirm_callback)(bool) = NULL;

static void prv_confirm_draw_bg(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  
  // Platform-specific card sizing
#if defined(PBL_PLATFORM_EMERY)
  GRect card = GRect(16, 60, b.size.w - 32, 108);
#elif defined(PBL_PLATFORM_CHALK)
  GRect card = GRect(14, 50, b.size.w - 28, 96);
#else
  GRect card = GRect(10, 40, b.size.w - 20, 88);
#endif
  
  // Semi-transparent overlay
#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  
  // Modal card with accent border
  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_rect(ctx, card, 8, GCornersAll);
  graphics_context_set_stroke_color(ctx, COLOR_PRIMARY);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_round_rect(ctx, card, 8);
#else
  // Diorite (Pebble 2) B&W: High contrast invert style
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, card, 4, GCornersAll);
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_round_rect(ctx, card, 4);
#endif
}

static void prv_confirm_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  
  // Background overlay
  s_confirm_bg_layer = layer_create(b);
  layer_set_update_proc(s_confirm_bg_layer, prv_confirm_draw_bg);
  layer_add_child(root, s_confirm_bg_layer);
  
  // Platform-specific positioning
#if defined(PBL_PLATFORM_EMERY)
  int16_t title_y = 70;
  int16_t msg_y = 100;
  GFont msg_font = fonts_get_system_font(FONT_KEY_GOTHIC_18);
#elif defined(PBL_PLATFORM_CHALK)
  int16_t title_y = 58;
  int16_t msg_y = 86;
  GFont msg_font = fonts_get_system_font(FONT_KEY_GOTHIC_14);
#else
  int16_t title_y = 48;
  int16_t msg_y = 72;
  GFont msg_font = fonts_get_system_font(FONT_KEY_GOTHIC_14);
#endif
  
  // Title
  s_confirm_title_layer = text_layer_create(GRect(16, title_y, b.size.w - 32, 26));
  text_layer_set_background_color(s_confirm_title_layer, GColorClear);
  text_layer_set_text_color(s_confirm_title_layer, GColorWhite);
  text_layer_set_font(s_confirm_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_confirm_title_layer, GTextAlignmentCenter);
  text_layer_set_text(s_confirm_title_layer, "End Workout?");
  layer_add_child(root, text_layer_get_layer(s_confirm_title_layer));
  
  // Message
  s_confirm_msg_layer = text_layer_create(GRect(16, msg_y, b.size.w - 32, 48));
  text_layer_set_background_color(s_confirm_msg_layer, GColorClear);
  text_layer_set_text_color(s_confirm_msg_layer, GColorLightGray);
  text_layer_set_font(s_confirm_msg_layer, msg_font);
  text_layer_set_text_alignment(s_confirm_msg_layer, GTextAlignmentCenter);
  text_layer_set_text(s_confirm_msg_layer, "SELECT: Yes\nBACK: Cancel");
  layer_add_child(root, text_layer_get_layer(s_confirm_msg_layer));
  
  s_confirm_active = true;
}

static void prv_confirm_window_unload(Window *window) {
  text_layer_destroy(s_confirm_title_layer);
  text_layer_destroy(s_confirm_msg_layer);
  layer_destroy(s_confirm_bg_layer);
  window_destroy(s_confirm_window);
  s_confirm_window = NULL;
  s_confirm_active = false;
}

static void prv_confirm_select_handler(ClickRecognizerRef recognizer, void *context) {
  s_confirm_result = true;
  window_stack_remove(s_confirm_window, true);
  if (s_confirm_callback) {
    s_confirm_callback(true);
  }
}

static void prv_confirm_back_handler(ClickRecognizerRef recognizer, void *context) {
  s_confirm_result = false;
  window_stack_remove(s_confirm_window, true);
  if (s_confirm_callback) {
    s_confirm_callback(false);
  }
}

static void prv_confirm_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_confirm_select_handler);
  window_single_click_subscribe(BUTTON_ID_BACK, prv_confirm_back_handler);
}

static void prv_show_confirmation(const char *title, const char *message, void (*callback)(bool)) {
  if (s_confirm_window) return;
  
  s_confirm_callback = callback;
  s_confirm_window = window_create();
  window_set_background_color(s_confirm_window, GColorClear);
  window_set_click_config_provider(s_confirm_window, prv_confirm_click_config);
  window_set_window_handlers(s_confirm_window, (WindowHandlers){
    .load = prv_confirm_window_load,
    .unload = prv_confirm_window_unload,
  });
  
  // Store title/message for use in load
  // For simplicity, using static strings - in production would copy to buffers
  
  // Animated push for smooth transition
  window_stack_push(s_confirm_window, true);
}

// ==================== End Workout Confirmation ====================

static void prv_end_run(void) {
  prv_update_elapsed();
  
  // Show confirmation dialog instead of immediate stop
  prv_show_confirmation("End Workout?", "Your progress will be saved.", prv_end_run_confirmed);
}

static void prv_end_run_confirmed(bool confirmed) {
  if (!confirmed) {
    text_layer_set_text(s_footer_layer, "Workout continues");
    return;
  }
  
  s_state = RUN_STATE_SUMMARY;
  prv_send_cmd(CMD_STOP);
  prv_send_summary();
  prv_update_ui_state_color();
  prv_update_layers();
  vibes_double_pulse(); // Success feedback
}

// Sport selection menu
static uint16_t prv_menu_get_num_sections(MenuLayer *menu_layer, void *context) {
  return 1;
}

static uint16_t prv_menu_get_num_rows(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  return 2; // Run, Swim
}

static int16_t prv_menu_get_cell_height(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  // Larger cells on Emery for easier touch targeting
#if defined(PBL_PLATFORM_EMERY)
  return 56;
#elif defined(PBL_PLATFORM_CHALK)
  return 52;
#else
  return 44; // default for Diorite and others
#endif
}

static void prv_menu_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *context) {
  const char *title = (cell_index->row == 0) ? "Run" : "Swim";
  const char *subtitle = (cell_index->row == 0) ? "Outdoor running" : "Pool swim (laps)";
  menu_cell_basic_draw(ctx, cell_layer, title, subtitle, NULL);
}

static void prv_menu_select(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  s_sport = (cell_index->row == 1) ? SPORT_SWIM : SPORT_RUN;
  if (s_sport == SPORT_SWIM) {
    s_auto_run = false;
    s_auto_stop = false;
  }
  prv_reset_thresholds();
  prv_update_layers();
  
  // Animated removal for smooth transition
  window_stack_remove(s_sport_window, true);
  s_sport_window = NULL;
  
  // Haptic feedback on selection
  vibes_short_pulse();
}

static void prv_sport_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  
  // Pebble Design System menu styling
  s_menu_layer = menu_layer_create(b);
  
#ifdef PBL_COLOR
  menu_layer_set_normal_colors(s_menu_layer, GColorDarkGray, GColorWhite);
  menu_layer_set_highlight_colors(s_menu_layer, COLOR_PRIMARY, GColorWhite);
#else
  menu_layer_set_normal_colors(s_menu_layer, GColorWhite, GColorBlack);
  menu_layer_set_highlight_colors(s_menu_layer, GColorBlack, GColorWhite);
#endif
  
  MenuLayerCallbacks cbs = {
    .get_num_sections = prv_menu_get_num_sections,
    .get_num_rows = prv_menu_get_num_rows,
    .get_cell_height = prv_menu_get_cell_height,
    .draw_row = prv_menu_draw_row,
    .select_click = prv_menu_select,
  };
  menu_layer_set_callbacks(s_menu_layer, NULL, cbs);
  menu_layer_set_click_config_onto_window(s_menu_layer, window);
  layer_add_child(root, menu_layer_get_layer(s_menu_layer));
}

static void prv_sport_window_unload(Window *window) {
  menu_layer_destroy(s_menu_layer);
}

static void prv_show_sport_selector(void) {
  if (s_sport_window) return;
  s_sport_window = window_create();
  
#ifdef PBL_COLOR
  window_set_background_color(s_sport_window, GColorDarkGray);
#else
  window_set_background_color(s_sport_window, GColorWhite);
#endif
  
  window_set_window_handlers(s_sport_window, (WindowHandlers){
    .load = prv_sport_window_load,
    .unload = prv_sport_window_unload,
  });
  
  // Animated push for smooth transition
  window_stack_push(s_sport_window, true);
}

static void prv_init(void) {
  prv_reset_session();
  
  // Platform detection for optimized layout
  // Pebble 2 (Diorite): 144x168, B&W, HR sensor, clicky buttons
  // Time 2 (Emery): 200x228, Color, HR sensor, larger screen
#if defined(PBL_PLATFORM_DIORITE)
  const char *platform_name = "Pebble 2";
#elif defined(PBL_PLATFORM_EMERY)
  const char *platform_name = "Time 2";
#elif defined(PBL_PLATFORM_CHALK)
  const char *platform_name = "Time Round";
#elif defined(PBL_PLATFORM_BASALT)
  const char *platform_name = "Time";
#else
  const char *platform_name = "Pebble";
#endif
  
  // Pebble Design System: Dark background for color, black for B&W
#ifdef PBL_COLOR
  GColor bg_color = GColorBlack;
#else
  GColor bg_color = GColorBlack;
#endif

  s_window = window_create();
  window_set_background_color(s_window, bg_color);
  window_set_click_config_provider(s_window, prv_click_config_provider);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  
  // Push main window with animation
  window_stack_push(s_window, true);

  // Prompt sport selection on startup (animated)
  prv_show_sport_selector();

  app_message_register_inbox_received(prv_inbox_received);
  app_message_register_inbox_dropped(prv_inbox_dropped);
  app_message_register_outbox_failed(prv_outbox_failed);
  app_message_register_outbox_sent(prv_outbox_sent);
  app_message_open(512, 512);

  tick_timer_service_subscribe(SECOND_UNIT, prv_tick_handler);
  
  // Startup feedback - platform-specific pattern
#if defined(PBL_PLATFORM_EMERY)
  // Time 2: Double pulse for color HR watch
  vibes_double_pulse();
#elif defined(PBL_PLATFORM_DIORITE)
  // Pebble 2: Single short pulse for B&W HR watch
  vibes_short_pulse();
#else
  vibes_short_pulse();
#endif
  
  // Store platform name for display
  snprintf(s_buf_footer, sizeof(s_buf_footer), "%s ready", platform_name);
}

static void prv_deinit(void) {
  tick_timer_service_unsubscribe();
  app_message_deregister_callbacks();
  window_destroy(s_window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
