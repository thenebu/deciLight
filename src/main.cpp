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
  
  // Initialize LED controller
  led_controller.init();
  
  // Initialize web service
  web_service.init();
  
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
  
  // Update LED display with current level and config
  led_controller.handleLevel(level_dB, web_service.config);
  
  // Update web interface with current level
  web_service.updateLevel(level_dB);
  
  yield();  // Allow FreeRTOS scheduler to run
}