#ifndef MICROPHONE_H
#define MICROPHONE_H

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "config.h"
#include "sos-iir-filter.h"

//
// Microphone Data Structure
//
struct AudioSample {
  float sum_sqr_SPL;
  float sum_sqr_weighted;
  uint32_t proc_ticks;
};

//
// Microphone Class - Manages I2S microphone and audio processing
//
class Microphone {
public:
  Microphone();
  void init();              // Initialize I2S peripheral and reader task
  double getLevel();        // Get current smoothed dB level
  
private:
  static void i2sReaderTaskWrapper(void *param);  // Static task wrapper
  void i2sReaderTask();     // Instance I2S reader task

  // Audio processing methods
  double rmsToDb(float rms);
  void smoothLevel(double &smoothed_db, double raw_db, float alpha);
  double processSamples(const AudioSample &q);
  esp_err_t i2sInit();

  // Member variables
  TaskHandle_t reader_task_handle;
  double current_level;
  double smoothed_level;

  // IIR filters applied to each block before computing the dBA level:
  // equalizer flattens the INMP441's frequency response, then A-weighting
  // approximates human loudness perception. Each owns its own filter state.
  SOS_IIR_Filter_Cpp equalizer_;
  SOS_IIR_Filter_Cpp aweight_;
};

// Global instance
extern Microphone microphone;

// Global buffer for the I2S reader task. Holds ONE I/O block
// (SAMPLES_CHUNK), not a whole measurement window - see config.h.
extern float samples[SAMPLES_CHUNK];
extern QueueHandle_t samples_queue;

#endif // MICROPHONE_H
