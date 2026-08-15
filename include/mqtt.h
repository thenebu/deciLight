#ifndef MQTT_H
#define MQTT_H

#include <Arduino.h>
#include <WiFiClient.h>
#include <PubSubClient.h>

#define MQTT_STATE_INTERVAL_MS 2000     // How often the state topic is republished
#define MQTT_DEBUG_INTERVAL_MS 60000    // How often the debug/diagnostics topic is republished (slow-changing, no need for 2s cadence)
#define MQTT_RECONNECT_INTERVAL_MS 5000 // Min gap between reconnect attempts
#define MQTT_SOCKET_TIMEOUT_S 2         // Bounds how long a failed connect() can block the web task

//
// MqttService Class - MQTT connection + Home Assistant discovery.
// Same singleton + init() pattern as WebService/LEDController/Microphone.
// Speaks MQTT only - no InfluxDB client. HA's own InfluxDB integration
// picks up whatever HA logs from these entities (see plan notes).
//
class MqttService {
public:
  MqttService();
  void init();  // Prepare the PubSubClient (server/buffer/callback); does not block on connect

  // Maintains the connection (reconnect gated by MQTT_RECONNECT_INTERVAL_MS,
  // no separate task), pumps PubSubClient's own loop(), and republishes
  // state every MQTT_STATE_INTERVAL_MS. Call from the existing 50ms web
  // task loop (WebService::webTaskHandler()), same as
  // NetworkService::handleOta() - MqttService needs NetworkSettings and
  // WebService::getCurrentDb()/getConfigSnapshot(), both of which are only
  // safe to read from that task (or don't need extra locking there).
  //
  // NOT fully non-blocking: PubSubClient::connect() itself blocks on the
  // underlying WiFiClient::connect() TCP handshake, bounded by
  // MQTT_SOCKET_TIMEOUT_S (set via client.setSocketTimeout() in init()).
  // A misconfigured/unreachable broker therefore stalls this task - and
  // with it HTTP serving and OTA - for up to that long, once per
  // MQTT_RECONNECT_INTERVAL_MS. Kept short deliberately; a truly
  // non-blocking reconnect would need an async TCP state machine, which
  // is more than this single-task architecture (matching the OTA
  // precedent) is set up for.
  void loop();

private:
  void reconnect();
  void publishDiscovery();
  void publishState();
  void publishDebug();

  // Builds device_id/topic_* once and caches them. They derive only from the
  // MAC address, which never changes at runtime, but deviceId() used to
  // rebuild them from WiFi.macAddress() on every call - and topicState() is
  // called every MQTT_STATE_INTERVAL_MS, each time allocating and freeing
  // half a dozen temporary Strings.
  void ensureTopics();

  String device_id;         // noiselight-<last 6 hex of MAC>
  String topic_state;
  String topic_availability;
  String topic_debug;
  bool topics_cached;

  WiFiClient wifi_client;
  PubSubClient client;

  String applied_host;   // host/port the client is currently configured for,
  uint16_t applied_port; // so a settings change is picked up without reboot

  unsigned long last_reconnect_attempt;
  unsigned long last_state_publish;
  unsigned long last_debug_publish;
  bool discovery_sent;
};

// Global instance
extern MqttService mqtt_service;

#endif // MQTT_H
