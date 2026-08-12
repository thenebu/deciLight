#include "mqtt.h"
#include "network.h"
#include "web.h"
#include "led.h"
#include <ArduinoJson.h>
#include <WiFi.h>

// Global instance
MqttService mqtt_service;

MqttService::MqttService()
  : client(wifi_client),
    applied_host(""),
    applied_port(0),
    last_reconnect_attempt(0),
    last_state_publish(0),
    discovery_sent(false)
{
}

void MqttService::init() {
  // Discovery payloads (with the shared device block) run a few hundred
  // bytes past PubSubClient's 128-byte default buffer.
  client.setBufferSize(1024);
}

String MqttService::deviceId() const {
  String mac = WiFi.macAddress();  // "AA:BB:CC:DD:EE:FF"
  mac.replace(":", "");
  String last6 = mac.substring(mac.length() - 6);
  last6.toLowerCase();
  return "noiselight-" + last6;
}

String MqttService::topicState() const {
  return "noiselight/" + deviceId() + "/state";
}

String MqttService::topicAvailability() const {
  return "noiselight/" + deviceId() + "/availability";
}

void MqttService::loop() {
  NetworkSettings settings = network_service.getSettings();

  if (settings.mqtt_host.length() == 0 || !network_service.isStaConnected()) {
    return;  // Nothing configured, or no route to a broker anyway
  }

  // Pick up a broker host/port change without requiring a reboot.
  if (settings.mqtt_host != applied_host || settings.mqtt_port != applied_port) {
    if (client.connected()) client.disconnect();
    applied_host = settings.mqtt_host;
    applied_port = settings.mqtt_port;
    client.setServer(applied_host.c_str(), applied_port);
    discovery_sent = false;
  }

  if (!client.connected()) {
    unsigned long now = millis();
    if (now - last_reconnect_attempt >= MQTT_RECONNECT_INTERVAL_MS) {
      last_reconnect_attempt = now;
      reconnect();
    }
    return;
  }

  client.loop();

  if (!discovery_sent) {
    publishDiscovery();
  }

  unsigned long now = millis();
  if (now - last_state_publish >= MQTT_STATE_INTERVAL_MS) {
    last_state_publish = now;
    publishState();
  }
}

void MqttService::reconnect() {
  NetworkSettings settings = network_service.getSettings();
  String id = deviceId();
  String avail = topicAvailability();

  const char* user = settings.mqtt_user.length() ? settings.mqtt_user.c_str() : nullptr;
  const char* pass = settings.mqtt_pass.length() ? settings.mqtt_pass.c_str() : nullptr;

  log_i("[MQTT] Connecting to %s:%u as %s...", settings.mqtt_host.c_str(), settings.mqtt_port, id.c_str());

  bool ok = client.connect(id.c_str(), user, pass, avail.c_str(), 0, true, "offline");
  if (ok) {
    log_i("[MQTT] Connected");
    client.publish(avail.c_str(), "online", true);
    discovery_sent = false;  // Re-announce after every (re)connect
  } else {
    log_i("[MQTT] Connect failed, rc=%d", client.state());
  }
}

void MqttService::publishDiscovery() {
  String id = deviceId();
  String state_topic = topicState();
  String avail_topic = topicAvailability();

  // Shared "device" block groups both sensors under one HA device entry.
  DynamicJsonDocument device_doc(256);
  JsonObject device = device_doc.to<JsonObject>();
  JsonArray idents = device.createNestedArray("identifiers");
  idents.add(id);
  device["name"] = "Noise Light";
  device["manufacturer"] = "deciLight";
  device["model"] = "ESP32-S3 Noise Traffic Light";

  {
    DynamicJsonDocument doc(768);
    doc["name"] = "Noise Level";
    doc["unique_id"] = id + "_db";
    doc["state_topic"] = state_topic;
    doc["availability_topic"] = avail_topic;
    doc["value_template"] = "{{ value_json.db }}";
    doc["unit_of_measurement"] = "dB";
    doc["state_class"] = "measurement";
    doc["device"] = device;

    String topic = "homeassistant/sensor/" + id + "/db/config";
    String payload;
    serializeJson(doc, payload);
    client.publish(topic.c_str(), payload.c_str(), true);
  }

  {
    DynamicJsonDocument doc(768);
    doc["name"] = "Noise Level Status";
    doc["unique_id"] = id + "_level";
    doc["state_topic"] = state_topic;
    doc["availability_topic"] = avail_topic;
    doc["value_template"] = "{{ value_json.level }}";
    doc["icon"] = "mdi:traffic-light";
    doc["device"] = device;

    String topic = "homeassistant/sensor/" + id + "/level/config";
    String payload;
    serializeJson(doc, payload);
    client.publish(topic.c_str(), payload.c_str(), true);
  }

  discovery_sent = true;
  log_i("[MQTT] Discovery published for %s", id.c_str());
}

void MqttService::publishState() {
  double db = web_service.getCurrentDb();
  Config cfg = web_service.getConfigSnapshot();
  NoiseLevel level = led_controller.getLevelForDb(db, cfg);

  const char* level_str = (level == NORMAL) ? "normal" : (level == WARNING) ? "warning" : "alert";

  DynamicJsonDocument doc(128);
  doc["db"] = db;
  doc["level"] = level_str;

  String payload;
  serializeJson(doc, payload);
  client.publish(topicState().c_str(), payload.c_str());
}
