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


//============================================
// Live-listen signal chain (48kHz raw -> AUDIO_STREAM_SAMPLE_RATE 16-bit PCM)
//
//   raw 24-bit  ->  FIR lowpass + decimate by STREAM_DECIM  ->  rumble highpass
//               ->  envelope-follower AGC with downward expander  ->  int16
//
// Everything here runs only while somebody is actually listening (gated on
// AudioStreamService::isStreamingActive()) and deliberately taps the RAW
// samples, before the equalizer/A-weighting filters that exist for dB
// *measurement* and would make speech sound dull.
//============================================

// --- Anti-alias decimation filter ---
//
// This used to be a 3-tap boxcar average, which is a terrible anti-alias
// filter for decimating 48kHz: it only attenuates ~3.5dB at the old 8kHz
// output Nyquist, so a whole octave folded back into the audible range. That
// what gave the stream its metallic, "recorded in an empty hall" character.
//
// Hamming-windowed sinc instead. The tap count is what decides how much of
// the passband survives, and getting it wrong is audible: the first version
// of this filter used 31 taps, whose transition band is 3.3*fs/N = 5.1kHz
// wide. Centred on a 5.5kHz cutoff that means the roll-off already starts
// around 3kHz and is ~20dB down by 5kHz - it eats the entire consonant range,
// which is exactly the "voice sounds muffled, like it's behind a wall"
// complaint. Aliasing was fixed; intelligibility was traded away for it.
//
// 63 taps narrows the transition to ~2.5kHz, which leaves room for a cutoff
// high enough to keep the passband flat well past the consonant range while
// the stopband still lands under the output Nyquist.
//
// Costs ~3M MAC/s (two passes over 1500 output samples, 16x/second) - a few
// percent of one core on an FPU-equipped S3, and only while somebody listens.
#define STREAM_DECIM 2
#define STREAM_FIR_TAPS 63

// The decimation factor, the stream rate and the block size have to agree, and
// silently disagreeing would show up as garbled audio rather than a build
// error. Cheaper to assert it.
static_assert(SAMPLE_RATE / STREAM_DECIM == AUDIO_STREAM_SAMPLE_RATE,
              "AUDIO_STREAM_SAMPLE_RATE must equal SAMPLE_RATE / STREAM_DECIM");
static_assert(SAMPLES_CHUNK % STREAM_DECIM == 0,
              "SAMPLES_CHUNK must divide evenly by STREAM_DECIM");

// Output scratch buffer for one decimated block, handed straight to
// AudioStreamService::pushSamples(). Only ever touched by the I2S task.
//
// There used to be a second, int32 "stream_chunk24" buffer alongside it
// holding the pre-gain 24-bit-domain values, so the AGC could measure the
// block before it was quantized to 16-bit. That is a second pass over the
// raw block now instead (measure, then filter+scale straight into this
// buffer), which is identical output for 8KB less permanent RAM - and the
// extra pass only runs while somebody is actually listening.
static int16_t stream_chunk[SAMPLES_CHUNK / STREAM_DECIM];
#define STREAM_FIR_ORDER (STREAM_FIR_TAPS - 1)
static float fir_coef[STREAM_FIR_TAPS];
static bool fir_ready = false;

// Tail of the previous raw block (24-bit domain), so the FIR window at the
// start of a block is filled with real history rather than zeros - otherwise
// every 62.5ms block boundary would produce an audible click.
static float fir_hist[STREAM_FIR_ORDER];

// --- DC / rumble highpass, applied after decimation (at the stream rate) ---
//
// The INMP441 has substantial sub-100Hz output (its own noise plus whatever
// the enclosure picks up structurally) that carries no useful information
// for a baby monitor but eats headroom and makes everything sound boomy.
//
// 80Hz rather than the 120Hz this started at: a male speaking fundamental
// sits around 100-120Hz, so the higher corner was cutting into the voice
// itself and thinning it out - which stacks with a dull top end into the
// same "behind a wall" impression. 80Hz still clears the rumble.
//
// The coefficient is computed from AUDIO_STREAM_SAMPLE_RATE in
// streamFirInit() rather than written out as a literal - it was a hardcoded
// 0.969 for 16kHz, and raising the stream rate would silently have moved the
// corner to 120Hz again without anything failing to build.
#define STREAM_HPF_CORNER_HZ 80.0f
static float stream_hpf_a = 0.0f;
static float hpf_x1 = 0.0f, hpf_y1 = 0.0f;

// --- AGC ---
//
// The mic is calibrated for accurate dB measurement (MIC_REF_DB /
// MIC_OVERLOAD_DB leave it a lot of headroom), so even a loud cry occupies
// only a small fraction of the 24-bit range and a plain bit-depth shift to
// 16-bit PCM is nearly inaudible. Some auto-gain is therefore unavoidable.
//
// What it must NOT do is what the previous version did: peak-normalise every
// 62.5ms block on its own with an instant release and a 72dB ceiling. In a
// quiet room that pushes the noise floor to full scale within one block, and
// the gain then lurches back down the moment anyone speaks - the pumping,
// room-tone-forward sound the "empty hall" impression comes from.
//
// Instead:
//   * an RMS envelope with instant attack but a ~2s release, so the gain no
//     longer rides up during every pause between sounds;
//   * a downward expander below AGC_KNEE_DB - quiet room tone stays quiet
//     (2:1 below the knee) instead of being normalised like real signal;
//   * gain ramped across the block instead of stepped at the boundary,
//     which removes the per-block zipper noise;
//   * no lower clamp on the gain, so loud sounds are turned DOWN to fit
//     rather than hard-clipped (the old `gain < 1.0f -> 1.0f` clamp meant
//     every loud cry was delivered as a square wave).
//
// The knee is expressed in dB SPL and converted using the SAME relation
// rmsToDb() uses, rather than being a magic amplitude constant. That matters:
// the first attempt at this set the knee to a hand-estimated raw amplitude
// which turned out to sit ~24dB too low, so a 36dB-SPL dishwasher three
// metres away was still being normalised to full scale and came back as a
// roar. Deriving it from MIC_REF_AMPL/MIC_REF_DB/MIC_OFFSET_DB keeps it
// automatically consistent with the meter's calibration - if MIC_REF_DB is
// ever re-calibrated, the knee follows.

// RMS the AGC normalises a signal to, in the 24-bit sample domain.
// ~-15.5dBFS RMS, which still leaves ~15dB of crest headroom before the
// clamp - speech peaks around 12-15dB above its own RMS.
static const float AGC_TARGET_RMS = 5500.0f;

// Below this SPL, the input is treated as room tone rather than as something
// worth normalising. 48dB is roughly "quiet living room with appliances
// running": above it (speech, crying, a door) the AGC brings things up to a
// consistent level; below it the expander lets them fall away. THIS is the
// knob to turn if live-listen feels too sensitive (raise it) or too deaf
// (lower it).
static const float AGC_KNEE_DB = 48.0f;

// rms such that rmsToDb(rms) == AGC_KNEE_DB - the inverse of the formula in
// rmsToDb(): dB = MIC_OFFSET_DB + MIC_REF_DB + 20*log10(rms / MIC_REF_AMPL).
static const float AGC_KNEE_RMS =
  (float)(MIC_REF_AMPL * pow(10.0, (AGC_KNEE_DB - MIC_OFFSET_DB - MIC_REF_DB) / 20.0));

// Gain applied exactly at the knee, and the ceiling for everything below it.
static const float AGC_KNEE_GAIN = AGC_TARGET_RMS / AGC_KNEE_RMS;

// Per-block envelope release: exp(-62.5ms / 2000ms).
static const float AGC_RELEASE = 0.969f;

static float stream_env = 0.0f;    // RMS envelope, 24-bit domain
static float stream_gain = 0.0f;   // gain at the end of the previous block
static bool stream_was_active = false;

// Builds the decimation filter's coefficients and the highpass coefficient on
// first use (rather than as hardcoded tables) - they all depend on
// SAMPLE_RATE and STREAM_DECIM, and this runs exactly once, the first time
// somebody starts listening.
static void streamFirInit() {
  const int M = STREAM_FIR_ORDER;

  // Cutoff is derived from the output Nyquist rather than named outright, so
  // that changing STREAM_DECIM moves it automatically. A Hamming window's
  // transition band is 3.3*fs/N wide; placing the cutoff half that width plus
  // a small margin below Nyquist puts the stopband edge just inside it, which
  // is the highest cutoff that still suppresses aliasing - and the higher the
  // cutoff, the more of the top end survives.
  const float nyquist = (float)AUDIO_STREAM_SAMPLE_RATE / 2.0f;
  const float half_transition = 1.65f * (float)SAMPLE_RATE / (float)STREAM_FIR_TAPS;
  const float fc = (nyquist - half_transition - 250.0f) / (float)SAMPLE_RATE;  // cycles/sample

  // One-pole highpass at STREAM_HPF_CORNER_HZ, applied after decimation and
  // therefore at the *output* rate.
  stream_hpf_a = expf(-2.0f * (float)PI * STREAM_HPF_CORNER_HZ / (float)AUDIO_STREAM_SAMPLE_RATE);

  float sum = 0.0f;

  for (int n = 0; n <= M; n++) {
    float m = (float)n - (float)M / 2.0f;
    float sinc = (fabsf(m) < 1e-6f) ? (2.0f * fc)
                                    : sinf(2.0f * (float)PI * fc * m) / ((float)PI * m);
    float w = 0.54f - 0.46f * cosf(2.0f * (float)PI * (float)n / (float)M);  // Hamming
    fir_coef[n] = sinc * w;
    sum += fir_coef[n];
  }
  // Normalise to unity DC gain so the filter neither boosts nor attenuates
  // the signal level the AGC below is calibrated against.
  for (int n = 0; n <= M; n++) fir_coef[n] /= sum;
  fir_ready = true;
}

// One decimated output sample: the FIR window for output k spans raw samples
// [k*3 - M .. k*3], reaching into fir_hist for the negative indices. Reads
// only - the history is updated once, after both passes, so pass 1 and pass 2
// see identical input and produce identical output.
static inline float streamFirAt(int k) {
  const int M = STREAM_FIR_ORDER;
  const int base = k * STREAM_DECIM - M;
  float acc = 0.0f;
  for (int t = 0; t <= M; t++) {
    int j = base + t;
    float v = (j >= 0) ? (float)(rawSampleAt(j) >> (SAMPLE_BITS - MIC_BITS))
                       : fir_hist[M + j];
    acc += fir_coef[t] * v;
  }
  return acc;
}

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

  // The live-listen AGC's state (envelope, gain, filter history) lives in the
  // file-scope statics next to the signal chain itself, not here - it has to
  // survive across listening sessions' worth of blocks and is reset from
  // stream_was_active when a new listener connects.

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
      if (!fir_ready) streamFirInit();

      // First block of a new listening session: start from a clean slate so
      // no stale filter tail or envelope from the last session bleeds in.
      if (!stream_was_active) {
        memset(fir_hist, 0, sizeof(fir_hist));
        hpf_x1 = hpf_y1 = 0.0f;
        stream_env = 0.0f;
        stream_gain = 0.0f;   // fade up from silence over the first block
        stream_was_active = true;
      }

      const int out_n = SAMPLES_CHUNK / STREAM_DECIM;

      // Pass 1: run the full decimate+highpass chain to measure this block's
      // RMS, on a *copy* of the highpass state so pass 2 can replay it from
      // the same starting point. Two passes rather than a 4KB float scratch
      // buffer - permanent RAM is scarcer here than CPU cycles.
      //
      // RMS rather than peak, so the level the AGC reacts to is the same
      // quantity rmsToDb()/AGC_KNEE_DB are expressed in. A double
      // accumulator because the squares reach ~1e13 and 1000 of them
      // overflow a float's 7 significant digits long before the sum is done.
      float hx1 = hpf_x1, hy1 = hpf_y1;
      double sum_sqr = 0.0;
      for (int k = 0; k < out_n; k++) {
        float x = streamFirAt(k);
        float y = stream_hpf_a * (hy1 + x - hx1);
        hx1 = x;
        hy1 = y;
        sum_sqr += (double)y * (double)y;
      }
      float rms = (float)sqrt(sum_sqr / out_n);

      // Envelope: instant attack, ~2s release (see AGC_RELEASE).
      stream_env *= AGC_RELEASE;
      if (rms > stream_env) stream_env = rms;

      // Above the knee, normalise to AGC_TARGET_RMS. Below it, hold the gain
      // at its knee value and scale it down proportionally - a 2:1 downward
      // expander, so 6dB less input becomes 12dB less output and room tone
      // falls away instead of being lifted to full scale.
      float target_gain;
      if (stream_env >= AGC_KNEE_RMS) {
        target_gain = AGC_TARGET_RMS / stream_env;
      } else {
        target_gain = AGC_KNEE_GAIN * (stream_env / AGC_KNEE_RMS);
      }

      // Pass 2: replay the identical chain, ramping the gain from where the
      // previous block ended to the new target rather than stepping at the
      // block boundary. A gain *decrease* is the response to something
      // getting louder, so it ramps over the first eighth of the block
      // (~8ms) to get out of the way of the transient; an increase is the
      // slow release and ramps across the whole 62.5ms, where an abrupt
      // change would be audible.
      hx1 = hpf_x1;
      hy1 = hpf_y1;
      int ramp_n = (target_gain < stream_gain) ? (out_n / 8) : out_n;
      if (ramp_n < 1) ramp_n = 1;
      float g = stream_gain;
      float dg = (target_gain - stream_gain) / (float)ramp_n;
      for (int k = 0; k < out_n; k++) {
        float x = streamFirAt(k);
        float y = stream_hpf_a * (hy1 + x - hx1);
        hx1 = x;
        hy1 = y;

        float v = y * g;
        if (k < ramp_n) g += dg; else g = target_gain;
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        stream_chunk[k] = (int16_t)v;
      }
      hpf_x1 = hx1;
      hpf_y1 = hy1;
      stream_gain = target_gain;

      // Carry this block's tail forward as the next block's FIR history.
      // Must happen after both passes, and before the MIC_CONVERT loop below
      // overwrites samples[] with floats.
      for (int t = 0; t < STREAM_FIR_ORDER; t++) {
        fir_hist[t] = (float)(rawSampleAt(SAMPLES_CHUNK - STREAM_FIR_ORDER + t)
                              >> (SAMPLE_BITS - MIC_BITS));
      }

      // ~every 2s while streaming: the level the AGC is actually seeing, in
      // the same dB the meter shows, next to the gain it decided on. Makes
      // AGC_KNEE_DB tunable from a serial log instead of by ear.
      if (sample_count % 32 == 0) {
        log_i("[AUDIO] env=%.1fdB gain=%.4f (knee %.1fdB)",
              rmsToDb(stream_env), target_gain, AGC_KNEE_DB);
      }

      audio_stream.pushSamples(stream_chunk, out_n);
    } else {
      stream_was_active = false;  // next session re-initialises the chain
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
