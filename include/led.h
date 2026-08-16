#ifndef LED_H
#define LED_H

#include <Adafruit_NeoPixel.h>
#include "config.h"
#include "web.h"

//
// LEDController Class - Manages NeoPixel strip and display modes
//
class LEDController {
public:
  LEDController();
  void init();              // Initialize NeoPixel strip
  void handleLevel(double dB_current, const Config& config);  // Update LED display

  // Classify a dB reading into a NoiseLevel using the configured
  // switchover thresholds. Public so MqttService can reuse the exact same
  // classification for its "level" sensor instead of duplicating it.
  NoiseLevel getLevelForDb(double dB_current, const Config& config);

private:
  // Display mode implementations
  uint32_t getColorForLevel(double dB_current, const Config& config);
  uint32_t getColorForNoiseLevel(NoiseLevel level, const Config& config);
  void displayTrafficLight(double dB_current, const Config& config, uint32_t now);
  void displayVUMeter(double dB_current, const Config& config, uint32_t now);

  // Babyphone mode (display_mode == 2): a static, dim, warm night-light
  // color. Deliberately takes NO dB reading and knows nothing about the
  // alarm state - the sustained-threshold alarm (WebService::
  // updateBabyphoneState) is reported ONLY via the MQTT binary_sensor, per
  // the plan. Do not wire dB/alarm state into this method - the LED must
  // stay visually unchanged no matter what the alarm is doing.
  void displayNightLight(const Config& config);

  // Solid color mode (display_mode == 3): one user-picked color, static or
  // animated (blink / breathe / chase). Unlike displayNightLight() this DOES
  // take `now`, since three of its four effects are time-driven animations.
  void displaySolidColor(const Config& config, uint32_t now);

  // Member variables
  Adafruit_NeoPixel *strip;

  // Every display mode dedups against its own "what did I last render"
  // state so it only touches the strip when something actually changed. That
  // is only sound as long as the strip really still shows what that state
  // says - which stops being true the moment another mode has drawn over it.
  // handleLevel() therefore invalidates all three caches whenever
  // display_mode changes; without that, switching e.g. Babyphone -> VU ->
  // Babyphone left the VU pattern on the strip indefinitely, because
  // displayNightLight() saw unchanged color/brightness and returned early.
  int last_display_mode;

  // LED state tracking (traffic light)
  uint32_t last_update_ms;
  uint32_t last_color;
  NoiseLevel last_noise_level;
  bool traffic_light_initialized;

  // Dedup + refresh-throttle state for displayVUMeter(). Without the
  // throttle this mode called strip->show() on every single loop()
  // iteration - a blocking RMT transfer plus a 300us reset gap, several
  // thousand times a second, for a 7-pixel strip.
  uint32_t last_vu_update_ms;
  int last_vu_count;
  uint32_t last_vu_color;
  bool vu_initialized;

  // Dedup state for displayNightLight(), separate from last_color/
  // last_noise_level above since those are traffic-light-specific and get
  // written to on every tick that mode runs.
  uint32_t last_night_color;
  uint8_t last_night_brightness;
  bool night_light_initialized;

  // Dedup + refresh-throttle state for displaySolidColor(), same shape as
  // the VU meter's. Only the static "fest" effect can dedup on unchanged
  // inputs (last_solid_color/last_solid_effect); the three animated effects
  // are time-driven and always redraw once the throttle gap has passed.
  uint32_t last_solid_update_ms;
  uint32_t last_solid_color;
  uint8_t last_solid_effect;
  bool solid_initialized;
};

// Global instance
extern LEDController led_controller;

#endif // LED_H
