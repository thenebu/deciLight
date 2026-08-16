#include "led.h"
#include "config.h"
#include <cmath>

// Global instance
LEDController led_controller;

//============================================
// LEDController Constructor
//============================================
LEDController::LEDController()
  : strip(nullptr),
    last_display_mode(-1),
    last_update_ms(0),
    last_color(0),
    last_noise_level(NORMAL),
    traffic_light_initialized(false),
    last_vu_update_ms(0),
    last_vu_count(-1),
    last_vu_color(0),
    vu_initialized(false),
    last_night_color(0),
    last_night_brightness(0),
    night_light_initialized(false),
    last_solid_update_ms(0),
    last_solid_color(0),
    last_solid_effect(0),
    solid_initialized(false)
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
}

//============================================
// LEDController::handleLevel() - Update LED display with dB level
//============================================
void LEDController::handleLevel(double dB_current, const Config& config) {
  if (!strip) return;

  uint32_t now = millis();

  // A mode switch leaves the strip showing the previous mode's pixels, which
  // every mode's dedup logic below would otherwise mistake for "already
  // correct" and never redraw. See the comment on last_display_mode in led.h.
  if (config.display_mode != last_display_mode) {
    last_display_mode = config.display_mode;
    traffic_light_initialized = false;
    vu_initialized = false;
    night_light_initialized = false;
    solid_initialized = false;
  }

  if (config.display_mode == 0) {
    strip->setBrightness(config.led_brightness);
    displayTrafficLight(dB_current, config, now);
  } else if (config.display_mode == 1) {
    strip->setBrightness(config.led_brightness);
    displayVUMeter(dB_current, config, now);
  } else if (config.display_mode == 2) {
    // Deliberately NOT setBrightness(config.led_brightness) here:
    // displayNightLight() sets its own, dimmer babyphone_night_brightness,
    // and Adafruit_NeoPixel::setBrightness() rescales the whole pixel buffer
    // in place (lossily, 8-bit truncation) on every change. Setting both
    // values in turn on every loop() iteration ground the buffer down to
    // zero within a fraction of a second.
    displayNightLight(config);
  } else {
    strip->setBrightness(config.led_brightness);
    displaySolidColor(config, now);
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
  bool changed = (!traffic_light_initialized || new_color != last_color);

  // Check response throttling (min update interval for color changes)
  if (changed && config.response_ms > 0 && (now - last_update_ms) < config.response_ms) {
    return;  // Not enough time passed, skip update
  }

  if (changed) {
    last_color = new_color;
    last_noise_level = display_level;
    last_update_ms = now;
    traffic_light_initialized = true;

    for (int i = 0; i < NUM_LEDS; i++) {
      strip->setPixelColor(i, new_color);
    }
    strip->show();
  }
}

//============================================
// LEDController::displayVUMeter() - Gradient with LED count
//============================================
void LEDController::displayVUMeter(double dB_current, const Config& config, uint32_t now) {
  // Same response throttling displayTrafficLight() applies - this mode used
  // to have none at all, so it drove a blocking strip->show() on every
  // loop() iteration. VU_MIN_REFRESH_MS is a hard floor underneath the
  // configurable value: a WS2812 strip cannot usefully be refreshed faster
  // than this, and response_ms == 0 ("no throttling") must not be read as
  // permission to burn a whole core on a 7-pixel strip.
  const uint32_t VU_MIN_REFRESH_MS = 20;
  uint32_t min_gap = (config.response_ms > VU_MIN_REFRESH_MS) ? config.response_ms : VU_MIN_REFRESH_MS;
  if (vu_initialized && (now - last_vu_update_ms) < min_gap) {
    return;
  }

  double dB_min = config.db_floor;
  double dB_max = 80.0;

  // Normalize dB to 0-1 range
  double dB_normalized = (dB_current - dB_min) / (dB_max - dB_min);
  dB_normalized = (dB_normalized < 0) ? 0 : (dB_normalized > 1) ? 1 : dB_normalized;

  int led_count = (int)(dB_normalized * NUM_LEDS);

  // Get base color for current level
  uint32_t base_color = getColorForLevel(dB_current, config);

  // Nothing visible would change - skip the redraw and the show() entirely.
  if (vu_initialized && led_count == last_vu_count && base_color == last_vu_color) {
    return;
  }
  last_vu_update_ms = now;
  last_vu_count = led_count;
  last_vu_color = base_color;
  vu_initialized = true;

  // Clear all LEDs
  for (int i = 0; i < NUM_LEDS; i++) {
    strip->setPixelColor(i, 0);
  }

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

//============================================
// LEDController::displayNightLight() - Babyphone mode (display_mode == 2):
// a static, dim, warm base color. Takes no dB reading and knows nothing
// about the sustained-threshold alarm state - the alarm is reported only
// via the MQTT binary_sensor (see MqttService::publishState()). Do NOT add
// blinking/color changes here on alarm - that's the whole point of this
// mode staying "just a night light" at the device itself.
//============================================
void LEDController::displayNightLight(const Config& config) {
  uint32_t color = config.babyphone_night_color;
  uint8_t brightness = config.babyphone_night_brightness;

  // Dedup writes, same pattern as displayTrafficLight(): only touch the
  // strip when the effective color or brightness actually changed.
  if (night_light_initialized && color == last_night_color && brightness == last_night_brightness) {
    return;
  }

  last_night_color = color;
  last_night_brightness = brightness;
  night_light_initialized = true;

  strip->setBrightness(brightness);
  for (int i = 0; i < NUM_LEDS; i++) {
    strip->setPixelColor(i, color);
  }
  strip->show();
}

//============================================
// LEDController::displaySolidColor() - user-picked color, static or animated
// (display_mode == 3). solid_speed_ms is the shared effect period: one
// blink cycle, one breathe cycle, or one lap of the chase around the strip.
//============================================
void LEDController::displaySolidColor(const Config& config, uint32_t now) {
  uint16_t speed = (config.solid_speed_ms > 0) ? config.solid_speed_ms : 1500;

  // "Fest" (static) is the one effect that can dedup on unchanged inputs -
  // same pattern as displayNightLight(). The three animated effects below
  // are time-driven and must redraw every throttle tick regardless.
  if (config.solid_effect == 0) {
    if (solid_initialized && last_solid_effect == 0 && config.solid_color == last_solid_color) {
      return;
    }
    last_solid_color = config.solid_color;
    last_solid_effect = 0;
    solid_initialized = true;

    for (int i = 0; i < NUM_LEDS; i++) {
      strip->setPixelColor(i, config.solid_color);
    }
    strip->show();
    return;
  }

  // Same refresh floor as displayVUMeter() - a WS2812 strip cannot usefully
  // be redrawn faster than this, and animations have no other throttle.
  const uint32_t SOLID_MIN_REFRESH_MS = 20;
  if (solid_initialized && (now - last_solid_update_ms) < SOLID_MIN_REFRESH_MS) {
    return;
  }
  last_solid_update_ms = now;
  last_solid_effect = config.solid_effect;
  last_solid_color = config.solid_color;
  solid_initialized = true;

  uint8_t base_r = (config.solid_color >> 16) & 0xFF;
  uint8_t base_g = (config.solid_color >> 8) & 0xFF;
  uint8_t base_b = config.solid_color & 0xFF;

  if (config.solid_effect == 1) {
    // Blinken: on for the first half of the period, off for the second.
    bool on = (now % speed) < (speed / 2);
    uint32_t color = on ? config.solid_color : 0;
    for (int i = 0; i < NUM_LEDS; i++) {
      strip->setPixelColor(i, color);
    }
  } else if (config.solid_effect == 2) {
    // Faden: smooth breathing via a sine wave, one full period == speed_ms.
    float phase = (float)(now % speed) / (float)speed;
    float factor = (sinf(phase * 2.0f * (float)M_PI - (float)M_PI / 2.0f) + 1.0f) / 2.0f;
    uint32_t color = strip->Color(
      (uint8_t)(base_r * factor), (uint8_t)(base_g * factor), (uint8_t)(base_b * factor));
    for (int i = 0; i < NUM_LEDS; i++) {
      strip->setPixelColor(i, color);
    }
  } else {
    // Lauflicht: a single lit pixel chases around the strip, one full lap
    // per speed_ms, with a dimmed trailing tail for visual continuity.
    uint32_t step_ms = speed / NUM_LEDS;
    if (step_ms == 0) step_ms = 1;
    int head = (int)((now / step_ms) % NUM_LEDS);

    for (int i = 0; i < NUM_LEDS; i++) {
      strip->setPixelColor(i, 0);
    }
    for (int t = 0; t < 3 && t < NUM_LEDS; t++) {
      int idx = (head - t + NUM_LEDS) % NUM_LEDS;
      float factor = 1.0f - (t * 0.4f);
      strip->setPixelColor(idx, strip->Color(
        (uint8_t)(base_r * factor), (uint8_t)(base_g * factor), (uint8_t)(base_b * factor)));
    }
  }

  strip->show();
}
