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
#include <esp_ota_ops.h>
#include "config.h"
#include "led.h"
#include "microphone.h"
#include "net_manager.h"
#include "mqtt.h"
#include "web.h"
#include "audio_stream.h"

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
  log_i("Firmware version: %s", FIRMWARE_VERSION);
  {
    // Diagnostic only - see the "still shows old FIRMWARE_VERSION after a
    // successful-looking OTA" investigation: which partition is actually
    // executing this boot, so it can be compared against the "target"
    // partition Update.begin() logs during the next OTA attempt.
    const esp_partition_t* running = esp_ota_get_running_partition();
    log_i("Running from partition: %s@0x%06x", running ? running->label : "?", running ? running->address : 0);
  }

  //
  // OTA ROLLBACK SELF-CONFIRMATION  (root cause of "OTA succeeds, reboot
  // shows the OLD firmware again")
  //
  // The bootloader flashed onto this board by the Arduino IDE (esp32 core
  // 3.x) is built with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y. With that
  // option, esp_ota_set_boot_partition() - which Update.end(true) calls
  // internally - does NOT simply say "boot app1 from now on". It writes an
  // otadata entry pointing at app1 whose ota_state is ESP_OTA_IMG_NEW, i.e.
  // "boot app1 once, on probation". That is why the diagnostic readback of
  // esp_ota_get_boot_partition() right after Update.end() correctly reports
  // app1: otadata really does point there. But the state machine then runs:
  //
  //   reset #1: bootloader flips NEW -> PENDING_VERIFY and boots app1
  //   app1 must call esp_ota_mark_app_valid_cancel_rollback() to commit
  //   reset #2: if it never did, the bootloader marks app1 ABORTED and
  //             rolls back to app0 - the old firmware, exactly as observed
  //
  // Arduino core 3.x makes that call for you inside initArduino(), but the
  // block is guarded by #ifdef CONFIG_APP_ROLLBACK_ENABLE, and the ESP-IDF
  // that PlatformIO's esp32 core 2.0.5 ships does NOT define that symbol.
  // So every .bin in firmware/ (all PlatformIO builds) boots once from app1
  // on probation, never commits, and gets rolled back to app0 on the next
  // reset - which is why USB flashing always works (esptool rewrites
  // otadata at 0xe000 outright and never involves the rollback state
  // machine) while /update never once stuck.
  //
  // Doing it explicitly here, with no #ifdef, makes the firmware commit
  // itself regardless of which toolchain built it or how the bootloader on
  // the device is configured. On a device whose bootloader has rollback
  // disabled, the state is simply ESP_OTA_IMG_UNDEFINED and this is a
  // no-op.
  {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    if (running && esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
      log_i("[OTA] Running partition state: %d", (int)ota_state);
      if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
          log_i("[OTA] Image was on rollback probation - marked valid, "
                "this firmware is now permanent");
        } else {
          log_e("[OTA] esp_ota_mark_app_valid_cancel_rollback() failed: %d", (int)err);
        }
      }
    } else {
      log_i("[OTA] Running partition has no otadata state (rollback not in use)");
    }
  }
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

  // Initialize the live-listen audio stream service (Babyphone Phase 2) -
  // own dedicated task/port, independent of the WebService/WebTask above.
  audio_stream.init();

  // Start RTOS tasks
  web_service.startTask();
  audio_stream.startTask();

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

  // The two statistics calls below are throttled rather than run on every
  // iteration: accumulateHourlyStat() calls time() + localtime_r() (not
  // cheap) and both work on elapsed-millisecond deltas, so a coarser tick
  // costs them no accuracy - the Babyphone sustain/clear windows are
  // measured in whole seconds. Unthrottled this ran thousands of times a
  // second for no benefit.
  static unsigned long last_stats_ms = 0;
  unsigned long now = millis();
  if (now - last_stats_ms >= 200) {
    last_stats_ms = now;

    // Track today's hour-of-day time distribution, using the same raw
    // classification MqttService reuses for its "level" sensor - not the
    // decayed/display level, which is a display-smoothing concern.
    web_service.accumulateHourlyStat(led_controller.getLevelForDb(level_dB, config));

    // Sustained-threshold Babyphone alarm detector (no-op unless
    // display_mode == 2) - same tick, same raw dB input as the call above.
    web_service.updateBabyphoneState(level_dB, config);
  }

  // A real delay, not just yield(): the microphone delivers one block every
  // ~125ms, so polling at 200Hz is already ~25x oversampled. Spinning this
  // task as fast as the scheduler allows only burned CPU (and power) on
  // re-reading the same values.
  delay(5);
}