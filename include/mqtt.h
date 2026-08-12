#ifndef MQTT_H
#define MQTT_H

#include <Arduino.h>
#include <WiFiClient.h>
#include <PubSubClient.h>

#define MQTT_STATE_INTERVAL_MS 2000     // How often the state topic is republished
#define MQTT_RECONNECT_INTERVAL_MS 5000 // Min gap between reconnect attempts

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

  // Non-blocking: maintains the connection (reconnect with a fixed retry
  // interval, no separate task), pumps PubSubClient's own loop(), and
  // republishes state every MQTT_STATE_INTERVAL_MS. Call from the existing
  // 50ms web task loop (WebService::webTaskHandler()), same as
  // NetworkService::handleOta() - MqttService needs NetworkSettings and
  // WebService::getCurrentDb()/getConfigSnapshot(), both of which are only
  // safe to read from that task (or don't need extra locking there).
  void loop();

private:
  void reconnect();
  void publishDiscovery();
  void publishState();
  String deviceId() const;   // noiselight-<last 6 hex of MAC>
  String topicState() const;
  String topicAvailability() const;

  WiFiClient wifi_client;
  PubSubClient client;

  String applied_host;   // host/port the client is currently configured for,
  uint16_t applied_port; // so a settings change is picked up without reboot

  unsigned long last_reconnect_attempt;
  unsigned long last_state_publish;
  bool discovery_sent;
};

// Global instance
extern MqttService mqtt_service;

#endif // MQTT_H
