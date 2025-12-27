#include <pebble.h>
#include <math.h>
#include "message_keys.auto.h"

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

static void prv_reset_thresholds(void);
static void prv_update_layers(void);
static void prv_show_sport_selector(void);

// Pebble libc lacks __errno symbol required by libm; provide a stub.
static int s_errno_stub = 0;
int *__errno(void) {
  return &s_errno_stub;
}

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
  const char *page_label = prv_page_label(s_page);
  switch (s_state) {
    case RUN_STATE_RUNNING:
      snprintf(s_buf_title, sizeof(s_buf_title), "%s - %s", s_sport == SPORT_SWIM ? "SWIM" : "RUN", page_label);
      break;
    case RUN_STATE_PAUSED:
      snprintf(s_buf_title, sizeof(s_buf_title), "PAUSED - %s", page_label);
      break;
    case RUN_STATE_SUMMARY:
      snprintf(s_buf_title, sizeof(s_buf_title), "SUMMARY");
      break;
    default:
      snprintf(s_buf_title, sizeof(s_buf_title), "READY");
      break;
  }
  text_layer_set_text(s_title_layer, s_buf_title);

  double unit_distance = s_use_metric ? s_distance_m / 1000.0 : s_distance_m / 1609.34;
  int32_t lap_elapsed_ms = s_elapsed_ms - s_lap_anchor_ms;
  double lap_distance_m = s_distance_m - s_lap_anchor_distance_m;

  if (s_sport == SPORT_SWIM) {
    // Two pages only: Main and Details (use PAGE_LAP slot)
    if (s_page == PAGE_MAIN) {
      // Main: Total time, distance (m/yd), lap count
      prv_format_time_ms(s_elapsed_ms, s_buf_primary, sizeof(s_buf_primary));
      if (s_use_metric) {
        int m = (int)(s_distance_m + 0.5);
        snprintf(s_buf_secondary, sizeof(s_buf_secondary), "%d m  Laps %d", m, s_lap_count);
      } else {
        int yd = (int)((s_distance_m / 0.9144) + 0.5);
        snprintf(s_buf_secondary, sizeof(s_buf_secondary), "%d yd  Laps %d", yd, s_lap_count);
      }
      // Show heart rate if available on tertiary line
#ifdef PBL_HEALTH
      if (s_heart_rate_bpm > 0) {
        snprintf(s_buf_tertiary, sizeof(s_buf_tertiary), "HR %ld bpm", (long)s_heart_rate_bpm);
      } else {
        snprintf(s_buf_tertiary, sizeof(s_buf_tertiary), "HR -- bpm");
      }
#else
      snprintf(s_buf_tertiary, sizeof(s_buf_tertiary), "HR -- bpm");
#endif
    } else { // Details
      // Show last lap time and pace per 100m/yd
      if (s_lap_count > 0) {
        Lap last = s_laps[s_lap_count - 1];
        prv_format_time_ms(last.elapsed_ms, s_buf_primary, sizeof(s_buf_primary));
        // Pace per 100m/yd
        double segment = s_use_metric ? 100.0 : 91.44; // 100m or 100yd (approx)
        if (last.distance_m > 0.0 && last.elapsed_ms > 0) {
          double pace_sec = ((double)last.elapsed_ms / 1000.0) / (last.distance_m / segment);
          int total_sec = (int)pace_sec;
          int minutes = total_sec / 60;
          int seconds = total_sec % 60;
          snprintf(s_buf_tertiary, sizeof(s_buf_tertiary), "%d:%02d /%s", minutes, seconds, s_use_metric ? "100m" : "100yd");
        } else {
          snprintf(s_buf_tertiary, sizeof(s_buf_tertiary), "--:--");
        }
        // Pool length info
        if (s_use_metric) {
          int m = (int)((s_pool_length_cm + 50) / 100);
          snprintf(s_buf_secondary, sizeof(s_buf_secondary), "Pool %dm", m);
        } else {
          // 1 yd = 91.44 cm
          int yd = (int)((s_pool_length_cm / 91.44) + 0.5);
          snprintf(s_buf_secondary, sizeof(s_buf_secondary), "Pool %dyd", yd);
        }
      } else {
        snprintf(s_buf_primary, sizeof(s_buf_primary), "No laps");
        snprintf(s_buf_secondary, sizeof(s_buf_secondary), "--");
        snprintf(s_buf_tertiary, sizeof(s_buf_tertiary), "--");
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
        snprintf(s_buf_primary, sizeof(s_buf_primary), "HR n/a");
        snprintf(s_buf_secondary, sizeof(s_buf_secondary), "--");
        snprintf(s_buf_tertiary, sizeof(s_buf_tertiary), "--");
        break;
      case PAGE_SPLITS:
        if (s_lap_count > 0) {
          Lap last = s_laps[s_lap_count - 1];
          prv_format_time_ms(last.elapsed_ms, s_buf_primary, sizeof(s_buf_primary));
          snprintf(s_buf_secondary, sizeof(s_buf_secondary), "Lap %d", s_lap_count);
          prv_format_unit_distance_2dp(last.distance_m, s_buf_tertiary, sizeof(s_buf_tertiary));
        } else {
          snprintf(s_buf_primary, sizeof(s_buf_primary), "No laps");
          snprintf(s_buf_secondary, sizeof(s_buf_secondary), "--");
          snprintf(s_buf_tertiary, sizeof(s_buf_tertiary), "--");
        }
        break;
      default:
        break;
    }
  }

  text_layer_set_text(s_primary_layer, s_buf_primary);
  text_layer_set_text(s_secondary_layer, s_buf_secondary);
  text_layer_set_text(s_tertiary_layer, s_buf_tertiary);
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
  vibes_short_pulse();
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

static void prv_end_run(void);

static void prv_tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  prv_update_elapsed();
  if (s_state == RUN_STATE_RUNNING && s_auto_stop && s_last_movement_ms > 0) {
    int64_t now = prv_now_ms();
    if ((now - s_last_movement_ms) > 15000) {
      prv_end_run();
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
  prv_update_layers();
}

static void prv_pause_run(void) {
  if (s_state != RUN_STATE_RUNNING) {
    return;
  }
  prv_update_elapsed();
  s_state = RUN_STATE_PAUSED;
  s_pause_started_ms = prv_now_ms();
  prv_send_cmd(CMD_PAUSE);
  prv_update_layers();
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
  prv_update_layers();
}

static void prv_send_summary(void) {
  if (s_elapsed_ms == 0) {
    return;
  }
  char summary[96];
  double dist_units = s_use_metric ? s_distance_m / 1000.0 : s_distance_m / 1609.34;
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

static void prv_end_run(void) {
  prv_update_elapsed();
  s_state = RUN_STATE_SUMMARY;
  prv_send_cmd(CMD_STOP);
  prv_send_summary();
  prv_update_layers();
}

static void prv_back_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  prv_end_run();
}

static void prv_back_single_click_handler(ClickRecognizerRef recognizer, void *context) {
  text_layer_set_text(s_footer_layer, "Hold back to end");
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

static void prv_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  prv_add_lap();
  prv_update_layers();
}

static void prv_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_sport == SPORT_SWIM) {
    if (s_state == RUN_STATE_RUNNING) {
      text_layer_set_text(s_footer_layer, "Pause for details");
      return;
    }
    s_page = (s_page == PAGE_MAIN) ? PAGE_LAP : PAGE_MAIN;
    prv_update_layers();
  } else {
    s_page = (Page)((s_page + 1) % PAGE_COUNT);
    prv_update_layers();
  }
}

static void prv_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click_handler);
  window_single_click_subscribe(BUTTON_ID_UP, prv_up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_down_click_handler);
  window_single_click_subscribe(BUTTON_ID_BACK, prv_back_single_click_handler);
  window_long_click_subscribe(BUTTON_ID_BACK, 700, prv_back_long_click_handler, NULL);
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
  graphics_context_set_stroke_color(ctx, GColorDarkGray);
  GRect b = layer_get_bounds(layer);
  graphics_draw_line(ctx, GPoint(0, 0), GPoint(b.size.w, 0));
}

static void prv_draw_dots(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  int count = (s_sport == SPORT_SWIM) ? 2 : PAGE_COUNT;
  int16_t spacing = 12;
  int16_t radius = 3;
  int total = (count - 1) * spacing;
  int16_t start_x = (b.size.w - total) / 2;
  int16_t y = b.size.h / 2;
  for (int i = 0; i < count; i++) {
    int16_t x = start_x + i * spacing;
    bool is_active = (i == s_page);
    #ifdef PBL_COLOR
    graphics_context_set_fill_color(ctx, is_active ? GColorWhite : GColorLightGray);
    #else
    graphics_context_set_fill_color(ctx, is_active ? GColorWhite : GColorDarkGray);
    #endif
    graphics_fill_circle(ctx, GPoint(x, y), radius);
  }
}

static void prv_draw_bg(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  int16_t pad = PBL_IF_ROUND_ELSE(12, 4);
  int16_t w = b.size.w - pad * 2;

  // Primary card
  GRect r1 = GRect(pad - 2, 26, w + 4, 60);
  // Secondary card
  GRect r2 = GRect(pad - 2, 78, w + 4, 32);
  // Tertiary pill
  GRect r3 = GRect(pad - 2, 106, w + 4, 32);

  #ifdef PBL_COLOR
  graphics_context_set_stroke_color(ctx, GColorVividCerulean);
  #else
  graphics_context_set_stroke_color(ctx, GColorWhite);
  #endif
  graphics_draw_round_rect(ctx, r1, 6);

  #ifdef PBL_COLOR
  graphics_context_set_stroke_color(ctx, GColorChromeYellow);
  #else
  graphics_context_set_stroke_color(ctx, GColorWhite);
  #endif
  graphics_draw_round_rect(ctx, r2, 6);

  #ifdef PBL_COLOR
  graphics_context_set_stroke_color(ctx, GColorJaegerGreen);
  #else
  graphics_context_set_stroke_color(ctx, GColorWhite);
  #endif
  graphics_draw_round_rect(ctx, r3, 16);
}

static void prv_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  s_font_title = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  s_font_primary = fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD);
  s_font_secondary = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);

  int16_t pad = PBL_IF_ROUND_ELSE(12, 4);
  int16_t w = bounds.size.w - pad * 2;

  s_title_layer = text_layer_create(GRect(pad, 0, w, 22));
  text_layer_set_background_color(s_title_layer, GColorClear);
  #ifdef PBL_COLOR
  text_layer_set_text_color(s_title_layer, GColorChromeYellow);
  #else
  text_layer_set_text_color(s_title_layer, GColorWhite);
  #endif
  text_layer_set_font(s_title_layer, s_font_title);
  text_layer_set_text_alignment(s_title_layer, PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft));
  text_layer_set_text(s_title_layer, "READY");
  layer_add_child(window_layer, text_layer_get_layer(s_title_layer));

  s_divider_layer = layer_create(GRect(pad, 22, w, 1));
  layer_set_update_proc(s_divider_layer, prv_draw_divider);
  layer_add_child(window_layer, s_divider_layer);

  // Fun background cards
  s_bg_layer = layer_create(bounds);
  layer_set_update_proc(s_bg_layer, prv_draw_bg);
  layer_add_child(window_layer, s_bg_layer);

  s_primary_layer = text_layer_create(GRect(pad, 28, w, 56));
  text_layer_set_background_color(s_primary_layer, GColorClear);
  text_layer_set_text_color(s_primary_layer, GColorWhite);
  text_layer_set_font(s_primary_layer, s_font_primary);
  text_layer_set_text_alignment(s_primary_layer, GTextAlignmentCenter);
  text_layer_set_text(s_primary_layer, "00:00");
  layer_add_child(window_layer, text_layer_get_layer(s_primary_layer));

  s_secondary_layer = text_layer_create(GRect(pad, 82, w, 28));
  text_layer_set_background_color(s_secondary_layer, GColorClear);
  #ifdef PBL_COLOR
  text_layer_set_text_color(s_secondary_layer, GColorChromeYellow);
  #else
  text_layer_set_text_color(s_secondary_layer, GColorWhite);
  #endif
  text_layer_set_font(s_secondary_layer, s_font_secondary);
  text_layer_set_text_alignment(s_secondary_layer, GTextAlignmentCenter);
  text_layer_set_text(s_secondary_layer, "0.00 km");
  layer_add_child(window_layer, text_layer_get_layer(s_secondary_layer));

  s_tertiary_layer = text_layer_create(GRect(pad, 110, w, 28));
  text_layer_set_background_color(s_tertiary_layer, GColorClear);
  #ifdef PBL_COLOR
  text_layer_set_text_color(s_tertiary_layer, GColorJaegerGreen);
  #else
  text_layer_set_text_color(s_tertiary_layer, GColorWhite);
  #endif
  text_layer_set_font(s_tertiary_layer, s_font_secondary);
  text_layer_set_text_alignment(s_tertiary_layer, GTextAlignmentCenter);
  text_layer_set_text(s_tertiary_layer, "--:--");
  layer_add_child(window_layer, text_layer_get_layer(s_tertiary_layer));

  s_footer_layer = text_layer_create(GRect(pad, bounds.size.h - 28, w, 18));
  text_layer_set_background_color(s_footer_layer, GColorClear);
  text_layer_set_text_color(s_footer_layer, GColorWhite);
  text_layer_set_font(s_footer_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_footer_layer, GTextAlignmentLeft);
  text_layer_set_text(s_footer_layer, "");
  layer_add_child(window_layer, text_layer_get_layer(s_footer_layer));

  s_dots_layer = layer_create(GRect(0, bounds.size.h - 10, bounds.size.w, 10));
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

// Sport selection menu
static uint16_t prv_menu_get_num_sections(MenuLayer *menu_layer, void *context) {
  return 1;
}

static uint16_t prv_menu_get_num_rows(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  return 2; // Run, Swim
}

static int16_t prv_menu_get_cell_height(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  return 44; // default basic cell height
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
  window_stack_remove(s_sport_window, true);
  s_sport_window = NULL;
}

static void prv_sport_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  s_menu_layer = menu_layer_create(b);
  menu_layer_set_normal_colors(s_menu_layer, GColorBlack, GColorWhite);
  menu_layer_set_highlight_colors(s_menu_layer, GColorBlack, GColorJaegerGreen);
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
  window_set_background_color(s_sport_window, GColorBlack);
  window_set_window_handlers(s_sport_window, (WindowHandlers){
    .load = prv_sport_window_load,
    .unload = prv_sport_window_unload,
  });
  window_stack_push(s_sport_window, true);
}

static void prv_init(void) {
  prv_reset_session();

  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_click_config_provider(s_window, prv_click_config_provider);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(s_window, true);

  // Prompt sport selection on startup
  prv_show_sport_selector();

  app_message_register_inbox_received(prv_inbox_received);
  app_message_register_inbox_dropped(prv_inbox_dropped);
  app_message_register_outbox_failed(prv_outbox_failed);
  app_message_register_outbox_sent(prv_outbox_sent);
  app_message_open(512, 512);

  tick_timer_service_subscribe(SECOND_UNIT, prv_tick_handler);
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
