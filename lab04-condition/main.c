/* The instrument application: four tasks over one shared sample pool and one
 * shared spectrum.
 *
 *   processing_task  priority 3  blocks on a descriptor, computes, publishes
 *   display_task     priority 2  every 100 ms, copies the spectrum and draws
 *   settings_task    priority 1  every 20 ms, polls buttons, writes settings
 *   reporting_task   priority 1  every second, prints counters and CPU shares
 *
 * The numbers in brackets [0]..[6] mark the source locations the lab manual
 * refers to. Everything this file owns is declared static: the components in
 * capture.c, pool.c, workload.c, panel.c and trace.c are separate modules with
 * their own contracts.
 */
#include <stdio.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "pico/sync.h"
#include "hardware/i2c.h"
#include "hardware/sync.h"
#include "capture.h"
#include "panel.h"
#include "trace.h"
#include "task_marker.h"
#include "gt911.h"
#ifndef EOS_SCHED_MODE
#define EOS_SCHED_MODE 0
#endif
#if EOS_SCHED_MODE
#include "sched_mode.h"
#endif
#if EOS_SIG_RTOS
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "queue.h"
#endif

#ifndef EOS_CORES
#define EOS_CORES 1
#endif
#ifndef EOS_PLACEMENT
#define EOS_PLACEMENT 0
#endif
#ifndef EOS_TARGET_KHZ
#define EOS_TARGET_KHZ 150000
#endif
#ifndef EOS_ALARM_PERIOD_MS
#define EOS_ALARM_PERIOD_MS 20
#endif
#ifndef EOS_ALARM_DEADLINE_US
#define EOS_ALARM_DEADLINE_US 2000
#endif
#ifndef EOS_SEED_SMP_BUG
#define EOS_SEED_SMP_BUG 1
#endif
#ifndef EOS_FAST_RING
#define EOS_FAST_RING 0
#endif
#ifndef EOS_ALARM_PRIORITY
#define EOS_ALARM_PRIORITY 4
#endif
#ifndef EOS_RING_DELAY_US
#define EOS_RING_DELAY_US 0
#endif
enum { EOS_ALARM_PIN = 26 };
enum { FAST_SAMPLES = 64, FAST_QUEUE_LEN = 8 };
typedef struct {
    int32_t samples[FAST_SAMPLES];
    uint32_t job;
    uint64_t ready_us;
    uint32_t check;     /* covers samples and job; detects a torn read */
} fast_block_t;

/* FNV-1a over the record. The consumer recomputes it, so half of one record
 * combined with half of another fails the test. */
static uint32_t fast_check(const fast_block_t *fb)
{
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < FAST_SAMPLES; ++i) {
        h = (h ^ (uint32_t)fb->samples[i]) * 16777619u;
    }
    return (h ^ fb->job) * 16777619u;
}

/* [0] Budget. One round is SIG_BLOCK ADC samples at SIG_RATE_HZ: 5.12 ms. The
 * pool holds SIG_POOL buffers, so a block that is still PROCESSING when DMA
 * needs a slot causes genuine loss: the DMA writes to discard storage and the
 * block is counted as dropped. The safe deadline therefore follows from the
 * buffer-ownership states, not from a fixed formula. */
enum { MARK_HEARTBEAT = 16, MARK_DISPLAY = 17, MARK_WORK = 21 };

#if EOS_SIG_RTOS
/* FreeRTOS run-time stats counter: the SDK's time_us_32 is a static inline, so
 * FreeRTOSConfig.h declares this helper instead. */
static uint32_t g_rt_epoch;
uint32_t eos_runtime_counter(void) { return time_us_32() - g_rt_epoch; }
void eos_runtime_counter_start(void) { g_rt_epoch = time_us_32(); }
#endif

static spectrum_result_t published_spectrum;
static uint32_t threshold_q15 = 800, timebase_div = 4;
static uint32_t level_q15, detections;
static uint32_t processed, misses, largest_response, largest_compute;
static uint32_t largest_draw, largest_settings_wait, largest_publish_wait;
static uint32_t late_blocks;
static uint32_t freshness_late;
static uint32_t largest_publication;
static volatile uint32_t g_fault;
static volatile uint32_t alarm_count, alarm_late, alarm_r_max_us;
static volatile uint32_t fast_drop, fast_torn, alarm_hits;
static QueueHandle_t fast_queue;
#if EOS_FAST_RING
/* TODO (4): choose the four memory orders for the cross-core ring.
 *
 * The producer is the DMA interrupt and writes only fast_head. The consumer is
 * the alarm task and writes only fast_tail. A record must be complete in the
 * slot before head announces it, and the consumer must see the announcement
 * before it reads the slot. memory_order_relaxed promises no ordering at all,
 * so the starter compiles and may appear to work; it is not correct.
 *
 * Set each operation below and write one sentence saying what it protects.
 */
#define ORDER_PRODUCER_LOAD_TAIL  memory_order_relaxed
#define ORDER_PRODUCER_STORE_HEAD memory_order_relaxed
#define ORDER_CONSUMER_LOAD_HEAD  memory_order_relaxed
#define ORDER_CONSUMER_STORE_TAIL memory_order_relaxed

static fast_block_t fast_ring[FAST_QUEUE_LEN];
static _Atomic uint32_t fast_head, fast_tail;
static bool fast_ring_push(const fast_block_t *fb)
{
    const uint32_t head = atomic_load_explicit(&fast_head, memory_order_relaxed);
    const uint32_t tail = atomic_load_explicit(&fast_tail, ORDER_PRODUCER_LOAD_TAIL);
    const uint32_t next = (head + 1u) % FAST_QUEUE_LEN;
    if (next == tail) { ++fast_drop; return false; }
    fast_ring[head] = *fb;
    atomic_store_explicit(&fast_head, next, ORDER_PRODUCER_STORE_HEAD);
    return true;
}
static bool fast_ring_pop(fast_block_t *fb)
{
    const uint32_t tail = atomic_load_explicit(&fast_tail, memory_order_relaxed);
    const uint32_t head = atomic_load_explicit(&fast_head, ORDER_CONSUMER_LOAD_HEAD);
    if (tail == head) return false;
    *fb = fast_ring[tail];
    atomic_store_explicit(&fast_tail, (tail + 1u) % FAST_QUEUE_LEN,
                          ORDER_CONSUMER_STORE_TAIL);
    return true;
}
#endif
static volatile uint32_t cmd_count, cmd_max_us;
static volatile uint32_t ui_latency_max_us, touch_count;
static volatile uint64_t g_button_edge_us;
static char g_touch_status[24] = "unknown";
static volatile uint32_t g_core_processing, g_core_display, g_core_settings;
static volatile uint32_t g_core_alarm, g_core_comm;
static volatile uint32_t alarm_cpu_max_us;
static volatile uint32_t seed_shared;
static critical_section_t stats_lock;
#define DWT_DEMCR (*(volatile uint32_t *)0xE000EDFCu)
#define DWT_CTRL  (*(volatile uint32_t *)0xE0001000u)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)

static uint32_t cycles_to_us(uint32_t cycles)
{
    return (uint32_t)((uint64_t)cycles * 1000000u / clock_get_hz(clk_sys));
}

static void cycle_counter_init(void);

typedef struct {
    uint32_t max_us, sum_us, count, core;
} task_stat_t;

static task_stat_t st_processing, st_display, st_settings, st_comm,
                    st_touch, st_alarm;
static bool dwt_ready[2];

static void ensure_dwt(void)
{
    const unsigned core = get_core_num();
    if (core < 2 && !dwt_ready[core]) {
        cycle_counter_init();
        dwt_ready[core] = true;
    }
}

static void stat_end(task_stat_t *s, uint32_t c0, unsigned core)
{
    if (get_core_num() != core) return;   /* migrated: the cycle counters differ */
    const uint32_t us = cycles_to_us(DWT_CYCCNT - c0);
    if (us > s->max_us) s->max_us = us;
    s->sum_us += us;
    s->count++;
    s->core = core;
}

static void cycle_counter_init(void)
{
    DWT_DEMCR |= 1u << 24;
    DWT_CYCCNT = 0;
    DWT_CTRL |= 1u;
}
static uint32_t seed_dsp_private, seed_display_private;
#if EOS_SIG_RTOS
static TaskHandle_t processing_handle, display_handle, settings_handle,
    reporting_handle, alarm_handle, comm_handle, touch_handle;
static SemaphoreHandle_t settings_mutex, spectrum_mutex;
static void require_success(BaseType_t result)
{
    /* A function, not an assertion macro: required operations always execute. */
    if (result != pdPASS) {
        g_fault = 4;
        taskDISABLE_INTERRUPTS();
        for (;;) {}
    }
}
#endif

/* [4] Settings lock. The settings task writes the settings under it and the
 * processing task reads them once per block. The display never uses it. Both
 * critical sections are short; a reader can still wait if the owner is
 * pre-empted while holding the lock, which is what the settings wait is. */
static void settings_acquire(void)
{
#if EOS_SIG_RTOS
    const uint64_t start = time_us_64();
    require_success(xSemaphoreTake(settings_mutex, portMAX_DELAY));
    const uint32_t waited = (uint32_t)(time_us_64() - start);
    if (waited > largest_settings_wait) largest_settings_wait = waited;
    if (waited > 10) sig_trace(SIG_SETTINGS_WAIT, 0, waited, 1);
#endif
}

/* Give the settings lock back. Short by design: a reader that needs the
 * settings must not wait for unrelated work. */
static void settings_release(void)
{
#if EOS_SIG_RTOS
    require_success(xSemaphoreGive(settings_mutex));
#endif
}

/* The spectrum lock protects the published spectrum: one complete result, which
 * the display copies and the processing task replaces. It is deliberately
 * separate from the settings lock, so a display critical section cannot be
 * confused with a settings-lock wait. */
static void spectrum_acquire(void)
{
#if EOS_SIG_RTOS
    require_success(xSemaphoreTake(spectrum_mutex, portMAX_DELAY));
#endif
}

/* Give the spectrum lock back. Called as soon as the copy is complete, so the
 * lock never spans the screen transfer. */
static void spectrum_release(void)
{
#if EOS_SIG_RTOS
    require_success(xSemaphoreGive(spectrum_mutex));
#endif
}

/* [3] Publication. The processing task owns a private result and copies it into
 * the shared published spectrum under the spectrum lock. Any wait here delays
 * this job's completion and therefore its next descriptor; the sample buffer is
 * already back in the pool, so storage reuse does not depend on this step. */
static void publish_spectrum(const spectrum_result_t *result, uint32_t threshold)
{
    const uint64_t start = time_us_64();
    spectrum_acquire();
    const uint32_t waited = (uint32_t)(time_us_64() - start);
    if (waited > largest_publish_wait) largest_publish_wait = waited;
    published_spectrum = *result;
    level_q15 = (uint32_t)(result->rms * 32768.0f);
    if (level_q15 > threshold) detections++;
    spectrum_release();
}

/* [2] Processing path for one delivered descriptor: read the settings, take the
 * block, compute, release the sample buffer, then publish the private result.
 * The sample buffer is released before publication because the samples have
 * been read for the last time at that point; publication is a separate
 * obligation with its own endpoint. */
static bool process_block(void)
{
    settings_acquire();
    const uint32_t threshold = threshold_q15;
    settings_release();

    sample_block_t block;
    if (!sig_capture_receive(&block)) return false;

    gpio_put(MARK_WORK, 1);
    const uint64_t start = sig_trace(SIG_WORK_START, block.job, block.id, 0);
    spectrum_result_t result;
    sig_dsp_process(block.samples, EOS_DSP_REPEATS, &result);
    const uint64_t finish = sig_trace(SIG_WORK_COMPLETE, block.job, block.id, 0);
    const uint32_t compute = (uint32_t)(finish - start);
    /* The samples have been read for the last time, so the input buffer can
     * return to the pool now. The private result does not refer to it, which is
     * why publication is a separate, later obligation. */
    sig_capture_release(block.id);
    const uint64_t release = sig_trace(SIG_BLOCK_RELEASE, block.job, block.id, 0);

    publish_spectrum(&result, threshold);
    const uint64_t published = sig_trace(SIG_RESULT_PUBLISHED, block.job, block.id, 0);
    gpio_put(MARK_WORK, 0);

    critical_section_enter_blocking(&stats_lock);
    processed++;
    const uint32_t buffer_return = (uint32_t)(release - block.ready_us);
    const uint32_t publication = (uint32_t)(published - block.ready_us);
    /* The deadline belongs to storage reuse: until this point DMA cannot use
     * the buffer again. Publication is reported separately. */
    const bool late = buffer_return > EOS_BUFFER_DEADLINE_US;
    if (late) misses++;
    if (buffer_return > largest_response) largest_response = buffer_return;
    if (publication > largest_publication) largest_publication = publication;
    if (publication > EOS_FRESHNESS_DEADLINE_US) { late_blocks++; freshness_late++; }
    if (compute > largest_compute) largest_compute = compute;
    critical_section_exit(&stats_lock);
    /* Close the trace window at the first miss, so a dump shows the events that
     * led to it instead of being overwritten by later blocks. */
    if (late) sig_trace_freeze();
    return true;
}

/* The settings service: compute the next values privately, then publish them
 * under the settings lock. Ownership is deliberately brief, because a reader
 * that needs these settings must never wait for unrelated work. */
static void write_settings(void)
{
    const uint32_t timebase = 4 + (timebase_div % 8);
    const uint32_t threshold = 800u + (timebase * 16u);

    settings_acquire();
    const uint64_t start = time_us_64();
    timebase_div = timebase;
    threshold_q15 = threshold;
    settings_release();
    sig_trace(SIG_SETTINGS_WRITE, 0, (uint32_t)(time_us_64() - start), 0);
}

/* [5] Display. Copy a consistent private snapshot of the published spectrum
 * under the lock, release the lock, then draw the snapshot. The physical
 * transfer is the display's own work and needs no shared ownership. */
static void draw_spectrum(void)
{
    gpio_put(MARK_DISPLAY, 1);
    sig_panel_set_tone(sig_signal_hz());
    const uint64_t start = sig_trace(SIG_DISPLAY_START, 0, EOS_DISPLAY_ROWS, 0);
    spectrum_result_t snapshot;
    spectrum_acquire();
    snapshot = published_spectrum;
    spectrum_release();
    const uint32_t lost = sig_capture_stats().dropped;
    sig_panel_draw(&snapshot, EOS_DISPLAY_ROWS, lost, late_blocks);
    static bool buttons_drawn = false;
    if (!buttons_drawn) {
        sig_panel_draw_buttons();
        buttons_drawn = true;
    }
    const uint64_t finish = sig_trace(SIG_DISPLAY_DONE, 0, 0, 0);
    const uint32_t elapsed = (uint32_t)(finish - start);
    gpio_put(MARK_DISPLAY, 0);
    static bool first_frame = true;
    if (!first_frame && elapsed > largest_draw) largest_draw = elapsed;
    first_frame = false;
}

/* GP14/GP15 are the station buttons: external pull-ups, active low. They step
 * the generated test tone. Polled from the existing settings task, so the task
 * set and priorities stay as the timing experiment expects. Debounced by
 * ignoring further transitions for 200 ms. */
static void buttons_poll(void)
{
    static bool previous[2];
    static uint64_t next_allowed;
    const unsigned pins[2] = {14, 15};
    const uint64_t now = time_us_64();
    for (unsigned i = 0; i < 2; ++i) {
        const bool pressed = !gpio_get(pins[i]);
        if (pressed && !previous[i] && now >= next_allowed) {
            sig_signal_step(i == 0 ? 1 : -1);
            next_allowed = now + 200000ull;
        }
        previous[i] = pressed;
    }
}

/* Print one status group and the kernel's CPU shares.
 *
 * All counters are read with interrupts masked so a group is internally
 * consistent: a value from the first line cannot be paired with a count from
 * the second line of a different group. The group is one record; keep whole
 * groups rather than splicing fields from several.
 */
static void report_status(void)
{
    const sig_capture_stats_t capture = sig_capture_stats();
    critical_section_enter_blocking(&stats_lock);
    const uint32_t done = processed, lost = misses;
    const uint32_t response = largest_response, compute = largest_compute;
    const uint32_t draw_us = largest_draw, settings_wait = largest_settings_wait;
    const uint32_t publish_wait = largest_publish_wait;
    const uint32_t publication = largest_publication;
    const uint32_t level = level_q15, hits = detections;
    const float checksum = published_spectrum.checksum;
    critical_section_exit(&stats_lock);
    printf("sig rtos=%d kernel=%s blocks=%lu processed=%lu "
           "dropped=%lu overrun=%lu buffer_late_%luus=%lu freshness_late=%lu "
           "t_ms=%llu\n",
           EOS_SIG_RTOS, EOS_KERNEL_VERSION,
           (unsigned long)capture.completed, (unsigned long)done,
           (unsigned long)capture.dropped, (unsigned long)capture.overruns,
           (unsigned long)EOS_BUFFER_DEADLINE_US, (unsigned long)lost,
           (unsigned long)freshness_late,
           (unsigned long long)(time_us_64() / 1000u));
    printf("buffer_return_max_us=%lu publication_response_max_us=%lu "
           "compute_max_us=%lu draw_max_us=%lu "
           "settings_wait_max_us=%lu publish_wait_max_us=%lu adc_hz=%lu "
           "spi_hz=%lu tone_hz=%lu rows=%u repeats=%u level=%lu detections=%lu "
           "trace=%lu/%lu/%lu "
           "checksum=%.6f\n",
           (unsigned long)response, (unsigned long)publication,
           (unsigned long)compute,
           (unsigned long)draw_us, (unsigned long)settings_wait,
           (unsigned long)publish_wait, (unsigned long)sig_adc_hz(),
           (unsigned long)sig_panel_spi_hz(),
           (unsigned long)sig_signal_hz(),
           (unsigned)EOS_DISPLAY_ROWS, (unsigned)EOS_DSP_REPEATS,
           (unsigned long)level, (unsigned long)hits,
           (unsigned long)g_sig_trace_count,
           (unsigned long)g_sig_trace_truncated,
           (unsigned long)g_sig_trace_frozen,
           (double)checksum);
#if EOS_SIG_RTOS
    printf("heap_free=%lu stack_free_words processing=%lu display=%lu "
           "settings=%lu\n",
           (unsigned long)xPortGetFreeHeapSize(),
           (unsigned long)uxTaskGetStackHighWaterMark(processing_handle),
           (unsigned long)uxTaskGetStackHighWaterMark(display_handle),
           (unsigned long)uxTaskGetStackHighWaterMark(settings_handle));
#endif
}

#if EOS_SIG_RTOS
/* [1] The completion interrupt delivers the descriptor through the FreeRTOS
 * queue created by the capture component. Processing blocks in receive, so it
 * needs no second wake-up object. */
static void processing_task(void *unused)
{
    (void)unused;
    ensure_dwt();
    g_core_processing = get_core_num();
    for (;;) {
        uint32_t c0 = DWT_CYCCNT;
        unsigned core = get_core_num();
        bool did = process_block();
        if (did) stat_end(&st_processing, c0, core);
#if EOS_SEED_SMP_BUG
        if (did) {
            ++seed_dsp_private;
            ++seed_shared;
        }
#endif
    }
}

/* Redraw every EOS_DISPLAY_PERIOD_MS. The period is a sleep after the draw, so
 * the frame rate depends on how long a redraw takes; that is the baseline
 * behaviour the display experiment is about. */
static void display_task(void *unused)
{
    (void)unused;
    g_core_display = get_core_num();
    for (;;) {
        uint32_t c0 = DWT_CYCCNT;
        unsigned core = get_core_num();
        draw_spectrum();
        stat_end(&st_display, c0, core);
#if EOS_SEED_SMP_BUG
        ++seed_display_private;
        ++seed_shared;
#endif
        vTaskDelay(pdMS_TO_TICKS(EOS_DISPLAY_PERIOD_MS));
    }
}

/* GPIO interrupt for K1/K2: record the edge time and wake the settings task.
 * The ISR does no printing and no SPI work. */
static void button_isr(uint gpio, uint32_t events)
{
    (void)gpio;
    (void)events;
    g_button_edge_us = time_us_64();
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(settings_handle, &woken);
    portYIELD_FROM_ISR(woken);
}

/* Wait for a button edge, or poll as a fallback. The latency from the GPIO edge
 * to this task is the measured ISR-to-task delay. */
static void settings_task(void *unused)
{
    (void)unused;
    g_core_settings = get_core_num();
    for (;;) {
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(EOS_UI_PERIOD_MS)) != 0) {
            uint64_t now = time_us_64();
            uint32_t latency = now > g_button_edge_us
                ? (uint32_t)(now - g_button_edge_us) : 0;
            if (latency > ui_latency_max_us) ui_latency_max_us = latency;
        }
        uint32_t c0 = DWT_CYCCNT;
        unsigned core = get_core_num();
        buttons_poll();
        write_settings();
        stat_end(&st_settings, c0, core);
    }
}

/* Poll the GT911. A fresh sample is acknowledged by the driver; -1 means "no
 * new sample", 0 means "all fingers lifted". Coordinates are portrait; map
 * them to the mirrored landscape panel and hit-test the bottom bar. */
static void touch_task(void *unused)
{
    (void)unused;
    uint32_t last_press_ms = 0;
    for (;;) {
        gt911_point_t pts[5];
        uint32_t tc0 = DWT_CYCCNT;
        unsigned tcore = get_core_num();
        int n = gt911_read(pts, 5);
        stat_end(&st_touch, tc0, tcore);
        if (n > 0) {
            uint32_t now_ms = to_ms_since_boot(get_absolute_time());
            if (now_ms - last_press_ms > 250u) {
                last_press_ms = now_ms;
                ++touch_count;
                /* Portrait 0..319 x 0..479 -> landscape, 180-degree mirror. */
                uint16_t x = (uint16_t)(479u - pts[0].y);
                uint16_t y = (uint16_t)(319u - pts[0].x);
                if (touch_count <= 10) {
                    printf("touch,raw=%u,%u,map=%u,%u\n",
                           pts[0].x, pts[0].y, x, y);
                }
                if (y >= 320 - 30) {
                    /* Glass order after the strip mirror: PREV | MODE | NEXT. */
                    if (x >= 320) {
                        sig_signal_step(-1);
                        printf("touch,zone=prev\n");
                    } else if (x >= 160) {
                        sig_signal_set_band(SIG_TONE_BAND_MID);
                        printf("touch,zone=mode\n");
                    } else {
                        sig_signal_step(1);
                        printf("touch,zone=next\n");
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* Print the status group and the kernel's CPU accounting once a second.
 * The output costs time at 115200 baud and appears in the measurements. */
static void reporting_task(void *unused)
{
    (void)unused;
    static char stats[640];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        report_status();
#if EOS_REPORT_ENABLE
        printf("l4,alarm_n=%lu,alarm_late=%lu,alarm_r_max_us=%lu,"
               "alarm_cpu_max_us=%lu,alarm_hits=%lu,cmd_n=%lu,cmd_max_us=%lu,ui_latency_max_us=%lu,"
               "fast_drop=%lu,fast_torn=%lu,seed_expected=%lu,seed_observed=%lu,"
               "core_dsp=%lu,core_display=%lu,core_settings=%lu,touch=%s,touch_n=%lu,"
               "core_alarm=%lu,core_comm=%lu\n",
               (unsigned long)alarm_count, (unsigned long)alarm_late,
               (unsigned long)alarm_r_max_us, (unsigned long)alarm_cpu_max_us,
               (unsigned long)alarm_hits,
               (unsigned long)cmd_count, (unsigned long)cmd_max_us,
               (unsigned long)ui_latency_max_us,
               (unsigned long)fast_drop, (unsigned long)fast_torn,
               (unsigned long)(seed_dsp_private + seed_display_private),
               (unsigned long)seed_shared,
               (unsigned long)g_core_processing, (unsigned long)g_core_display,
               (unsigned long)g_core_settings, g_touch_status,
               (unsigned long)touch_count,
               (unsigned long)g_core_alarm, (unsigned long)g_core_comm);
        const task_stat_t *tasks[] = { &st_processing, &st_display, &st_settings,
                                       &st_comm, &st_touch, &st_alarm };
        const char *names[] = { "processing", "display", "settings",
                                "comm", "touch", "alarm" };
        for (unsigned i = 0; i < sizeof tasks / sizeof tasks[0]; ++i) {
            const task_stat_t *t = tasks[i];
            printf("task,%s,elapsed_max_us=%lu,elapsed_avg_us=%lu,n=%lu,core=%lu\n",
                   names[i], (unsigned long)t->max_us,
                   (unsigned long)(t->count ? t->sum_us / t->count : 0),
                   (unsigned long)t->count, (unsigned long)t->core);
        }
        for (unsigned i = 0; i < sizeof tasks / sizeof tasks[0]; ++i) {
            task_stat_t *t = (task_stat_t *)tasks[i];
            t->max_us = t->sum_us = t->count = 0;
        }
        alarm_late = 0;
        alarm_r_max_us = 0;
        cmd_count = 0;
        cmd_max_us = 0;
        ui_latency_max_us = 0;
        touch_count = 0;
        alarm_cpu_max_us = 0;
        alarm_hits = 0;
        seed_dsp_private = 0;
        seed_display_private = 0;
        seed_shared = 0;
        fast_drop = 0;
        fast_torn = 0;
        vTaskGetRunTimeStats(stats);
        printf("%s", stats);
#endif
    }
}

/* Static allocation is enabled for the ready-descriptor queue, so the kernel
 * asks the application for the idle task's storage instead of using the heap. */
static StaticTask_t idle_task_control;
static StackType_t idle_task_stack[configMINIMAL_STACK_SIZE];

void vApplicationGetIdleTaskMemory(StaticTask_t **control, StackType_t **stack,
                                   configSTACK_DEPTH_TYPE *size)
{
    *control = &idle_task_control;
    *stack = idle_task_stack;
    *size = configMINIMAL_STACK_SIZE;
}

#if configNUMBER_OF_CORES > 1
static StaticTask_t passive_idle_task_control;
static StackType_t passive_idle_task_stack[configMINIMAL_STACK_SIZE];

void vApplicationGetPassiveIdleTaskMemory(StaticTask_t **control,
                                          StackType_t **stack,
                                          configSTACK_DEPTH_TYPE *size,
                                          BaseType_t index)
{
    (void)index;
    *control = &passive_idle_task_control;
    *stack = passive_idle_task_stack;
    *size = configMINIMAL_STACK_SIZE;
}
#endif

/* FreeRTOS hooks: stop with the reason in g_fault rather than continue with a
 * broken heap or stack. A debugger can read g_fault after the target halts. */
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task; (void)name;
    g_fault = 1;
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}

void vApplicationMallocFailedHook(void)
{
    g_fault = 2;
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}
#endif

#if EOS_SIG_RTOS
/* Alarm task: independent fast path. It consumes the short window copied by
 * the processing task and computes a level check. Its response is measured
 * from the DMA block-ready timestamp, so queueing and preemption are included. */
static void alarm_task(void *unused)
{
    (void)unused;
    ensure_dwt();
    g_core_alarm = get_core_num();
    for (;;) {
        fast_block_t fb;
#if EOS_FAST_RING
        if (!fast_ring_pop(&fb)) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
#else
        if (xQueueReceive(fast_queue, &fb, portMAX_DELAY) != pdPASS) continue;
#endif
        if (fb.check != fast_check(&fb)) ++fast_torn;
#if EOS_RING_DELAY_US > 0
        busy_wait_us_32(EOS_RING_DELAY_US);   /* slow-consumer experiment */
#endif
        uint32_t c0 = DWT_CYCCNT;
        unsigned core = get_core_num();
        int64_t sum = 0;
        for (unsigned i = 0; i < FAST_SAMPLES; ++i) {
            const int32_t v = fb.samples[i] - 2048;
            sum += (int64_t)v * v;
        }
        const uint32_t rms = (uint32_t)sqrtf((float)sum / FAST_SAMPLES);
        stat_end(&st_alarm, c0, core);
        uint64_t now = time_us_64();
        uint32_t response = now > fb.ready_us ? (uint32_t)(now - fb.ready_us) : 0;
        if (response > alarm_r_max_us) alarm_r_max_us = response;
        if (response > EOS_ALARM_DEADLINE_US) ++alarm_late;
        const bool alarm = rms > 1500u;
        gpio_put(EOS_ALARM_PIN, alarm ? 1 : 0);
        if (alarm) ++alarm_hits;
        ++alarm_count;
    }
}

/* Command task: one query or a burst of ten, over UART for now. */
static void comm_task(void *unused)
{
    (void)unused;
    g_core_comm = get_core_num();
    for (;;) {
        int key = getchar_timeout_us(0);
        if (key == 'q' || key == 'b') {
            uint32_t c0 = DWT_CYCCNT;
            unsigned core = get_core_num();
            uint32_t t0 = (uint32_t)time_us_64();
            int count = key == 'b' ? 10 : 1;
            for (int i = 0; i < count; ++i) {
                spectrum_acquire();
                uint32_t level = level_q15;
                spectrum_release();
                printf("cmd,type=%c,seq=%d,level_q15=%u,core=%u\n",
                       key, i, level, (unsigned)get_core_num());
            }
            uint32_t dt = (uint32_t)time_us_64() - t0;
            if (dt > cmd_max_us) cmd_max_us = dt;
            ++cmd_count;
            stat_end(&st_comm, c0, core);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
#endif

/* DMA-interrupt hook: copy the short window and wake the fast task. */
static void fast_hook(const int32_t *samples, uint32_t job, uint64_t ready_us)
{
    fast_block_t fb;
    for (unsigned i = 0; i < FAST_SAMPLES; ++i) fb.samples[i] = samples[i];
    fb.job = job;
    fb.ready_us = ready_us;
    fb.check = fast_check(&fb);
#if EOS_FAST_RING
    fast_ring_push(&fb);
#else
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(fast_queue, &fb, &woken) != pdPASS) ++fast_drop;
    portYIELD_FROM_ISR(woken);
#endif
}

#if !EOS_SCHED_MODE
int main(void)
{
    set_sys_clock_khz(EOS_TARGET_KHZ, true);
    stdio_init_all();
    cycle_counter_init();
    sig_trace_init();
    critical_section_init(&stats_lock);
    if (gt911_init()) {
        snprintf(g_touch_status, sizeof g_touch_status, "gt911");
    } else {
        snprintf(g_touch_status, sizeof g_touch_status, "absent");
    }
    const unsigned pins[] = { MARK_HEARTBEAT, MARK_DISPLAY, SIG_ISR_MARK_PIN,
                              MARK_WORK };
    for (unsigned i = 0; i < sizeof pins / sizeof pins[0]; ++i) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_OUT);
        gpio_put(pins[i], 0);
    }
    gpio_init(EOS_ALARM_PIN);
    gpio_set_dir(EOS_ALARM_PIN, GPIO_OUT);
    gpio_put(EOS_ALARM_PIN, 0);
    const unsigned button_pins[] = {14, 15};
    for (unsigned i = 0; i < sizeof button_pins / sizeof button_pins[0]; ++i) {
        gpio_init(button_pins[i]);
        gpio_set_dir(button_pins[i], GPIO_IN);
    }
    sig_dsp_init();
    sig_panel_init();
    sig_signal_start();
#if EOS_SIG_RTOS
    fast_queue = xQueueCreate(FAST_QUEUE_LEN, sizeof(fast_block_t));
    require_success(fast_queue ? pdPASS : pdFAIL);
    spectrum_mutex = xSemaphoreCreateMutex();
    require_success(spectrum_mutex ? pdPASS : pdFAIL);
    /* A mutex, because settings have an owner that a reader may need to wait
     * for; inheritance is what bounds that wait. */
    settings_mutex = xSemaphoreCreateMutex();
    require_success(settings_mutex ? pdPASS : pdFAIL);
    /* [6] Priorities: processing 3, display 2, settings 1. A higher priority
     * means more urgent work, not more CPU: a task runs only when it is
     * runnable. */
    /* The marker pins are shared with the analyzer; the switch hook writes the
     * running task's number to them. */
    const unsigned marker_pins[] = { 16, 19, 20 };
    for (unsigned i = 0; i < sizeof marker_pins / sizeof marker_pins[0]; ++i) {
        gpio_init(marker_pins[i]);
        gpio_set_dir(marker_pins[i], GPIO_OUT);
    }
    task_marker_publish(TASK_ID_IDLE);

#if EOS_CORES == 2 && EOS_PLACEMENT == 2
    const UBaseType_t mask0 = (UBaseType_t)(1u << 0);
    const UBaseType_t mask1 = (UBaseType_t)(1u << 1);
#else
    const UBaseType_t mask0 = (UBaseType_t)3u;
    const UBaseType_t mask1 = (UBaseType_t)3u;
#endif
#if EOS_CORES == 2
    require_success(xTaskCreateAffinitySet(processing_task, "processing", 2048, NULL, 3, mask0, &processing_handle));
    require_success(xTaskCreateAffinitySet(display_task, "display", 1024, NULL, 2, mask1, &display_handle));
    require_success(xTaskCreateAffinitySet(settings_task, "settings", 512, NULL, 1, mask0, &settings_handle));
    require_success(xTaskCreateAffinitySet(reporting_task, "reporting", 1024, NULL, 1, mask0, &reporting_handle));
    require_success(xTaskCreateAffinitySet(alarm_task, "alarm", 512, NULL, EOS_ALARM_PRIORITY, mask1, &alarm_handle));
    require_success(xTaskCreateAffinitySet(comm_task, "comm", 768, NULL, 1, mask1, &comm_handle));
    require_success(xTaskCreateAffinitySet(touch_task, "touch", 512, NULL, 2, mask1, &touch_handle));
#else
    (void)mask0;
    (void)mask1;
    require_success(xTaskCreate(processing_task, "processing", 2048, NULL, 3,
                                &processing_handle));
    require_success(xTaskCreate(display_task, "display", 1024, NULL, 2,
                                &display_handle));
    require_success(xTaskCreate(settings_task, "settings", 512, NULL, 1,
                                &settings_handle));
    require_success(xTaskCreate(reporting_task, "reporting", 1024, NULL, 1,
                                &reporting_handle));
    require_success(xTaskCreate(alarm_task, "alarm", 512, NULL, EOS_ALARM_PRIORITY,
                                &alarm_handle));
    require_success(xTaskCreate(comm_task, "comm", 768, NULL, 1, &comm_handle));
    require_success(xTaskCreate(touch_task, "touch", 512, NULL, 2, &touch_handle));
#endif

    /* Numbers identify a task on the marker pins and in the status report; the
     * idle task keeps number 0. */
    vTaskSetTaskNumber(processing_handle, TASK_ID_DSP);
    vTaskSetTaskNumber(display_handle, TASK_ID_DISPLAY);
    vTaskSetTaskNumber(settings_handle, TASK_ID_UI);
    vTaskSetTaskNumber(reporting_handle, TASK_ID_REPORT);
    vTaskSetTaskNumber(alarm_handle, TASK_ID_CONTROLLER);
    vTaskSetTaskNumber(comm_handle, 6);
    vTaskSetTaskNumber(touch_handle, 7);
    printf("lab04-condition cores=%d placement=%d target_khz=%d alarm_period_ms=%d alarm_deadline_us=%d alarm_prio=%d\n",
           EOS_CORES, EOS_PLACEMENT, EOS_TARGET_KHZ, EOS_ALARM_PERIOD_MS,
           EOS_ALARM_DEADLINE_US, EOS_ALARM_PRIORITY);
    gpio_set_irq_enabled_with_callback(14, GPIO_IRQ_EDGE_FALL, true, button_isr);
    gpio_set_irq_enabled(15, GPIO_IRQ_EDGE_FALL, true);
    sig_capture_fast_hook = fast_hook;
    sig_capture_start();
    vTaskStartScheduler();
    g_fault = 3;
    for (;;) {}
#else
    sig_capture_start();
    uint64_t display_due = time_us_64() + EOS_DISPLAY_PERIOD_MS * 1000ull;
    uint64_t ui_due = time_us_64() + EOS_UI_PERIOD_MS * 1000ull;
    uint64_t report_due = time_us_64() + 1000000ull;
    uint64_t heartbeat_due = time_us_64() + 500000ull;
    bool heartbeat = false;
    for (;;) {
        if (!process_block()) tight_loop_contents();
        const uint64_t now = time_us_64();
        if (now >= display_due) {
            draw_spectrum();
            display_due = time_us_64() + EOS_DISPLAY_PERIOD_MS * 1000ull;
        }
        if (now >= ui_due) {
            buttons_poll();
            write_settings();
            ui_due = time_us_64() + EOS_UI_PERIOD_MS * 1000ull;
        }
        if (now >= report_due) {
            report_status();
            report_due = time_us_64() + 1000000ull;
        }
        if (now >= heartbeat_due) {
            heartbeat = !heartbeat;
            gpio_put(MARK_HEARTBEAT, heartbeat);
            heartbeat_due = time_us_64() + 500000ull;
        }
    }
#endif
}

#endif /* !EOS_SCHED_MODE */

#if EOS_SCHED_MODE
int main(void)
{
    return sched_mode_main();
}
#endif
