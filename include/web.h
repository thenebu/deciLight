#ifndef WEB_H
#define WEB_H

#include <WebServer.h>
#include <freertos/FreeRTOS.h>
#include <ArduinoJson.h>

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
  void handleNetworkGet();
  void handleNetworkSet();
  void handleConfigExport();
  void handleConfigImport();
  void handleNotFound();

  // Configuration methods
  void loadConfig();
  void saveConfig();

  // JSON (de)serialization helpers, shared by the /api/config handlers and
  // (in a later phase) the export/import endpoints.
  static void configToJson(const Config& cfg, JsonObject obj);
  static void applyJsonToConfig(JsonObjectConst obj, Config& cfg);

  // Member variables
  WebServer *server;
  double current_dB;
  unsigned long last_dB_update;
  bool needs_save;
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
};

// Global instance
extern WebService web_service;

#endif // WEB_H
