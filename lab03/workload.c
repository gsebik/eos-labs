/* Frequency-band calculation and the adjustable load: implementation.
 *
 * The transform is a prepared radix-2 FFT over a Hann-windowed block. Band
 * strengths are log-scaled powers, which is why the display maps a band value
 * of about 8 to a nearly full-height bar. The `repeats` loop afterwards is
 * deliberate synthetic CPU demand: it reads the already-computed bands and
 * mixes them through fixed random weights, so it costs time without changing
 * what the instrument displays.
 */
#include "workload.h"
#include <math.h>
#include <string.h>

static float real_part[SIG_SAMPLES], imag_part[SIG_SAMPLES];
static float hann[SIG_SAMPLES];
static float weights[8][SIG_SAMPLES];
static float state[16];

/* Build the Hann window once, then the fixed weight table. Both are constants
 * of the experiment, so nothing here depends on the signal. */
void sig_dsp_init(void)
{
    for (unsigned i = 0; i < SIG_SAMPLES; ++i) {
        hann[i] = 0.5f - 0.5f * cosf(6.28318530718f * i / (SIG_SAMPLES - 1));
    }
    uint32_t seed = 2350;
    for (unsigned row = 0; row < 8; ++row) {
        for (unsigned i = 0; i < SIG_SAMPLES; ++i) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            weights[row][i] = ((int)(seed & 255u) - 128) / 2048.0f;
        }
    }
    memset(state, 0, sizeof state);
}

/* In-place radix-2 FFT over real_part/imag_part.
 *
 * The bit-reversal pass permutes only the real half, which is valid because the
 * imaginary array is cleared before the transform. Supplied code: the lab
 * measures this work, it does not ask you to write it.
 */
static void fft(void)
{
    for (unsigned i = 1, j = 0; i < SIG_SAMPLES; ++i) {
        unsigned bit = SIG_SAMPLES >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float value = real_part[i];
            real_part[i] = real_part[j];
            real_part[j] = value;
        }
    }
    for (unsigned width = 2; width <= SIG_SAMPLES; width <<= 1) {
        const float angle = -6.28318530718f / width;
        const float step_r = cosf(angle), step_i = sinf(angle);
        for (unsigned base = 0; base < SIG_SAMPLES; base += width) {
            float wr = 1, wi = 0;
            for (unsigned k = 0; k < width / 2; ++k) {
                const unsigned a = base + k, b = a + width / 2;
                const float tr = wr * real_part[b] - wi * imag_part[b];
                const float ti = wr * imag_part[b] + wi * real_part[b];
                real_part[b] = real_part[a] - tr;
                imag_part[b] = imag_part[a] - ti;
                real_part[a] += tr;
                imag_part[a] += ti;
                const float next = wr * step_r - wi * step_i;
                wi = wr * step_i + wi * step_r;
                wr = next;
            }
        }
    }
}

/* Centre and window the block, transform it, then reduce 4 bins per band.
 *
 * `peak_bin` and `checksum` are diagnostics: the first says where the energy
 * is, the second changes when the data changes, so a capture can show that both
 * the signal path and the arithmetic are live.
 */
void sig_dsp_process(const int32_t samples[SIG_SAMPLES], unsigned repeats,
                     spectrum_result_t *result)
{
    float energy = 0;
    for (unsigned i = 0; i < SIG_SAMPLES; ++i) {
        /* ADC result is unsigned 12-bit, centred here for the DSP stage. */
        const float value = (float)(samples[i] - 2048) / 2048.0f;
        real_part[i] = value * hann[i];
        imag_part[i] = 0;
        energy += value * value;
    }
    result->rms = sqrtf(energy / SIG_SAMPLES);
    fft();
    float largest = -1;
    result->peak_bin = 0;
    for (unsigned band = 0; band < SIG_BANDS; ++band) {
        float sum = 0;
        for (unsigned j = 0; j < 4; ++j) {
            const unsigned bin = band * 4 + j;
            const float power = real_part[bin] * real_part[bin]
                              + imag_part[bin] * imag_part[bin];
            sum += power;
            if (power > largest) {
                largest = power;
                result->peak_bin = bin;
            }
        }
        result->bands[band] = log1pf(sum);
    }

    float checksum = 0;
    for (unsigned repeat = 0; repeat < repeats; ++repeat) {
        for (unsigned row = 0; row < 8; ++row) {
            float acc = 0;
            for (unsigned i = 0; i < SIG_SAMPLES; ++i) {
                acc += result->bands[i % SIG_BANDS] * weights[row][i];
            }
            state[row] = state[row] * 0.5f + acc * 0.5f;
            checksum += state[row];
        }
    }
    result->checksum = checksum;
}
