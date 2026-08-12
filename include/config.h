#ifndef CONFIG_H
#define CONFIG_H

#include <cmath>

//
// NOISE LEVEL ENUM
//
enum NoiseLevel {
  NORMAL = 0,    // Green - below green switchover
  WARNING = 1,   // Yellow - between green and yellow switchover
  ALERT = 2      // Red - above yellow switchover
};

//
// PIN CONFIGURATION - ESP32 Noise Light
//
#define DATA_PIN 2              // WS2812 LED strip (12 LEDs)
#define I2S_LR 4                // I2S L/R Select - GREEN wire (set HIGH=RIGHT channel)
#define I2S_WS 5                // I2S Word Select (L/R Clock) - BLUE wire
#define I2S_SCK 6               // I2S Serial Clock (BCLK) - WHITE wire
#define I2S_SD 7                // I2S Serial Data - YELLOW wire
#define I2S_PORT I2S_NUM_0      // Use I2S peripheral 0

//
// LED CONFIGURATION
//
#define NUM_LEDS 13             // 12 WS2812 pixels
#define LED_BRIGHTNESS 25       // 0-255
#define DISPLAY_MODE 1          // 0=TRAFFIC_LIGHT, 1=VU_METER

//
// NOISE THRESHOLDS
//
#define DB_FLOOR 37.0           // Noise floor baseline (~37dB)
#define DB_NORMAL_SWITCHOVER 50.0   // Below: NORMAL, Above: WARNING
#define DB_WARNING_SWITCHOVER 65.0  // Below: WARNING, Above: ALERT

//
// MICROPHONE CONFIGURATION
//
#define SAMPLE_RATE 48000       // Hz
#define SAMPLE_BITS 32          // bits
#define SAMPLE_T int32_t
// Each I2S block is used directly as the measurement window (no separate
// LEQ accumulation across blocks).
#define SAMPLES_SHORT (SAMPLE_RATE / 8)  // ~125ms blocks
#define DMA_BANK_SIZE (SAMPLES_SHORT / 16)
#define DMA_BANKS 32

// Microphone parameters for I2S MEMS (INMP441-compatible)
#define MIC_EQUALIZER INMP441
#define MIC_SENSITIVITY -26     // dBFS (from microphone datasheet)
#define MIC_REF_DB 80.0         // Reference dB value (calibrated for this setup)
#define MIC_OVERLOAD_DB 116.0   // Max input before clipping
#define MIC_NOISE_DB 29         // Noise floor
#define MIC_BITS 24             // Bits from microphone
#define MIC_OFFSET_DB 3.0103    // Sine-wave RMS offset
#define MIC_CONVERT(s) (s >> (SAMPLE_BITS - MIC_BITS))

// Reference amplitude at compile time
constexpr double MIC_REF_AMPL = pow(10, double(MIC_SENSITIVITY) / 20) * ((1 << (MIC_BITS - 1)) - 1);

//
// I2S READER TASK
//
#define I2S_TASK_PRI 4
#define I2S_TASK_STACK 8192

//
// OTA UPDATES
//
// CHANGE THIS before deploying to your home network - anyone on the same
// network can attempt an OTA flash otherwise. ArduinoOTA only enables once
// WiFi STA is connected (see NetworkService::init()), so the AP-fallback
// path never exposes this.
//
// If you change it, also update the matching --auth= value in
// platformio.ini's esp32-s3-devkitc1-n4r2-ota env - PlatformIO can't read
// this #define, so the two are independent literals with no build-time
// check keeping them in sync.
#define OTA_PASSWORD "changeme-ota"

#endif // CONFIG_H
