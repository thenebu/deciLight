#include "web.h"
#include "config.h"
#include <Preferences.h>
#include <WiFi.h>
#include <ArduinoJson.h>

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
    task_handle(nullptr),
    config_mux(portMUX_INITIALIZER_UNLOCKED),
    dB_mux(portMUX_INITIALIZER_UNLOCKED)
{
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
  
  // Start WiFi in AP mode
  WiFi.mode(WIFI_AP);
  log_i("[WEB] WiFi mode set to AP");
  
  WiFi.softAP("NoiseLight", "12345678");  // SSID, Password
  log_i("[WEB] WiFi AP configured");
  
  IPAddress ap_ip = WiFi.softAPIP();
  log_i("[WEB] WiFi AP Started: http://%s", ap_ip.toString().c_str());
  
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
  
  server->onNotFound([this]() { this->handleNotFound(); });
  log_i("[WEB] 404 handler registered");
  
  server->begin();
  log_i("[WEB] Web server started on port 80");
  
  // Load config from storage
  log_i("[WEB] Loading config from NVS...");
  loadConfig();
  log_i("[WEB] Config loaded");
}

//============================================
// WebService::startTask() - Create FreeRTOS task
//============================================
void WebService::startTask() {
  log_i("[WEB] Creating WebTask...");
  
  BaseType_t ret = xTaskCreatePinnedToCore(
    WebService::webTaskWrapper,  // Static wrapper function
    "WebTask",                   // Task name
    4096,                        // Stack size
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
    
    // Check if config save is pending (deferred from HTTP handler)
    if (needs_save) {
      needs_save = false;
      saveConfig();
      log_i("Config updated via web: decay=%dms response=%dms",
        config.decay_ms, config.response_ms);
    }
    
    vTaskDelay(pdMS_TO_TICKS(50));  // Yield for 50ms
  }
}

//============================================
// WebService::updateLevel() - Update current dB level
//============================================
void WebService::updateLevel(double dB_current) {
  portENTER_CRITICAL(&dB_mux);
  current_dB = dB_current;
  last_dB_update = millis();
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

  DynamicJsonDocument doc(64);
  doc["db"] = dB_snapshot;
  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
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

// Embedded HTML UI (kept as static const for readability)
const char* html_ui = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Noise Light Config</title>
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
    <h1>🎵 Noise Light</h1>
    
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
  </div>

  <script>
    async function updateLiveLevel() {
      try {
        const res = await fetch('/api/status');
        const data = await res.json();
        document.getElementById('live-db').textContent = data.db.toFixed(1);
        
        const minDb = 37;
        const maxDb = 80;
        const normalized = Math.max(0, Math.min(1, (data.db - minDb) / (maxDb - minDb)));
        document.getElementById('live-bar').style.width = (normalized * 100) + '%';
      } catch (e) {
        console.error('Status update failed:', e);
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

    window.onload = function() {
      loadConfig();
      updateLiveLevel();
      setInterval(updateLiveLevel, 200);
    };
  </script>
</body>
</html>
)rawliteral";
