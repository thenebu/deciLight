#ifndef WEB_H
#define WEB_H

#include <WebServer.h>
#include <freertos/FreeRTOS.h>
#include <ArduinoJson.h>
#include <time.h>
#include "net_manager.h"
#include "config.h"

// Ring buffer size for the history graph: 300 entries @ 1 sample/sec = 5
// minutes of history.
#define HISTORY_SIZE 300

// Configuration structure for runtime settings
struct Config {
  int display_mode;           // 0=TRAFFIC_LIGHT, 1=VU_METER
  float db_floor;
  float db_normal_switchover;   // Switchover from NORMAL to WARNING
  float db_warning_switchover;  // Switchover from WARNING to ALERT
  uint8_t led_brightness;  // 0-255
  uint32_t color_normal;   // 0xRRGGBB
  uint32_t color_warning;  // 0xRRGGBB
  uint32_t color_alert;    // 0xRRGGBB
  uint16_t decay_ms;       // How long to hold color after sound (0-3000ms)
  uint16_t response_ms;    // Min time between LED updates (0-500ms)

  // Babyphone mode (display_mode == 2) settings.
  float babyphone_trigger_db;          // dB threshold that must be exceeded sustained
  uint16_t babyphone_sustain_s;        // seconds in NVS/UI, converted to ms internally
  uint16_t babyphone_clear_s;
  uint32_t babyphone_night_color;      // 0xRRGGBB
  uint8_t babyphone_night_brightness;  // 0-255
};

//
// WebService Class - Manages HTTP server and config persistence
//
class WebService {
public:
  WebService();
  void init();              // Initialize WiFi AP and HTTP server
  void startTask();         // Create and start the web task
  void updateLevel(double dB_current);  // Update current dB level for status endpoint
  Config getConfigSnapshot();  // Thread-safe copy of the current config
  double getCurrentDb();  // Thread-safe copy of the current dB reading (used by MqttService)

  // Thread-safe copy of the epoch of the most recent ALERT classification
  // since boot (0 = never triggered, or not yet NTP-synced when it would
  // have triggered). Guarded by hourly_mux, same as last_alert_epoch's
  // write side in accumulateHourlyStat(). Used by MqttService for the
  // "last alert" HA timestamp sensor.
  time_t getLastAlertEpoch();

  // Sustained-threshold state machine for Babyphone mode (display_mode ==
  // 2). Called once per main-loop iteration (main.cpp), right next to
  // accumulateHourlyStat(), with the raw (non-decayed) dB reading. No-ops
  // (and resets to QUIET) when display_mode isn't 2. See web.cpp for the
  // asymmetric-hysteresis rationale.
  void updateBabyphoneState(double dB_current, const Config& config);

  // Thread-safe snapshot of the current alarm state, guarded by hourly_mux
  // like getLastAlertEpoch() above. Used by handleApiStatus() and
  // MqttService for the HA binary_sensor.
  bool getBabyphoneAlarmActive();

  // Accumulates time spent at `level` into the current hour's bucket for
  // today's hourly stats. Called once per main-loop iteration (main.cpp)
  // with the raw (non-decayed) classification of the current dB reading.
  // No-ops until NTP has synced (see accumulateHourlyStat()'s epoch sanity
  // check in web.cpp).
  void accumulateHourlyStat(NoiseLevel level);

  // Clears today's hourly buckets immediately (manual reset button) and
  // persists right away, unlike the throttled periodic flush.
  void resetHourlyStats();

  // Set once during setup(), before any other task exists (see main.cpp's
  // boot-time warmup average) and only ever read afterward - unlike
  // current_dB/config it doesn't change at runtime, so no lock is needed.
  void setSuggestedFloor(double dB) { suggested_floor = dB; }

private:
  static void webTaskWrapper(void *param);  // Static task wrapper
  void webTaskHandler();    // Instance task handler
  
  // HTTP handler methods
  void handleRoot();
  void handleApiGet();
  void handleApiSet();
  void handleApiStatus();
  void handleApiHistory();
  void handleHourlyGet();
  void handleHourlyReset();
  void handleNetworkGet();
  void handleNetworkSet();
  void handleConfigExport();
  void handleConfigImport();
  void handleUpdateUpload();   // Streaming upload callback for POST /update
  void handleUpdateResult();   // Final response handler for POST /update
  void handleNotFound();

  // Configuration methods
  void loadConfig();
  void saveConfig();

  // Hourly-stats persistence (separate "hourstats" NVS namespace - see
  // web.cpp for the rationale on throttled vs. immediate flush).
  void loadHourlyStats();
  void saveHourlyStats();

  // JSON (de)serialization helpers, shared by the /api/config handlers and
  // (in a later phase) the export/import endpoints.
  static void configToJson(const Config& cfg, JsonObject obj);
  static void applyJsonToConfig(JsonObjectConst obj, Config& cfg);

  // Shared by handleNetworkSet() and handleConfigImport() - previously
  // duplicated inline in both, and had drifted (one guarded empty password
  // fields, the other didn't). allow_empty_password controls that: false
  // for the UI's incremental save (blank field = "keep existing"), true
  // for import (an exported file's fields, including an intentionally
  // empty password, should be applied as-is).
  static void applyJsonToNetworkSettings(JsonObjectConst obj, NetworkSettings& s, bool allow_empty_password);

  // Member variables
  WebServer *server;
  double current_dB;
  unsigned long last_dB_update;
  bool needs_save;

  // NetworkService::applySettings() blocks for up to WIFI_CONNECT_TIMEOUT_MS
  // reconnecting WiFi. Calling it directly from an HTTP handler - even
  // after server->send() - still blocks the browser, because the
  // underlying TCP connection isn't actually closed until the handler
  // function returns. Deferring it one tick into webTaskHandler()'s loop
  // (same pattern as needs_save) lets the response finish flushing first.
  bool network_settings_pending;
  NetworkSettings pending_network_settings;

  // Same deferred-action pattern as network_settings_pending above:
  // ESP.restart() can't happen inside the HTTP handler or the browser
  // never gets the success response for the upload, since the underlying
  // TCP connection isn't actually closed until the handler function
  // returns. webTaskHandler() reboots one tick later, once the response
  // has had a chance to flush.
  bool update_reboot_pending;
  unsigned long update_reboot_at_ms;

  // Set at UPLOAD_FILE_START only once the request has authenticated AND
  // Update.begin() has succeeded. WebServer keeps invoking the upload
  // callback for every chunk of the multipart body regardless of what that
  // first call decided, so without this flag a rejected upload still ran
  // Update.write() per 2KB chunk - each failing and logging an error line,
  // hundreds of them for a 1MB image.
  bool update_started;

  TaskHandle_t task_handle;
  double suggested_floor = 0.0;  // 0 = no suggestion available yet (see setSuggestedFloor)

  // 1-sample/sec ring buffer feeding the web UI's history graph. Written
  // from updateLevel() (core 1, downsampled off last_dB_update) and read
  // from handleApiHistory() (web task, core 0) - same cross-core shape as
  // current_dB, so it's guarded by the same dB_mux rather than a new lock.
  // Plain floats only (no heap allocation), so it's safe to touch inside
  // that existing critical section.
  float history[HISTORY_SIZE];
  int history_count;               // valid entries so far, caps at HISTORY_SIZE
  int history_next;                // next write index (ring buffer)
  unsigned long last_history_sample_ms;

  // config is written from the web task (core 0, via handleApiSet) and read
  // from the main loop (core 1, via getConfigSnapshot); guard both sides
  // with this spinlock so readers never observe a torn write.
  portMUX_TYPE config_mux;
  Config config;

  // current_dB/last_dB_update have the same cross-core shape: written from
  // the main loop (core 1, via updateLevel) and read from the web task
  // (core 0, via handleApiStatus) - guard with their own spinlock.
  portMUX_TYPE dB_mux;

  // Today's hour-of-day time distribution: hourly_ms[hour][NoiseLevel] in
  // milliseconds. Written from the main loop (core 1, via
  // accumulateHourlyStat) and read/reset from the web task (core 0, via
  // handleHourlyGet/handleHourlyReset) - same cross-core shape as
  // current_dB/history[], guarded by its own spinlock rather than sharing
  // dB_mux, mirroring the config_mux/dB_mux split.
  uint32_t hourly_ms[24][3];      // [hour][NoiseLevel] accumulated ms, "today"
  int hourly_day;                 // tm_yday of the day these buckets represent
  int hourly_year;                // tm_year, paired with hourly_day (year-boundary safety)
  time_t hourly_reset_at;         // epoch of last reset (manual or midnight rollover); 0 = unknown/no time yet
  time_t last_alert_epoch;        // epoch of the most recent ALERT classification since boot; 0 = never
  unsigned long last_hourly_ms;   // millis() of the last accumulate call, for elapsed-delta
  unsigned long last_hourly_flush_ms;  // throttles NVS writes
  bool hourly_dirty;
  portMUX_TYPE hourly_mux;

  // Babyphone sustained-threshold state machine (see
  // updateBabyphoneState() in web.cpp). Only ever touched by the single
  // writer task (main loop, core 1), so no lock needed for these - only
  // babyphone_alarm_active is read cross-core, guarded by hourly_mux above.
  enum BabyphoneState { QUIET, ALARMED };
  BabyphoneState babyphone_state = QUIET;
  unsigned long babyphone_above_since_ms = 0;  // 0 = not currently above threshold
  unsigned long babyphone_below_since_ms = 0;  // 0 = not currently below threshold
  bool babyphone_alarm_active = false;         // externally read flag, guarded by hourly_mux
};

// Global instance
extern WebService web_service;

#endif // WEB_H
