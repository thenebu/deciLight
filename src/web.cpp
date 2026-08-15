#include "web.h"
#include "config.h"
#include "net_manager.h"
#include "mqtt.h"
#include <Preferences.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <esp_ota_ops.h>

// Global instance
WebService web_service;

// Static pointer for task wrapper (allows access to instance)
static WebService* g_web_service = nullptr;

// Forward declaration of static HTML UI
extern const char* html_ui;

//============================================
// WebService Constructor
//============================================
WebService::WebService()
  : server(nullptr),
    current_dB(0.0),
    last_dB_update(0),
    needs_save(false),
    network_settings_pending(false),
    update_reboot_pending(false),
    update_reboot_at_ms(0),
    task_handle(nullptr),
    config_mux(portMUX_INITIALIZER_UNLOCKED),
    dB_mux(portMUX_INITIALIZER_UNLOCKED),
    history_count(0),
    history_next(0),
    last_history_sample_ms(0),
    hourly_day(-1),
    hourly_year(-1),
    hourly_reset_at(0),
    last_alert_epoch(0),
    last_hourly_ms(0),
    last_hourly_flush_ms(0),
    hourly_dirty(false),
    hourly_mux(portMUX_INITIALIZER_UNLOCKED)
{
  memset(hourly_ms, 0, sizeof(hourly_ms));
  config = {
    .display_mode = DISPLAY_MODE,
    .db_floor = DB_FLOOR,
    .db_normal_switchover = DB_NORMAL_SWITCHOVER,
    .db_warning_switchover = DB_WARNING_SWITCHOVER,
    .led_brightness = LED_BRIGHTNESS,
    .color_normal = 0x00FF00,
    .color_warning = 0xFFFF00,
    .color_alert = 0xFF0000,
    .decay_ms = 1500,
    .response_ms = 100
  };
}

//============================================
// WebService::init() - Initialize web server
//============================================
void WebService::init() {
  log_i("[WEB] Initializing web server...");
  
  // Create WebServer instance
  server = new WebServer(80);
  
  // Store global pointer for task wrapper
  g_web_service = this;

  // WiFi mode (STA with AP fallback) and mDNS are handled by
  // NetworkService, initialized by main.cpp before this. WebService only
  // owns the HTTP server itself.

  // Setup web server routes with lambda captures
  log_i("[WEB] Registering routes...");
  
  server->on("/", HTTP_GET, [this]() { this->handleRoot(); });
  log_i("[WEB] Route / registered");
  
  server->on("/api/config", HTTP_GET, [this]() { this->handleApiGet(); });
  log_i("[WEB] Route /api/config GET registered");
  
  server->on("/api/config", HTTP_POST, [this]() { this->handleApiSet(); });
  log_i("[WEB] Route /api/config POST registered");
  
  server->on("/api/status", HTTP_GET, [this]() { this->handleApiStatus(); });
  log_i("[WEB] Route /api/status registered");

  server->on("/api/history", HTTP_GET, [this]() { this->handleApiHistory(); });
  log_i("[WEB] Route /api/history registered");

  server->on("/api/hourly", HTTP_GET, [this]() { this->handleHourlyGet(); });
  server->on("/api/hourly/reset", HTTP_POST, [this]() { this->handleHourlyReset(); });
  log_i("[WEB] Route /api/hourly, /api/hourly/reset registered");

  server->on("/api/network", HTTP_GET, [this]() { this->handleNetworkGet(); });
  server->on("/api/network", HTTP_POST, [this]() { this->handleNetworkSet(); });
  log_i("[WEB] Route /api/network registered");

  server->on("/api/config/export", HTTP_GET, [this]() { this->handleConfigExport(); });
  server->on("/api/config/import", HTTP_POST, [this]() { this->handleConfigImport(); });
  log_i("[WEB] Route /api/config/export, /api/config/import registered");

  server->on("/update", HTTP_POST,
    [this]() { this->handleUpdateResult(); },
    [this]() { this->handleUpdateUpload(); });
  log_i("[WEB] Route /update registered");

  server->onNotFound([this]() { this->handleNotFound(); });
  log_i("[WEB] 404 handler registered");
  
  server->begin();
  log_i("[WEB] Web server started on port 80");
  
  // Load config from storage
  log_i("[WEB] Loading config from NVS...");
  loadConfig();
  log_i("[WEB] Config loaded");

  log_i("[WEB] Loading hourly stats from NVS...");
  loadHourlyStats();
  log_i("[WEB] Hourly stats loaded");
}

//============================================
// WebService::startTask() - Create FreeRTOS task
//============================================
void WebService::startTask() {
  log_i("[WEB] Creating WebTask...");
  
  BaseType_t ret = xTaskCreatePinnedToCore(
    WebService::webTaskWrapper,  // Static wrapper function
    "WebTask",                   // Task name
    // 4096 was enough for plain HTTP + OTA, but MQTT reconnect/discovery
    // (PubSubClient::connect()'s TCP handshake plus JSON serialization,
    // called synchronously from this same task in MqttService::loop())
    // adds enough call depth to overflow it once a broker is actually
    // configured - manifests as a boot loop (device drops off WiFi ~1s
    // after connecting, right after MQTT settings are saved).
    8192,                        // Stack size
    this,                        // Parameter (pass this pointer)
    3,                           // Priority
    &task_handle,                // Task handle
    0                            // Core 0
  );
  
  if (ret != pdPASS) {
    log_e("[WEB] Failed to create WebTask!");
  } else {
    log_i("[WEB] WebTask created successfully");
  }
}

//============================================
// WebService::webTaskWrapper() - Static task wrapper
//============================================
void WebService::webTaskWrapper(void *param) {
  WebService* pThis = static_cast<WebService*>(param);
  pThis->webTaskHandler();
}

//============================================
// WebService::webTaskHandler() - Instance task handler
//============================================
void WebService::webTaskHandler() {
  log_i("[TASK] WebTask started on core %d", xPortGetCoreID());
  
  while (1) {
    // Handle incoming web requests (non-blocking)
    if (server) {
      server->handleClient();
    }

    // Pump ArduinoOTA (no-ops until STA is connected and OTA is armed)
    network_service.handleOta();

    // Maintain MQTT connection + periodic state publish (no-ops until a
    // broker host is configured and STA is connected)
    mqtt_service.loop();

    // Check if config save is pending (deferred from HTTP handler)
    if (needs_save) {
      needs_save = false;
      saveConfig();
      log_i("Config updated via web: decay=%dms response=%dms",
        config.decay_ms, config.response_ms);
    }

    // Throttled flush for the hourly-stats buckets - 288 bytes of NVS
    // writes, so this shouldn't happen on every 50ms tick like the small
    // Config struct does. Only actually writes when something changed
    // (hourly_dirty) and at most once per 5 minutes.
    {
      unsigned long now = millis();
      bool dirty;
      portENTER_CRITICAL(&hourly_mux);
      dirty = hourly_dirty;
      portEXIT_CRITICAL(&hourly_mux);

      if (dirty && (now - last_hourly_flush_ms) > (5UL * 60UL * 1000UL)) {
        last_hourly_flush_ms = now;
        saveHourlyStats();
        portENTER_CRITICAL(&hourly_mux);
        hourly_dirty = false;
        portEXIT_CRITICAL(&hourly_mux);
      }
    }

    // Apply pending network settings (deferred from handleNetworkSet()/
    // handleConfigImport()) here, one tick after the HTTP response was
    // sent - by now server->handleClient() has already returned control
    // for that request and the connection has had a chance to close, so
    // the up-to-WIFI_CONNECT_TIMEOUT_MS block in applySettings() no
    // longer delays the browser's response.
    if (network_settings_pending) {
      network_settings_pending = false;
      network_service.applySettings(pending_network_settings);
    }

    // Reboot into the newly flashed firmware (deferred from
    // handleUpdateUpload()'s UPLOAD_FILE_END) - one second after
    // Update.end() succeeded gives handleUpdateResult()'s response time to
    // flush to the browser before the device drops off the network.
    if (update_reboot_pending && (millis() - update_reboot_at_ms > 1000)) {
      update_reboot_pending = false;
      log_i("[WEB] Rebooting into newly flashed firmware...");
      ESP.restart();
    }

    vTaskDelay(pdMS_TO_TICKS(50));  // Yield for 50ms
  }
}

//============================================
// WebService::updateLevel() - Update current dB level
//============================================
void WebService::updateLevel(double dB_current) {
  unsigned long now = millis();

  portENTER_CRITICAL(&dB_mux);
  current_dB = dB_current;
  last_dB_update = now;

  // Downsample into the 1-sample/sec history ring buffer, reusing the
  // same timestamp this call already computed.
  if (history_count == 0 || (now - last_history_sample_ms) >= 1000) {
    last_history_sample_ms = now;
    history[history_next] = (float)dB_current;
    history_next = (history_next + 1) % HISTORY_SIZE;
    if (history_count < HISTORY_SIZE) history_count++;
  }
  portEXIT_CRITICAL(&dB_mux);
}

//============================================
// WebService::getConfigSnapshot() - Thread-safe copy of config
//============================================
Config WebService::getConfigSnapshot() {
  portENTER_CRITICAL(&config_mux);
  Config snapshot = config;
  portEXIT_CRITICAL(&config_mux);
  return snapshot;
}

double WebService::getCurrentDb() {
  portENTER_CRITICAL(&dB_mux);
  double snapshot = current_dB;
  portEXIT_CRITICAL(&dB_mux);
  return snapshot;
}

// Minimum epoch value treated as "NTP has synced" - anything before this
// means the SNTP client hasn't gotten a response yet (ESP32 boots with its
// clock at 1970-01-01). Comfortably in the past relative to when this code
// was written, so it's a cheap, good-enough sanity check.
#define HOURLY_STATS_MIN_VALID_EPOCH 1700000000

//============================================
// WebService::accumulateHourlyStat() - called once per main-loop tick with
// the raw classification of the current dB reading, to build "today"'s
// time-in-each-color-per-hour distribution.
//============================================
void WebService::accumulateHourlyStat(NoiseLevel level) {
  time_t now = time(nullptr);
  unsigned long now_ms = millis();

  if (now < (time_t)HOURLY_STATS_MIN_VALID_EPOCH) {
    // No NTP sync yet - nothing to bucket. Keep last_hourly_ms current so
    // the untimed gap before sync isn't counted as elapsed time once it
    // does sync.
    portENTER_CRITICAL(&hourly_mux);
    last_hourly_ms = now_ms;
    portEXIT_CRITICAL(&hourly_mux);
    return;
  }

  struct tm tmnow;
  localtime_r(&now, &tmnow);  // already local time - configTime() baked the offset in

  portENTER_CRITICAL(&hourly_mux);

  if (hourly_day != tmnow.tm_yday || hourly_year != tmnow.tm_year) {
    // First tick after sync, or the day has rolled over (midnight, or a
    // year boundary) - start today's buckets fresh.
    memset(hourly_ms, 0, sizeof(hourly_ms));
    hourly_day = tmnow.tm_yday;
    hourly_year = tmnow.tm_year;
    hourly_reset_at = now;
    hourly_dirty = true;
    last_hourly_ms = now_ms;  // avoid crediting the gap since the last tick to hour 0
  }

  unsigned long elapsed = now_ms - last_hourly_ms;
  // Clamp to avoid a huge jump after the "no time yet" gap, a long web-task
  // stall, or a millis() wraparound - 5s is far more than one loop() tick
  // should ever take.
  if (elapsed > 5000) elapsed = 5000;

  hourly_ms[tmnow.tm_hour][level] += elapsed;
  hourly_dirty = true;
  last_hourly_ms = now_ms;

  if (level == ALERT) {
    last_alert_epoch = now;
  }

  portEXIT_CRITICAL(&hourly_mux);
}

// Thread-safe copy of last_alert_epoch - see comment in web.h.
time_t WebService::getLastAlertEpoch() {
  portENTER_CRITICAL(&hourly_mux);
  time_t snapshot = last_alert_epoch;
  portEXIT_CRITICAL(&hourly_mux);
  return snapshot;
}

// Clears today's buckets immediately (manual reset) and persists right
// away, unlike the throttled periodic flush in webTaskHandler().
void WebService::resetHourlyStats() {
  time_t now = time(nullptr);
  bool synced = now >= (time_t)HOURLY_STATS_MIN_VALID_EPOCH;

  portENTER_CRITICAL(&hourly_mux);
  memset(hourly_ms, 0, sizeof(hourly_ms));
  hourly_reset_at = synced ? now : 0;
  if (synced) {
    struct tm tmnow;
    localtime_r(&now, &tmnow);
    hourly_day = tmnow.tm_yday;
    hourly_year = tmnow.tm_year;
  } else {
    hourly_day = -1;
    hourly_year = -1;
  }
  hourly_dirty = false;  // about to persist immediately below
  portEXIT_CRITICAL(&hourly_mux);

  saveHourlyStats();
  last_hourly_flush_ms = millis();
}

//============================================
// HTTP HANDLERS
//============================================

void WebService::handleRoot() {
  server->send(200, "text/html", html_ui);
}

// Fills a JsonDocument with the current config's fields. Shared by
// handleApiGet() and the export endpoint (added in a later phase) so both
// stay in sync with the Config struct's shape.
void WebService::configToJson(const Config& cfg, JsonObject obj) {
  obj["display_mode"] = cfg.display_mode;
  obj["db_floor"] = cfg.db_floor;
  obj["db_normal_switchover"] = cfg.db_normal_switchover;
  obj["db_warning_switchover"] = cfg.db_warning_switchover;
  obj["led_brightness"] = cfg.led_brightness;
  obj["color_normal"] = cfg.color_normal;
  obj["color_warning"] = cfg.color_warning;
  obj["color_alert"] = cfg.color_alert;
  obj["decay_ms"] = cfg.decay_ms;
  obj["response_ms"] = cfg.response_ms;
}

// Applies whichever recognized fields are present in `obj` onto `cfg`,
// clamped to the same ranges the web UI's sliders allow so a malformed or
// malicious request body can't push the config into a nonsensical or
// overflowing state. Missing fields are left untouched. Shared by
// handleApiSet() and the import endpoint (added in a later phase).
void WebService::applyJsonToConfig(JsonObjectConst obj, Config& cfg) {
  if (obj["display_mode"].is<int>())
    cfg.display_mode = constrain(obj["display_mode"].as<int>(), 0, 1);

  if (obj["db_floor"].is<float>())
    cfg.db_floor = constrain(obj["db_floor"].as<float>(), 20.0f, 60.0f);

  if (obj["db_normal_switchover"].is<float>())
    cfg.db_normal_switchover = constrain(obj["db_normal_switchover"].as<float>(), 30.0f, 70.0f);

  if (obj["db_warning_switchover"].is<float>())
    cfg.db_warning_switchover = constrain(obj["db_warning_switchover"].as<float>(), 40.0f, 85.0f);

  if (obj["led_brightness"].is<int>())
    cfg.led_brightness = constrain(obj["led_brightness"].as<int>(), 0, 255);

  if (obj["color_normal"].is<uint32_t>())
    cfg.color_normal = constrain(obj["color_normal"].as<uint32_t>(), 0u, 0xFFFFFFu);

  if (obj["color_warning"].is<uint32_t>())
    cfg.color_warning = constrain(obj["color_warning"].as<uint32_t>(), 0u, 0xFFFFFFu);

  if (obj["color_alert"].is<uint32_t>())
    cfg.color_alert = constrain(obj["color_alert"].as<uint32_t>(), 0u, 0xFFFFFFu);

  if (obj["decay_ms"].is<int>())
    cfg.decay_ms = constrain(obj["decay_ms"].as<int>(), 0, 3000);

  if (obj["response_ms"].is<int>())
    cfg.response_ms = constrain(obj["response_ms"].as<int>(), 0, 500);
}

// Shared field-merge for NetworkSettings, used by both handleNetworkSet()
// (incremental UI save - allow_empty_password=false, so a blank password
// field means "keep what's stored") and handleConfigImport()
// (allow_empty_password=true, so a re-imported export applies exactly what
// it contains, including a deliberately empty password).
void WebService::applyJsonToNetworkSettings(JsonObjectConst obj, NetworkSettings& s, bool allow_empty_password) {
  if (obj["wifi_ssid"].is<const char*>()) s.wifi_ssid = obj["wifi_ssid"].as<const char*>();
  if (obj["wifi_pass"].is<const char*>() && (allow_empty_password || strlen(obj["wifi_pass"]) > 0))
    s.wifi_pass = obj["wifi_pass"].as<const char*>();

  if (obj["mqtt_host"].is<const char*>()) s.mqtt_host = obj["mqtt_host"].as<const char*>();
  if (obj["mqtt_port"].is<uint16_t>()) s.mqtt_port = obj["mqtt_port"].as<uint16_t>();
  if (obj["mqtt_user"].is<const char*>()) s.mqtt_user = obj["mqtt_user"].as<const char*>();
  if (obj["mqtt_pass"].is<const char*>() && (allow_empty_password || strlen(obj["mqtt_pass"]) > 0))
    s.mqtt_pass = obj["mqtt_pass"].as<const char*>();

  // POSIX TZ string, e.g. "CET-1CEST,M3.5.0,M10.5.0/3" - no numeric range to
  // clamp here (configTzTime() just ignores a malformed string and treats
  // it as UTC), an empty field falls back to the default.
  if (obj["tz_string"].is<const char*>() && strlen(obj["tz_string"]) > 0)
    s.tz_string = obj["tz_string"].as<const char*>();
}

void WebService::handleApiGet() {
  DynamicJsonDocument doc(384);
  configToJson(config, doc.to<JsonObject>());
  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

void WebService::handleApiSet() {
  log_i("[WEB] Config update received");
  if (!server->hasArg("plain")) {
    server->send(400, "application/json", "{\"error\":\"No body\"}");
    return;
  }

  String body = server->arg("plain");
  log_i("Received JSON: %s", body.c_str());

  DynamicJsonDocument doc(384);
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    log_e("[WEB] JSON parse failed: %s", err.c_str());
    server->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }

  // Parse into a local copy first, so the shared `config` is only touched
  // once, briefly, under the lock below.
  Config new_config = getConfigSnapshot();
  applyJsonToConfig(doc.as<JsonObjectConst>(), new_config);

  portENTER_CRITICAL(&config_mux);
  config = new_config;
  portEXIT_CRITICAL(&config_mux);

  // Send response IMMEDIATELY (don't block on NVS writes)
  server->send(200, "application/json", "{\"status\":\"ok\"}");

  // Set flag to save later (outside of handler)
  needs_save = true;
}

void WebService::handleApiStatus() {
  portENTER_CRITICAL(&dB_mux);
  double dB_snapshot = current_dB;
  portEXIT_CRITICAL(&dB_mux);

  DynamicJsonDocument doc(160);
  doc["db"] = dB_snapshot;
  if (suggested_floor > 0.0) {
    doc["suggested_floor"] = suggested_floor;
  }
  doc["firmware"] = FIRMWARE_VERSION;
  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

// GET /api/history - oldest-to-newest JSON array of the last HISTORY_SIZE
// (5 minutes @ 1/sec) dB readings, for the web UI's canvas line plot.
void WebService::handleApiHistory() {
  // Static, not a stack array: the web task's stack is only 4096 bytes
  // (see startTask()), and HISTORY_SIZE*sizeof(float) plus this function's
  // own frame would eat a meaningful chunk of that. handleClient() only
  // processes one request at a time on this task, so a single shared
  // buffer here is safe.
  static float snapshot[HISTORY_SIZE];
  int count, next;

  portENTER_CRITICAL(&dB_mux);
  count = history_count;
  next = history_next;
  memcpy(snapshot, history, sizeof(history));
  portEXIT_CRITICAL(&dB_mux);

  // JSON array is built outside the critical section - ArduinoJson
  // allocates on the heap, which (like String) must not happen inside a
  // portENTER_CRITICAL/portEXIT_CRITICAL section on ESP32.
  DynamicJsonDocument doc(HISTORY_SIZE * 16 + 64);
  JsonArray arr = doc.to<JsonArray>();

  int oldest = (count < HISTORY_SIZE) ? 0 : next;
  for (int i = 0; i < count; i++) {
    int idx = (oldest + i) % HISTORY_SIZE;
    arr.add(snapshot[idx]);
  }

  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

// GET /api/hourly - today's hour-of-day time distribution across the three
// NoiseLevel colors, for the web UI's stacked-bar "Tagesstatistik" chart.
void WebService::handleHourlyGet() {
  static uint32_t snapshot[24][3];
  time_t reset_at;
  time_t now = time(nullptr);
  bool synced = now >= (time_t)HOURLY_STATS_MIN_VALID_EPOCH;

  portENTER_CRITICAL(&hourly_mux);
  memcpy(snapshot, hourly_ms, sizeof(hourly_ms));
  reset_at = hourly_reset_at;
  portEXIT_CRITICAL(&hourly_mux);

  // JSON is built outside the critical section - ArduinoJson allocates on
  // the heap, which must not happen inside a portENTER_CRITICAL section.
  DynamicJsonDocument doc(1536);
  doc["time_synced"] = synced;
  doc["reset_at"] = (uint32_t)reset_at;
  JsonArray hours = doc.createNestedArray("hours");
  for (int h = 0; h < 24; h++) {
    JsonArray bucket = hours.createNestedArray();
    bucket.add(snapshot[h][NORMAL]);
    bucket.add(snapshot[h][WARNING]);
    bucket.add(snapshot[h][ALERT]);
  }

  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

// POST /api/hourly/reset - manual reset button for the hourly stats.
void WebService::handleHourlyReset() {
  log_i("[WEB] Hourly stats reset requested");
  resetHourlyStats();
  server->send(200, "application/json", "{\"status\":\"ok\"}");
}

// NetworkSettings lives outside the Config spinlock (see net_manager.h); this
// handler only ever runs on the web task, same as NetworkService's own
// read/write paths, so no additional locking is needed here.
void WebService::handleNetworkGet() {
  NetworkSettings s = network_service.getSettings();

  DynamicJsonDocument doc(512);
  doc["wifi_ssid"] = s.wifi_ssid;
  // Passwords are intentionally omitted from GET responses so they aren't
  // echoed back to the browser/DOM on every page load - the UI leaves the
  // password field blank and handleNetworkSet() only overwrites a stored
  // password when the field is non-empty (see below).
  doc["wifi_connected"] = network_service.isStaConnected();
  doc["mqtt_host"] = s.mqtt_host;
  doc["mqtt_port"] = s.mqtt_port;
  doc["mqtt_user"] = s.mqtt_user;
  doc["tz_string"] = s.tz_string;

  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

void WebService::handleNetworkSet() {
  log_i("[WEB] Network settings update received");
  if (!server->hasArg("plain")) {
    server->send(400, "application/json", "{\"error\":\"No body\"}");
    return;
  }

  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, server->arg("plain"));
  if (err) {
    server->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }

  // Start from the currently stored settings so omitted/blank password
  // fields don't clobber a previously saved credential.
  NetworkSettings s = network_service.getSettings();
  applyJsonToNetworkSettings(doc.as<JsonObjectConst>(), s, /*allow_empty_password=*/false);

  // Defer the actual reconnect to webTaskHandler()'s loop - applySettings()
  // blocks for up to WIFI_CONNECT_TIMEOUT_MS, and calling it here (even
  // after send()) still delays the browser, since the HTTP handler
  // function - and with it, the connection teardown - doesn't complete
  // until this function returns.
  pending_network_settings = s;
  network_settings_pending = true;
  server->send(200, "application/json", "{\"status\":\"ok\"}");
}

// GET /api/config/export - dumps both `config` and `network` (WiFi/MQTT
// settings, including credentials in cleartext) as one JSON object, for a
// client-side download/backup. Security note: this is consistent with the
// device's existing security posture (open AP, no auth on any endpoint) -
// anyone who can reach the web UI can already read/change these values one
// field at a time via /api/config and /api/network. Documented, not
// blocking, per the plan.
void WebService::handleConfigExport() {
  Config cfg = getConfigSnapshot();
  NetworkSettings net = network_service.getSettings();

  DynamicJsonDocument doc(1024);
  JsonObject config_obj = doc.createNestedObject("config");
  configToJson(cfg, config_obj);

  JsonObject network_obj = doc.createNestedObject("network");
  network_obj["wifi_ssid"] = net.wifi_ssid;
  network_obj["wifi_pass"] = net.wifi_pass;
  network_obj["mqtt_host"] = net.mqtt_host;
  network_obj["mqtt_port"] = net.mqtt_port;
  network_obj["mqtt_user"] = net.mqtt_user;
  network_obj["mqtt_pass"] = net.mqtt_pass;
  network_obj["tz_string"] = net.tz_string;

  String json;
  serializeJson(doc, json);
  server->sendHeader("Content-Disposition", "attachment; filename=\"noiselight-config.json\"");
  server->send(200, "application/json", json);
}

// POST /api/config/import - accepts the same shape handleConfigExport()
// produces. Reuses applyJsonToConfig()'s clamping for the `config` half
// (same validation as handleApiSet()) and NetworkSettings' own field
// merge for the `network` half - both sections are optional, so a
// config-only or network-only file also imports fine.
void WebService::handleConfigImport() {
  log_i("[WEB] Config import received");
  if (!server->hasArg("plain")) {
    server->send(400, "application/json", "{\"error\":\"No body\"}");
    return;
  }

  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, server->arg("plain"));
  if (err) {
    server->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }

  if (doc["config"].is<JsonObjectConst>()) {
    Config new_config = getConfigSnapshot();
    applyJsonToConfig(doc["config"].as<JsonObjectConst>(), new_config);

    portENTER_CRITICAL(&config_mux);
    config = new_config;
    portEXIT_CRITICAL(&config_mux);
    needs_save = true;
  }

  if (doc["network"].is<JsonObjectConst>()) {
    JsonObjectConst net_obj = doc["network"].as<JsonObjectConst>();
    NetworkSettings s = network_service.getSettings();
    // Import applies fields as exported, including an intentionally empty
    // password, unlike the UI's incremental save.
    applyJsonToNetworkSettings(net_obj, s, /*allow_empty_password=*/true);

    // Deferred to webTaskHandler()'s loop - see handleNetworkSet().
    pending_network_settings = s;
    network_settings_pending = true;
    server->send(200, "application/json", "{\"status\":\"ok\"}");
    return;
  }

  server->send(200, "application/json", "{\"status\":\"ok\"}");
}

// POST /update - streaming firmware upload, gated by OTA_PASSWORD (the same
// credential ArduinoOTA already uses) via HTTP Basic Auth, since unlike the
// other endpoints this one can brick or replace the firmware outright.
// WebServer's upload API splits handling across two callbacks registered
// together in init(): this one is invoked repeatedly as the multipart body
// streams in, handleUpdateResult() once at the end to produce the actual
// HTTP response.
void WebService::handleUpdateUpload() {
  HTTPUpload& upload = server->upload();

  if (upload.status == UPLOAD_FILE_START) {
    if (!server->authenticate(OTA_USERNAME, OTA_PASSWORD)) {
      // Don't start the flash - handleUpdateResult() will see the same
      // failed authenticate() check and send the 401.
      log_e("[WEB] /update upload rejected: bad credentials");
      return;
    }

    log_i("[WEB] /update upload starting: %s", upload.filename.c_str());

    // Diagnostic only (see the "still shows old FIRMWARE_VERSION after a
    // successful-looking OTA" investigation): log which partition is
    // currently running vs. which one Update.begin() is about to target,
    // so a stuck/repeating target partition is visible in the serial log
    // instead of only showing up as "the version never changes" after the
    // fact.
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* next = esp_ota_get_next_update_partition(NULL);
    log_i("[WEB] OTA partitions: running=%s@0x%06x, target=%s@0x%06x",
      running ? running->label : "?", running ? running->address : 0,
      next ? next->label : "?", next ? next->address : 0);

    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      log_e("[WEB] Update.begin() failed: %s", Update.errorString());
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      log_e("[WEB] Update.write() failed: %s", Update.errorString());
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      log_i("[WEB] /update upload complete: %u bytes", upload.totalSize);

      // Diagnostic only - see the OTA-reverts-to-old-partition investigation:
      // esp_ota_get_boot_partition() reads back what otadata says the NEXT
      // boot should use, right now, before we ever call ESP.restart(). If
      // this already shows the old partition instead of the one we just
      // wrote, esp_ota_set_boot_partition() (called internally by
      // Update.end(true) above) silently failed to persist - a write-time
      // bug, not a bootloader-ignoring-otadata bug.
      const esp_partition_t* boot_target = esp_ota_get_boot_partition();
      log_i("[WEB] otadata boot partition immediately after Update.end(): %s@0x%06x",
        boot_target ? boot_target->label : "?", boot_target ? boot_target->address : 0);

      // ...and, just as importantly, in WHICH STATE. On a bootloader built
      // with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE (which is what the
      // Arduino IDE esp32 3.x core flashes onto this board),
      // esp_ota_set_boot_partition() marks the new slot ESP_OTA_IMG_NEW (0)
      // rather than committing it outright - "boot it once, on probation".
      // The new image then has to call
      // esp_ota_mark_app_valid_cancel_rollback() or the bootloader reverts
      // to the previous slot on the following reset. setup() in main.cpp
      // does exactly that as of 1.1.6; before 1.1.6 nothing did, which is
      // why every /update reported success and every reboot came back
      // running the old firmware. Values: 0=NEW, 1=PENDING_VERIFY, 2=VALID,
      // 3=INVALID, 4=ABORTED, 0xFFFFFFFF=UNDEFINED (rollback not in use).
      if (boot_target) {
        esp_ota_img_states_t target_state = ESP_OTA_IMG_UNDEFINED;
        if (esp_ota_get_state_partition(boot_target, &target_state) == ESP_OK) {
          log_i("[WEB] otadata state of %s after Update.end(): %d "
                "(0=NEW/on-probation, 0xFFFFFFFF=UNDEFINED/committed)",
            boot_target->label, (int)target_state);
        }
      }

      update_reboot_pending = true;
      update_reboot_at_ms = millis();
    } else {
      log_e("[WEB] Update.end() failed: %s", Update.errorString());
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    log_e("[WEB] /update upload aborted");
    Update.end();
  }
}

// Final response for POST /update, sent after handleUpdateUpload() above
// has already streamed and applied (or failed to apply) the .bin.
void WebService::handleUpdateResult() {
  if (!server->authenticate(OTA_USERNAME, OTA_PASSWORD)) {
    server->requestAuthentication();
    return;
  }

  if (Update.hasError()) {
    String msg = String("{\"status\":\"error\",\"message\":\"") + Update.errorString() + "\"}";
    server->send(500, "application/json", msg);
  } else {
    server->send(200, "application/json", "{\"status\":\"ok\"}");
  }
}

void WebService::handleNotFound() {
  server->send(404, "text/plain", "Not Found");
}

//============================================
// CONFIG PERSISTENCE
//============================================

void WebService::loadConfig() {
  Preferences prefs;
  prefs.begin("noiselight", true);  // readonly
  config.display_mode = prefs.getInt("mode", DISPLAY_MODE);
  config.db_floor = prefs.getFloat("floor", DB_FLOOR);
  config.db_normal_switchover = prefs.getFloat("normal", DB_NORMAL_SWITCHOVER);
  config.db_warning_switchover = prefs.getFloat("warning", DB_WARNING_SWITCHOVER);
  config.led_brightness = prefs.getUChar("brightness", LED_BRIGHTNESS);
  config.color_normal = prefs.getUInt("col_normal", 0x00FF00);
  config.color_warning = prefs.getUInt("col_warning", 0xFFFF00);
  config.color_alert = prefs.getUInt("col_alert", 0xFF0000);
  config.decay_ms = prefs.getUShort("decay_ms", 1500);
  config.response_ms = prefs.getUShort("response_ms", 100);
  prefs.end();
  
  log_i("Config loaded: mode=%d decay=%dms response=%dms", 
    config.display_mode, config.decay_ms, config.response_ms);
}

void WebService::saveConfig() {
  Preferences prefs;
  prefs.begin("noiselight", false);  // readwrite
  prefs.putInt("mode", config.display_mode);
  prefs.putFloat("floor", config.db_floor);
  prefs.putFloat("normal", config.db_normal_switchover);
  prefs.putFloat("warning", config.db_warning_switchover);
  prefs.putUChar("brightness", config.led_brightness);
  prefs.putUInt("col_normal", config.color_normal);
  prefs.putUInt("col_warning", config.color_warning);
  prefs.putUInt("col_alert", config.color_alert);
  prefs.putUShort("decay_ms", config.decay_ms);
  prefs.putUShort("response_ms", config.response_ms);
  prefs.end();
  
  const char* mode_str = (config.display_mode == 0) ? "TRAFFIC_LIGHT" : "VU_METER";
  log_i("=== CONFIG SAVED ===");
  log_i("Mode: %s", mode_str);
  log_i("Floor: %.1f dB", config.db_floor);
  log_i("Switchovers: normal=%.1f dB, warning=%.1f dB", 
    config.db_normal_switchover, config.db_warning_switchover);
  log_i("LED Brightness: %d", config.led_brightness);
  log_i("Colors: normal=0x%06X, warning=0x%06X, alert=0x%06X",
    config.color_normal, config.color_warning, config.color_alert);
  log_i("Timing: decay=%dms, response=%dms",
    config.decay_ms, config.response_ms);
}

//============================================
// HOURLY STATS PERSISTENCE
//============================================

// Loads "today"'s hourly buckets from NVS, if any were saved. If the saved
// day/year don't match today (or time hasn't synced yet), the normal
// day-rollover check inside accumulateHourlyStat() clears them on its first
// tick - no special-casing needed here.
void WebService::loadHourlyStats() {
  Preferences prefs;
  prefs.begin("hourstats", true);  // readonly

  uint32_t loaded[24][3];
  memset(loaded, 0, sizeof(loaded));
  size_t got = prefs.getBytes("buckets", loaded, sizeof(loaded));
  int day = prefs.getInt("day", -1);
  int year = prefs.getInt("year", -1);
  uint32_t reset_at = prefs.getUInt("reset_at", 0);
  prefs.end();

  portENTER_CRITICAL(&hourly_mux);
  if (got == sizeof(loaded)) {
    memcpy(hourly_ms, loaded, sizeof(hourly_ms));
  }
  hourly_day = day;
  hourly_year = year;
  hourly_reset_at = (time_t)reset_at;
  hourly_dirty = false;
  portEXIT_CRITICAL(&hourly_mux);

  log_i("[WEB] Hourly stats loaded: day=%d year=%d reset_at=%u", day, year, reset_at);
}

// Persists "today"'s hourly buckets. Called from the throttled flush in
// webTaskHandler() and immediately from resetHourlyStats().
void WebService::saveHourlyStats() {
  uint32_t snapshot[24][3];
  int day, year;
  uint32_t reset_at;

  portENTER_CRITICAL(&hourly_mux);
  memcpy(snapshot, hourly_ms, sizeof(hourly_ms));
  day = hourly_day;
  year = hourly_year;
  reset_at = (uint32_t)hourly_reset_at;
  portEXIT_CRITICAL(&hourly_mux);

  Preferences prefs;
  prefs.begin("hourstats", false);  // readwrite
  prefs.putBytes("buckets", snapshot, sizeof(snapshot));
  prefs.putInt("day", day);
  prefs.putInt("year", year);
  prefs.putUInt("reset_at", reset_at);
  prefs.end();

  log_i("[WEB] Hourly stats saved (day=%d year=%d)", day, year);
}

// Embedded HTML UI (kept as static const for readability)
const char* html_ui = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>noiselight Config</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
      background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
      min-height: 100vh;
      display: flex;
      align-items: center;
      justify-content: center;
      padding: 20px;
    }
    .container {
      background: white;
      border-radius: 12px;
      box-shadow: 0 10px 40px rgba(0,0,0,0.2);
      max-width: 500px;
      width: 100%;
      padding: 30px;
    }
    h1 {
      color: #333;
      margin-bottom: 30px;
      font-size: 28px;
      text-align: center;
    }
    .section {
      margin-bottom: 25px;
    }
    .section-title {
      font-size: 16px;
      font-weight: 600;
      color: #667eea;
      margin-bottom: 12px;
      text-transform: uppercase;
      letter-spacing: 0.5px;
    }
    label {
      display: block;
      margin-bottom: 8px;
      color: #555;
      font-size: 14px;
      font-weight: 500;
    }
    input[type="radio"] {
      margin-right: 8px;
    }
    .radio-group {
      display: flex;
      gap: 20px;
      margin-bottom: 12px;
    }
    .radio-group label {
      display: flex;
      align-items: center;
      margin: 0;
    }
    input[type="range"] {
      width: 100%;
      height: 6px;
      border-radius: 3px;
      background: #ddd;
      outline: none;
      -webkit-appearance: none;
      margin-bottom: 6px;
    }
    input[type="range"]::-webkit-slider-thumb {
      -webkit-appearance: none;
      appearance: none;
      width: 18px;
      height: 18px;
      border-radius: 50%;
      background: #667eea;
      cursor: pointer;
      box-shadow: 0 2px 4px rgba(0,0,0,0.2);
    }
    input[type="range"]::-moz-range-thumb {
      width: 18px;
      height: 18px;
      border-radius: 50%;
      background: #667eea;
      cursor: pointer;
      border: none;
    }
    .value-display {
      text-align: right;
      font-size: 13px;
      color: #888;
      font-weight: 500;
    }
    .range-container {
      margin-bottom: 15px;
    }
    .led-preview {
      display: flex;
      gap: 6px;
      height: 40px;
      border-radius: 6px;
      overflow: hidden;
      margin: 15px 0;
      box-shadow: inset 0 2px 4px rgba(0,0,0,0.1);
    }
    .led { flex: 1; }
    .status {
      margin-top: 20px;
      padding: 12px;
      background: #f0f4ff;
      border-left: 4px solid #667eea;
      border-radius: 4px;
      font-size: 13px;
      color: #555;
    }
    .status.success {
      background: #f0fdf4;
      border-left-color: #22c55e;
      color: #166534;
    }
    .live-level {
      background: #f8f9fa;
      border-radius: 8px;
      padding: 20px;
      margin-bottom: 25px;
    }
    .level-value {
      display: flex;
      justify-content: space-between;
      align-items: baseline;
      margin-bottom: 12px;
    }
    .level-number {
      font-size: 36px;
      font-weight: 700;
      color: #333;
    }
    .level-unit {
      font-size: 14px;
      color: #999;
    }
    .level-bar-container {
      width: 100%;
      height: 24px;
      background: #e5e7eb;
      border-radius: 4px;
      overflow: hidden;
      margin-top: 12px;
    }
    .level-bar {
      height: 100%;
      width: 0%;
      background: linear-gradient(90deg, #22c55e 0%, #eab308 50%, #ef4444 100%);
      transition: width 0.1s ease;
    }
    button {
      width: 100%;
      padding: 12px;
      background: #667eea;
      color: white;
      border: none;
      border-radius: 6px;
      font-size: 16px;
      font-weight: 600;
      cursor: pointer;
      margin-top: 20px;
      transition: background 0.3s;
    }
    button:hover { background: #5568d3; }
    button:active { transform: scale(0.98); }
    .color-picker {
      width: 60px;
      height: 40px;
      border: none;
      border-radius: 4px;
      cursor: pointer;
    }
    .color-group {
      display: flex;
      gap: 10px;
      align-items: center;
      margin-bottom: 10px;
    }
    .color-group label {
      width: 80px;
      margin: 0;
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>🎵 noiselight</h1>
    
    <div class="live-level">
      <div class="level-value">
        <div>
          <div class="level-number" id="live-db">--</div>
          <div class="level-unit">Current Level</div>
        </div>
      </div>
      <div class="level-bar-container">
        <div class="level-bar" id="live-bar"></div>
      </div>
    </div>

    <div class="section">
      <div class="section-title">Verlauf (5 min)</div>
      <canvas id="history-canvas" width="440" height="120"
        style="width:100%; height:120px; background:#f8f9fa; border-radius:8px;"></canvas>
    </div>

    <div class="section">
      <div class="section-title">Tagesstatistik <span id="hourly-since" style="text-transform:none; letter-spacing:normal; font-weight:400; color:#999;"></span></div>
      <canvas id="hourly-canvas" width="480" height="140"
        style="width:100%; height:140px; background:#f8f9fa; border-radius:8px;"></canvas>
      <div id="hourly-note" style="display:none; font-size:12px; color:#999; margin-top:6px;"></div>
      <button onclick="resetHourlyStats()" style="margin-top:8px;">Zurücksetzen</button>
    </div>

    <div class="section">
      <div class="section-title">Display Mode</div>
      <div class="radio-group">
        <label><input type="radio" name="mode" value="0" onchange="updatePreview()"> Traffic Light</label>
        <label><input type="radio" name="mode" value="1" onchange="updatePreview()"> VU Meter</label>
      </div>
    </div>

    <div class="section">
      <div class="section-title">LED Settings</div>
      
      <div class="range-container">
        <label>Brightness <span class="value-display" id="brightness-val">25</span></label>
        <input type="range" id="brightness-slider" min="0" max="255" step="1" value="25" onchange="updatePreview()">
      </div>
      
      <div style="margin-top: 20px;">
        <div class="color-group">
          <label>Green</label>
          <input type="color" id="color-green" value="#00FF00" class="color-picker" onchange="updatePreview()">
        </div>
        <div class="color-group">
          <label>Yellow</label>
          <input type="color" id="color-yellow" value="#FFFF00" class="color-picker" onchange="updatePreview()">
        </div>
        <div class="color-group">
          <label>Red</label>
          <input type="color" id="color-red" value="#FF0000" class="color-picker" onchange="updatePreview()">
        </div>
      </div>
    </div>

    <div class="section">
      <div class="section-title">Color Switchover Points (dB)</div>
      
      <div class="range-container">
        <div style="display: flex; justify-content: space-between; align-items: center;">
          <label style="margin-bottom: 0;">Floor Level <span class="value-display" id="floor-val">37 dB</span></label>
          <button style="width: auto; padding: 6px 12px; margin-top: 0; font-size: 12px;" onclick="setFloorToCurrent()">Current</button>
        </div>
        <input type="range" id="floor-slider" min="20" max="60" step="1" value="37" onchange="updatePreview()">
        <div id="suggested-floor-hint" style="display:none; font-size: 12px; color: #667eea; margin-top: 4px;"></div>
      </div>

      <div class="range-container">
        <label>Green→Yellow Switchover <span class="value-display" id="green-val">50 dB</span></label>
        <input type="range" id="green-slider" min="30" max="70" step="1" value="50" onchange="updatePreview()">
      </div>
      
      <div class="range-container">
        <label>Yellow→Red Switchover <span class="value-display" id="yellow-val">65 dB</span></label>
        <input type="range" id="yellow-slider" min="40" max="85" step="1" value="65" onchange="updatePreview()">
      </div>
      
      <div class="led-preview" id="preview"></div>
    </div>

    <div class="section">
      <div class="section-title">Response Timing</div>
      
      <div class="range-container">
        <label>Decay Time <span class="value-display" id="decay-val">1500 ms</span></label>
        <input type="range" id="decay-slider" min="0" max="3000" step="100" value="1500" onchange="updatePreview()">
      </div>
      
      <div class="range-container">
        <label>Response Time <span class="value-display" id="response-val">100 ms</span></label>
        <input type="range" id="response-slider" min="0" max="500" step="50" value="100" onchange="updatePreview()">
      </div>
    </div>

    <div id="status" class="status" style="display:none;"></div>

    <button onclick="saveConfig()">Save Configuration</button>

    <div class="section" style="margin-top: 25px;">
      <div class="section-title">WLAN</div>

      <div id="wifi-state" style="font-size: 13px; color: #888; margin-bottom: 10px;">--</div>

      <div class="range-container">
        <label>SSID</label>
        <input type="text" id="wifi-ssid" placeholder="Heimnetz-Name"
          style="width:100%; padding:8px; border:1px solid #ddd; border-radius:4px;">
      </div>

      <div class="range-container">
        <label>Passwort <span style="font-weight:400; color:#999;">(leer lassen, um bestehendes zu behalten)</span></label>
        <input type="password" id="wifi-pass" placeholder="········"
          style="width:100%; padding:8px; border:1px solid #ddd; border-radius:4px;">
      </div>

      <div class="range-container">
        <label>Zeitzone (POSIX TZ) <span style="font-weight:400; color:#999;">- für Tagesstatistik-Stunden, berücksichtigt Sommer-/Winterzeit</span></label>
        <input type="text" id="tz-string" placeholder="CET-1CEST,M3.5.0,M10.5.0/3"
          style="width:100%; padding:8px; border:1px solid #ddd; border-radius:4px;">
      </div>

      <div id="wifi-status" class="status" style="display:none;"></div>

      <button onclick="saveNetwork()">WLAN speichern &amp; verbinden</button>
    </div>

    <div class="section" style="margin-top: 25px;">
      <div class="section-title">MQTT / Home Assistant</div>

      <div class="range-container">
        <label>Broker-Host <span style="font-weight:400; color:#999;">(leer lassen, um MQTT zu deaktivieren)</span></label>
        <input type="text" id="mqtt-host" placeholder="z.B. 192.168.1.10 oder homeassistant.local"
          style="width:100%; padding:8px; border:1px solid #ddd; border-radius:4px;">
      </div>

      <div class="range-container">
        <label>Port</label>
        <input type="number" id="mqtt-port" value="1883" min="1" max="65535"
          style="width:100%; padding:8px; border:1px solid #ddd; border-radius:4px;">
      </div>

      <div class="range-container">
        <label>Benutzer <span style="font-weight:400; color:#999;">(optional)</span></label>
        <input type="text" id="mqtt-user"
          style="width:100%; padding:8px; border:1px solid #ddd; border-radius:4px;">
      </div>

      <div class="range-container">
        <label>Passwort <span style="font-weight:400; color:#999;">(leer lassen, um bestehendes zu behalten)</span></label>
        <input type="password" id="mqtt-pass" placeholder="········"
          style="width:100%; padding:8px; border:1px solid #ddd; border-radius:4px;">
      </div>

      <div id="mqtt-status" class="status" style="display:none;"></div>

      <button onclick="saveMqtt()">MQTT speichern</button>
    </div>

    <div class="section" style="margin-top: 25px;">
      <div class="section-title">Firmware-Update</div>
      <div style="font-size: 12px; color: #999; margin-bottom: 10px;">
        Neue .bin-Datei hochladen, um die Firmware zu aktualisieren. Das Gerät startet danach automatisch neu.
      </div>

      <div class="range-container">
        <label>OTA-Passwort</label>
        <input type="password" id="update-pass" placeholder="········"
          style="width:100%; padding:8px; border:1px solid #ddd; border-radius:4px;">
      </div>

      <div class="range-container">
        <label>Firmware-Datei</label>
        <input type="file" id="update-file" accept=".bin"
          style="width:100%; padding:8px; border:1px solid #ddd; border-radius:4px;">
      </div>

      <div class="level-bar-container" id="update-progress-container" style="display:none;">
        <div class="level-bar" id="update-progress-bar" style="background:#667eea;"></div>
      </div>

      <div id="update-status" class="status" style="display:none;"></div>

      <button onclick="uploadFirmware()">Firmware hochladen</button>
    </div>

    <div class="section" style="margin-top: 25px;">
      <div class="section-title">Konfiguration</div>
      <div style="font-size: 12px; color: #999; margin-bottom: 10px;">
        Export enthält WLAN-/MQTT-Passwörter im Klartext - Datei entsprechend behandeln.
      </div>
      <div style="display:flex; gap:10px;">
        <button style="margin-top:0;" onclick="exportConfig()">Export</button>
        <button style="margin-top:0;" onclick="document.getElementById('import-file').click()">Import</button>
      </div>
      <input type="file" id="import-file" accept="application/json" style="display:none;" onchange="importConfig(event)">
      <div id="config-io-status" class="status" style="display:none;"></div>
    </div>

    <div style="font-size:11px; color:#bbb; text-align:center; margin-top:20px;">
      <span id="fw-version"></span>
    </div>
  </div>

  <script>
    async function updateLiveLevel() {
      try {
        const res = await fetch('/api/status');
        const data = await res.json();
        document.getElementById('live-db').textContent = data.db.toFixed(1);
        document.getElementById('fw-version').textContent = 'Firmware ' + data.firmware;

        const minDb = 37;
        const maxDb = 80;
        const normalized = Math.max(0, Math.min(1, (data.db - minDb) / (maxDb - minDb)));
        document.getElementById('live-bar').style.width = (normalized * 100) + '%';

        const hint = document.getElementById('suggested-floor-hint');
        if (data.suggested_floor !== undefined) {
          hint.innerHTML = 'Vorschlag beim letzten Boot: ' + data.suggested_floor.toFixed(1) +
            ' dB - <a href="#" onclick="applySuggestedFloor(' + data.suggested_floor + '); return false;">übernehmen?</a>';
          hint.style.display = 'block';
        }
      } catch (e) {
        console.error('Status update failed:', e);
      }
    }

    function applySuggestedFloor(value) {
      document.getElementById('floor-slider').value = Math.round(value);
      updatePreview();
    }

    async function updateHistory() {
      try {
        const res = await fetch('/api/history');
        const data = await res.json();
        if (!Array.isArray(data) || data.length < 2) return;

        const canvas = document.getElementById('history-canvas');
        const ctx = canvas.getContext('2d');
        const w = canvas.width, h = canvas.height;
        ctx.clearRect(0, 0, w, h);

        const minDb = Math.min(...data) - 2;
        const maxDb = Math.max(...data) + 2;
        const range = Math.max(1, maxDb - minDb);

        ctx.strokeStyle = '#667eea';
        ctx.lineWidth = 2;
        ctx.beginPath();
        data.forEach((db, i) => {
          const x = (i / (data.length - 1)) * w;
          const y = h - ((db - minDb) / range) * h;
          if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
        });
        ctx.stroke();
      } catch (e) {
        console.error('History update failed:', e);
      }
    }

    async function drawHourlyStats() {
      try {
        const res = await fetch('/api/hourly');
        const data = await res.json();

        const canvas = document.getElementById('hourly-canvas');
        const ctx = canvas.getContext('2d');
        const w = canvas.width, h = canvas.height;
        ctx.clearRect(0, 0, w, h);

        const note = document.getElementById('hourly-note');
        if (!data.time_synced) {
          note.textContent = 'Uhrzeit noch nicht synchronisiert - Statistik startet, sobald das Gerät die Zeit per NTP erhalten hat.';
          note.style.display = 'block';
        } else {
          note.style.display = 'none';
        }

        const sinceEl = document.getElementById('hourly-since');
        if (data.reset_at) {
          sinceEl.textContent = '(seit ' + new Date(data.reset_at * 1000).toLocaleTimeString() + ')';
        } else {
          sinceEl.textContent = '';
        }

        if (!Array.isArray(data.hours) || data.hours.length !== 24) return;

        const colorNormal = document.getElementById('color-green').value;
        const colorWarning = document.getElementById('color-yellow').value;
        const colorAlert = document.getElementById('color-red').value;

        const barW = w / 24;
        const msPerHour = 3600000;

        data.hours.forEach((bucket, hour) => {
          const [normalMs, warningMs, alertMs] = bucket;
          const total = Math.min(normalMs + warningMs + alertMs, msPerHour);
          const x = hour * barW;

          let y = h;
          const segments = [[normalMs, colorNormal], [warningMs, colorWarning], [alertMs, colorAlert]];
          segments.forEach(([ms, color]) => {
            const segH = (Math.min(ms, msPerHour) / msPerHour) * h;
            ctx.fillStyle = color;
            ctx.fillRect(x + 1, y - segH, barW - 2, segH);
            y -= segH;
          });

          if (hour % 4 === 0) {
            ctx.fillStyle = '#999';
            ctx.font = '10px sans-serif';
            ctx.fillText(hour + 'h', x + 2, h - 2);
          }
        });
      } catch (e) {
        console.error('Hourly stats update failed:', e);
      }
    }

    async function resetHourlyStats() {
      if (!confirm('Statistik wirklich zurücksetzen?')) return;
      try {
        await fetch('/api/hourly/reset', { method: 'POST' });
        drawHourlyStats();
      } catch (e) {
        console.error('Hourly stats reset failed:', e);
      }
    }

    async function loadConfig() {
      try {
        const res = await fetch('/api/config');
        const data = await res.json();
        document.querySelector(`input[name="mode"][value="${data.display_mode}"]`).checked = true;
        document.getElementById('floor-slider').value = Math.round(data.db_floor);
        document.getElementById('green-slider').value = Math.round(data.db_normal_switchover);
        document.getElementById('yellow-slider').value = Math.round(data.db_warning_switchover);
        document.getElementById('brightness-slider').value = data.led_brightness;
        document.getElementById('color-green').value = '#' + ('000000' + data.color_normal.toString(16).toUpperCase()).slice(-6);
        document.getElementById('color-yellow').value = '#' + ('000000' + data.color_warning.toString(16).toUpperCase()).slice(-6);
        document.getElementById('color-red').value = '#' + ('000000' + data.color_alert.toString(16).toUpperCase()).slice(-6);
        document.getElementById('decay-slider').value = data.decay_ms;
        document.getElementById('response-slider').value = data.response_ms;
        updatePreview();
      } catch (e) {
        console.error('Failed to load config:', e);
      }
    }

    function updatePreview() {
      const brightness = parseInt(document.getElementById('brightness-slider').value);
      const floorLevel = parseInt(document.getElementById('floor-slider').value);
      const greenThresh = parseInt(document.getElementById('green-slider').value);
      const yellowThresh = parseInt(document.getElementById('yellow-slider').value);
      const decay = parseInt(document.getElementById('decay-slider').value);
      const response = parseInt(document.getElementById('response-slider').value);
      
      document.getElementById('brightness-val').textContent = brightness;
      document.getElementById('floor-val').textContent = floorLevel + ' dB';
      document.getElementById('green-val').textContent = greenThresh + ' dB';
      document.getElementById('yellow-val').textContent = yellowThresh + ' dB';
      document.getElementById('decay-val').textContent = decay + ' ms';
      document.getElementById('response-val').textContent = response + ' ms';
      
      const colors = [];
      for (let i = 0; i < 13; i++) {
        const dbAtLED = 37 + (i / 12) * 43;
        let color;
        if (dbAtLED < greenThresh) {
          color = '#22c55e';
        } else if (dbAtLED < yellowThresh) {
          color = '#eab308';
        } else {
          color = '#ef4444';
        }
        colors.push(`<div class="led" style="background:${color}"></div>`);
      }
      document.getElementById('preview').innerHTML = colors.join('');
    }

    function setFloorToCurrent() {
      const currentDb = parseFloat(document.getElementById('live-db').textContent);
      if (!isNaN(currentDb)) {
        document.getElementById('floor-slider').value = Math.round(currentDb);
        updatePreview();
      }
    }

    function hexToInt(hex) {
      return parseInt(hex.substring(1), 16);
    }

    async function saveConfig() {
      const mode = document.querySelector('input[name="mode"]:checked').value;
      const floor = parseInt(document.getElementById('floor-slider').value);
      const green = parseInt(document.getElementById('green-slider').value);
      const yellow = parseInt(document.getElementById('yellow-slider').value);
      const brightness = parseInt(document.getElementById('brightness-slider').value);
      const colorGreen = hexToInt(document.getElementById('color-green').value);
      const colorYellow = hexToInt(document.getElementById('color-yellow').value);
      const colorRed = hexToInt(document.getElementById('color-red').value);
      const decay = parseInt(document.getElementById('decay-slider').value);
      const response = parseInt(document.getElementById('response-slider').value);
      
      try {
        const res = await fetch('/api/config', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({
            display_mode: parseInt(mode),
            db_floor: floor,
            db_normal_switchover: green,
            db_warning_switchover: yellow,
            led_brightness: brightness,
            color_normal: colorGreen,
            color_warning: colorYellow,
            color_alert: colorRed,
            decay_ms: decay,
            response_ms: response
          })
        });
        
        const status = document.getElementById('status');
        if (res.ok) {
          status.className = 'status success';
          status.textContent = '✓ Configuration saved successfully!';
        } else {
          status.className = 'status';
          status.textContent = '✗ Failed to save configuration';
        }
        status.style.display = 'block';
        setTimeout(() => { status.style.display = 'none'; }, 3000);
      } catch (e) {
        console.error('Save failed:', e);
      }
    }

    async function loadNetwork() {
      try {
        const res = await fetch('/api/network');
        const data = await res.json();
        document.getElementById('wifi-ssid').value = data.wifi_ssid || '';
        document.getElementById('wifi-state').textContent = data.wifi_connected
          ? ('Verbunden (' + data.wifi_ssid + ')')
          : (data.wifi_ssid ? 'Nicht verbunden - AP-Fallback aktiv' : 'Kein WLAN konfiguriert - AP-Fallback aktiv');

        document.getElementById('mqtt-host').value = data.mqtt_host || '';
        document.getElementById('mqtt-port').value = data.mqtt_port || 1883;
        document.getElementById('mqtt-user').value = data.mqtt_user || '';
        document.getElementById('tz-string').value = data.tz_string || '';
      } catch (e) {
        console.error('Failed to load network settings:', e);
      }
    }

    async function saveNetwork() {
      const ssid = document.getElementById('wifi-ssid').value;
      const pass = document.getElementById('wifi-pass').value;
      const tzString = document.getElementById('tz-string').value;

      try {
        const res = await fetch('/api/network', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ wifi_ssid: ssid, wifi_pass: pass, tz_string: tzString })
        });

        const status = document.getElementById('wifi-status');
        if (res.ok) {
          status.className = 'status success';
          status.textContent = '✓ Gespeichert - Gerät verbindet neu (kann bis zu 15s dauern)...';
        } else {
          status.className = 'status';
          status.textContent = '✗ Speichern fehlgeschlagen';
        }
        status.style.display = 'block';
        document.getElementById('wifi-pass').value = '';
        setTimeout(() => { status.style.display = 'none'; loadNetwork(); }, 4000);
      } catch (e) {
        console.error('Network save failed:', e);
      }
    }

    async function saveMqtt() {
      const host = document.getElementById('mqtt-host').value;
      const port = parseInt(document.getElementById('mqtt-port').value) || 1883;
      const user = document.getElementById('mqtt-user').value;
      const pass = document.getElementById('mqtt-pass').value;

      try {
        const res = await fetch('/api/network', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ mqtt_host: host, mqtt_port: port, mqtt_user: user, mqtt_pass: pass })
        });

        const status = document.getElementById('mqtt-status');
        if (res.ok) {
          status.className = 'status success';
          status.textContent = '✓ MQTT-Einstellungen gespeichert';
        } else {
          status.className = 'status';
          status.textContent = '✗ Speichern fehlgeschlagen';
        }
        status.style.display = 'block';
        document.getElementById('mqtt-pass').value = '';
        setTimeout(() => { status.style.display = 'none'; loadNetwork(); }, 4000);
      } catch (e) {
        console.error('MQTT save failed:', e);
      }
    }

    async function exportConfig() {
      try {
        const res = await fetch('/api/config/export');
        const blob = await res.blob();
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = 'noiselight-config.json';
        document.body.appendChild(a);
        a.click();
        a.remove();
        URL.revokeObjectURL(url);
      } catch (e) {
        console.error('Export failed:', e);
      }
    }

    async function importConfig(event) {
      const file = event.target.files[0];
      event.target.value = '';  // allow re-selecting the same file later
      if (!file) return;

      const status = document.getElementById('config-io-status');
      try {
        const text = await file.text();
        const res = await fetch('/api/config/import', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: text
        });

        if (res.ok) {
          status.className = 'status success';
          status.textContent = '✓ Importiert - Seite lädt neu...';
          status.style.display = 'block';
          setTimeout(() => location.reload(), 2000);
        } else {
          status.className = 'status';
          status.textContent = '✗ Import fehlgeschlagen (ungültige Datei?)';
          status.style.display = 'block';
        }
      } catch (e) {
        console.error('Import failed:', e);
        status.className = 'status';
        status.textContent = '✗ Import fehlgeschlagen';
        status.style.display = 'block';
      }
    }

    function uploadFirmware() {
      const fileInput = document.getElementById('update-file');
      const password = document.getElementById('update-pass').value;
      const status = document.getElementById('update-status');
      const progressContainer = document.getElementById('update-progress-container');
      const progressBar = document.getElementById('update-progress-bar');

      const file = fileInput.files[0];
      if (!file) {
        status.className = 'status';
        status.textContent = '✗ Bitte zuerst eine .bin-Datei auswählen';
        status.style.display = 'block';
        return;
      }

      // XMLHttpRequest (not fetch) so xhr.upload.onprogress can drive the
      // progress bar - fetch doesn't expose upload progress.
      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/update', true);
      xhr.setRequestHeader('Authorization', 'Basic ' + btoa('admin:' + password));

      progressContainer.style.display = 'block';
      progressBar.style.width = '0%';
      status.className = 'status';
      status.textContent = 'Wird hochgeladen...';
      status.style.display = 'block';

      xhr.upload.onprogress = function(e) {
        if (e.lengthComputable) {
          const pct = (e.loaded / e.total) * 100;
          progressBar.style.width = pct + '%';
        }
      };

      xhr.onload = function() {
        if (xhr.status === 200) {
          status.className = 'status success';
          status.textContent = '✓ Erfolgreich - Gerät startet neu...';
          setTimeout(() => location.reload(), 8000);
        } else {
          status.className = 'status';
          let message = 'Upload fehlgeschlagen (Status ' + xhr.status + ')';
          try {
            const data = JSON.parse(xhr.responseText);
            if (data.message) message = data.message;
          } catch (e) {}
          status.textContent = '✗ ' + message;
        }
      };

      xhr.onerror = function() {
        status.className = 'status';
        status.textContent = '✗ Upload fehlgeschlagen (Netzwerkfehler)';
      };

      const formData = new FormData();
      formData.append('firmware', file);
      xhr.send(formData);
    }

    window.onload = function() {
      loadConfig();
      loadNetwork();
      updateLiveLevel();
      setInterval(updateLiveLevel, 200);
      updateHistory();
      setInterval(updateHistory, 5000);
      drawHourlyStats();
      setInterval(drawHourlyStats, 60000);
    };
  </script>
</body>
</html>
)rawliteral";
