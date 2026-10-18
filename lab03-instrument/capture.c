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

static int32_t buffers[SIG_POOL][SIG_BLOCK];
static int32_t discard[SIG_BLOCK];
static kit_pool_t pool;
static int active = -1, channel;
static uint32_t jobs[SIG_POOL];
static uint64_t ready_time[SIG_POOL];
static uint32_t adc_rate;
static sig_capture_stats_t stats;
static void (*notify_ready)(void);
_Static_assert(SIG_POOL == KIT_POOL_SIZE, "Pool capacity must match storage");

static void start_next(void)
{
    active = kit_pool_dma(&pool);
    dma_channel_set_write_addr(channel,
        active < 0 ? discard : buffers[active], false);
    dma_channel_set_trans_count(channel, SIG_BLOCK, true);
}

static void dma_done(void)
{
    if (!dma_channel_get_irq0_status(channel)) return;
    dma_channel_acknowledge_irq0(channel);
    gpio_put(SIG_ISR_MARK_PIN, 1);
    stats.completed++;
    const int done = active;
    if (done >= 0) {
        const bool queued = kit_pool_complete(&pool, (unsigned)done);
        hard_assert(queued);
        jobs[done] = stats.completed;
        ready_time[done] = sig_trace(SIG_BLOCK_READY, stats.completed,
                                     (uint32_t)done, 0);
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
    if (done >= 0 && notify_ready) notify_ready();
    gpio_put(SIG_ISR_MARK_PIN, 0);
}

/* Bin-aligned test tones. The divider stays at 1, so the wrap must be an
 * integer that fits the 16-bit counter: wrap + 1 = clk / (bin * rate / block). */
static const uint16_t tone_bins[] = {12, 16, 20, 24, 32, 40, 48, 64, 80, 96};
static uint tone_slice;
static unsigned tone_index;
static uint32_t tone_hz;

static void apply_tone(uint16_t bin)
{
    const uint32_t per_block = clock_get_hz(clk_sys) / SIG_RATE_HZ * SIG_BLOCK;
    const uint32_t wrap_plus_1 = per_block / bin;
    if (wrap_plus_1 < 2 || wrap_plus_1 > 0x10000u) return;
    pwm_set_wrap(tone_slice, (uint16_t)(wrap_plus_1 - 1));
    pwm_set_gpio_level(SIG_PWM_PIN, (uint16_t)(wrap_plus_1 / 2));
    tone_hz = clock_get_hz(clk_sys) / wrap_plus_1;
}

void sig_signal_start(void)
{
    tone_slice = pwm_gpio_to_slice_num(SIG_PWM_PIN);
    gpio_set_function(SIG_PWM_PIN, GPIO_FUNC_PWM);
    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv(&config, 1.0f);
    pwm_init(tone_slice, &config, true);
    tone_index = 1;                     /* bin 16: the 3125 Hz reference tone */
    apply_tone(tone_bins[tone_index]);
}

void sig_signal_step(int direction)
{
    const unsigned count = sizeof tone_bins / sizeof tone_bins[0];
    tone_index = (unsigned)(((int)tone_index + direction + (int)count) % (int)count);
    apply_tone(tone_bins[tone_index]);
}

uint32_t sig_signal_hz(void) { return tone_hz; }


uint32_t sig_adc_hz(void) { return adc_rate; }

void sig_capture_start(void (*ready_from_isr)(void))
{
    hard_assert(get_core_num() == 0);
    kit_pool_init(&pool);
    notify_ready = ready_from_isr;

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

bool sig_capture_take(sig_block_t *block)
{
    const uint32_t saved = save_and_disable_interrupts();
    const int taken = kit_pool_take(&pool);
    hard_assert(taken != -2);
    if (taken < 0) {
        restore_interrupts(saved);
        return false;
    }
    const unsigned id = (unsigned)taken;
    *block = (sig_block_t){ buffers[id], id, jobs[id], ready_time[id] };
    restore_interrupts(saved);
    return true;
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
