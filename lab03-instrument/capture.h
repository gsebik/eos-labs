#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "workload.h"

/* ADC + DMA capture with a PWM loopback signal generator.
 *
 * PWM on SIG_PWM_PIN is jumpered to the ADC input on SIG_ADC_PIN. The ADC free
 * runs at SIG_RATE_HZ; the DMA channel fills one pool buffer at a time and its
 * completion interrupt publishes a READY block. No external signal source is
 * required, so every station can reproduce the same measurement.
 *
 * SIG_PWM_HZ is an exact multiple of the FFT bin spacing (SIG_RATE_HZ /
 * SIG_BLOCK), so the captured tone lands in one bin instead of between bins.
 */
enum {
    SIG_RATE_HZ = 50000,
    SIG_BLOCK = 256,
    SIG_POOL = 3,
    SIG_ADC_CHANNEL = 2,
    SIG_PWM_HZ = 3125,      /* SIG_RATE_HZ / SIG_BLOCK * 16, i.e. FFT bin 16 */
    SIG_ISR_MARK_PIN = 18,
};
enum { SIG_PWM_PIN = 22, SIG_ADC_PIN = 28 };

typedef struct {
    const int32_t *samples;
    uint32_t id, job;
    uint64_t ready_us;
} sig_block_t;

typedef struct {
    uint32_t completed, dropped, overruns;
} sig_capture_stats_t;

/* Single-core capture component. All calls and its DMA IRQ belong to core 0.
 * A PROCESSING buffer is never assigned to DMA. With no free buffer, the DMA
 * writes into private discard storage and the completed block is counted as
 * dropped, so a reader is never overwritten. */
void sig_capture_start(void (*ready_from_isr)(void));
void sig_signal_start(void);
/* Move the generated test tone to the next/previous bin-aligned frequency. */
void sig_signal_step(int direction);
/* Current generated tone in Hz, for reporting. */
uint32_t sig_signal_hz(void);
bool sig_capture_take(sig_block_t *block);
void sig_capture_release(uint32_t id);
sig_capture_stats_t sig_capture_stats(void);
uint32_t sig_adc_hz(void);
