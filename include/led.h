#ifndef LED_H
#define LED_H

#include <Adafruit_NeoPixel.h>
#include <freertos/FreeRTOS.h>
#include "config.h"
#include "web.h"

//
// LEDController Class - Manages NeoPixel strip and display modes
//
class LEDController {
public:
  LEDController();
  void init();              // Initialize NeoPixel strip
  void startTask();         // Create and start the LED task
  void handleLevel(double dB_current, const Config& config);  // Update LED display

private:
  static void ledTaskWrapper(void *param);  // Static task wrapper
  void ledTaskHandler();    // Instance task handler

  // Display mode implementations
  NoiseLevel getLevelForDb(double dB_current, const Config& config);
  uint32_t getColorForLevel(double dB_current, const Config& config);
  uint32_t getColorForNoiseLevel(NoiseLevel level, const Config& config);
  void displayTrafficLight(double dB_current, const Config& config, uint32_t now);
  void displayVUMeter(double dB_current, const Config& config);
  void displayWithConfig(double dB_current, const Config& config);

  // Member variables
  Adafruit_NeoPixel *strip;
  TaskHandle_t task_handle;

  // LED state tracking
  uint32_t last_update_ms;
  uint32_t last_color;
  NoiseLevel last_noise_level;
};

// Global instance
extern LEDController led_controller;

#endif // LED_H
