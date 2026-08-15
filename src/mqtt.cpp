#include "mqtt.h"
#include "config.h"
#include "net_manager.h"
#include "web.h"
#include "led.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_system.h>
#include <time.h>

// Global instance
MqttService mqtt_service;

MqttService::MqttService()
  : client(wifi_client),
    applied_host(""),
    applied_port(0),
    last_reconnect_attempt(0),
    last_state_publish(0),
    last_debug_publish(0),
    discovery_sent(false)
{
}

void MqttService::init() {
  // Discovery payloads (with the shared device block) run a few hundred
  // bytes past PubSubClient's 128-byte default buffer.
  client.setBufferSize(1024);

  // PubSubClient's default socket timeout (15s) means a single failed
  // connect() to an unreachable/misconfigured broker would stall this
  // service's shared web task for that long (see the "NOT fully
  // non-blocking" note in mqtt.h) - shorten it so a bad broker config
  // degrades HTTP/OTA responsiveness far less.
  client.setSocketTimeout(MQTT_SOCKET_TIMEOUT_S);
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

String MqttService::topicDebug() const {
  return "noiselight/" + deviceId() + "/debug";
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
  if (now - last_debug_publish >= MQTT_DEBUG_INTERVAL_MS) {
    last_debug_publish = now;
    publishDebug();
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
  device["name"] = "noiselight";
  device["manufacturer"] = "deciLight";
  device["model"] = "ESP32-S3 noiselight";

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

  String debug_topic = topicDebug();

  {
    DynamicJsonDocument doc(768);
    doc["name"] = "Firmware Version";
    doc["unique_id"] = id + "_firmware";
    doc["state_topic"] = debug_topic;
    doc["availability_topic"] = avail_topic;
    doc["value_template"] = "{{ value_json.firmware }}";
    doc["icon"] = "mdi:chip";
    doc["entity_category"] = "diagnostic";
    doc["device"] = device;

    String topic = "homeassistant/sensor/" + id + "/firmware/config";
    String payload;
    serializeJson(doc, payload);
    client.publish(topic.c_str(), payload.c_str(), true);
  }

  {
    DynamicJsonDocument doc(768);
    doc["name"] = "Uptime";
    doc["unique_id"] = id + "_uptime";
    doc["state_topic"] = debug_topic;
    doc["availability_topic"] = avail_topic;
    doc["value_template"] = "{{ value_json.uptime_s }}";
    doc["device_class"] = "duration";
    doc["unit_of_measurement"] = "s";
    doc["entity_category"] = "diagnostic";
    doc["device"] = device;

    String topic = "homeassistant/sensor/" + id + "/uptime/config";
    String payload;
    serializeJson(doc, payload);
    client.publish(topic.c_str(), payload.c_str(), true);
  }

  {
    DynamicJsonDocument doc(768);
    doc["name"] = "Free Heap";
    doc["unique_id"] = id + "_free_heap";
    doc["state_topic"] = debug_topic;
    doc["availability_topic"] = avail_topic;
    doc["value_template"] = "{{ value_json.free_heap }}";
    doc["unit_of_measurement"] = "B";
    doc["state_class"] = "measurement";
    doc["icon"] = "mdi:memory";
    doc["entity_category"] = "diagnostic";
    doc["device"] = device;

    String topic = "homeassistant/sensor/" + id + "/free_heap/config";
    String payload;
    serializeJson(doc, payload);
    client.publish(topic.c_str(), payload.c_str(), true);
  }

  {
    DynamicJsonDocument doc(768);
    doc["name"] = "WiFi Signal";
    doc["unique_id"] = id + "_rssi";
    doc["state_topic"] = debug_topic;
    doc["availability_topic"] = avail_topic;
    doc["value_template"] = "{{ value_json.rssi }}";
    doc["device_class"] = "signal_strength";
    doc["unit_of_measurement"] = "dBm";
    doc["state_class"] = "measurement";
    doc["entity_category"] = "diagnostic";
    doc["device"] = device;

    String topic = "homeassistant/sensor/" + id + "/rssi/config";
    String payload;
    serializeJson(doc, payload);
    client.publish(topic.c_str(), payload.c_str(), true);
  }

  {
    DynamicJsonDocument doc(768);
    doc["name"] = "IP Address";
    doc["unique_id"] = id + "_ip";
    doc["state_topic"] = debug_topic;
    doc["availability_topic"] = avail_topic;
    doc["value_template"] = "{{ value_json.ip }}";
    doc["icon"] = "mdi:ip-network";
    doc["entity_category"] = "diagnostic";
    doc["device"] = device;

    String topic = "homeassistant/sensor/" + id + "/ip/config";
    String payload;
    serializeJson(doc, payload);
    client.publish(topic.c_str(), payload.c_str(), true);
  }

  {
    DynamicJsonDocument doc(768);
    doc["name"] = "Last Reset Reason";
    doc["unique_id"] = id + "_reset_reason";
    doc["state_topic"] = debug_topic;
    doc["availability_topic"] = avail_topic;
    doc["value_template"] = "{{ value_json.reset_reason }}";
    doc["icon"] = "mdi:restart-alert";
    doc["entity_category"] = "diagnostic";
    doc["device"] = device;

    String topic = "homeassistant/sensor/" + id + "/reset_reason/config";
    String payload;
    serializeJson(doc, payload);
    client.publish(topic.c_str(), payload.c_str(), true);
  }

  {
    DynamicJsonDocument doc(768);
    doc["name"] = "Letzter Alarm (Rot)";
    doc["unique_id"] = id + "_last_alert";
    doc["state_topic"] = debug_topic;
    doc["availability_topic"] = avail_topic;
    doc["value_template"] = "{{ value_json.last_alert_at if value_json.last_alert_at else None }}";
    doc["device_class"] = "timestamp";
    doc["icon"] = "mdi:alert-octagon";
    doc["device"] = device;

    String topic = "homeassistant/sensor/" + id + "/last_alert/config";
    String payload;
    serializeJson(doc, payload);
    client.publish(topic.c_str(), payload.c_str(), true);
  }

  discovery_sent = true;
  log_i("[MQTT] Discovery published for %s", id.c_str());
}

// Collapses the various watchdog-reset variants into one "watchdog" reason -
// HA just needs a short human-readable diagnostic string, not the exact
// WDT subsystem that fired.
static const char* resetReasonToString(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    case ESP_RST_UNKNOWN:
    default:                return "unknown";
  }
}

// Formats an epoch as UTC ISO-8601 ("2026-08-15T12:34:56Z") for HA's
// timestamp device_class, which expects an ISO-8601 string. 0 (never
// triggered / not yet NTP-synced) yields "" so the discovery entity's
// value_template can turn that into Jinja's None (shown as "unknown" in
// HA) instead of rendering the 1970-01-01 epoch. Always UTC/"Z" regardless
// of the device's configured POSIX TZ string - time_t itself is always UTC
// seconds, so no tz_string handling is needed here.
static String isoTimestamp(time_t epoch) {
  if (epoch == 0) return "";
  struct tm tm_utc;
  gmtime_r(&epoch, &tm_utc);
  char buf[21];  // "YYYY-MM-DDTHH:MM:SSZ" + NUL
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
  return String(buf);
}

void MqttService::publishDebug() {
  DynamicJsonDocument doc(320);
  doc["firmware"] = FIRMWARE_VERSION;
  doc["uptime_s"] = millis() / 1000;
  doc["free_heap"] = ESP.getFreeHeap();
  doc["rssi"] = WiFi.RSSI();
  doc["ip"] = WiFi.localIP().toString();
  doc["reset_reason"] = resetReasonToString(esp_reset_reason());
  doc["last_alert_at"] = isoTimestamp(web_service.getLastAlertEpoch());

  String payload;
  serializeJson(doc, payload);
  client.publish(topicDebug().c_str(), payload.c_str());
}

void MqttService::publishState() {
  double db = web_service.getCurrentDb();
  Config cfg = web_service.getConfigSnapshot();
  NoiseLevel level = led_controller.getLevelForDb(db, cfg);

  const char* level_str = (level == NORMAL) ? "normal" : (level == WARNING) ? "warning" : "alert";

  DynamicJsonDocument doc(128);
  doc["db"] = (int)round(db);  // whole dB is all anyone reads off the HA dashboard
  doc["level"] = level_str;

  String payload;
  serializeJson(doc, payload);
  client.publish(topicState().c_str(), payload.c_str());
}
