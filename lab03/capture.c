/* ADC + DMA capture: implementation.
 *
 * The public contracts are documented in capture.h; this file explains how the
 * hardware is driven. In one round:
 *
 *   1. start_next() claims a FREE buffer and points the DMA at it;
 *   2. the DMA fills 256 samples and raises its completion interrupt;
 *   3. dma_done() records the block as READY, delivers its descriptor and
 *      immediately arms the next transfer;
 *   4. the processing task takes the descriptor, reads the samples and returns
 *      the buffer with sig_capture_release().
 *
 * The interrupt does the minimum: no allocation, no printing, no waiting.
 */
#include "capture.h"
#include "pool.h"
#include "trace.h"
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/sync.h"
#if EOS_SIG_RTOS
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#endif

/* Pool storage. `discard` is where the DMA writes when every buffer is in use:
 * the samples are dropped on purpose, so a reader's buffer is never
 * overwritten by a later transfer. */
static int32_t buffers[SIG_POOL][SIG_BLOCK];
static int32_t discard[SIG_BLOCK];
static kit_pool_t pool;
static int active = -1, channel;
static uint32_t adc_rate;
static sig_capture_stats_t stats;
_Static_assert(SIG_POOL == KIT_POOL_SIZE, "Pool capacity must match storage");

#if EOS_SIG_RTOS
/* Statically allocated so creation cannot fail for want of heap. Three entries
 * match the pool: every queued descriptor corresponds to one READY buffer. */
static uint8_t queue_storage[SIG_POOL * sizeof(sample_block_t)];
static StaticQueue_t queue_control;
static QueueHandle_t ready_blocks;
#else
/* The foreground target keeps a small ring; it has no blocking receiver. */
static uint32_t jobs[SIG_POOL];
static uint64_t ready_time[SIG_POOL];
#endif

/* Arm the next transfer: claim a FREE buffer, or fall back to discard storage
 * so acquisition continues (and the loss is counted) when none is available. */
static void start_next(void)
{
    active = kit_pool_dma(&pool);
    dma_channel_set_write_addr(channel,
        active < 0 ? discard : buffers[active], false);
    dma_channel_set_trans_count(channel, SIG_BLOCK, true);
}

/* DMA completion interrupt: close out the block that just finished, then arm
 * the next one.
 *
 * The order in the interrupt matters. The buffer becomes READY and its
 * descriptor is queued before start_next() re-arms the DMA, so a woken reader
 * can never be handed storage that is already the next destination. If the
 * descriptor cannot be queued the buffer is reclaimed instead of leaked, and
 * the failure is counted separately from a storage shortage.
 */
static void dma_done(void)
{
    if (!dma_channel_get_irq0_status(channel)) return;
    dma_channel_acknowledge_irq0(channel);
    gpio_put(SIG_ISR_MARK_PIN, 1);
    stats.completed++;
    const int done = active;
    if (done >= 0) {
        const unsigned id = (unsigned)done;
        const uint32_t job = stats.completed;
        const uint64_t ready = sig_trace(SIG_BLOCK_READY, job, id, 0);
#if EOS_SIG_RTOS
        /* The descriptor is copied into the queue; the 256 samples stay in the
         * pool. A failed send must not leave the buffer unreachable, so it is
         * reclaimed here and counted as a rejection, not as a free-buffer drop. */
        if (!kit_pool_ready(&pool, id)) {
            stats.dropped++;
            sig_trace(SIG_DROP, job, id, 0);
        } else {
            const sample_block_t block = { buffers[id], id, job, ready };
            BaseType_t woken = pdFALSE;
            if (xQueueSendFromISR(ready_blocks, &block, &woken) != pdPASS) {
                kit_pool_begin(&pool, id);
                kit_pool_release(&pool, id);
                stats.queue_rejected++;
                sig_trace(SIG_DROP, job, id, 0);
            }
            portYIELD_FROM_ISR(woken);
        }
#else
        const bool queued = kit_pool_complete(&pool, id);
        hard_assert(queued);
        jobs[id] = job;
        ready_time[id] = ready;
#endif
    } else {
        stats.dropped++;
        sig_trace(SIG_DROP, stats.completed, 0, 0);
    }
    /* OVER is write-one-to-clear. hw_set_bits preserves the FIFO and DREQ
     * enables, which a whole-register write would silently clear and stall
     * acquisition. */
    if (adc_hw->fcs & ADC_FCS_OVER_BITS) {
        stats.overruns++;
        hw_set_bits(&adc_hw->fcs, ADC_FCS_OVER_BITS);
    }
    start_next();
    gpio_put(SIG_ISR_MARK_PIN, 0);
}

/* Tone generator: one frequency per displayed band.
 *
 * Band b covers FFT bins 4b..4b+3, so the tone sits at bin 4b+2: the middle of
 * the band, which keeps the fundamental inside one bar as the sweep moves. The
 * PWM divider stays at 1, so the wrap must fit the 16-bit counter:
 * wrap + 1 = clk_sys / (bin * SIG_RATE_HZ / SIG_BLOCK).
 *
 * The lowest usable band is 3 (about 2.3 kHz): below it the wrap would exceed
 * the counter. The highest is 30 (about 23.4 kHz), still below the 25 kHz
 * Nyquist limit of the 50 kS/s sampler.
 */
static uint tone_slice;
static unsigned tone_band = SIG_TONE_BAND_DEFAULT;
static uint32_t tone_hz;

/* Program the PWM slice for one band as a 50 % duty square wave.
 * Returns without changing anything when the band is out of range or its wrap
 * would not fit, so a bad request cannot leave the generator at a wrong
 * frequency. */
static void apply_band(unsigned band)
{
    if (band < SIG_TONE_BAND_MIN) band = SIG_TONE_BAND_MIN;
    if (band > SIG_TONE_BAND_MAX) band = SIG_TONE_BAND_MAX;
    const uint32_t bin = band * 4u + 2u;
    const uint32_t per_block = clock_get_hz(clk_sys) / SIG_RATE_HZ * SIG_BLOCK;
    const uint32_t wrap_plus_1 = per_block / bin;
    if (wrap_plus_1 < 2 || wrap_plus_1 > 0x10000u) return;
    pwm_set_wrap(tone_slice, (uint16_t)(wrap_plus_1 - 1));
    pwm_set_gpio_level(SIG_PWM_PIN, (uint16_t)(wrap_plus_1 / 2));
    tone_band = band;
    tone_hz = clock_get_hz(clk_sys) / wrap_plus_1;
}

void sig_signal_start(void)
{
    tone_slice = pwm_gpio_to_slice_num(SIG_PWM_PIN);
    gpio_set_function(SIG_PWM_PIN, GPIO_FUNC_PWM);
    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv(&config, 1.0f);
    pwm_init(tone_slice, &config, true);
    apply_band(SIG_TONE_BAND_DEFAULT);
}

void sig_signal_set_band(unsigned band) { apply_band(band); }

unsigned sig_signal_band(void) { return tone_band; }

void sig_signal_step(int direction)
{
    const int next = (int)tone_band + direction;
    apply_band((unsigned)next);
}

uint32_t sig_signal_hz(void) { return tone_hz; }

uint32_t sig_adc_hz(void) { return adc_rate; }

void sig_capture_start(void)
{
    hard_assert(get_core_num() == 0);
    kit_pool_init(&pool);
#if EOS_SIG_RTOS
    ready_blocks = xQueueCreateStatic(SIG_POOL, sizeof(sample_block_t),
                                      queue_storage, &queue_control);
    hard_assert(ready_blocks != NULL);
#endif

    adc_init();
    adc_gpio_init(SIG_ADC_PIN);
    adc_select_input(SIG_ADC_CHANNEL);
    /* The SDK documents the sample period as (1 + div) ADC clock cycles, so the
     * divisor is one less than the cycles per sample. */
    const uint32_t adc_clk = clock_get_hz(clk_adc);
    const uint32_t cycles = (adc_clk + SIG_RATE_HZ / 2) / SIG_RATE_HZ;
    hard_assert(cycles >= 96);          /* conversion takes at least 96 cycles */
    adc_set_clkdiv((float)(cycles - 1));
    adc_rate = adc_clk / cycles;
    adc_fifo_setup(true, true, 1, false, false);

    channel = dma_claim_unused_channel(true);
    dma_channel_config dma = dma_channel_get_default_config(channel);
    channel_config_set_transfer_data_size(&dma, DMA_SIZE_32);
    channel_config_set_read_increment(&dma, false);
    channel_config_set_write_increment(&dma, true);
    channel_config_set_dreq(&dma, DREQ_ADC);
    dma_channel_configure(channel, &dma, discard, &adc_hw->fifo,
                          SIG_BLOCK, false);
    dma_channel_set_irq0_enabled(channel, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_done);
    irq_set_priority(DMA_IRQ_0, 0x80);
    irq_set_enabled(DMA_IRQ_0, true);
    start_next();
    adc_run(true);
}

bool sig_capture_receive(sample_block_t *block)
{
#if EOS_SIG_RTOS
    /* Block until a descriptor arrives. The task is woken by the queue, so no
     * separate notification object is needed for this handoff. */
    if (xQueueReceive(ready_blocks, block, portMAX_DELAY) != pdPASS) return false;
    const uint32_t saved = save_and_disable_interrupts();
    const bool begun = kit_pool_begin(&pool, block->id);
    restore_interrupts(saved);
    hard_assert(begun);
    return true;
#else
    const uint32_t saved = save_and_disable_interrupts();
    const int taken = kit_pool_take(&pool);
    hard_assert(taken != -2);
    if (taken < 0) {
        restore_interrupts(saved);
        return false;
    }
    const unsigned id = (unsigned)taken;
    *block = (sample_block_t){ buffers[id], id, jobs[id], ready_time[id] };
    restore_interrupts(saved);
    return true;
#endif
}

void sig_capture_release(uint32_t id)
{
    const uint32_t saved = save_and_disable_interrupts();
    const bool released = kit_pool_release(&pool, id);
    hard_assert(released);
    restore_interrupts(saved);
}

sig_capture_stats_t sig_capture_stats(void)
{
    const uint32_t saved = save_and_disable_interrupts();
    const sig_capture_stats_t snapshot = stats;
    restore_interrupts(saved);
    return snapshot;
}
