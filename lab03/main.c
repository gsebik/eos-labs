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
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "capture.h"
#include "panel.h"
#include "trace.h"
#include "task_marker.h"
#if EOS_SIG_RTOS
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#endif

/* [0] Budget. One round is SIG_BLOCK ADC samples at SIG_RATE_HZ: 5.12 ms. The
 * pool holds SIG_POOL buffers, so a block that is still PROCESSING when DMA
 * needs a slot causes genuine loss: the DMA writes to discard storage and the
 * block is counted as dropped. The safe deadline therefore follows from the
 * buffer-ownership states, not from a fixed formula. */
enum { MARK_HEARTBEAT = 16, MARK_DISPLAY = 17, MARK_WORK = 21 };

#if EOS_SIG_RTOS
/* FreeRTOS run-time stats counter: the SDK's time_us_32 is a static inline, so
 * FreeRTOSConfig.h declares this helper instead. */
uint32_t eos_runtime_counter(void) { return time_us_32(); }
#endif

static spectrum_result_t published_spectrum;
static uint32_t threshold_q15 = 800, timebase_div = 4;
static uint32_t level_q15, detections;
static uint32_t processed, misses, largest_response, largest_compute;
static uint32_t largest_draw, largest_settings_wait, largest_publish_wait;
static uint32_t largest_publication;
static volatile uint32_t g_fault;

static uint32_t late_blocks;                /* publication later than one period */

/* Sweep state. The tone walks one band per step and reverses at the ends, so
 * the picture moves one bar at a time without any input from the user. */
static bool sweep_running = true;
static int sweep_direction = 1;
static unsigned sweep_ticks;
enum { SWEEP_TICKS_PER_STEP = 8 };          /* 8 x EOS_UI_PERIOD_MS = 160 ms */

#if EOS_SIG_RTOS
static TaskHandle_t processing_handle, display_handle, settings_handle,
    reporting_handle;
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

    const uint32_t saved = save_and_disable_interrupts();
    processed++;
    const uint32_t buffer_return = (uint32_t)(release - block.ready_us);
    const uint32_t publication = (uint32_t)(published - block.ready_us);
    /* The instrument promises a result within one block period. Missing that
     * promise loses nothing, so it is counted separately from dropped input. */
    if (publication > SIG_BLOCK_PERIOD_US) late_blocks++;
    /* The deadline belongs to storage reuse: until this point DMA cannot use
     * the buffer again. Publication is reported separately. */
    const bool late = buffer_return > EOS_DEADLINE_US;
    if (late) misses++;
    if (buffer_return > largest_response) largest_response = buffer_return;
    if (publication > largest_publication) largest_publication = publication;
    if (compute > largest_compute) largest_compute = compute;
    restore_interrupts(saved);
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
    const uint32_t saved = save_and_disable_interrupts();
    const uint32_t lost = sig_capture_stats().dropped;
    const uint32_t late = late_blocks;
    restore_interrupts(saved);
    sig_panel_draw(&snapshot, EOS_DISPLAY_ROWS, lost, late);
    const uint64_t finish = sig_trace(SIG_DISPLAY_DONE, 0, 0, 0);
    const uint32_t elapsed = (uint32_t)(finish - start);
    gpio_put(MARK_DISPLAY, 0);
    if (elapsed > largest_draw) largest_draw = elapsed;
}

/* GP14/GP15 are the station buttons: external pull-ups, active low.
 *
 * GP14 stops or restarts the sweep. GP15 steps one band by hand, which also
 * stops the sweep so the chosen band stays put. Polled from the settings task,
 * so the task set and priorities stay as the timing experiment expects.
 * Debounced by ignoring further transitions for 200 ms.
 */
static void buttons_poll(void)
{
    static bool previous[2];
    static uint64_t next_allowed;
    const unsigned pins[2] = {14, 15};
    const uint64_t now = time_us_64();
    for (unsigned i = 0; i < 2; ++i) {
        const bool pressed = !gpio_get(pins[i]);
        if (pressed && !previous[i] && now >= next_allowed) {
            if (i == 0) {
                sweep_running = !sweep_running;
            } else {
                sweep_running = false;
                sig_signal_step(1);
            }
            next_allowed = now + 200000ull;
        }
        previous[i] = pressed;
    }
}

/* Advance the sweep by one band when it is due. Called from the settings task,
 * so the moving tone costs nothing in the processing path. */
static void sweep_poll(void)
{
    if (!sweep_running) return;
    if (++sweep_ticks < SWEEP_TICKS_PER_STEP) return;
    sweep_ticks = 0;
    const int next = (int)sig_signal_band() + sweep_direction;
    if (next > SIG_TONE_BAND_MAX || next < SIG_TONE_BAND_MIN) sweep_direction = -sweep_direction;
    sig_signal_step(sweep_direction);
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
    const uint32_t saved = save_and_disable_interrupts();
    const uint32_t done = processed, lost = misses;
    const uint32_t response = largest_response, compute = largest_compute;
    const uint32_t draw_us = largest_draw, settings_wait = largest_settings_wait;
    const uint32_t publish_wait = largest_publish_wait;
    const uint32_t publication = largest_publication;
    const uint32_t late_count = late_blocks;
    const uint32_t level = level_q15, hits = detections;
    const float checksum = published_spectrum.checksum;
    restore_interrupts(saved);
    printf("sig rtos=%d kernel=%s blocks=%lu processed=%lu "
           "dropped=%lu late=%lu overrun=%lu misses_%luus=%lu\n",
           EOS_SIG_RTOS, EOS_KERNEL_VERSION,
           (unsigned long)capture.completed, (unsigned long)done,
           (unsigned long)capture.dropped, (unsigned long)late_count,
           (unsigned long)capture.overruns,
           (unsigned long)EOS_DEADLINE_US, (unsigned long)lost);
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
    for (;;) {
        while (process_block()) {}
    }
}

/* Redraw every EOS_DISPLAY_PERIOD_MS. The period is a sleep after the draw, so
 * the frame rate depends on how long a redraw takes; that is the baseline
 * behaviour the display experiment is about. */
static void display_task(void *unused)
{
    (void)unused;
    for (;;) {
        draw_spectrum();
        vTaskDelay(pdMS_TO_TICKS(EOS_DISPLAY_PERIOD_MS));
    }
}

/* Poll the buttons, then republish the settings, once per EOS_UI_PERIOD_MS.
 * Both operations are short, which is why this lock has no long owner. */
static void settings_task(void *unused)
{
    (void)unused;
    TickType_t release = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&release, pdMS_TO_TICKS(EOS_UI_PERIOD_MS));
        buttons_poll();
        sweep_poll();
        write_settings();
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
        vTaskGetRunTimeStats(stats);
        printf("%s", stats);
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

int main(void)
{
    stdio_init_all();
    const unsigned pins[] = { MARK_HEARTBEAT, MARK_DISPLAY, SIG_ISR_MARK_PIN,
                              MARK_WORK };
    for (unsigned i = 0; i < sizeof pins / sizeof pins[0]; ++i) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_OUT);
        gpio_put(pins[i], 0);
    }
    const unsigned button_pins[] = {14, 15};
    for (unsigned i = 0; i < sizeof button_pins / sizeof button_pins[0]; ++i) {
        gpio_init(button_pins[i]);
        gpio_set_dir(button_pins[i], GPIO_IN);
    }
    sig_dsp_init();
    sig_panel_init();
    sig_signal_start();
#if EOS_SIG_RTOS
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

    require_success(xTaskCreate(processing_task, "processing", 2048, NULL, 3,
                                &processing_handle));
    require_success(xTaskCreate(display_task, "display", 1024, NULL, 2,
                                &display_handle));
    require_success(xTaskCreate(settings_task, "settings", 512, NULL, 1,
                                &settings_handle));
    require_success(xTaskCreate(reporting_task, "reporting", 1024, NULL, 1,
                                &reporting_handle));

    /* Numbers identify a task on the marker pins and in the status report; the
     * idle task keeps number 0. */
    vTaskSetTaskNumber(processing_handle, TASK_ID_DSP);
    vTaskSetTaskNumber(display_handle, TASK_ID_DISPLAY);
    vTaskSetTaskNumber(settings_handle, TASK_ID_UI);
    vTaskSetTaskNumber(reporting_handle, TASK_ID_REPORT);
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
