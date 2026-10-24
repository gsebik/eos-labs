#pragma once
#include <stdint.h>

enum { SIG_SAMPLES = 256, SIG_BANDS = 32 };
typedef struct {
    float bands[SIG_BANDS];
    float rms;
    float checksum;
    unsigned peak_bin;
} spectrum_result_t;

/* Block processing: window, FFT, band energies, then a bounded amount of
 * additional arithmetic whose size is set by `repeats`. The extra arithmetic
 * stands in for a heavier feature so students can change the execution demand
 * without writing a new algorithm. `checksum` is a correctness/compute probe,
 * not a measurement of a physical signal. */
void sig_dsp_init(void);
void sig_dsp_process(const int32_t samples[SIG_SAMPLES], unsigned repeats,
                     spectrum_result_t *result);
