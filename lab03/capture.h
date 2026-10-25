/* ADC + DMA capture with a PWM loopback signal generator.
 *
 * PWM on SIG_PWM_PIN is jumpered to the ADC input on SIG_ADC_PIN, so the
 * instrument supplies its own repeatable test signal and every station can
 * reproduce the same measurement. The ADC free-runs at SIG_RATE_HZ; one DMA
 * channel fills one pool buffer at a time, and its completion interrupt
 * publishes the finished block.
 *
 * Two deliveries exist for the same hardware. The RTOS target puts a
 * three-descriptor FreeRTOS queue between that interrupt and the processing
 * task; the foreground target keeps a small ring and polls it instead. All
 * calls and the DMA interrupt belong to core 0.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "workload.h"

enum {
    SIG_RATE_HZ = 50000,    /* ADC sample rate */
    SIG_BLOCK = 256,        /* samples per block, and the FFT window length */
    SIG_POOL = 3,           /* sample buffers: enough to overlap one block */
    SIG_ADC_CHANNEL = 2,    /* ADC channel 2 is GP28 */
    SIG_PWM_HZ = 3125,      /* SIG_RATE_HZ / SIG_BLOCK * 16: FFT bin 16 */
    SIG_ISR_MARK_PIN = 18,  /* analyzer marker around the DMA handler body */
};
enum {
    SIG_PWM_PIN = 22,       /* generated tone out; jumper to SIG_ADC_PIN */
    SIG_ADC_PIN = 28,       /* ADC input; jumper from SIG_PWM_PIN */
};

/* One finished block, as delivered to the processing task.
 *
 * `samples` points into pool storage; it is not a copy. The receiving task owns
 * that buffer from delivery until it calls sig_capture_release(). `job` numbers
 * blocks in arrival order, and `ready_us` is the microsecond timestamp the
 * interrupt recorded, so every later event can be measured from one origin.
 */
typedef struct {
    const int32_t *samples;  /* SIG_BLOCK 12-bit ADC readings, centred later */
    uint32_t id;             /* pool buffer index, 0 .. SIG_POOL-1 */
    uint32_t job;            /* arrival order, starting at 1 */
    uint64_t ready_us;       /* time_us_64() when the block became READY */
} sample_block_t;

/* Why input was lost. The three cases are separate faults, so they stay
 * separate counters rather than one "errors" total. */
typedef struct {
    uint32_t completed;       /* blocks captured and delivered */
    uint32_t dropped;         /* done, but no free buffer: real input loss */
    uint32_t overruns;        /* ADC FIFO overran: samples were overwritten */
    uint32_t queue_rejected;  /* send failed: delivery fault, not storage */
} sig_capture_stats_t;

/* Create the delivery object, program the ADC and DMA, and start acquiring.
 *
 * Call after sig_dsp_init() and before any receive. A buffer that is
 * PROCESSING is never assigned to the DMA: when no buffer is free, the DMA
 * writes into private discard storage and the block is counted as dropped, so
 * a reader is never overwritten. Call once, from core 0.
 */
void sig_capture_start(void);

/* Configure the PWM slice and start the generated test tone.
 *
 * The generator runs independently of the CPU and the DMA, so stepping or
 * observing it does not disturb an acquisition in progress. Called once from
 * main() before acquisition starts.
 */
void sig_signal_start(void);

/* Move the generated tone to the next or previous bin-aligned frequency.
 *
 * Every step lands the tone on an exact FFT bin, so its energy stays in one
 * band instead of leaking between bands. @param direction +1 for the next
 * higher bin, -1 for the next lower one; the list wraps at both ends.
 */
void sig_signal_step(int direction);

/* The frequency the generator is currently producing, in hertz.
 *
 * This is the value the display prints, so a capture can record the tone it
 * was taken at.
 */
uint32_t sig_signal_hz(void);

/* Wait for one completed block and take ownership of its sample buffer.
 *
 * The RTOS build blocks in xQueueReceive() until a descriptor arrives, so it
 * needs no separate wake-up object. The foreground build polls and returns
 * false when nothing is ready.
 *
 * @param block out: descriptor of the block now owned by the caller
 * @return true when a block was delivered, false when none is ready
 */
bool sig_capture_receive(sample_block_t *block);

/* Return a sample buffer to the pool, making it available to the DMA again.
 *
 * Call after the last read of block.samples. The private result computed from
 * those samples stays valid, so publication is a separate, later step.
 *
 * @param id the buffer index taken from the received descriptor
 */
void sig_capture_release(uint32_t id);

/* A consistent copy of the capture counters, taken with interrupts masked. */
sig_capture_stats_t sig_capture_stats(void);

/* The ADC sample rate actually programmed, in hertz. */
uint32_t sig_adc_hz(void);
