#include <Arduino.h>
#include "microphone.h"
#include "config.h"
#include "audio_stream.h"
#include <driver/i2s.h>
#include <cmath>
#include <string.h>

// Global instance
Microphone microphone;

// Static pointer for task wrapper
static Microphone* g_microphone = nullptr;

// Global buffers (needed for I2S DMA)
QueueHandle_t samples_queue = nullptr;

// ONE buffer for the whole block pipeline, not two, and only half a
// measurement window long (SAMPLES_CHUNK - see config.h).
//
// i2s_read() fills it with int32 samples; the live-listen tap and the
// int32 -> float conversion then read those raw values back out through
// rawSampleAt() below, and the conversion writes each float over the int32 it
// just consumed (same width, element-wise, so no element is ever clobbered
// before it is read).
//
// This used to be a separate 24KB `raw_samples` int32 array purely to avoid a
// strict-aliasing violation from reinterpreting samples[] in place. memcpy is
// the well-defined way to do that type pun (access through unsigned char is
// always allowed and the compiler folds it into a plain load/store), so the
// second buffer bought nothing but 24KB of permanently unavailable heap.
float samples[SAMPLES_CHUNK] __attribute__((aligned(4)));

static inline int32_t rawSampleAt(int i) {
  int32_t v;
  memcpy(&v, &samples[i], sizeof(v));
  return v;
}

// Decimate-by-3 (48kHz -> 16kHz) scratch buffer for the live-listen audio
// stream tap (Babyphone Phase 2) - see the decimation block in
// i2sReaderTask(). Only ever touched by that one task.
//
// There used to be a second, int32 "stream_chunk24" buffer here holding the
// pre-gain 24-bit-domain values, so the per-block AGC peak could be measured
// before quantizing to 16-bit. That is now a two-pass scan over the raw block
// instead (peak first, then decimate+scale straight into this buffer), which
// is bit-for-bit identical output for 8KB less permanent RAM - and the extra
// pass only runs while somebody is actually listening.
static int16_t stream_chunk[SAMPLES_CHUNK / 3];

// Per-block auto-gain for the live-listen stream: the mic is calibrated for
// accurate dB *measurement* (MIC_REF_DB/MIC_OVERLOAD_DB give it a lot of
// headroom), so even a loud cry only occupies a small fraction of the
// 24-bit range - a plain bit-depth shift to 16-bit PCM (what this used to
// do) is close to inaudible. Instead, scale each 125ms block so its peak
// sample hits AGC_TARGET_PEAK, capped at AGC_MAX_GAIN so near-silence
// doesn't get amplified into audible hiss.
static const float AGC_TARGET_PEAK = 24000.0f;  // ~-2.4dBFS, headroom against in-block transients
static const float AGC_MAX_GAIN = 4000.0f;       // ~72dB ceiling - keeps silence quiet

//
// SOS IIR FILTER COEFFICIENTS
// (SOS_IIR_Filter struct is defined in sos-iir-filter.h, included via microphone.h)
//

// INMP441 Equalizer Filter
const SOS_IIR_Filter INMP441_filter = {
  .gain = 1.00197834654696,
  .sos = {{-1.986920458344451, +0.986963226946616, +1.995178510504166, -0.995184322194091}}
};

// A-weighting Filter
const SOS_IIR_Filter A_weighting_filter = {
  .gain = 0.169994948147430,
  .sos = {
    {-2.00026996133106, +1.00027056142719, -1.060868438509278, -0.163987445885926},
    {+4.35912384203144, +3.09120265783884, +1.208419926363593, -0.273166998428332},
    {+0.50612794482493, +0.04765969541352, +1.199801396247742, -0.322883302271233},
    {-0.70930303489759, -0.29071868393580, +1.982242159753048, -0.982298594928989}
  }
};

//============================================
// Microphone Constructor
//============================================
Microphone::Microphone()
  : reader_task_handle(nullptr),
    current_level(30.0),
    smoothed_level(30.0),
    equalizer_(&INMP441_filter, 1),
    aweight_(&A_weighting_filter, 4)
{
}

//============================================
// Microphone::init() - Initialize queue and I2S task
//============================================
void Microphone::init() {
  log_i("Microphone: Initializing...");
  
  // Create a length-1 "mailbox" queue: getLevel() only ever wants the most
  // recent block, so the reader task uses xQueueOverwrite() to keep the
  // latest sample without ever blocking (see i2sReaderTask()).
  samples_queue = xQueueCreate(1, sizeof(AudioSample));

  if (samples_queue == nullptr) {
    log_e("ERROR: Queue creation failed!");
    return;
  }
  log_i("Queue: OK");

  // Store global pointer for task wrapper
  g_microphone = this;
  
  // Create I2S reader task
  log_i("Task: Creating I2S reader...");
  BaseType_t task_result = xTaskCreate(
    Microphone::i2sReaderTaskWrapper,  // Static wrapper
    "Mic I2S Reader",
    I2S_TASK_STACK,
    this,
    I2S_TASK_PRI,
    &reader_task_handle
  );
  
  if (task_result != pdPASS) {
    log_e("ERROR: I2S task creation failed!");
    return;
  }
  log_i("Task: OK");
}

//============================================
// Microphone::i2sReaderTaskWrapper() - Static task wrapper
//============================================
void Microphone::i2sReaderTaskWrapper(void *param) {
  Microphone* pThis = static_cast<Microphone*>(param);
  pThis->i2sReaderTask();
}

//============================================
// Microphone::getLevel() - Get current dB level (async)
//============================================
double Microphone::getLevel() {
  AudioSample q;
  // Non-blocking queue check
  if (xQueueReceive(samples_queue, &q, 0) == pdTRUE) {
    current_level = processSamples(q);
  }
  return current_level;
}

//============================================
// Microphone Private Methods
//============================================

double Microphone::rmsToDb(float rms) {
  if (rms < 1e-8) rms = 1e-8;
  
  double db = MIC_OFFSET_DB + MIC_REF_DB + 20.0 * log10(rms / MIC_REF_AMPL);
  
  if (isnan(db) || isinf(db)) db = 30.0;
  
  return db;
}

void Microphone::smoothLevel(double &smoothed_db, double raw_db, float alpha) {
  smoothed_db = smoothed_db * (1.0 - alpha) + raw_db * alpha;
}

double Microphone::processSamples(const AudioSample &q) {
  static unsigned long last_log = 0;
  
  if (q.sum_sqr_weighted <= 0) return smoothed_level;
  
  double rms = sqrt(q.sum_sqr_weighted / SAMPLES_SHORT);
  double raw_dB = rmsToDb(rms);
  
  smoothLevel(smoothed_level, raw_dB, 0.3);
  
  unsigned long now = millis();
  if (now - last_log > 1000) {
    log_i("[SOUND] measured dB: %.1f", smoothed_level);
    last_log = now;
  }
  
  return smoothed_level;
}

esp_err_t Microphone::i2sInit() {
  log_i("I2S init: rate=%d bits=%d", SAMPLE_RATE, SAMPLE_BITS);
  
  pinMode(I2S_LR, OUTPUT);
  digitalWrite(I2S_LR, HIGH);
  log_i("I2S L/R pin: GPIO%d set HIGH", I2S_LR);
  
  const i2s_config_t i2s_config = {
    .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = i2s_bits_per_sample_t(SAMPLE_BITS),
    .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
    .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = DMA_BANKS,
    .dma_buf_len = DMA_BANK_SIZE,
    .use_apll = false,          // ESP32-S3 has no APLL for I2S; silently ignored if true
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  const i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = -1,
    .data_in_num = I2S_SD
  };

  esp_err_t install_err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  if (install_err != ESP_OK) {
    log_e("I2S install failed: %d", install_err);
    return install_err;
  }
  log_i("I2S driver installed");
  
  esp_err_t pin_err = i2s_set_pin(I2S_PORT, &pin_config);
  if (pin_err != ESP_OK) {
    log_e("I2S pin config failed: %d", pin_err);
    return pin_err;
  }
  log_i("I2S pins: WS=%d SCK=%d SD=%d", I2S_WS, I2S_SCK, I2S_SD);
  
  return ESP_OK;
}

void Microphone::i2sReaderTask() {
  log_i("I2S Reader Task started");
  
  esp_err_t err = i2sInit();
  if (err != ESP_OK) {
    log_e("I2S init failed: %d", err);
    vTaskDelete(NULL);
    return;
  }
  log_i("I2S init OK");

  // Discard first block
  size_t bytes_read = 0;
  const size_t bytes_wanted = SAMPLES_CHUNK * sizeof(int32_t);
  err = i2s_read(I2S_PORT, samples, bytes_wanted, &bytes_read, 500 / portTICK_PERIOD_MS);
  if (err != ESP_OK || bytes_read == 0) {
    // i2s_read() returns ESP_OK even on timeout, just with bytes_read < wanted -
    // must check bytes_read explicitly or a silent/dead mic reads as ESP_OK
    // forever and getLevel() never sees anything but the constructor default.
    log_e("First I2S read failed: err=%d bytes_read=%u/%u", err, (unsigned)bytes_read, (unsigned)bytes_wanted);
    vTaskDelete(NULL);
    return;
  }
  log_i("I2S reader ready");

  uint32_t sample_count = 0;   // completed 125ms measurement windows

  // A measurement window is read as SAMPLES_CHUNKS_PER_WINDOW separate I/O
  // blocks (see config.h). The sums below accumulate across those blocks so
  // the window itself stays 125ms; only the buffer got shorter. The IIR
  // filters need no special handling at all - their state lives in the
  // SOS_IIR_Filter_Cpp objects and carries over between calls, so the
  // filtered output is identical to processing one long block.
  int chunk_in_window = 0;
  float acc_sum_sqr_SPL = 0;
  float acc_sum_sqr_weighted = 0;

  // Sliding peak across the last SAMPLES_CHUNKS_PER_WINDOW blocks, so the
  // live-listen AGC keeps reacting over a 125ms horizon rather than
  // re-deciding its gain twice as often (which would be audible as faster
  // pumping). Costs 4 bytes instead of the 8KB buffer the old per-block
  // implementation needed.
  int32_t prev_stream_peak = 0;

  while (true) {
    err = i2s_read(I2S_PORT, samples, bytes_wanted, &bytes_read, 500 / portTICK_PERIOD_MS);
    if (err != ESP_OK || bytes_read < bytes_wanted) {
      log_e("I2S read short: err=%d bytes_read=%u/%u", err, (unsigned)bytes_read, (unsigned)bytes_wanted);
      continue;
    }

    if (sample_count < 3 || sample_count % 400 == 0) {
      log_i("raw[0..3]=%ld %ld %ld %ld", (long)rawSampleAt(0), (long)rawSampleAt(1),
            (long)rawSampleAt(2), (long)rawSampleAt(3));
    }

    // Live-listen audio stream tap (Babyphone Phase 2) - decimate-by-3
    // boxcar average (48kHz -> 16kHz) straight off the RAW samples, before
    // the equalizer/A-weighting filters below run on samples[]. Those
    // filters flatten frequency response and approximate human loudness
    // perception for *measurement* purposes - exactly the wrong thing for
    // natural-sounding playback (would make e.g. a baby's cry sound dull/
    // unnatural). Gated on isStreamingActive() so this is near-zero-cost
    // whenever nobody is listening.
    if (audio_stream.isStreamingActive()) {
      // Pass 1: peak of the decimated, 24-bit-domain block. Measured before
      // any quantization so the AGC gain below is computed against the true
      // block peak, without needing an int32 scratch buffer to hold the
      // intermediate values.
      int32_t peak = 0;
      for (int i = 0; i + 2 < SAMPLES_CHUNK; i += 3) {
        int32_t avg = (rawSampleAt(i) + rawSampleAt(i + 1) + rawSampleAt(i + 2)) / 3;
        // Same shift MIC_CONVERT uses (32-bit raw -> 24-bit sample).
        int32_t s24 = avg >> (SAMPLE_BITS - MIC_BITS);
        int32_t a = s24 < 0 ? -s24 : s24;
        if (a > peak) peak = a;
      }

      // Per-window AGC (see the constants above) - scale this block so the
      // loudest sample of the last ~125ms lands at AGC_TARGET_PEAK, then
      // quantize to 16-bit with saturation (a sample above that peak, e.g.
      // right at a transient's edge, must clip cleanly rather than wrap).
      // Taking the max with the previous block's peak gives fast attack
      // (a new transient lowers the gain immediately) and a one-block
      // release, instead of letting the gain jump on every 62.5ms block.
      int32_t agc_peak = (peak > prev_stream_peak) ? peak : prev_stream_peak;
      prev_stream_peak = peak;

      float gain = (agc_peak > 0) ? (AGC_TARGET_PEAK / (float)agc_peak) : AGC_MAX_GAIN;
      if (gain > AGC_MAX_GAIN) gain = AGC_MAX_GAIN;
      if (gain < 1.0f) gain = 1.0f;

      // Pass 2: same decimation again, scaled and quantized straight into the
      // 16-bit output buffer.
      int out_i = 0;
      for (int i = 0; i + 2 < SAMPLES_CHUNK; i += 3) {
        int32_t avg = (rawSampleAt(i) + rawSampleAt(i + 1) + rawSampleAt(i + 2)) / 3;
        float v = (float)(avg >> (SAMPLE_BITS - MIC_BITS)) * gain;
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        stream_chunk[out_i++] = (int16_t)v;
      }

      audio_stream.pushSamples(stream_chunk, out_i);
    } else {
      prev_stream_peak = 0;  // don't carry a stale peak into the next session
    }

    // Convert the raw int32 samples to float in place - each element is read
    // out (via rawSampleAt's memcpy) before the float overwrites it, so no
    // sample is lost and no second buffer is needed. Must stay AFTER the
    // live-listen tap above, which still wants the raw values.
    for (int i = 0; i < SAMPLES_CHUNK; i++) {
      samples[i] = MIC_CONVERT(rawSampleAt(i));
    }

    for (int i = 0; i < SAMPLES_CHUNK; i++) {
      acc_sum_sqr_SPL += samples[i] * samples[i];
    }

    // Equalize for the INMP441's frequency response, then apply A-weighting
    // to approximate human loudness perception before computing the level.
    equalizer_.filter(samples, samples, SAMPLES_CHUNK);
    acc_sum_sqr_weighted += aweight_.filter(samples, samples, SAMPLES_CHUNK);

    // Still mid-window - keep accumulating, don't publish a level yet.
    if (++chunk_in_window < SAMPLES_CHUNKS_PER_WINDOW) {
      continue;
    }
    chunk_in_window = 0;

    AudioSample q;
    q.sum_sqr_SPL = acc_sum_sqr_SPL;
    q.sum_sqr_weighted = acc_sum_sqr_weighted;
    q.proc_ticks = 0;
    acc_sum_sqr_SPL = 0;
    acc_sum_sqr_weighted = 0;

    {
      static unsigned long last_overload_log = 0;
      double raw_dB = rmsToDb(sqrt(q.sum_sqr_SPL / SAMPLES_SHORT));
      unsigned long now = millis();
      if (raw_dB >= MIC_OVERLOAD_DB && now - last_overload_log > 1000) {
        log_w("[MIC] Overload/clipping detected: %.1f dB (limit %.1f dB)", raw_dB, (double)MIC_OVERLOAD_DB);
        last_overload_log = now;
      }
    }

    if (sample_count < 3 || sample_count % 400 == 0) {
      log_i("sum_sqr_SPL=%g sum_sqr_weighted=%g (isnan=%d isinf=%d)",
            q.sum_sqr_SPL, q.sum_sqr_weighted,
            (int)isnan(q.sum_sqr_weighted), (int)isinf(q.sum_sqr_weighted));
    }

    xQueueOverwrite(samples_queue, &q);

    if (++sample_count % 400 == 0) {
      log_d("I2S: %u windows read", sample_count);
    }
  }
}
