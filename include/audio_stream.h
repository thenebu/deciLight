#ifndef AUDIO_STREAM_H
#define AUDIO_STREAM_H

#include <Arduino.h>
#include <WiFiServer.h>
#include <WiFiClient.h>
#include <freertos/FreeRTOS.h>
#include "config.h"

//
// AudioStreamService - Babyphone Phase 2 "Live hoeren"
//
// Serves a raw 16-bit PCM mono @ AUDIO_STREAM_SAMPLE_RATE stream on its own
// TCP port (AUDIO_STREAM_PORT), completely decoupled from the existing
// WebService/WebTask (see the plan doc for why: no WebSocket/AsyncTCP
// dependency in this project, and a live audio stream must not be able to
// block the config/OTA/MQTT-pumping WebTask, or vice versa).
//
// Structured like WebService/Microphone: a singleton with init()/startTask()
// called from main.cpp's setup().
//
class AudioStreamService {
public:
  AudioStreamService();

  // Allocates the PSRAM ring buffer and starts listening on
  // AUDIO_STREAM_PORT. Must be called after WiFi/network init (mirrors
  // WebService::init()), before startTask().
  void init();

  // Creates AudioStreamTask (core 0, priority 2 - lower than WebTask's 3
  // and the I2S reader's I2S_TASK_PRI=4, since this is best-effort).
  void startTask();

  // Producer-side (called from Microphone::i2sReaderTask(), core-unpinned
  // mic task): appends `count` 16-bit mono samples to the ring buffer.
  // Single-producer/single-consumer, guarded by ring_mux. Never blocks -
  // on overflow, oldest bytes are dropped (tail advances) so the I2S
  // reader never has to wait on a slow/absent network consumer.
  void pushSamples(const int16_t *samples, size_t count);

  // Thread-safe check, read by the mic task once per block to decide
  // whether to bother decimating/pushing at all when nobody is listening.
  bool isStreamingActive();

private:
  static void streamTaskWrapper(void *param);
  void streamTaskHandler();

  // Serves exactly one client end-to-end: parses the request line + auth
  // header, responds 200/401/400, then streams ring-buffer bytes until
  // disconnect. While serving, any additional incoming connection is
  // drained and rejected with a short "503 stream busy" response (v1-scope:
  // single listener only).
  void serveClient(WiFiClient &client);
  void rejectExtraClient(WiFiClient &client);
  bool readRequestLine(WiFiClient &client, String &method, String &path);
  bool readHeadersAndCheckAuth(WiFiClient &client);

  // Consumer-side helper: copies up to max_len available ring-buffer bytes
  // into out[], advancing the tail. Returns bytes actually copied.
  size_t readAvailable(uint8_t *out, size_t max_len);

  // The ring buffer is allocated when a listener connects and released the
  // moment it disconnects, rather than being held for the entire uptime -
  // nobody is listening the overwhelming majority of the time, and on a
  // board where the PSRAM allocation fails this is AUDIO_STREAM_RING_BYTES
  // of internal SRAM that the WiFi stack would rather have.
  //
  // Both are safe against the producer running concurrently on the mic task:
  // releaseRing() clears ring_buf INSIDE the ring_mux critical section and
  // only calls free() after leaving it, while pushSamples()/readAvailable()
  // read ring_buf inside that same critical section. A producer that already
  // passed the isStreamingActive() check therefore either gets the still-valid
  // pointer (and finishes before the free) or sees nullptr and bails out.
  bool acquireRing();
  void releaseRing();

  WiFiServer *server;
  TaskHandle_t task_handle;

  // Ring buffer, PSRAM-backed when available - see AUDIO_STREAM_RING_BYTES
  // in config.h. nullptr whenever no listener is connected.
  uint8_t *ring_buf;
  size_t ring_head;   // next write index
  size_t ring_tail;   // next read index
  size_t ring_count;  // valid bytes currently buffered

  bool streaming_active;
  portMUX_TYPE ring_mux;

  // "Basic <base64(LIVE_USERNAME:LIVE_PASSWORD)>" computed once in init(),
  // compared verbatim against the incoming Authorization header value.
  String expected_auth;
};

// Global instance
extern AudioStreamService audio_stream;

#endif // AUDIO_STREAM_H
