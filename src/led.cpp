#include "led.h"
#include "config.h"
#include <cmath>

// Global instance
LEDController led_controller;

// Static pointer for task wrapper
static LEDController* g_led_controller = nullptr;

//============================================
// LEDController Constructor
//============================================
LEDController::LEDController()
  : strip(nullptr),
    task_handle(nullptr),
    last_update_ms(0),
    last_color(0),
    last_noise_level(NORMAL)
{
}

//============================================
// LEDController::init() - Initialize NeoPixel strip
//============================================
void LEDController::init() {
  log_i("LED: Init %d pixels on GPIO %d", NUM_LEDS, DATA_PIN);
  
  strip = new Adafruit_NeoPixel(NUM_LEDS, DATA_PIN, NEO_GRB + NEO_KHZ800);
  strip->begin();
  strip->show();
  strip->setBrightness(LED_BRIGHTNESS);
  
  // Clear all pixels
  for (int i = 0; i < NUM_LEDS; i++) {
    strip->setPixelColor(i, 0);
  }
  strip->show();
  log_i("LED: Ready");
  
  // Store global pointer for task wrapper
  g_led_controller = this;
}

//============================================
// LEDController::startTask() - Create FreeRTOS LED task
//============================================
void LEDController::startTask() {
  log_i("[LED] Creating LEDTask...");
  
  BaseType_t ret = xTaskCreatePinnedToCore(
    LEDController::ledTaskWrapper,  // Static wrapper function
    "LEDTask",                      // Task name
    4096,                           // Stack size
    this,                           // Parameter (pass this pointer)
    2,                              // Priority
    &task_handle,                   // Task handle
    1                               // Core 1
  );
  
  if (ret != pdPASS) {
    log_e("[LED] Failed to create LEDTask!");
  } else {
    log_i("[LED] LEDTask created successfully");
  }
}

//============================================
// LEDController::ledTaskWrapper() - Static task wrapper
//============================================
void LEDController::ledTaskWrapper(void *param) {
  LEDController* pThis = static_cast<LEDController*>(param);
  pThis->ledTaskHandler();
}

//============================================
// LEDController::ledTaskHandler() - Instance task handler
//============================================
void LEDController::ledTaskHandler() {
  log_i("[TASK] LEDTask started on core %d", xPortGetCoreID());
  
  while (1) {
    // LED updates are now driven from main loop
    // This task is mainly a placeholder for future async updates
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

//============================================
// LEDController::handleLevel() - Update LED display with dB level
//============================================
void LEDController::handleLevel(double dB_current, const Config& config) {
  if (!strip) return;
  
  strip->setBrightness(config.led_brightness);
  
  uint32_t now = millis();
  
  if (config.display_mode == 0) {
    displayTrafficLight(dB_current, config, now);
  } else {
    displayVUMeter(dB_current, config);
  }
}

//============================================
// LEDController::getLevelForDb() - Classify a dB reading into a NoiseLevel
//============================================
NoiseLevel LEDController::getLevelForDb(double dB_current, const Config& config) {
  if (dB_current < config.db_normal_switchover) {
    return NORMAL;
  } else if (dB_current < config.db_warning_switchover) {
    return WARNING;
  } else {
    return ALERT;
  }
}

//============================================
// LEDController::getColorForNoiseLevel() - Configured color for a NoiseLevel
//============================================
uint32_t LEDController::getColorForNoiseLevel(NoiseLevel level, const Config& config) {
  switch (level) {
    case NORMAL:  return config.color_normal;
    case WARNING: return config.color_warning;
    default:      return config.color_alert;
  }
}

//============================================
// LEDController::getColorForLevel() - Determine color for dB level
//============================================
uint32_t LEDController::getColorForLevel(double dB_current, const Config& config) {
  return getColorForNoiseLevel(getLevelForDb(dB_current, config), config);
}

//============================================
// LEDController::displayTrafficLight() - All LEDs same color
//============================================
void LEDController::displayTrafficLight(double dB_current, const Config& config, uint32_t now) {
  NoiseLevel computed_level = getLevelForDb(dB_current, config);
  NoiseLevel display_level = computed_level;

  // Handle decay: only allow escalation during the decay period, hold the
  // last displayed level if the new reading would be quieter.
  uint32_t time_since_color_change = now - last_update_ms;
  if (config.decay_ms > 0 && time_since_color_change < config.decay_ms) {
    if (computed_level < last_noise_level) {
      display_level = last_noise_level;
    }
  }

  uint32_t new_color = getColorForNoiseLevel(display_level, config);

  // Check response throttling (min update interval for color changes)
  if (new_color != last_color && config.response_ms > 0 && (now - last_update_ms) < config.response_ms) {
    return;  // Not enough time passed, skip update
  }

  if (new_color != last_color) {
    last_color = new_color;
    last_noise_level = display_level;
    last_update_ms = now;

    for (int i = 0; i < NUM_LEDS; i++) {
      strip->setPixelColor(i, new_color);
    }
    strip->show();
  }
}

//============================================
// LEDController::displayVUMeter() - Gradient with LED count
//============================================
void LEDController::displayVUMeter(double dB_current, const Config& config) {
  double dB_min = config.db_floor;
  double dB_max = 80.0;
  
  // Normalize dB to 0-1 range
  double dB_normalized = (dB_current - dB_min) / (dB_max - dB_min);
  dB_normalized = (dB_normalized < 0) ? 0 : (dB_normalized > 1) ? 1 : dB_normalized;
  
  int led_count = (int)(dB_normalized * NUM_LEDS);
  
  // Clear all LEDs
  for (int i = 0; i < NUM_LEDS; i++) {
    strip->setPixelColor(i, 0);
  }
  
  // Get base color for current level
  uint32_t base_color = getColorForLevel(dB_current, config);
  
  // Extract RGB components (format: 0xRRGGBB)
  uint8_t base_r = (base_color >> 16) & 0xFF;
  uint8_t base_g = (base_color >> 8) & 0xFF;
  uint8_t base_b = base_color & 0xFF;
  
  // Light up LEDs with brightness ramp (50% to 100%)
  for (int i = 0; i < led_count; i++) {
    float brightness_factor = 0.5 + 0.5 * ((float)i / NUM_LEDS);
    
    uint8_t r = (uint8_t)(base_r * brightness_factor);
    uint8_t g = (uint8_t)(base_g * brightness_factor);
    uint8_t b = (uint8_t)(base_b * brightness_factor);
    
    strip->setPixelColor(i, strip->Color(r, g, b));
  }
  
  strip->show();
}
