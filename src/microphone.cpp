#include <Arduino.h>
#include "microphone.h"
#include "config.h"
#include <driver/i2s.h>
#include <cmath>

// Global instance
Microphone microphone;

// Static pointer for task wrapper
static Microphone* g_microphone = nullptr;

// Global buffers (needed for I2S DMA)
QueueHandle_t samples_queue = nullptr;
float samples[SAMPLES_SHORT] __attribute__((aligned(4)));

// Raw I2S read buffer, kept separate from `samples` to avoid strict-aliasing
// violations when converting int32 -> float in place.
static int32_t raw_samples[SAMPLES_SHORT] __attribute__((aligned(4)));

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
    .use_apll = true,
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
  err = i2s_read(I2S_PORT, raw_samples, SAMPLES_SHORT * sizeof(int32_t), &bytes_read, 150 / portTICK_PERIOD_MS);
  if (err != ESP_OK) {
    log_e("First I2S read failed: %d", err);
    vTaskDelete(NULL);
    return;
  }
  log_i("I2S reader ready");

  uint32_t sample_count = 0;
  while (true) {
    err = i2s_read(I2S_PORT, raw_samples, SAMPLES_SHORT * sizeof(int32_t), &bytes_read, 150 / portTICK_PERIOD_MS);
    if (err != ESP_OK) {
      log_e("I2S read failed: %d", err);
      continue;
    }

    // Convert raw int32 samples to float (separate buffers avoid a
    // strict-aliasing violation from reinterpreting samples[] in place)
    for (int i = 0; i < SAMPLES_SHORT; i++) {
      samples[i] = MIC_CONVERT(raw_samples[i]);
    }

    AudioSample q;
    q.sum_sqr_SPL = 0;
    for (int i = 0; i < SAMPLES_SHORT; i++) {
      q.sum_sqr_SPL += samples[i] * samples[i];
    }

    {
      static unsigned long last_overload_log = 0;
      double raw_dB = rmsToDb(sqrt(q.sum_sqr_SPL / SAMPLES_SHORT));
      unsigned long now = millis();
      if (raw_dB >= MIC_OVERLOAD_DB && now - last_overload_log > 1000) {
        log_w("[MIC] Overload/clipping detected: %.1f dB (limit %.1f dB)", raw_dB, (double)MIC_OVERLOAD_DB);
        last_overload_log = now;
      }
    }

    // Equalize for the INMP441's frequency response, then apply A-weighting
    // to approximate human loudness perception before computing the level.
    equalizer_.filter(samples, samples, SAMPLES_SHORT);
    q.sum_sqr_weighted = aweight_.filter(samples, samples, SAMPLES_SHORT);
    q.proc_ticks = 0;

    xQueueOverwrite(samples_queue, &q);
    
    if (++sample_count % 400 == 0) {
      log_d("I2S: %u blocks read", sample_count);
    }
  }
}
