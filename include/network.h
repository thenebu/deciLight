#ifndef NETWORK_H
#define NETWORK_H

#include <Arduino.h>

// Fallback AP credentials, used when no WiFi SSID is configured yet or the
// configured network can't be reached at boot - same values as the
// previous AP-only behavior, so first-time setup still works unchanged.
#define AP_FALLBACK_SSID "NoiseLight"
#define AP_FALLBACK_PASSWORD "12345678"

#define MDNS_HOSTNAME "noiselight"

// WiFi STA connect timeout before falling back to AP mode.
#define WIFI_CONNECT_TIMEOUT_MS 15000

//
// NetworkSettings - WiFi/MQTT credentials, kept OUT of the Config struct in
// web.h. Config is copied under a portMUX_TYPE spinlock (see
// WebService::getConfigSnapshot()) for cross-core safety; String members
// allocate on the heap, which is not safe to do inside a critical section
// on ESP32. NetworkSettings is only ever read/written from the web task,
// so it needs no lock of its own.
//
struct NetworkSettings {
  String wifi_ssid;
  String wifi_pass;

  String mqtt_host;
  uint16_t mqtt_port = 1883;
  String mqtt_user;
  String mqtt_pass;
};

//
// NetworkService Class - Manages WiFi mode (STA with AP fallback) and mDNS.
// Same singleton + init() pattern as WebService/LEDController/Microphone.
//
class NetworkService {
public:
  NetworkService();
  void init();  // Load settings, connect WiFi (STA or AP fallback), start mDNS

  // Thread-safe-by-construction accessors: NetworkSettings is only touched
  // from the web task (HTTP handlers run there, and init() itself runs
  // during setup() before other tasks exist), so no lock is needed.
  NetworkSettings getSettings() const { return settings; }
  void applySettings(const NetworkSettings& new_settings);  // Save + reconnect
  bool isStaConnected() const;

  // Pumps ArduinoOTA's handler - call from the existing 50ms web task loop
  // (WebService::webTaskHandler()), no separate task needed. Safe to call
  // even when OTA hasn't been started (STA never connected) - no-ops then.
  void handleOta();

private:
  void loadSettings();
  void saveSettings();
  bool connectSta(const String& ssid, const String& pass);
  void startApFallback();
  void startMdns();
  void startOta();

  bool ota_started = false;

  NetworkSettings settings;
};

// Global instance
extern NetworkService network_service;

#endif // NETWORK_H
