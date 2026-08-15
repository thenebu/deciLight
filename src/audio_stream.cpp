#include "audio_stream.h"
#include "config.h"
#include <esp_heap_caps.h>
#include <base64.h>

// Global instance
AudioStreamService audio_stream;

//============================================
// AudioStreamService Constructor
//============================================
AudioStreamService::AudioStreamService()
  : server(nullptr),
    task_handle(nullptr),
    ring_buf(nullptr),
    ring_head(0),
    ring_tail(0),
    ring_count(0),
    streaming_active(false),
    ring_mux(portMUX_INITIALIZER_UNLOCKED)
{
}

//============================================
// AudioStreamService::init() - allocate ring buffer, start listening
//============================================
void AudioStreamService::init() {
  log_i("[AUDIO] Initializing audio stream service...");
  log_i("[AUDIO] ESP.getPsramSize()=%u ESP.getFreePsram()=%u", (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());

  // The ring buffer is NOT allocated here any more - see acquireRing(), which
  // is called when a listener actually connects. Holding it for the whole
  // uptime cost AUDIO_STREAM_RING_BYTES permanently even though live-listen
  // is used for minutes at a time at most, and on the internal-SRAM fallback
  // path that came straight out of the same heap the WiFi stack uses.

  // Computed once at boot - compared verbatim against the incoming
  // Authorization header value on every /listen request.
  // LIVE_*, not OTA_* - live-listen has its own credential so that sharing
  // it with the household does not also grant the ability to reflash the
  // device. See the rationale in config.h.
  expected_auth = "Basic " + base64::encode(String(LIVE_USERNAME) + ":" + String(LIVE_PASSWORD));

  server = new WiFiServer(AUDIO_STREAM_PORT);
  server->begin();
  log_i("[AUDIO] Listening on port %d", AUDIO_STREAM_PORT);
}

//============================================
// AudioStreamService::acquireRing() - allocate on listener connect
//============================================
bool AudioStreamService::acquireRing() {
  // Prefer PSRAM (the project targets PSRAM hardware, see plan doc) so the
  // buffer doesn't eat into the scarce internal SRAM (320KB, shared with the
  // WiFi/BT stack). But if PSRAM isn't actually available on this unit at
  // runtime for any reason - wrong/unpopulated chip variant, psramInit()
  // having silently no-op'd, etc. - fall back to internal SRAM rather than
  // refusing to stream at all. Live-listen works either way; only the
  // "spares internal RAM" property is lost.
  uint8_t *buf = (uint8_t *)heap_caps_malloc(AUDIO_STREAM_RING_BYTES, MALLOC_CAP_SPIRAM);
  if (buf != nullptr) {
    log_i("[AUDIO] Ring buffer: %d bytes in PSRAM (free PSRAM now: %u)",
      AUDIO_STREAM_RING_BYTES, (unsigned)ESP.getFreePsram());
  } else {
    log_w("[AUDIO] No PSRAM available for ring buffer - falling back to internal SRAM");
    buf = (uint8_t *)heap_caps_malloc(AUDIO_STREAM_RING_BYTES, MALLOC_CAP_8BIT);
    if (buf == nullptr) {
      log_e("[AUDIO] ERROR: failed to allocate %d bytes for ring buffer (SRAM fallback also failed)",
        AUDIO_STREAM_RING_BYTES);
      return false;
    }
    log_i("[AUDIO] Ring buffer: %d bytes in internal SRAM (free heap now: %u)",
      AUDIO_STREAM_RING_BYTES, (unsigned)ESP.getFreeHeap());
  }

  portENTER_CRITICAL(&ring_mux);
  ring_buf = buf;
  ring_head = 0;
  ring_tail = 0;
  ring_count = 0;
  streaming_active = true;
  portEXIT_CRITICAL(&ring_mux);
  return true;
}

//============================================
// AudioStreamService::releaseRing() - free on listener disconnect
//============================================
void AudioStreamService::releaseRing() {
  uint8_t *to_free;

  portENTER_CRITICAL(&ring_mux);
  streaming_active = false;
  to_free = ring_buf;
  ring_buf = nullptr;   // producers bail out from here on
  ring_head = 0;
  ring_tail = 0;
  ring_count = 0;
  portEXIT_CRITICAL(&ring_mux);

  // Outside the critical section: free() may take a lock of its own, and by
  // now no producer can still be holding this pointer - any that was inside
  // pushSamples() held ring_mux, which we just waited on.
  if (to_free) heap_caps_free(to_free);
}

//============================================
// AudioStreamService::startTask() - Create FreeRTOS task
//============================================
void AudioStreamService::startTask() {
  log_i("[AUDIO] Creating AudioStreamTask...");

  BaseType_t ret = xTaskCreatePinnedToCore(
    AudioStreamService::streamTaskWrapper,  // Static wrapper function
    "AudioStreamTask",                      // Task name
    4096,                                   // Stack size
    this,                                   // Parameter (pass this pointer)
    2,                                       // Priority - lower than WebTask(3)/I2S reader(4), best-effort
    &task_handle,                           // Task handle
    0                                        // Core 0
  );

  if (ret != pdPASS) {
    log_e("[AUDIO] Failed to create AudioStreamTask!");
  } else {
    log_i("[AUDIO] AudioStreamTask created successfully");
  }
}

//============================================
// AudioStreamService::streamTaskWrapper() - Static task wrapper
//============================================
void AudioStreamService::streamTaskWrapper(void *param) {
  AudioStreamService *pThis = static_cast<AudioStreamService *>(param);
  pThis->streamTaskHandler();
}

//============================================
// AudioStreamService::streamTaskHandler() - Instance task handler
//============================================
void AudioStreamService::streamTaskHandler() {
  log_i("[TASK] AudioStreamTask started on core %d", xPortGetCoreID());

  if (server == nullptr) {
    log_e("[AUDIO] AudioStreamTask exiting - not initialized");
    vTaskDelete(NULL);
    return;
  }

  while (true) {
    WiFiClient client = server->available();
    if (!client || !client.connected()) {
      vTaskDelay(50 / portTICK_PERIOD_MS);
      continue;
    }
    serveClient(client);
  }
}

//============================================
// AudioStreamService::pushSamples() - producer side (mic task)
//============================================
void AudioStreamService::pushSamples(const int16_t *samples, size_t count) {
  if (count == 0) return;

  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(samples);
  size_t byte_count = count * sizeof(int16_t);

  // A whole block larger than the ring can only end up as its last
  // AUDIO_STREAM_RING_BYTES anyway - skip straight to those instead of
  // copying bytes that are guaranteed to be overwritten.
  if (byte_count > AUDIO_STREAM_RING_BYTES) {
    bytes += byte_count - AUDIO_STREAM_RING_BYTES;
    byte_count = AUDIO_STREAM_RING_BYTES;
  }

  portENTER_CRITICAL(&ring_mux);
  uint8_t *buf = ring_buf;  // may be nullptr: no listener connected
  if (buf != nullptr) {
    // Two memcpy's (up to the wrap, then from the start) rather than the
    // byte-at-a-time loop with a modulo per byte this used to be. That loop
    // ran with interrupts disabled for thousands of iterations against
    // (slow) PSRAM on every 125ms block - hundreds of microseconds of IRQ
    // latency, enough to jitter WiFi and the I2S DMA.
    size_t first = AUDIO_STREAM_RING_BYTES - ring_head;
    if (first > byte_count) first = byte_count;
    memcpy(buf + ring_head, bytes, first);
    if (byte_count > first) memcpy(buf, bytes + first, byte_count - first);

    ring_head += byte_count;
    if (ring_head >= AUDIO_STREAM_RING_BYTES) ring_head -= AUDIO_STREAM_RING_BYTES;

    ring_count += byte_count;
    if (ring_count > AUDIO_STREAM_RING_BYTES) {
      // Overflow: drop the oldest bytes instead of blocking - the I2S reader
      // must never wait on a slow/absent network consumer.
      size_t dropped = ring_count - AUDIO_STREAM_RING_BYTES;
      ring_tail += dropped;
      if (ring_tail >= AUDIO_STREAM_RING_BYTES) ring_tail -= AUDIO_STREAM_RING_BYTES;
      ring_count = AUDIO_STREAM_RING_BYTES;
    }
  }
  portEXIT_CRITICAL(&ring_mux);
}

//============================================
// AudioStreamService::isStreamingActive()
//============================================
bool AudioStreamService::isStreamingActive() {
  portENTER_CRITICAL(&ring_mux);
  bool active = streaming_active;
  portEXIT_CRITICAL(&ring_mux);
  return active;
}

//============================================
// AudioStreamService::readAvailable() - consumer side (stream task)
//============================================
size_t AudioStreamService::readAvailable(uint8_t *out, size_t max_len) {
  size_t n = 0;

  portENTER_CRITICAL(&ring_mux);
  uint8_t *buf = ring_buf;
  if (buf != nullptr) {
    n = ring_count < max_len ? ring_count : max_len;
    // Same two-memcpy split as pushSamples(), for the same reason.
    size_t first = AUDIO_STREAM_RING_BYTES - ring_tail;
    if (first > n) first = n;
    memcpy(out, buf + ring_tail, first);
    if (n > first) memcpy(out + first, buf, n - first);

    ring_tail += n;
    if (ring_tail >= AUDIO_STREAM_RING_BYTES) ring_tail -= AUDIO_STREAM_RING_BYTES;
    ring_count -= n;
  }
  portEXIT_CRITICAL(&ring_mux);
  return n;
}

//============================================
// AudioStreamService::readRequestLine() - minimal hand-parsed HTTP
//============================================
// Caps on what an unauthenticated peer can make this task read into RAM
// before it has proven anything. Without them a client could stream headers
// indefinitely: every line grows a String on the heap, and each one buys
// another 3s of the (single-listener) stream task's time.
#define AUDIO_STREAM_MAX_LINE_LEN 512
#define AUDIO_STREAM_MAX_HEADERS 32

// Reads one CRLF-terminated line, giving up once it exceeds
// AUDIO_STREAM_MAX_LINE_LEN. Returns false on timeout/overlong line.
static bool readLimitedLine(WiFiClient &client, String &line) {
  line = "";
  unsigned long deadline = millis() + 3000;

  while (millis() < deadline) {
    if (!client.available()) {
      if (!client.connected()) return false;
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    int c = client.read();
    if (c < 0) continue;
    if (c == '\n') return true;
    if (line.length() >= AUDIO_STREAM_MAX_LINE_LEN) return false;
    line += (char)c;
  }
  return false;
}

bool AudioStreamService::readRequestLine(WiFiClient &client, String &method, String &path) {
  String line;
  if (!readLimitedLine(client, line)) return false;
  line.trim();
  if (line.length() == 0) return false;

  int sp1 = line.indexOf(' ');
  int sp2 = (sp1 >= 0) ? line.indexOf(' ', sp1 + 1) : -1;
  if (sp1 < 0 || sp2 < 0) return false;

  method = line.substring(0, sp1);
  path = line.substring(sp1 + 1, sp2);
  return true;
}

//============================================
// AudioStreamService::readHeadersAndCheckAuth() - reads headers until the
// blank line, checking Authorization along the way.
//============================================
bool AudioStreamService::readHeadersAndCheckAuth(WiFiClient &client) {
  bool auth_ok = false;
  String line;

  for (int i = 0; i < AUDIO_STREAM_MAX_HEADERS; i++) {
    if (!readLimitedLine(client, line)) return false;  // timeout or overlong
    line.trim();
    if (line.length() == 0) return auth_ok;  // blank line terminates the block

    if (line.startsWith("Authorization:")) {
      String value = line.substring(strlen("Authorization:"));
      value.trim();
      if (value == expected_auth) {
        auth_ok = true;
      }
    }
  }

  // Header limit hit without a terminating blank line - treat as malformed.
  log_w("[AUDIO] Too many request headers, dropping connection");
  return false;
}

//============================================
// AudioStreamService::rejectExtraClient() - v1-scope: single listener only
//============================================
void AudioStreamService::rejectExtraClient(WiFiClient &client) {
  client.print("HTTP/1.1 503 Service Unavailable\r\n"
               "Content-Type: text/plain\r\n"
               "Access-Control-Allow-Origin: *\r\n"
               "Connection: close\r\n\r\n"
               "503 stream busy");
  client.stop();
  log_w("[AUDIO] Rejected second concurrent listener");
}

//============================================
// AudioStreamService::serveClient() - handles one connection end-to-end
//============================================
void AudioStreamService::serveClient(WiFiClient &client) {
  log_i("[AUDIO] Client connecting: %s", client.remoteIP().toString().c_str());

  String method, path;
  if (!readRequestLine(client, method, path)) {
    client.print("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n");
    client.stop();
    log_w("[AUDIO] Bad request (empty/malformed request line)");
    return;
  }

  // The WebUI lives on port 80; this stream is on its own port
  // (AUDIO_STREAM_PORT) - that's a different origin as far as the browser
  // is concerned, so a fetch() with a custom Authorization header triggers
  // a CORS preflight OPTIONS request before the browser will even attempt
  // the real GET. Must answer it (with permissive CORS headers) or every
  // fetch() from the WebUI fails before our auth check ever runs.
  if (method == "OPTIONS") {
    client.print("HTTP/1.1 204 No Content\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
                 "Access-Control-Allow-Headers: Authorization\r\n"
                 "Access-Control-Max-Age: 600\r\n"
                 "Connection: close\r\n\r\n");
    client.stop();
    return;
  }

  if (method != "GET" || path != "/listen") {
    client.print("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n");
    client.stop();
    log_w("[AUDIO] Bad request (method=%s path=%s)", method.c_str(), path.c_str());
    return;
  }

  if (!readHeadersAndCheckAuth(client)) {
    client.print("HTTP/1.1 401 Unauthorized\r\n"
                 "WWW-Authenticate: Basic realm=\"noiselight\"\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "Connection: close\r\n\r\n");
    client.stop();
    log_w("[AUDIO] Auth failed for %s", client.remoteIP().toString().c_str());
    return;
  }

  // Allocate the ring buffer only now that an authenticated listener is
  // actually here (and free it again at the end of this function).
  if (!acquireRing()) {
    client.print("HTTP/1.1 503 Service Unavailable\r\n"
                 "Content-Type: text/plain\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "Connection: close\r\n\r\n"
                 "503 out of memory");
    client.stop();
    return;
  }

  client.print("HTTP/1.1 200 OK\r\n"
               "Content-Type: application/octet-stream\r\n"
               "Cache-Control: no-store\r\n"
               "Access-Control-Allow-Origin: *\r\n"
               "Connection: close\r\n\r\n");

  log_i("[AUDIO] Streaming started for %s", client.remoteIP().toString().c_str());

  uint8_t chunk[512];
  while (client.connected()) {
    // Drain and reject any additional connection attempt that arrived
    // while we're busy serving this one (v1-scope: single listener).
    if (server->hasClient()) {
      WiFiClient extra = server->available();
      rejectExtraClient(extra);
    }

    size_t n = readAvailable(chunk, sizeof(chunk));
    if (n == 0) {
      vTaskDelay(20 / portTICK_PERIOD_MS);
      continue;
    }

    size_t written = client.write(chunk, n);
    if (written != n) {
      break;  // write failed - client is gone
    }
  }

  releaseRing();
  client.stop();
  log_i("[AUDIO] Client disconnected, streaming stopped");
}
