/* Frequency-band calculation and the adjustable processing load.
 *
 * One block of 256 samples becomes 32 band strengths: DC to 25 kHz in 781.25 Hz
 * groups. The extra repeats parameter adds bounded arithmetic after the FFT so
 * the lab can raise execution demand without changing what is displayed.
 *
 * The FFT is supplied and ready to use; it is not a student exercise.
 */
#pragma once
#include <stdint.h>

enum {
    SIG_SAMPLES = 256,  /* FFT window length, in samples */
    SIG_BANDS = 32,     /* displayed bands, 4 FFT bins each */
};

/* The band range the generator and the frequency bar both cover: low enough to
 * fit the PWM counter, high enough to stay below the 25 kHz Nyquist limit. */
enum {
    SIG_TONE_BAND_MIN = 3,      /* 2.73 kHz */
    SIG_TONE_BAND_MAX = 30,     /* 23.83 kHz */
    SIG_TONE_BAND_MID = 16,     /* 12.89 kHz, the middle of the sweep */
};

/* One computed spectrum. All members are private to the caller until the
 * result is published, so a reader may keep a copy of this struct safely. */
typedef struct {
    float bands[SIG_BANDS];  /* log-scaled band strength; display maps 0..10 */
    float rms;               /* full-scale-normalised RMS of this block */
    float checksum;          /* arithmetic probe: changes only with the data */
    unsigned peak_bin;       /* FFT bin with the most power, 0..127 */
} spectrum_result_t;

/* Prepare the window table and the load-generator weights. Call once. */
void sig_dsp_init(void);

/* Window, transform and reduce one block, then add the synthetic load.
 *
 * Reads only the caller's sample array and writes only `result`; it takes no
 * lock and touches no shared state, so two callers would not interfere.
 *
 * @param samples SIG_SAMPLES readings, 0..4095 as delivered by the ADC
 * @param repeats extra arithmetic passes after the FFT; 0 leaves the FFT alone
 * @param result out: the computed spectrum, valid until overwritten
 */
void sig_dsp_process(const int32_t samples[SIG_SAMPLES], unsigned repeats,
                     spectrum_result_t *result);
