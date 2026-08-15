#ifndef NET_MANAGER_H
#define NET_MANAGER_H

#include <Arduino.h>

// Fallback AP credentials, used when no WiFi SSID is configured yet or the
// configured network can't be reached at boot - same values as the
// previous AP-only behavior, so first-time setup still works unchanged.
#define AP_FALLBACK_SSID "noiselight"
#define AP_FALLBACK_PASSWORD "12345678"

#define MDNS_HOSTNAME "noiselight"

// WiFi STA connect timeout before falling back to AP mode.
#define WIFI_CONNECT_TIMEOUT_MS 15000

// Default POSIX TZ string (see NetworkSettings::tz_string) - Europe/Berlin,
// including its DST rule (CEST from the last Sunday in March to the last
// Sunday in October). Just a starting point; overridable via the web UI
// for any other region.
#define TZ_STRING_DEFAULT "CET-1CEST,M3.5.0,M10.5.0/3"

// Baked in at build time from a gitignored .env file (see .env.example and
// load_env.py) - only used as the first-boot NVS default in
// NetworkService::loadSettings(). Once WiFi settings are saved via the web
// UI, the NVS-stored value always wins; this just lets a device join a
// known network without ever needing to connect to its AP fallback.
#ifndef WIFI_SSID_DEFAULT
#define WIFI_SSID_DEFAULT ""
#endif
#ifndef WIFI_PASSWORD_DEFAULT
#define WIFI_PASSWORD_DEFAULT ""
#endif

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

  // POSIX TZ string (e.g. "CET-1CEST,M3.5.0,M10.5.0/3" for Europe/Berlin),
  // used to convert NTP-synced UTC to local wall-clock time for the
  // hourly-stats hour-of-day buckets. Passed straight to configTzTime(),
  // which - unlike a plain fixed UTC offset - correctly applies the DST
  // transition rule encoded in the string, so hour buckets stay correct
  // across summer/winter time changes without needing manual adjustment.
  String tz_string = TZ_STRING_DEFAULT;
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

  // Persists new_settings. Only reconnects WiFi (which briefly disrupts
  // connectivity/blocks the caller) if wifi_ssid/wifi_pass actually
  // changed - e.g. saving MQTT-only settings must not bounce the WiFi link.
  void applySettings(const NetworkSettings& new_settings);
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

#endif // NET_MANAGER_H
