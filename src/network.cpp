#include "network.h"
#include "config.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoOTA.h>

// Global instance
NetworkService network_service;

NetworkService::NetworkService() {
}

void NetworkService::init() {
  log_i("[NET] Initializing network...");

  loadSettings();

  bool sta_ok = false;
  if (settings.wifi_ssid.length() > 0) {
    sta_ok = connectSta(settings.wifi_ssid, settings.wifi_pass);
  } else {
    log_i("[NET] No WiFi SSID configured, skipping STA connect");
  }

  if (sta_ok) {
    startMdns();
    startOta();
  } else {
    startApFallback();
    // mDNS also works while in AP mode (clients on the AP's subnet can
    // still resolve noiselight.local), so start it either way. OTA stays
    // off in AP mode - espota targets noiselight.local, which only
    // resolves usefully once the device is actually on the home network.
    startMdns();
  }
}

void NetworkService::startOta() {
  ArduinoOTA.setHostname(MDNS_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.begin();
  ota_started = true;
  log_i("[NET] ArduinoOTA ready (hostname=%s)", MDNS_HOSTNAME);
}

void NetworkService::handleOta() {
  if (ota_started) {
    ArduinoOTA.handle();
  }
}

bool NetworkService::connectSta(const String& ssid, const String& pass) {
  log_i("[NET] Connecting to WiFi SSID '%s'...", ssid.c_str());

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    log_i("[NET] WiFi connected: http://%s", WiFi.localIP().toString().c_str());
    return true;
  }

  log_i("[NET] WiFi connect timed out after %dms, falling back to AP mode", WIFI_CONNECT_TIMEOUT_MS);
  WiFi.disconnect(true);
  return false;
}

void NetworkService::startApFallback() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_FALLBACK_SSID, AP_FALLBACK_PASSWORD);
  IPAddress ap_ip = WiFi.softAPIP();
  log_i("[NET] AP fallback started: http://%s", ap_ip.toString().c_str());
}

void NetworkService::startMdns() {
  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    log_i("[NET] mDNS started: http://%s.local", MDNS_HOSTNAME);
  } else {
    log_e("[NET] mDNS init failed");
  }
}

bool NetworkService::isStaConnected() const {
  return WiFi.getMode() == WIFI_STA && WiFi.status() == WL_CONNECTED;
}

void NetworkService::applySettings(const NetworkSettings& new_settings) {
  bool wifi_changed = (new_settings.wifi_ssid != settings.wifi_ssid) ||
                       (new_settings.wifi_pass != settings.wifi_pass);

  settings = new_settings;
  saveSettings();

  if (!wifi_changed) {
    return;  // e.g. an MQTT-only save - don't bounce a working WiFi link
  }

  // Reconnect immediately so a changed SSID/password takes effect without
  // requiring a reboot. If the new credentials don't work, fall back to AP
  // mode just like at boot.
  bool sta_ok = false;
  if (settings.wifi_ssid.length() > 0) {
    sta_ok = connectSta(settings.wifi_ssid, settings.wifi_pass);
  }
  if (!sta_ok) {
    startApFallback();
  }
  startMdns();
  if (sta_ok && !ota_started) {
    startOta();
  }
}

void NetworkService::loadSettings() {
  Preferences prefs;
  prefs.begin("network", true);  // readonly
  settings.wifi_ssid = prefs.getString("wifi_ssid", "");
  settings.wifi_pass = prefs.getString("wifi_pass", "");
  settings.mqtt_host = prefs.getString("mqtt_host", "");
  settings.mqtt_port = prefs.getUShort("mqtt_port", 1883);
  settings.mqtt_user = prefs.getString("mqtt_user", "");
  settings.mqtt_pass = prefs.getString("mqtt_pass", "");
  prefs.end();

  log_i("[NET] Settings loaded (ssid=%s, mqtt_host=%s)",
    settings.wifi_ssid.length() ? settings.wifi_ssid.c_str() : "<none>",
    settings.mqtt_host.length() ? settings.mqtt_host.c_str() : "<none>");
}

void NetworkService::saveSettings() {
  Preferences prefs;
  prefs.begin("network", false);  // readwrite
  prefs.putString("wifi_ssid", settings.wifi_ssid);
  prefs.putString("wifi_pass", settings.wifi_pass);
  prefs.putString("mqtt_host", settings.mqtt_host);
  prefs.putUShort("mqtt_port", settings.mqtt_port);
  prefs.putString("mqtt_user", settings.mqtt_user);
  prefs.putString("mqtt_pass", settings.mqtt_pass);
  prefs.end();

  log_i("[NET] Settings saved");
}
