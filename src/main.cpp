/*
 * NOISE TRAFFIC LIGHT
 * 
 * ESP32 Noise-based RGB Traffic Light
 * Adapted from deciLight by bbbenji
 * Original: https://github.com/bbbenji/deciLight
 * 
 * This monitors ambient sound level via I2S microphone and displays:
 * - GREEN: Noise is LOW (below threshold)
 * - YELLOW: Noise is MEDIUM (between thresholds)
 * - RED: Noise is HIGH (above threshold)
 * 
 * CLASS-BASED ARCHITECTURE:
 * - config.h: All configuration constants and #defines
 * - Microphone: I2S microphone and audio processing (includes reader task)
 * - LEDController: LED control and display modes (includes LED task)
 * - WebService: WiFi AP and web configuration interface (includes web task)
 * - main.cpp: Setup, instance creation, and task coordination
 */

#include <Arduino.h>
#include "config.h"
#include "led.h"
#include "microphone.h"
#include "network.h"
#include "mqtt.h"
#include "web.h"

//
// SETUP
//
void setup() {
  delay(100);
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  delay(500);  // Extra delay for USB CDC to initialize
  
  Serial.println("\n\n--- BOOT START ---");
  
  log_e("\n=== NOISE TRAFFIC LIGHT STARTING ===");
  log_i("CPU: %d MHz, PSRAM: %d bytes", ESP.getCpuFreqMHz(), ESP.getFreePsram());

  // Initialize microphone (starts I2S reader task)
  microphone.init();

  // Auto-baseline: sample the ambient level for a few seconds right after
  // boot and average it into a suggested noise floor. Deliberately NOT
  // written into config.db_floor automatically - that would silently
  // override a saved user setting on every reboot. Instead it's exposed
  // as `suggested_floor` on /api/status, and the web UI offers it as a
  // one-click suggestion next to the existing "Current" button.
  {
    const uint32_t warmup_ms = 4000;
    const uint32_t sample_interval_ms = 200;
    double sum = 0.0;
    int samples = 0;
    uint32_t warmup_start = millis();
    log_i("[BOOT] Sampling ambient level for baseline suggestion (%dms)...", warmup_ms);
    while (millis() - warmup_start < warmup_ms) {
      sum += microphone.getLevel();
      samples++;
      delay(sample_interval_ms);
    }
    if (samples > 0) {
      double suggested = sum / samples;
      web_service.setSuggestedFloor(suggested);
      log_i("[BOOT] Suggested floor: %.1f dB (%d samples)", suggested, samples);
    }
  }

  // Initialize LED controller
  led_controller.init();

  // Initialize networking (WiFi STA with AP fallback, mDNS) before the web
  // server so the HTTP server comes up on whichever interface is active.
  network_service.init();

  // Initialize web service
  web_service.init();

  // Initialize MQTT (connection itself is deferred and driven from the web
  // task loop - see WebService::webTaskHandler())
  mqtt_service.init();

  // Start RTOS tasks
  web_service.startTask();
  
  log_e("=== SYSTEM READY ===\n");
}

//
// MAIN LOOP - Coordinator task
// Gets audio level from microphone and passes to LED controller
//
void loop() {
  // Get audio level from microphone
  double level_dB = microphone.getLevel();
  Config config = web_service.getConfigSnapshot();

  // Update LED display with current level and config
  led_controller.handleLevel(level_dB, config);

  // Update web interface with current level
  web_service.updateLevel(level_dB);

  // Track today's hour-of-day time distribution, using the same raw
  // classification MqttService reuses for its "level" sensor - not the
  // decayed/display level, which is a display-smoothing concern.
  web_service.accumulateHourlyStat(led_controller.getLevelForDb(level_dB, config));

  yield();  // Allow FreeRTOS scheduler to run
}