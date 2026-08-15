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
<html lang="de">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>noiselight</title>
  <style>
    :root{
      --ground:#F3F5F0;
      --paper:#FFFFFF;
      --paper-2:#E9ECE5;
      --line:#D8DDD3;
      --line-soft:#E6EAE1;
      --ink:#141814;
      --ink-2:#59615A;
      --ink-3:#8B938B;
      --quiet:#2F9E5F;
      --warn:#D89A18;
      --alert:#D14C3C;
      --mono:ui-monospace,"SF Mono",SFMono-Regular,Menlo,Consolas,monospace;
      --sans:system-ui,-apple-system,"Segoe UI",Roboto,"Helvetica Neue",Arial,sans-serif;
    }
    *{box-sizing:border-box}
    html,body{margin:0;padding:0}
    body{
      background:var(--ground);color:var(--ink);font-family:var(--sans);
      font-size:15px;line-height:1.45;-webkit-font-smoothing:antialiased;
    }
    .ui-wrap{max-width:1040px;margin:0 auto}

    /* top bar */
    .ui-top{
      display:flex;align-items:center;gap:10px;
      padding:13px 16px;border-bottom:1px solid var(--line);background:var(--paper);
    }
    .ui-mark{display:flex;align-items:center;gap:9px;min-width:0}
    .ui-lamp{width:11px;height:11px;border-radius:50%;background:var(--quiet);flex:none}
    .ui-mark b{font-size:16px;font-weight:660;letter-spacing:-.02em}
    .ui-spacer{flex:1}
    .ui-lang{
      display:flex;border:1px solid var(--line);border-radius:7px;overflow:hidden;flex:none;
      font-family:var(--mono);font-size:11px;letter-spacing:.06em;cursor:pointer;background:none;
    }
    .ui-lang span{padding:5px 10px;color:var(--ink-3);background:var(--paper)}
    .ui-lang span.on{background:var(--ink);color:var(--paper);font-weight:600}

    .ui-pad{padding:16px 16px 28px;display:flex;flex-direction:column;gap:14px}
    .ui-col{display:flex;flex-direction:column;gap:14px;min-width:0}

    /* live card */
    .ui-live{
      border:1px solid var(--line);border-radius:12px;padding:16px;background:var(--paper);
      display:flex;flex-direction:column;gap:14px;
    }
    .ui-live-row{display:flex;align-items:flex-end;gap:10px}
    .ui-db{
      font-family:var(--mono);font-size:52px;line-height:.85;font-weight:600;letter-spacing:-.04em;
      font-variant-numeric:tabular-nums;color:var(--ink);
    }
    .ui-db-u{font-family:var(--mono);font-size:15px;color:var(--ink-3);padding-bottom:3px}
    .ui-pill{
      margin-left:auto;display:inline-flex;align-items:center;gap:6px;
      border-radius:999px;padding:4px 11px 4px 8px;font-size:12.5px;font-weight:600;
      background:var(--paper-2);color:var(--ink-2);border:1px solid var(--line);
    }
    .ui-pill i{width:7px;height:7px;border-radius:50%;background:currentColor;display:block}
    .ui-pill.g{color:var(--quiet);background:rgba(47,158,95,.14);border-color:rgba(47,158,95,.32)}
    .ui-pill.y{color:var(--warn);background:rgba(216,154,24,.14);border-color:rgba(216,154,24,.32)}
    .ui-pill.r{color:var(--alert);background:rgba(209,76,60,.14);border-color:rgba(209,76,60,.32)}

    .ui-meter{position:relative;height:12px;border-radius:6px;overflow:hidden;background:var(--paper-2)}
    .ui-meter-mask{position:absolute;inset:0;background:var(--paper-2);opacity:.82}
    .ui-needle{position:absolute;top:-4px;bottom:-4px;width:2px;background:var(--ink);border-radius:2px}
    .ui-scale{display:flex;justify-content:space-between;font-family:var(--mono);font-size:10.5px;color:var(--ink-3);
      font-variant-numeric:tabular-nums;margin-top:4px}
    .ui-facts{display:flex;gap:0;border-top:1px solid var(--line-soft);padding-top:11px}
    .ui-fact{flex:1;display:flex;flex-direction:column;gap:2px;min-width:0}
    .ui-fact + .ui-fact{border-left:1px solid var(--line-soft);padding-left:12px}
    .ui-fact em{font-style:normal;font-size:10.5px;letter-spacing:.09em;text-transform:uppercase;color:var(--ink-3);font-family:var(--mono)}
    .ui-fact b{font-family:var(--mono);font-size:14px;font-weight:600;font-variant-numeric:tabular-nums;
      white-space:nowrap;overflow:hidden;text-overflow:ellipsis;display:block}

    /* card / drawers */
    .ui-card{border:1px solid var(--line);border-radius:12px;background:var(--paper);overflow:hidden}
    .ui-grouptitle{
      font-family:var(--mono);font-size:10.5px;letter-spacing:.16em;text-transform:uppercase;
      color:var(--ink-3);padding:6px 2px 0;
    }
    .ui-d{border-bottom:1px solid var(--line-soft)}
    .ui-d:last-child{border-bottom:0}
    .ui-sum{
      display:flex;align-items:center;gap:10px;padding:13px 14px;cursor:pointer;list-style:none;
    }
    .ui-sum::-webkit-details-marker{display:none}
    .ui-sum b{font-size:14.5px;font-weight:600;letter-spacing:-.005em}
    .ui-sum .ui-val{margin-left:auto;font-family:var(--mono);font-size:12px;color:var(--ink-3);
      font-variant-numeric:tabular-nums;text-align:right;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:46%}
    .ui-chev{width:9px;height:9px;border-right:1.6px solid var(--ink-3);border-bottom:1.6px solid var(--ink-3);
      transform:rotate(45deg) translate(-2px,-2px);flex:none;margin-left:2px}
    details[open] > .ui-sum .ui-chev{transform:rotate(-135deg) translate(-1px,-1px)}
    .ui-body{padding:0 14px 16px;display:flex;flex-direction:column;gap:14px}

    /* form atoms */
    .ui-field{display:flex;flex-direction:column;gap:6px}
    .ui-lab{font-size:12px;color:var(--ink-2);font-weight:550;display:flex;align-items:baseline;gap:8px}
    .ui-lab .ui-num{margin-left:auto;font-family:var(--mono);font-size:12.5px;color:var(--ink);font-variant-numeric:tabular-nums}
    .ui-hint{font-size:11.5px;color:var(--ink-3);line-height:1.4}
    .ui-in{
      border:1px solid var(--line);border-radius:8px;background:var(--paper);color:var(--ink);
      padding:9px 11px;font-size:14px;font-family:var(--sans);width:100%;
    }
    .ui-in.ui-mono{font-family:var(--mono);font-size:12.5px}
    .ui-in::placeholder{color:var(--ink-3)}
    .ui-row2{display:grid;grid-template-columns:1fr 1fr;gap:10px}
    .ui-row-btn{display:flex;gap:8px;align-items:center;flex-wrap:wrap}

    /* range sliders */
    input[type=range].ui-range{
      -webkit-appearance:none;appearance:none;width:100%;height:22px;background:transparent;margin:0;
      --pct:0%;
    }
    input[type=range].ui-range::-webkit-slider-runnable-track{
      height:4px;border-radius:2px;border:1px solid var(--line-soft);
      background:linear-gradient(var(--fill,var(--ink)),var(--fill,var(--ink))) 0/var(--pct) 100% no-repeat, var(--paper-2);
    }
    input[type=range].ui-range::-moz-range-track{
      height:4px;border-radius:2px;border:1px solid var(--line-soft);background:var(--paper-2);
    }
    input[type=range].ui-range::-moz-range-progress{
      height:4px;border-radius:2px;background:var(--fill,var(--ink));
    }
    input[type=range].ui-range::-webkit-slider-thumb{
      -webkit-appearance:none;margin-top:-7px;width:18px;height:18px;border-radius:50%;
      background:var(--paper);border:1.5px solid var(--fill,var(--ink));box-shadow:0 1px 3px rgba(0,0,0,.18);
    }
    input[type=range].ui-range::-moz-range-thumb{
      width:16px;height:16px;border-radius:50%;background:var(--paper);border:1.5px solid var(--fill,var(--ink));
    }
    input[type=range].ui-range.ui-y{--fill:var(--warn)}
    input[type=range].ui-range.ui-r{--fill:var(--alert)}

    .ui-btn{
      border:1px solid var(--line);border-radius:8px;background:var(--paper);color:var(--ink);
      padding:9px 14px;font-size:13.5px;font-weight:560;font-family:var(--sans);white-space:nowrap;cursor:pointer;
    }
    .ui-btn.ui-primary{background:var(--ink);color:var(--paper);border-color:var(--ink)}
    .ui-btn.ui-wide{width:100%;text-align:center}
    .ui-btn.ui-sm{padding:6px 10px;font-size:12px}
    .ui-btn:disabled{opacity:.55;cursor:default}

    .ui-choice{display:grid;grid-template-columns:1fr 1fr;gap:10px}
    .ui-opt{border:1px solid var(--line);border-radius:10px;padding:11px 12px;display:flex;flex-direction:column;gap:5px;
      background:var(--paper);cursor:pointer;text-align:left}
    .ui-opt.on{border-color:var(--ink);box-shadow:inset 0 0 0 1px var(--ink)}
    .ui-opt .ui-optt{display:flex;align-items:center;gap:7px;font-size:13.5px;font-weight:600}
    .ui-radio{width:14px;height:14px;border-radius:50%;border:1.5px solid var(--ink-3);flex:none}
    .ui-opt.on .ui-radio{border-color:var(--ink);box-shadow:inset 0 0 0 3px var(--paper),inset 0 0 0 9px var(--ink)}
    .ui-opt small{color:var(--ink-3);font-size:11.5px;line-height:1.35}

    .ui-swatchrow{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}
    .ui-swatch{border:1px solid var(--line);border-radius:9px;padding:8px;display:flex;flex-direction:column;gap:7px;align-items:center}
    .ui-swatch input[type=color]{
      display:block;width:100%;height:26px;border-radius:5px;border:none;padding:0;cursor:pointer;background:none;
    }
    .ui-swatch code{font-family:var(--mono);font-size:10.5px;color:var(--ink-3)}
    .ui-swatch em{font-style:normal;font-size:11px;color:var(--ink-2);font-weight:550}

    .ui-strip{display:flex;gap:3px;padding:9px;border:1px solid var(--line);border-radius:9px;background:var(--paper-2)}
    .ui-strip i{flex:1;height:26px;border-radius:3px;display:block}

    .ui-chart{width:100%;height:auto;display:block}
    .ui-xaxis{display:flex;justify-content:space-between;font-family:var(--mono);font-size:10px;color:var(--ink-3)}
    .ui-legend{display:flex;gap:14px;flex-wrap:wrap;font-size:11.5px;color:var(--ink-2)}
    .ui-legend span{display:inline-flex;align-items:center;gap:6px}
    .ui-legend i{width:9px;height:9px;border-radius:2px;display:block}
    .ui-bars{display:flex;gap:2px;height:78px;align-items:flex-end}
    .ui-bar{flex:1;height:100%;display:flex;flex-direction:column-reverse;border-radius:2px;overflow:hidden;background:var(--paper-2)}
    .ui-bar i{background:var(--quiet);display:block}
    .ui-bar b{background:var(--warn);display:block;margin:0}
    .ui-bar s{background:var(--alert);display:block;text-decoration:none}
    .ui-bar.ui-empty{background:repeating-linear-gradient(45deg,var(--paper-2) 0 3px,transparent 3px 6px)}

    .ui-status{display:flex;align-items:center;gap:7px;font-size:12.5px;color:var(--ink-2)}
    .ui-status i{width:7px;height:7px;border-radius:50%;background:var(--quiet);flex:none}
    .ui-status.ui-off i{background:var(--ink-3)}
    .ui-note{
      font-size:11.5px;color:var(--ink-2);background:rgba(216,154,24,.12);
      border:1px solid rgba(216,154,24,.30);border-radius:8px;padding:9px 11px;line-height:1.4;
    }
    .ui-saved{font-family:var(--mono);font-size:11.5px;color:var(--quiet);min-height:1.4em}
    .ui-file{
      display:flex;align-items:center;gap:9px;border:1px dashed var(--line);border-radius:8px;padding:9px 11px;
      font-size:12.5px;color:var(--ink-2);cursor:pointer;
    }
    .ui-file code{font-family:var(--mono);font-size:12px;color:var(--ink);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
    .ui-file input{display:none}
    .ui-track{height:6px;border-radius:3px;background:var(--paper-2);border:1px solid var(--line-soft);overflow:hidden}
    .ui-fill{height:100%;background:var(--ink);border-radius:3px;width:0%;transition:width .2s}
    .ui-foot{
      padding:14px 16px 20px;border-top:1px solid var(--line);
      font-family:var(--mono);font-size:10.5px;color:var(--ink-3);display:flex;flex-wrap:wrap;gap:4px 12px;
    }

    @media (min-width:860px){
      .ui-pad{display:grid;grid-template-columns:minmax(0,380px) minmax(0,1fr);gap:20px;align-items:start}
    }
  </style>
</head>
<body>
<div class="ui-wrap">

  <div class="ui-top">
    <div class="ui-mark"><i class="ui-lamp" id="lamp"></i><b>noiselight</b></div>
    <div class="ui-spacer"></div>
    <div class="ui-lang" id="lang-toggle">
      <span data-lang="de" id="lang-de">DE</span><span data-lang="en" id="lang-en">EN</span>
    </div>
  </div>

  <div class="ui-pad">

    <!-- LEFT: status rail -->
    <div class="ui-col">

      <div class="ui-live">
        <div class="ui-live-row">
          <div class="ui-db" id="live-db">--</div>
          <div class="ui-db-u">dB</div>
          <div class="ui-pill" id="live-pill"><i></i><span id="live-pill-text">--</span></div>
        </div>
        <div>
          <div class="ui-meter"><div class="ui-meter-mask" id="meter-mask"></div><div class="ui-needle" id="meter-needle"></div></div>
          <div class="ui-scale">
            <span id="scale-0">--</span><span id="scale-1">--</span><span id="scale-2">--</span><span id="scale-3">--</span>
          </div>
        </div>
        <div class="ui-facts">
          <div class="ui-fact"><em data-t="fact.peak">Spitze 5 min</em><b id="fact-peak">--</b></div>
          <div class="ui-fact"><em data-t="fact.thresh">Schwellen</em><b id="fact-thresh">-- / --</b></div>
          <div class="ui-fact"><em data-t="fact.mode">Modus</em><b id="fact-mode">--</b></div>
        </div>
      </div>

      <div class="ui-card">
        <div class="ui-sum"><b data-t="hist.title">Verlauf</b><span class="ui-val" data-t="hist.sub">letzte 5 min</span></div>
        <div class="ui-body">
          <svg class="ui-chart" id="history-svg" viewBox="0 0 320 108" role="img" aria-label="History">
            <g id="history-bands"></g>
            <g id="history-lines"></g>
            <g id="history-labels" font-family="ui-monospace,Menlo,monospace" font-size="8.5" fill="var(--ink-3)"></g>
            <polyline id="history-poly" fill="none" stroke="var(--ink)" stroke-width="1.6" stroke-linejoin="round" stroke-linecap="round" points=""></polyline>
            <circle id="history-dot" cx="0" cy="0" r="3" fill="var(--quiet)"></circle>
          </svg>
          <div class="ui-xaxis"><span>&minus;5 min</span><span>&minus;3</span><span>&minus;1</span><span data-t="hist.now">jetzt</span></div>
        </div>
      </div>

      <div class="ui-card">
        <div class="ui-sum"><b data-t="today.title">Heute</b><span class="ui-val" id="today-sub">seit 00:00</span></div>
        <div class="ui-body">
          <div class="ui-bars" id="hourly-bars"></div>
          <div class="ui-xaxis"><span>00</span><span>06</span><span>12</span><span>18</span><span>23</span></div>
          <div class="ui-legend">
            <span><i style="background:var(--quiet)"></i><span id="leg-normal">--</span></span>
            <span><i style="background:var(--warn)"></i><span id="leg-warning">--</span></span>
            <span><i style="background:var(--alert)"></i><span id="leg-alert">--</span></span>
          </div>
          <div class="ui-note" id="hourly-note" style="display:none"></div>
          <div class="ui-row-btn">
            <button class="ui-btn ui-sm" type="button" id="btn-hourly-reset" data-t="today.reset">Tag zur&uuml;cksetzen</button>
            <span class="ui-hint" data-t="today.since">Z&auml;hlt seit 00:00 Uhr heute.</span>
          </div>
        </div>
      </div>

    </div>

    <!-- RIGHT: settings drawers -->
    <div class="ui-col">

      <div class="ui-grouptitle" data-t="group.licht">Licht</div>
      <div class="ui-card">

        <details class="ui-d" id="d-mode">
          <summary class="ui-sum"><b data-t="mode.summary">Anzeigemodus</b><span class="ui-val" id="mode-val">--</span><i class="ui-chev"></i></summary>
          <div class="ui-body">
            <div class="ui-choice">
              <button type="button" class="ui-opt" id="opt-mode-0" data-mode="0">
                <div class="ui-optt"><i class="ui-radio"></i><span data-t="mode.traffic">Ampel</span></div>
                <small data-t="mode.traffic.desc">Der ganze Streifen leuchtet gr&uuml;n, gelb oder rot.</small>
              </button>
              <button type="button" class="ui-opt" id="opt-mode-1" data-mode="1">
                <div class="ui-optt"><i class="ui-radio"></i><span data-t="mode.vu">VU-Meter</span></div>
                <small data-t="mode.vu.desc">Der Ausschlag w&auml;chst mit der Lautst&auml;rke.</small>
              </button>
            </div>
          </div>
        </details>

        <details class="ui-d" id="d-bright">
          <summary class="ui-sum"><b data-t="bright.summary">Helligkeit &amp; Farben</b><span class="ui-val" id="bright-val">--</span><i class="ui-chev"></i></summary>
          <div class="ui-body">
            <div class="ui-field">
              <div class="ui-lab"><span data-t="bright.label">Helligkeit</span><span class="ui-num" id="bright-num">-- / 255</span></div>
              <input type="range" class="ui-range" id="brightness-slider" min="0" max="255" step="1" value="180">
            </div>
            <div class="ui-swatchrow">
              <div class="ui-swatch"><input type="color" id="color-normal" value="#22C55E"><em data-t="sw.normal">Ruhig</em><code id="color-normal-hex">#22C55E</code></div>
              <div class="ui-swatch"><input type="color" id="color-warning" value="#EAB308"><em data-t="sw.warning">Laut</em><code id="color-warning-hex">#EAB308</code></div>
              <div class="ui-swatch"><input type="color" id="color-alert" value="#EF4444"><em data-t="sw.alert">Zu laut</em><code id="color-alert-hex">#EF4444</code></div>
            </div>
          </div>
        </details>

        <details class="ui-d" id="d-thresh">
          <summary class="ui-sum"><b data-t="thresh.summary">Schaltschwellen</b><span class="ui-val" id="thresh-val">-- / -- dB</span><i class="ui-chev"></i></summary>
          <div class="ui-body">
            <div class="ui-strip" id="led-strip"></div>
            <div class="ui-hint" data-t="thresh.previewhint">Vorschau des LED-Streifens mit den aktuellen Werten.</div>

            <div class="ui-field">
              <div class="ui-lab"><span data-t="floor.label">Grundpegel</span><span class="ui-num" id="floor-num">-- dB</span></div>
              <input type="range" class="ui-range" id="floor-slider" min="20" max="60" step="1" value="37">
              <div class="ui-row-btn">
                <button class="ui-btn ui-sm" type="button" id="btn-floor-current" data-t="floor.apply">Aktuellen Pegel &uuml;bernehmen</button>
                <span class="ui-hint" id="floor-current-hint"></span>
              </div>
              <div class="ui-hint" id="suggested-floor-hint" style="display:none"></div>
            </div>

            <div class="ui-field">
              <div class="ui-lab"><span data-t="green.label">Gr&uuml;n wird gelb ab</span><span class="ui-num" id="green-num">-- dB</span></div>
              <input type="range" class="ui-range ui-y" id="green-slider" min="30" max="70" step="1" value="50">
            </div>

            <div class="ui-field">
              <div class="ui-lab"><span data-t="yellow.label">Gelb wird rot ab</span><span class="ui-num" id="yellow-num">-- dB</span></div>
              <input type="range" class="ui-range ui-r" id="yellow-slider" min="40" max="85" step="1" value="65">
            </div>
          </div>
        </details>

        <details class="ui-d" id="d-timing">
          <summary class="ui-sum"><b data-t="timing.summary">Reaktion</b><span class="ui-val" id="timing-val">-- / -- ms</span><i class="ui-chev"></i></summary>
          <div class="ui-body">
            <div class="ui-field">
              <div class="ui-lab"><span data-t="decay.label">Nachleuchten</span><span class="ui-num" id="decay-num">-- ms</span></div>
              <input type="range" class="ui-range" id="decay-slider" min="0" max="3000" step="100" value="1500">
              <div class="ui-hint" data-t="decay.hint">Wie lange die Farbe nach einem Ger&auml;usch stehen bleibt.</div>
            </div>
            <div class="ui-field">
              <div class="ui-lab"><span data-t="response.label">Ansprechzeit</span><span class="ui-num" id="response-num">-- ms</span></div>
              <input type="range" class="ui-range" id="response-slider" min="0" max="500" step="50" value="100">
              <div class="ui-hint" data-t="response.hint">Kleiner Wert = zappeliger, gr&ouml;&szlig;erer Wert = ruhiger.</div>
            </div>
          </div>
        </details>
      </div>

      <div class="ui-row-btn">
        <button class="ui-btn ui-primary ui-wide" type="button" id="btn-save-licht" data-t="save.licht">Lichteinstellungen speichern</button>
      </div>
      <div class="ui-saved" id="save-licht-status"></div>

      <div class="ui-grouptitle" data-t="group.netzwerk">Netzwerk</div>
      <div class="ui-card">
        <details class="ui-d" id="d-wifi">
          <summary class="ui-sum"><b data-t="wifi.summary">WLAN</b><span class="ui-val" id="wifi-val">--</span><i class="ui-chev"></i></summary>
          <div class="ui-body">
            <div class="ui-status" id="wifi-status-line"><i></i><span id="wifi-status-text">--</span></div>
            <div class="ui-field"><div class="ui-lab" data-t="wifi.ssid">Netzwerkname (SSID)</div>
              <input class="ui-in" type="text" id="wifi-ssid" placeholder="Heimnetz-Name"></div>
            <div class="ui-field"><div class="ui-lab" data-t="wifi.pass">Passwort</div>
              <input class="ui-in" type="password" id="wifi-pass" data-t-ph="wifi.pass.hint" placeholder="Leer lassen, um das gespeicherte zu behalten"></div>
            <div class="ui-field"><div class="ui-lab" data-t="wifi.tz">Zeitzone</div>
              <input class="ui-in ui-mono" type="text" id="tz-string" placeholder="CET-1CEST,M3.5.0,M10.5.0/3">
              <div class="ui-hint" data-t="wifi.tz.hint">POSIX-Zeitzone. F&uuml;r Deutschland den Vorgabewert lassen.</div>
            </div>
            <button class="ui-btn ui-wide" type="button" id="btn-save-wifi" data-t="wifi.save">Speichern &amp; verbinden</button>
            <div class="ui-saved" id="wifi-save-status"></div>
          </div>
        </details>
        <details class="ui-d" id="d-mqtt">
          <summary class="ui-sum"><b data-t="mqtt.summary">Home Assistant (MQTT)</b><span class="ui-val" id="mqtt-val">--</span><i class="ui-chev"></i></summary>
          <div class="ui-body">
            <div class="ui-row2">
              <div class="ui-field"><div class="ui-lab" data-t="mqtt.host">Broker</div>
                <input class="ui-in ui-mono" type="text" id="mqtt-host" placeholder="192.168.1.20"></div>
              <div class="ui-field"><div class="ui-lab" data-t="mqtt.port">Port</div>
                <input class="ui-in ui-mono" type="number" id="mqtt-port" min="1" max="65535" value="1883"></div>
            </div>
            <div class="ui-row2">
              <div class="ui-field"><div class="ui-lab" data-t="mqtt.user">Benutzer</div>
                <input class="ui-in" type="text" id="mqtt-user"></div>
              <div class="ui-field"><div class="ui-lab" data-t="mqtt.pass">Passwort</div>
                <input class="ui-in" type="password" id="mqtt-pass" data-t-ph="mqtt.pass.hint" placeholder="unver&auml;ndert"></div>
            </div>
            <button class="ui-btn ui-wide" type="button" id="btn-save-mqtt" data-t="mqtt.save">MQTT speichern</button>
            <div class="ui-saved" id="mqtt-save-status"></div>
          </div>
        </details>
      </div>

      <div class="ui-grouptitle" data-t="group.geraet">Ger&auml;t</div>
      <div class="ui-card">
        <details class="ui-d" id="d-fw">
          <summary class="ui-sum"><b data-t="fw.summary">Firmware aktualisieren</b><span class="ui-val" id="fw-val">--</span><i class="ui-chev"></i></summary>
          <div class="ui-body">
            <div class="ui-field"><div class="ui-lab" data-t="fw.pass">OTA-Passwort</div>
              <input class="ui-in" type="password" id="update-pass"></div>
            <label class="ui-file" for="update-file">
              <span data-t="fw.file.label">Datei</span><code id="update-filename" data-t="fw.file.placeholder">Keine Datei ausgew&auml;hlt</code>
              <input type="file" id="update-file" accept=".bin">
            </label>
            <div>
              <div class="ui-track"><div class="ui-fill" id="update-progress-bar"></div></div>
              <div class="ui-hint" id="update-status" data-t="fw.ready" style="margin-top:6px">Bereit. Das Ger&auml;t startet nach dem Update neu.</div>
            </div>
            <button class="ui-btn ui-wide" type="button" id="btn-upload-fw" data-t="fw.upload">Hochladen &amp; installieren</button>
          </div>
        </details>
        <details class="ui-d" id="d-backup">
          <summary class="ui-sum"><b data-t="backup.summary">Sicherung</b><span class="ui-val" data-t="backup.subtitle">Export / Import</span><i class="ui-chev"></i></summary>
          <div class="ui-body">
            <div class="ui-row-btn">
              <button class="ui-btn" type="button" id="btn-export" data-t="backup.export">Exportieren</button>
              <button class="ui-btn" type="button" id="btn-import" data-t="backup.import">Importieren</button>
            </div>
            <input type="file" id="import-file" accept="application/json" style="display:none">
            <div class="ui-note" id="backup-warn"><b data-t="backup.warn.b">Achtung:</b> <span data-t="backup.warn">Die Exportdatei enth&auml;lt WLAN- und MQTT-Passw&ouml;rter im Klartext. Nicht weitergeben.</span></div>
            <div class="ui-saved" id="backup-status"></div>
          </div>
        </details>
      </div>

    </div>
  </div>

  <div class="ui-foot">
    <span id="fw-version">Firmware --</span>
  </div>
</div>

<script>
(function(){
  "use strict";

  var STR = {
    de: {
      'fact.peak':'Spitze 5 min','fact.thresh':'Schwellen','fact.mode':'Modus',
      'mode.traffic':'Ampel','mode.vu':'VU-Meter',
      'pill.quiet':'Ruhig','pill.warn':'Laut','pill.alert':'Zu laut',
      'hist.title':'Verlauf','hist.sub':'letzte 5 min','hist.now':'jetzt',
      'today.title':'Heute','today.sub':'seit 00:00','today.reset':'Tag zurücksetzen',
      'today.since':'Zählt seit 00:00 Uhr heute.',
      'today.since.prefix':'seit ',
      'today.notsynced':'Uhrzeit noch nicht synchronisiert – Statistik startet, sobald das Gerät die Zeit per NTP erhalten hat.',
      'today.resetconfirm':'Statistik wirklich zurücksetzen?',
      'leg.normal':'Ruhig','leg.warning':'Laut','leg.alert':'Zu laut',
      'group.licht':'Licht','group.netzwerk':'Netzwerk','group.geraet':'Gerät',
      'mode.summary':'Anzeigemodus',
      'mode.traffic.desc':'Der ganze Streifen leuchtet grün, gelb oder rot.',
      'mode.vu.desc':'Der Ausschlag wächst mit der Lautstärke.',
      'bright.summary':'Helligkeit & Farben',
      'bright.label':'Helligkeit',
      'sw.normal':'Ruhig','sw.warning':'Laut','sw.alert':'Zu laut',
      'thresh.summary':'Schaltschwellen',
      'thresh.previewhint':'Vorschau des LED-Streifens mit den aktuellen Werten.',
      'floor.label':'Grundpegel',
      'floor.apply':'Aktuellen Pegel übernehmen',
      'green.label':'Grün wird gelb ab',
      'yellow.label':'Gelb wird rot ab',
      'timing.summary':'Reaktion',
      'decay.label':'Nachleuchten','decay.hint':'Wie lange die Farbe nach einem Geräusch stehen bleibt.',
      'response.label':'Ansprechzeit','response.hint':'Kleiner Wert = zappeliger, größerer Wert = ruhiger.',
      'save.licht':'Lichteinstellungen speichern',
      'save.ok':'Gespeichert um ',
      'save.fail':'Speichern fehlgeschlagen',
      'wifi.summary':'WLAN',
      'wifi.connected':'Verbunden',
      'wifi.notconfigured':'Kein WLAN konfiguriert – AP-Fallback aktiv',
      'wifi.notconnected':'Nicht verbunden – AP-Fallback aktiv',
      'wifi.ssid':'Netzwerkname (SSID)',
      'wifi.pass':'Passwort','wifi.pass.hint':'Leer lassen, um das gespeicherte zu behalten',
      'wifi.tz':'Zeitzone','wifi.tz.hint':'POSIX-Zeitzone. Für Deutschland den Vorgabewert lassen.',
      'wifi.save':'Speichern & verbinden',
      'wifi.saved':'Gespeichert – Gerät verbindet neu (kann bis zu 15s dauern)…',
      'wifi.notset':'kein WLAN',
      'mqtt.summary':'Home Assistant (MQTT)',
      'mqtt.host':'Broker','mqtt.port':'Port','mqtt.user':'Benutzer','mqtt.pass':'Passwort','mqtt.pass.hint':'unverändert',
      'mqtt.save':'MQTT speichern','mqtt.saved':'MQTT-Einstellungen gespeichert',
      'mqtt.disabled':'nicht konfiguriert',
      'mqtt.configured':'konfiguriert',
      'fw.summary':'Firmware aktualisieren',
      'fw.pass':'OTA-Passwort',
      'fw.file.label':'Datei',
      'fw.file.placeholder':'Keine Datei ausgewählt',
      'fw.upload':'Hochladen & installieren',
      'fw.ready':'Bereit. Das Gerät startet nach dem Update neu.',
      'fw.uploading':'Wird hochgeladen…',
      'fw.ok':'Erfolgreich – Gerät startet neu…',
      'fw.fail':'Upload fehlgeschlagen',
      'fw.nofile':'Bitte zuerst eine .bin-Datei auswählen',
      'backup.summary':'Sicherung',
      'backup.subtitle':'Export / Import',
      'backup.export':'Exportieren','backup.import':'Importieren',
      'backup.warn.b':'Achtung:',
      'backup.warn':'Die Exportdatei enthält WLAN- und MQTT-Passwörter im Klartext. Nicht weitergeben.',
      'backup.importok':'Importiert – Seite lädt neu…',
      'backup.importfail':'Import fehlgeschlagen (ungültige Datei?)',
      'foot.firmware':'Firmware ',
      'suggested.prefix':'Vorschlag beim letzten Boot: ',
      'suggested.apply':'übernehmen?'
    },
    en: {
      'fact.peak':'Peak 5 min','fact.thresh':'Thresholds','fact.mode':'Mode',
      'mode.traffic':'Traffic light','mode.vu':'VU meter',
      'pill.quiet':'Quiet','pill.warn':'Loud','pill.alert':'Too loud',
      'hist.title':'History','hist.sub':'last 5 min','hist.now':'now',
      'today.title':'Today','today.sub':'since 00:00','today.reset':'Reset today',
      'today.since':'Counting since 00:00 today.',
      'today.since.prefix':'since ',
      'today.notsynced':'Clock not yet synchronised – stats start once the device gets time via NTP.',
      'today.resetconfirm':'Really reset today’s stats?',
      'leg.normal':'Quiet','leg.warning':'Loud','leg.alert':'Too loud',
      'group.licht':'Light','group.netzwerk':'Network','group.geraet':'Device',
      'mode.summary':'Display mode',
      'mode.traffic.desc':'The whole strip glows green, yellow or red.',
      'mode.vu.desc':'The lit portion grows with volume.',
      'bright.summary':'Brightness & colours',
      'bright.label':'Brightness',
      'sw.normal':'Quiet','sw.warning':'Loud','sw.alert':'Too loud',
      'thresh.summary':'Colour thresholds',
      'thresh.previewhint':'Preview of the LED strip with the current values.',
      'floor.label':'Floor level',
      'floor.apply':'Use current level',
      'green.label':'Green turns yellow at',
      'yellow.label':'Yellow turns red at',
      'timing.summary':'Response',
      'decay.label':'Decay','decay.hint':'How long the colour stays after a sound.',
      'response.label':'Response time','response.hint':'Lower = twitchier, higher = calmer.',
      'save.licht':'Save light settings',
      'save.ok':'Saved at ',
      'save.fail':'Save failed',
      'wifi.summary':'WiFi',
      'wifi.connected':'Connected',
      'wifi.notconfigured':'No WiFi configured – AP fallback active',
      'wifi.notconnected':'Not connected – AP fallback active',
      'wifi.ssid':'Network name (SSID)',
      'wifi.pass':'Password','wifi.pass.hint':'Leave empty to keep the stored one',
      'wifi.tz':'Timezone','wifi.tz.hint':'POSIX timezone string. Leave the default for Germany.',
      'wifi.save':'Save & connect',
      'wifi.saved':'Saved – device is reconnecting (can take up to 15s)…',
      'wifi.notset':'no WiFi',
      'mqtt.summary':'Home Assistant (MQTT)',
      'mqtt.host':'Broker','mqtt.port':'Port','mqtt.user':'User','mqtt.pass':'Password','mqtt.pass.hint':'unchanged',
      'mqtt.save':'Save MQTT','mqtt.saved':'MQTT settings saved',
      'mqtt.disabled':'not configured',
      'mqtt.configured':'configured',
      'fw.summary':'Firmware update',
      'fw.pass':'OTA password',
      'fw.file.label':'File',
      'fw.file.placeholder':'No file selected',
      'fw.upload':'Upload & install',
      'fw.ready':'Ready. The device restarts after the update.',
      'fw.uploading':'Uploading…',
      'fw.ok':'Success – device is restarting…',
      'fw.fail':'Upload failed',
      'fw.nofile':'Please choose a .bin file first',
      'backup.summary':'Backup',
      'backup.subtitle':'Export / Import',
      'backup.export':'Export','backup.import':'Import',
      'backup.warn.b':'Warning:',
      'backup.warn':'The export file contains WiFi and MQTT passwords in plain text. Handle it accordingly.',
      'backup.importok':'Imported – page is reloading…',
      'backup.importfail':'Import failed (invalid file?)',
      'foot.firmware':'Firmware ',
      'suggested.prefix':'Suggested at last boot: ',
      'suggested.apply':'apply?'
    }
  };

  var lang = (function(){
    try {
      var saved = localStorage.getItem('noiselight_lang');
      if (saved === 'de' || saved === 'en') return saved;
    } catch (e) {}
    return (navigator.language || '').toLowerCase().indexOf('de') === 0 ? 'de' : 'en';
  })();

  function t(key) {
    return (STR[lang] && STR[lang][key]) || key;
  }

  // Cached lookup - renderLive() alone calls this ~20x per 200ms tick;
  // re-querying getElementById() every time was measurably heavy enough on
  // iOS Safari to delay native <details> toggle handling (fine on desktop's
  // faster main thread, laggy - multi-second - on a phone).
  var elCache = {};
  function el(id) { return elCache[id] || (elCache[id] = document.getElementById(id)); }

  // ---- cached last-fetched data, re-rendered on language switch ----
  var lastStatus = null, lastConfig = null, lastNetwork = null, lastHourly = null, lastHistory = null;

  function classify(db, cfg) {
    if (db >= cfg.db_warning_switchover) return 2;
    if (db >= cfg.db_normal_switchover) return 1;
    return 0;
  }

  function fmtDb(v) { return (Math.round(v * 10) / 10).toFixed(1); }

  function applyLang() {
    document.documentElement.lang = lang;
    document.querySelectorAll('[data-t]').forEach(function(node) {
      node.textContent = t(node.getAttribute('data-t'));
    });
    document.querySelectorAll('[data-t-ph]').forEach(function(node) {
      node.placeholder = t(node.getAttribute('data-t-ph'));
    });
    el('lang-de').classList.toggle('on', lang === 'de');
    el('lang-en').classList.toggle('on', lang === 'en');
    renderAll();
  }

  function setLang(l) {
    lang = l;
    try { localStorage.setItem('noiselight_lang', lang); } catch (e) {}
    applyLang();
  }

  el('lang-toggle').addEventListener('click', function(e) {
    var t = e.target.closest('[data-lang]');
    if (t) setLang(t.getAttribute('data-lang'));
  });

  // ---- live status / meter ----
  // Meter gradient only depends on cfg (thresholds), not on the live db
  // reading - rebuilding this string 5x/sec (every fetchStatus tick) was
  // pure waste and part of what made <details> toggles laggy on iPhone.
  // Tracked so it's only rebuilt when the underlying thresholds change.
  var meterGradientKey = null;

  function renderMeterGradient(cfg, minDb, maxDb) {
    var pctGreen = Math.max(0, Math.min(100, (cfg.db_normal_switchover - minDb) / (maxDb - minDb) * 100));
    var pctYellow = Math.max(0, Math.min(100, (cfg.db_warning_switchover - minDb) / (maxDb - minDb) * 100));
    var key = pctGreen + '|' + pctYellow;
    if (key === meterGradientKey) return;
    meterGradientKey = key;
    el('meter-mask').parentElement.style.background = 'linear-gradient(90deg,' +
      'var(--quiet) 0%, var(--quiet) ' + pctGreen + '%,' +
      'var(--warn) ' + pctGreen + '%, var(--warn) ' + pctYellow + '%,' +
      'var(--alert) ' + pctYellow + '%, var(--alert) 100%)';
  }

  // The "apply suggested floor" link's content only ever changes when
  // suggested_floor itself changes (set once at boot) - rebuilding its
  // innerHTML (and re-attaching a fresh click listener) on every 200ms tick
  // was the other big contributor to the iPhone <details> lag.
  var suggestedFloorRendered = null;

  function renderSuggestedFloorHint() {
    var hint = el('suggested-floor-hint');
    if (lastStatus.suggested_floor === undefined) return;
    if (suggestedFloorRendered === lastStatus.suggested_floor) return;
    suggestedFloorRendered = lastStatus.suggested_floor;
    hint.innerHTML = t('suggested.prefix') + fmtDb(lastStatus.suggested_floor) + ' dB – ' +
      '<a href="#" id="apply-suggested-floor">' + t('suggested.apply') + '</a>';
    hint.style.display = 'block';
    el('apply-suggested-floor').addEventListener('click', function(e) {
      e.preventDefault();
      el('floor-slider').value = Math.round(lastStatus.suggested_floor);
      onConfigInput();
    });
  }

  function renderLive() {
    if (!lastStatus || !lastConfig) return;
    var db = lastStatus.db;
    var cfg = lastConfig;
    el('live-db').textContent = fmtDb(db);

    var level = classify(db, cfg);
    var pillCls = ['g', 'y', 'r'][level];
    var pillKey = ['pill.quiet', 'pill.warn', 'pill.alert'][level];
    var pill = el('live-pill');
    pill.className = 'ui-pill ' + pillCls;
    el('live-pill-text').textContent = t(pillKey);

    var minDb = cfg.db_floor;
    var maxDb = Math.max(cfg.db_warning_switchover + 20, cfg.db_normal_switchover + 5, minDb + 10);
    var pct = Math.max(0, Math.min(1, (db - minDb) / (maxDb - minDb))) * 100;

    renderMeterGradient(cfg, minDb, maxDb);
    el('meter-needle').style.left = pct + '%';
    el('meter-mask').style.clipPath = 'inset(0 0 0 ' + pct + '%)';

    el('scale-0').textContent = Math.round(minDb);
    el('scale-1').textContent = Math.round(cfg.db_normal_switchover);
    el('scale-2').textContent = Math.round(cfg.db_warning_switchover);
    el('scale-3').textContent = Math.round(maxDb);

    el('fact-thresh').textContent = Math.round(cfg.db_normal_switchover) + ' / ' + Math.round(cfg.db_warning_switchover);
    el('fact-mode').textContent = t(cfg.display_mode === 0 ? 'mode.traffic' : 'mode.vu');

    if (lastHistory && lastHistory.length) {
      var peak = Math.max.apply(null, lastHistory);
      el('fact-peak').textContent = fmtDb(peak) + ' dB';
    }

    el('fw-version').textContent = t('foot.firmware') + (lastStatus.firmware || '--');

    renderSuggestedFloorHint();

    el('floor-current-hint').textContent = fmtDb(db) + ' dB';
  }

  // ---- history SVG ----
  function renderHistory() {
    if (!lastHistory || lastHistory.length < 2) return;
    var data = lastHistory;
    var w = 320, h = 108, padL = 30, padT = 8, padB = 14;
    var plotH = h - padT - padB;
    var minDb = Math.min.apply(null, data) - 2;
    var maxDb = Math.max.apply(null, data) + 2;
    var range = Math.max(1, maxDb - minDb);

    var pts = data.map(function(db, i) {
      var x = padL + (i / (data.length - 1)) * (w - padL);
      var y = padT + (1 - (db - minDb) / range) * plotH;
      return x.toFixed(1) + ',' + y.toFixed(1);
    });
    el('history-poly').setAttribute('points', pts.join(' '));
    var last = pts[pts.length - 1].split(',');
    el('history-dot').setAttribute('cx', last[0]);
    el('history-dot').setAttribute('cy', last[1]);

    var labels = el('history-labels');
    labels.innerHTML = '';
    [minDb, minDb + range * 0.34, minDb + range * 0.67, maxDb].forEach(function(v, i) {
      var y = padT + (1 - (v - minDb) / range) * plotH;
      var text = document.createElementNS('http://www.w3.org/2000/svg', 'text');
      text.setAttribute('x', 0);
      text.setAttribute('y', (y + 3).toFixed(1));
      text.textContent = Math.round(v);
      labels.appendChild(text);
    });
  }

  // ---- hourly stacked bars ----
  function renderHourly() {
    if (!lastHourly) return;
    var data = lastHourly;
    var note = el('hourly-note');
    if (!data.time_synced) {
      note.textContent = t('today.notsynced');
      note.style.display = 'block';
    } else {
      note.style.display = 'none';
    }

    var sub = el('today-sub');
    if (data.reset_at) {
      sub.textContent = t('today.since.prefix') + new Date(data.reset_at * 1000).toLocaleTimeString();
    } else {
      sub.textContent = t('today.sub');
    }

    if (!Array.isArray(data.hours) || data.hours.length !== 24) return;

    var msPerHour = 3600000;
    var totals = [0, 0, 0];
    var bars = el('hourly-bars');
    bars.innerHTML = '';
    data.hours.forEach(function(bucket) {
      var normalMs = bucket[0], warningMs = bucket[1], alertMs = bucket[2];
      totals[0] += normalMs; totals[1] += warningMs; totals[2] += alertMs;
      var total = normalMs + warningMs + alertMs;
      var bar = document.createElement('div');
      if (total <= 0) {
        bar.className = 'ui-bar ui-empty';
      } else {
        bar.className = 'ui-bar';
        var i = document.createElement('i');
        i.style.height = (Math.min(normalMs, msPerHour) / msPerHour * 100) + '%';
        var b = document.createElement('b');
        b.style.height = (Math.min(warningMs, msPerHour) / msPerHour * 100) + '%';
        var s = document.createElement('s');
        s.style.height = (Math.min(alertMs, msPerHour) / msPerHour * 100) + '%';
        bar.appendChild(i); bar.appendChild(b); bar.appendChild(s);
      }
      bars.appendChild(bar);
    });

    var grand = totals[0] + totals[1] + totals[2];
    function pct(v) { return grand > 0 ? Math.round(v / grand * 100) : 0; }
    el('leg-normal').textContent = t('leg.normal') + ' ' + pct(totals[0]) + ' %';
    el('leg-warning').textContent = t('leg.warning') + ' ' + pct(totals[1]) + ' %';
    el('leg-alert').textContent = t('leg.alert') + ' ' + pct(totals[2]) + ' %';
  }

  // ---- Licht config drawers ----
  function ledColorAt(fracDb, cfg) {
    if (fracDb < cfg.db_normal_switchover) return el('color-normal').value;
    if (fracDb < cfg.db_warning_switchover) return el('color-warning').value;
    return el('color-alert').value;
  }

  function renderLichtSummaries() {
    var mode = document.querySelector('#opt-mode-0.on') ? 0 : (document.querySelector('#opt-mode-1.on') ? 1 : 0);
    el('mode-val').textContent = t(mode === 0 ? 'mode.traffic' : 'mode.vu');

    var brightness = parseInt(el('brightness-slider').value, 10);
    el('bright-val').textContent = Math.round(brightness / 255 * 100) + ' %';
    el('bright-num').textContent = brightness + ' / 255';

    var floor = parseInt(el('floor-slider').value, 10);
    var green = parseInt(el('green-slider').value, 10);
    var yellow = parseInt(el('yellow-slider').value, 10);
    el('floor-num').textContent = floor + ' dB';
    el('green-num').textContent = green + ' dB';
    el('yellow-num').textContent = yellow + ' dB';
    el('thresh-val').textContent = green + ' / ' + yellow + ' dB';

    var decay = parseInt(el('decay-slider').value, 10);
    var response = parseInt(el('response-slider').value, 10);
    el('decay-num').textContent = decay + ' ms';
    el('response-num').textContent = response + ' ms';
    el('timing-val').textContent = decay + ' / ' + response + ' ms';

    el('color-normal-hex').textContent = el('color-normal').value.toUpperCase();
    el('color-warning-hex').textContent = el('color-warning').value.toUpperCase();
    el('color-alert-hex').textContent = el('color-alert').value.toUpperCase();

    // slider fill percentages
    setRangeFill(el('brightness-slider'));
    setRangeFill(el('floor-slider'));
    setRangeFill(el('green-slider'));
    setRangeFill(el('yellow-slider'));
    setRangeFill(el('decay-slider'));
    setRangeFill(el('response-slider'));

    // LED strip preview
    var cfg = { db_normal_switchover: green, db_warning_switchover: yellow };
    var strip = el('led-strip');
    strip.innerHTML = '';
    for (var i = 0; i < 13; i++) {
      var dbAt = floor + (i / 12) * (Math.max(yellow, green) + 15 - floor);
      var lamp = document.createElement('i');
      lamp.style.background = ledColorAt(dbAt, cfg);
      strip.appendChild(lamp);
    }
  }

  function setRangeFill(input) {
    var min = parseFloat(input.min), max = parseFloat(input.max), val = parseFloat(input.value);
    var pct = max > min ? (val - min) / (max - min) * 100 : 0;
    input.style.setProperty('--pct', pct + '%');
  }

  function setModeUi(mode) {
    document.getElementById('opt-mode-0').classList.toggle('on', mode === 0);
    document.getElementById('opt-mode-1').classList.toggle('on', mode === 1);
    document.getElementById('opt-mode-0').querySelector('.ui-radio');
  }

  function onConfigInput() {
    renderLichtSummaries();
    renderLive();
  }

  ['brightness-slider', 'floor-slider', 'green-slider', 'yellow-slider', 'decay-slider', 'response-slider'].forEach(function(id) {
    el(id).addEventListener('input', onConfigInput);
  });
  ['color-normal', 'color-warning', 'color-alert'].forEach(function(id) {
    el(id).addEventListener('input', onConfigInput);
  });
  [0, 1].forEach(function(m) {
    el('opt-mode-' + m).addEventListener('click', function() {
      setModeUi(m);
      onConfigInput();
    });
  });
  el('btn-floor-current').addEventListener('click', function() {
    if (lastStatus) {
      el('floor-slider').value = Math.round(lastStatus.db);
      onConfigInput();
    }
  });

  function currentMode() {
    return document.getElementById('opt-mode-1').classList.contains('on') ? 1 : 0;
  }

  function hexToInt(hex) { return parseInt(hex.substring(1), 16); }

  el('btn-save-licht').addEventListener('click', function() {
    var body = {
      display_mode: currentMode(),
      db_floor: parseInt(el('floor-slider').value, 10),
      db_normal_switchover: parseInt(el('green-slider').value, 10),
      db_warning_switchover: parseInt(el('yellow-slider').value, 10),
      led_brightness: parseInt(el('brightness-slider').value, 10),
      color_normal: hexToInt(el('color-normal').value),
      color_warning: hexToInt(el('color-warning').value),
      color_alert: hexToInt(el('color-alert').value),
      decay_ms: parseInt(el('decay-slider').value, 10),
      response_ms: parseInt(el('response-slider').value, 10)
    };
    fetch('/api/config', {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body)
    }).then(function(res) {
      var status = el('save-licht-status');
      if (res.ok) {
        status.textContent = '✓ ' + t('save.ok') + new Date().toLocaleTimeString();
      } else {
        status.textContent = '✗ ' + t('save.fail');
      }
      setTimeout(function() { status.textContent = ''; }, 4000);
    }).catch(function() {
      el('save-licht-status').textContent = '✗ ' + t('save.fail');
    });
  });

  // ---- Network / MQTT ----
  function renderNetwork() {
    if (!lastNetwork) return;
    var n = lastNetwork;
    el('wifi-ssid').value = n.wifi_ssid || '';
    el('tz-string').value = n.tz_string || '';
    el('mqtt-host').value = n.mqtt_host || '';
    el('mqtt-port').value = n.mqtt_port || 1883;
    el('mqtt-user').value = n.mqtt_user || '';

    var wifiLine = el('wifi-status-line');
    var wifiText = el('wifi-status-text');
    if (n.wifi_connected) {
      wifiLine.classList.remove('ui-off');
      wifiText.textContent = t('wifi.connected') + (n.wifi_ssid ? ' · ' + n.wifi_ssid : '');
      el('wifi-val').textContent = n.wifi_ssid || t('wifi.connected');
    } else {
      wifiLine.classList.add('ui-off');
      wifiText.textContent = n.wifi_ssid ? t('wifi.notconnected') : t('wifi.notconfigured');
      el('wifi-val').textContent = n.wifi_ssid || t('wifi.notset');
    }

    el('mqtt-val').textContent = n.mqtt_host ? t('mqtt.configured') : t('mqtt.disabled');
  }

  el('btn-save-wifi').addEventListener('click', function() {
    var body = {
      wifi_ssid: el('wifi-ssid').value,
      wifi_pass: el('wifi-pass').value,
      tz_string: el('tz-string').value
    };
    fetch('/api/network', {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body)
    }).then(function(res) {
      var status = el('wifi-save-status');
      status.textContent = res.ok ? ('✓ ' + t('wifi.saved')) : ('✗ ' + t('save.fail'));
      el('wifi-pass').value = '';
      setTimeout(function() { status.textContent = ''; fetchNetwork(); }, 4000);
    }).catch(function() {
      el('wifi-save-status').textContent = '✗ ' + t('save.fail');
    });
  });

  el('btn-save-mqtt').addEventListener('click', function() {
    var body = {
      mqtt_host: el('mqtt-host').value,
      mqtt_port: parseInt(el('mqtt-port').value, 10) || 1883,
      mqtt_user: el('mqtt-user').value,
      mqtt_pass: el('mqtt-pass').value
    };
    fetch('/api/network', {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body)
    }).then(function(res) {
      var status = el('mqtt-save-status');
      status.textContent = res.ok ? ('✓ ' + t('mqtt.saved')) : ('✗ ' + t('save.fail'));
      el('mqtt-pass').value = '';
      setTimeout(function() { status.textContent = ''; fetchNetwork(); }, 4000);
    }).catch(function() {
      el('mqtt-save-status').textContent = '✗ ' + t('save.fail');
    });
  });

  // ---- Hourly reset ----
  el('btn-hourly-reset').addEventListener('click', function() {
    if (!confirm(t('today.resetconfirm'))) return;
    fetch('/api/hourly/reset', { method: 'POST' }).then(fetchHourly).catch(function() {});
  });

  // ---- Firmware update ----
  el('update-file').addEventListener('change', function() {
    var f = this.files[0];
    el('update-filename').textContent = f ? f.name : t('fw.file.placeholder');
    el('update-filename').removeAttribute('data-t');
  });

  el('btn-upload-fw').addEventListener('click', function() {
    var fileInput = el('update-file');
    var password = el('update-pass').value;
    var status = el('update-status');
    var bar = el('update-progress-bar');

    var file = fileInput.files[0];
    if (!file) {
      status.removeAttribute('data-t');
      status.textContent = '✗ ' + t('fw.nofile');
      return;
    }

    var xhr = new XMLHttpRequest();
    xhr.open('POST', '/update', true);
    xhr.setRequestHeader('Authorization', 'Basic ' + btoa('admin:' + password));

    bar.style.width = '0%';
    status.removeAttribute('data-t');
    status.textContent = t('fw.uploading');

    xhr.upload.onprogress = function(e) {
      if (e.lengthComputable) {
        bar.style.width = ((e.loaded / e.total) * 100) + '%';
      }
    };

    xhr.onload = function() {
      if (xhr.status === 200) {
        status.textContent = '✓ ' + t('fw.ok');
        setTimeout(function() { location.reload(); }, 8000);
      } else {
        var message = t('fw.fail') + ' (HTTP ' + xhr.status + ')';
        try {
          var data = JSON.parse(xhr.responseText);
          if (data.message) message = data.message;
        } catch (e) {}
        status.textContent = '✗ ' + message;
      }
    };

    xhr.onerror = function() {
      status.textContent = '✗ ' + t('fw.fail');
    };

    var formData = new FormData();
    formData.append('firmware', file);
    xhr.send(formData);
  });

  // ---- Config export/import ----
  el('btn-export').addEventListener('click', function() {
    fetch('/api/config/export').then(function(res) { return res.blob(); }).then(function(blob) {
      var url = URL.createObjectURL(blob);
      var a = document.createElement('a');
      a.href = url;
      a.download = 'noiselight-config.json';
      document.body.appendChild(a);
      a.click();
      a.remove();
      URL.revokeObjectURL(url);
    }).catch(function() {});
  });

  el('btn-import').addEventListener('click', function() { el('import-file').click(); });

  el('import-file').addEventListener('change', function(event) {
    var file = event.target.files[0];
    event.target.value = '';
    if (!file) return;

    var status = el('backup-status');
    file.text().then(function(text) {
      return fetch('/api/config/import', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: text
      });
    }).then(function(res) {
      if (res.ok) {
        status.textContent = '✓ ' + t('backup.importok');
        setTimeout(function() { location.reload(); }, 2000);
      } else {
        status.textContent = '✗ ' + t('backup.importfail');
      }
    }).catch(function() {
      status.textContent = '✗ ' + t('backup.importfail');
    });
  });

  // ---- fetch loops ----
  function fetchStatus() {
    fetch('/api/status').then(function(res) { return res.json(); }).then(function(data) {
      lastStatus = data;
      renderLive();
    }).catch(function() {});
  }

  function fetchHistory() {
    fetch('/api/history').then(function(res) { return res.json(); }).then(function(data) {
      if (Array.isArray(data) && data.length >= 2) {
        lastHistory = data;
        renderHistory();
        renderLive();
      }
    }).catch(function() {});
  }

  function fetchHourly() {
    fetch('/api/hourly').then(function(res) { return res.json(); }).then(function(data) {
      lastHourly = data;
      renderHourly();
    }).catch(function() {});
  }

  function fetchNetwork() {
    fetch('/api/network').then(function(res) { return res.json(); }).then(function(data) {
      lastNetwork = data;
      renderNetwork();
    }).catch(function() {});
  }

  function fetchConfig() {
    fetch('/api/config').then(function(res) { return res.json(); }).then(function(data) {
      lastConfig = data;
      setModeUi(data.display_mode);
      el('brightness-slider').value = data.led_brightness;
      el('floor-slider').value = Math.round(data.db_floor);
      el('green-slider').value = Math.round(data.db_normal_switchover);
      el('yellow-slider').value = Math.round(data.db_warning_switchover);
      el('decay-slider').value = data.decay_ms;
      el('response-slider').value = data.response_ms;
      el('color-normal').value = '#' + ('000000' + data.color_normal.toString(16).toUpperCase()).slice(-6);
      el('color-warning').value = '#' + ('000000' + data.color_warning.toString(16).toUpperCase()).slice(-6);
      el('color-alert').value = '#' + ('000000' + data.color_alert.toString(16).toUpperCase()).slice(-6);
      renderLichtSummaries();
      renderLive();
    }).catch(function() {});
  }

  function renderAll() {
    renderLive();
    renderHistory();
    renderHourly();
    renderNetwork();
    if (lastConfig) renderLichtSummaries();
  }

  fetchConfig();
  fetchNetwork();
  fetchStatus();
  setInterval(fetchStatus, 200);
  fetchHistory();
  setInterval(fetchHistory, 5000);
  fetchHourly();
  setInterval(fetchHourly, 60000);

  applyLang();
})();
</script>
</body>
</html>
)rawliteral";
