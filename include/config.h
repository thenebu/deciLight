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
#define BUTTON_PIN 6             // Mode button, wired to GND, uses internal pull-up (active LOW)

//
// BUTTON TIMING
//
#define BUTTON_DEBOUNCE_MS 30              // Ignore raw level changes faster than this
#define BUTTON_LONG_PRESS_MS 600           // Held longer than this = long press (brightness ramp) instead of short press (mode switch)
#define BUTTON_RAMP_INTERVAL_MS 30         // Time between brightness steps while held
#define BUTTON_RAMP_STEP 4                 // Brightness change (0-255) per ramp step

//
// LED CONFIGURATION
//
#define NUM_LEDS 7               // 7 WS2812 pixels
#define LED_BRIGHTNESS 25       // 0-255
#define DISPLAY_MODE 1          // 0=TRAFFIC_LIGHT, 1=VU_METER, 2=BABYPHONE, 3=SOLID_COLOR

//
// NOISE THRESHOLDS
// Grounded in WHO "Guidelines for Community Noise" (1999), Table 4.1: 50 dB LAeq is
// the moderate-annoyance guideline value for outdoor/living areas, 65 dB LAeq is the
// point past which normal conversation requires a raised voice and annoyance turns
// serious. See README "Scientific Basis" section for the full source list.
//
#define DB_FLOOR 37.0           // Noise floor baseline (~37dB)
#define DB_NORMAL_SWITCHOVER 50.0   // Below: NORMAL, Above: WARNING
#define DB_WARNING_SWITCHOVER 65.0  // Below: WARNING, Above: ALERT

//
// SOLID COLOR MODE (display_mode == 3): one user-picked color, shown either
// static or animated (blink / breathe / chase around the strip).
//
#define SOLID_COLOR 0x3B82F6     // default: a calm blue
#define SOLID_EFFECT 0           // 0=fest, 1=blinken, 2=faden, 3=lauflicht
#define SOLID_SPEED_MS 1500      // effect period: blink cycle / breathe cycle / one lap of the strip

//
// BABYPHONE MODE
//
#define BABYPHONE_TRIGGER_DB 65.0        // dB threshold that must be exceeded sustained
#define BABYPHONE_SUSTAIN_MS 5000        // how long above threshold before the alarm fires
#define BABYPHONE_CLEAR_MS 3000          // how long below threshold before the alarm clears
#define BABYPHONE_NIGHT_COLOR 0xFF3C00   // very warm amber/candlelight (almost no blue)
#define BABYPHONE_NIGHT_BRIGHTNESS 15    // 0-255, dimmed base brightness

//
// MICROPHONE CONFIGURATION
//
#define SAMPLE_RATE 48000       // Hz
#define SAMPLE_BITS 32          // bits
#define SAMPLE_T int32_t
// MEASUREMENT WINDOW: 6000 samples = 125ms. One dB value is produced per
// window. 125ms is not arbitrary - it is the "FAST" time weighting defined by
// IEC 61672, which is what every sound level meter (including the phone app
// MIC_REF_DB was calibrated against) uses. Changing it would invalidate that
// calibration and, because smoothLevel()'s alpha is applied once per window,
// would silently halve or double the display's effective time constant.
#define SAMPLES_SHORT (SAMPLE_RATE / 8)

// I/O BLOCK: how much is read from I2S - and therefore buffered in RAM - at
// a time. Deliberately HALF the measurement window: the equalizer/A-weighting
// filters carry their state across calls (see SOS_IIR_Filter_Cpp::state_ in
// sos-iir-filter.h), so feeding them two 62.5ms halves back-to-back produces
// bit-for-bit the same output as one 125ms block. i2sReaderTask() accumulates
// the sum-of-squares over both halves and only then emits a measurement, so
// the window above stays exactly 125ms while the sample buffer costs half the
// RAM (12KB instead of 24KB, plus 2KB on the live-listen chunk).
//
// Must divide SAMPLES_SHORT evenly, and SAMPLES_CHUNK must stay divisible by
// the live-listen decimation factor of 2 (6000 -> 3000 -> 1500, both exact).
// A static_assert in microphone.cpp enforces both that and the matching
// SAMPLE_RATE / STREAM_DECIM == AUDIO_STREAM_SAMPLE_RATE relation.
#define SAMPLES_CHUNK (SAMPLES_SHORT / 2)
#define SAMPLES_CHUNKS_PER_WINDOW (SAMPLES_SHORT / SAMPLES_CHUNK)
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
// AUDIO STREAMING (Babyphone Phase 2: "Live hoeren")
//
// Dedicated raw-TCP port for the live-listen PCM stream - deliberately NOT
// sharing the WebServer's port 80 / WebTask (see include/audio_stream.h for
// the full rationale: no WebSocket/AsyncTCP dependency in this project).
#define AUDIO_STREAM_PORT 8081
// Downsampled from the mic's 48kHz by STREAM_DECIM (see microphone.cpp).
// 24kHz, i.e. decimate-by-2: at 16kHz the 8kHz Nyquist was the binding limit
// on how open speech could sound, no matter how good the anti-alias filter
// got. 12kHz of bandwidth covers sibilance properly. Costs 50% more WiFi
// (48KB/s) and 16KB more ring buffer than 16kHz did - both cheap here.
#define AUDIO_STREAM_SAMPLE_RATE 24000
// 1s (was 2s): the ring only has to bridge network hiccups between the mic
// task's 125ms blocks and the stream task's 20ms drain loop, and every
// buffered second is a second of added listening latency. Halving it halves
// the allocation, which matters most on the internal-SRAM fallback path in
// AudioStreamService::acquireRing().
#define AUDIO_STREAM_RING_SECONDS 1
#define AUDIO_STREAM_RING_BYTES (AUDIO_STREAM_SAMPLE_RATE * 2 * AUDIO_STREAM_RING_SECONDS)  // 16-bit mono

//
// CREDENTIALS
//
// Two separate passwords, deliberately not one shared secret: they guard
// very different things and are handed out to different people. Flashing new
// firmware can brick or replace the device outright, so that credential
// should stay with whoever maintains it. Listening in on the room is an
// everyday action for everyone in the household - and it is the one you type
// on a phone, half asleep, when the baby cries. Sharing the live password
// must not also hand out the ability to reflash the device.
//
// CHANGE BOTH before deploying to your home network - anyone on the same
// network can otherwise attempt an OTA flash or listen in.
//

// Guards ArduinoOTA (network flashing from the IDE/PlatformIO) and the web
// UI's POST /update endpoint. ArduinoOTA only enables once WiFi STA is
// connected (see NetworkService::init()), so the AP-fallback path never
// exposes it.
//
// If you change it, also update the matching --auth= value in
// platformio.ini's esp32-s3-devkitc1-n4r2-ota env - PlatformIO can't read
// this #define, so the two are independent literals with no build-time
// check keeping them in sync.
#define OTA_PASSWORD "changeme-ota"

// Guards the live-listen audio stream on AUDIO_STREAM_PORT (see
// AudioStreamService::readHeadersAndCheckAuth()).
#define LIVE_PASSWORD "changeme-live"

// Usernames paired with the two passwords above for HTTP Basic Auth.
// ArduinoOTA itself checks only a password and ignores these; they exist for
// the two HTTP endpoints (/update on port 80, /listen on AUDIO_STREAM_PORT).
//
// NOTE: the embedded web UI builds its Authorization headers in JavaScript
// (html_ui in web.cpp), which cannot see these #defines - the usernames are
// repeated as literals there. If you change either one, change it in both
// places, or the browser will be rejected with a 401.
#define OTA_USERNAME "admin"
#define LIVE_USERNAME "admin"

// Manually bumped on each release - not tied to git/build automatically.
// Shown in the WebUI footer and published over MQTT, so an OTA update can
// be confirmed to have actually taken.
#define FIRMWARE_VERSION "1.9.0"

#endif // CONFIG_H
