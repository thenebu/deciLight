#ifndef CONFIG_H
#define CONFIG_H

#include <cmath>
#include <Arduino.h>

// On native-USB-CDC boards (no USB-UART bridge chip, e.g. the ESP32-S3-Zero),
// the stock log_e/log_i/log_w/log_d macros route through ets_printf to UART0
// (physically unconnected here), NOT to the Serial/USB-CDC console - so they
// never show up in a USB serial monitor regardless of Core Debug Level.
// Redefine them to go straight to Serial, which does reach the USB-CDC host.
#undef log_e
#undef log_w
#undef log_i
#undef log_d
#define DBG_LOG(L, fmt, ...) Serial.printf("[" L "][%s:%d] " fmt "\r\n", __FILE__, __LINE__, ##__VA_ARGS__)
#define log_e(fmt, ...) DBG_LOG("E", fmt, ##__VA_ARGS__)
#define log_w(fmt, ...) DBG_LOG("W", fmt, ##__VA_ARGS__)
#define log_i(fmt, ...) DBG_LOG("I", fmt, ##__VA_ARGS__)
#define log_d(fmt, ...) DBG_LOG("D", fmt, ##__VA_ARGS__)

//
// NOISE LEVEL ENUM
//
enum NoiseLevel {
  NORMAL = 0,    // Green - below green switchover
  WARNING = 1,   // Yellow - between green and yellow switchover
  ALERT = 2      // Red - above yellow switchover
};

//
// PIN CONFIGURATION - ESP32 noiselight
//
#define DATA_PIN 1               // WS2812 LED strip (7 LEDs)
#define I2S_LR 3                // I2S L/R Select - GREEN wire (set HIGH=RIGHT channel)
#define I2S_WS 4                // I2S Word Select (L/R Clock) - BLUE wire
#define I2S_SCK 5                // I2S Serial Clock (BCLK) - WHITE wire
#define I2S_SD 2                // I2S Serial Data - YELLOW wire
#define I2S_PORT I2S_NUM_0      // Use I2S peripheral 0

//
// LED CONFIGURATION
//
#define NUM_LEDS 7               // 7 WS2812 pixels
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
#define DMA_BANK_SIZE 256       // ~24 KB total DMA RAM instead of ~96 KB
#define DMA_BANKS 12

// Microphone parameters for I2S MEMS (INMP441-compatible)
#define MIC_EQUALIZER INMP441
#define MIC_SENSITIVITY -26     // dBFS (from microphone datasheet)
#define MIC_REF_DB 67.0         // Reference dB value (calibrated against a phone SPL meter: was 80.0, read 13dB high)
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

// Username paired with OTA_PASSWORD for the web /update endpoint's HTTP
// Basic Auth (in addition to being paired with OTA_PASSWORD for
// ArduinoOTA, which only checks the password - ArduinoOTA itself doesn't
// need a username, this define is purely for the web endpoint).
#define OTA_USERNAME "admin"

// Manually bumped on each release - not tied to git/build automatically.
// Shown in the WebUI footer and published over MQTT, so an OTA update can
// be confirmed to have actually taken.
#define FIRMWARE_VERSION "1.2.0"

#endif // CONFIG_H
