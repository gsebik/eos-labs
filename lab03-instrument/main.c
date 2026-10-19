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

static sig_result_t latest;
static uint32_t threshold_q15 = 800, timebase_div = 4;
static uint32_t level_q15, detections;
static uint32_t processed, misses, largest_response, largest_compute;
static uint32_t largest_draw, largest_settings_wait, largest_publish_wait;
static uint32_t largest_publication;
static volatile uint32_t g_fault;
#if EOS_SIG_RTOS
static TaskHandle_t dsp_handle, display_handle, ui_handle, report_handle;
static SemaphoreHandle_t settings_lock, result_lock;
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

/* [4] Settings lock. The UI writes the settings under it; the DSP reads them
 * once per block. The display never uses it, so display work cannot extend the
 * UI's critical section unless the scheduler lets the display pre-empt the UI. */
static void settings_take(void)
{
#if EOS_SIG_RTOS
    const uint64_t start = time_us_64();
    require_success(xSemaphoreTake(settings_lock, portMAX_DELAY));
    const uint32_t waited = (uint32_t)(time_us_64() - start);
    if (waited > largest_settings_wait) largest_settings_wait = waited;
    if (waited > 10) sig_trace(SIG_SETTINGS_WAIT, 0, waited, 1);
#endif
}

static void settings_give(void)
{
#if EOS_SIG_RTOS
    require_success(xSemaphoreGive(settings_lock));
#endif
}

/* The result lock protects the published spectrum. It is deliberately separate
 * from the settings lock: an overlong display critical section must not be
 * confused with a settings-lock inversion. */
static void result_take(void)
{
#if EOS_SIG_RTOS
    require_success(xSemaphoreTake(result_lock, portMAX_DELAY));
#endif
}

static void result_give(void)
{
#if EOS_SIG_RTOS
    require_success(xSemaphoreGive(result_lock));
#endif
}

/* [3] Publication. The DSP writes the result under the result lock, so a
 * display that holds that lock across its whole redraw delays this step while
 * the DSP still owns its pool buffer. */
static void publish(const sig_result_t *result, uint32_t threshold)
{
    const uint64_t start = time_us_64();
    result_take();
    const uint32_t waited = (uint32_t)(time_us_64() - start);
    if (waited > largest_publish_wait) largest_publish_wait = waited;
    latest = *result;
    level_q15 = (uint32_t)(result->rms * 32768.0f);
    if (level_q15 > threshold) detections++;
    result_give();
}

/* [2] DSP path: read settings, take a READY block, process it, publish, then
 * release. The buffer is held from take until release, so a delayed publication
 * also delays the release. */
static bool process_one(void)
{
    settings_take();
    const uint32_t threshold = threshold_q15;
    settings_give();

    sig_block_t block;
    if (!sig_capture_receive(&block)) return false;
    gpio_put(MARK_WORK, 1);
    const uint64_t start = sig_trace(SIG_WORK_START, block.job, block.id, 0);
    sig_result_t result;
    sig_dsp_process(block.samples, EOS_DSP_REPEATS, &result);
    const uint64_t finish = sig_trace(SIG_WORK_COMPLETE, block.job, block.id, 0);
    const uint32_t compute = (uint32_t)(finish - start);
    /* The samples have been read for the last time, so the input buffer can
     * return to the pool now. The private result does not refer to it, which is
     * why publication is a separate, later obligation. */
    sig_capture_release(block.id);
    const uint64_t release = sig_trace(SIG_BLOCK_RELEASE, block.job, block.id, 0);

    publish(&result, threshold);
    const uint64_t published = sig_trace(SIG_RESULT_PUBLISHED, block.job, block.id, 0);
    gpio_put(MARK_WORK, 0);

    const uint32_t saved = save_and_disable_interrupts();
    processed++;
    const uint32_t buffer_return = (uint32_t)(release - block.ready_us);
    const uint32_t publication = (uint32_t)(published - block.ready_us);
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
 * under the settings mutex. Ownership is deliberately brief, because a reader
 * that needs these settings must never wait for unrelated work. */
static void write_settings(void)
{
    const uint32_t timebase = 4 + (timebase_div % 8);
    const uint32_t threshold = 800u + (timebase * 16u);

    settings_take();
    const uint64_t start = time_us_64();
    timebase_div = timebase;
    threshold_q15 = threshold;
    settings_give();
    sig_trace(SIG_SETTINGS_WRITE, 0, (uint32_t)(time_us_64() - start), 0);
}

/* [5] Display. Both paths perform the same physical redraw. They differ only in
 * how long the shared spectrum is owned:
 *
 *   OFF (baseline) copies a consistent snapshot, releases the mutex, then draws.
 *   ON  (comparison) holds the mutex across the redraw as well.
 *
 * The comparison exists to make ownership duration visible in the measurements.
 * It is a controlled variation, not a defect waiting to be repaired. */
static void draw(void)
{
    gpio_put(MARK_DISPLAY, 1);
    sig_panel_set_tone(sig_signal_hz());
    const uint64_t start = sig_trace(SIG_DISPLAY_START, 0, EOS_DISPLAY_ROWS, 0);
    sig_result_t snapshot;
#if EOS_LONG_RESULT_LOCK
    /* Comparison: shared ownership lasts as long as the physical transfer. */
    result_take();
    snapshot = latest;
    sig_panel_draw(&snapshot, EOS_DISPLAY_ROWS);
    result_give();
#else
    /* Baseline: the snapshot is consistent, and the transfer needs no lock. */
    result_take();
    snapshot = latest;
    result_give();
    sig_panel_draw(&snapshot, EOS_DISPLAY_ROWS);
#endif
    const uint64_t finish = sig_trace(SIG_DISPLAY_DONE, 0, 0, 0);
    const uint32_t elapsed = (uint32_t)(finish - start);
    gpio_put(MARK_DISPLAY, 0);
    if (elapsed > largest_draw) largest_draw = elapsed;
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

static void report(void)
{
    const sig_capture_stats_t capture = sig_capture_stats();
    const uint32_t saved = save_and_disable_interrupts();
    const uint32_t done = processed, lost = misses;
    const uint32_t response = largest_response, compute = largest_compute;
    const uint32_t draw = largest_draw, settings_wait = largest_settings_wait;
    const uint32_t publish = largest_publish_wait;
    const uint32_t publication = largest_publication;
    const uint32_t level = level_q15, hits = detections;
    const float checksum = latest.checksum;
    restore_interrupts(saved);
    printf("sig rtos=%d mutex=%d long_lock=%d blocks=%lu processed=%lu "
           "dropped=%lu overrun=%lu misses_%luus=%lu\n",
           EOS_SIG_RTOS, EOS_SIG_SETTINGS_MUTEX, EOS_LONG_RESULT_LOCK,
           (unsigned long)capture.completed, (unsigned long)done,
           (unsigned long)capture.dropped, (unsigned long)capture.overruns,
           (unsigned long)EOS_DEADLINE_US, (unsigned long)lost);
    printf("buffer_return_max_us=%lu publication_response_max_us=%lu "
           "compute_max_us=%lu draw_max_us=%lu "
           "settings_wait_max_us=%lu publish_wait_max_us=%lu adc_hz=%lu "
           "spi_hz=%lu tone_hz=%lu rows=%u repeats=%u level=%lu detections=%lu "
           "trace=%lu/%lu/%lu "
           "checksum=%.6f\n",
           (unsigned long)response, (unsigned long)publication,
           (unsigned long)compute,
           (unsigned long)draw, (unsigned long)settings_wait,
           (unsigned long)publish, (unsigned long)sig_adc_hz(),
           (unsigned long)sig_panel_spi_hz(),
           (unsigned long)sig_signal_hz(),
           (unsigned)EOS_DISPLAY_ROWS, (unsigned)EOS_DSP_REPEATS,
           (unsigned long)level, (unsigned long)hits,
           (unsigned long)g_sig_trace_count,
           (unsigned long)g_sig_trace_truncated,
           (unsigned long)g_sig_trace_frozen,
           (double)checksum);
#if EOS_SIG_RTOS
    printf("heap_free=%lu stack_free_words dsp=%lu display=%lu ui=%lu\n",
           (unsigned long)xPortGetFreeHeapSize(),
           (unsigned long)uxTaskGetStackHighWaterMark(dsp_handle),
           (unsigned long)uxTaskGetStackHighWaterMark(display_handle),
           (unsigned long)uxTaskGetStackHighWaterMark(ui_handle));
#endif
}

#if EOS_SIG_RTOS
/* [1] The completion interrupt delivers the descriptor through the FreeRTOS
 * queue created by the capture component. Processing blocks in receive, so it
 * needs no second wake-up object. */
static void dsp_task(void *unused)
{
    (void)unused;
    for (;;) {
        while (process_one()) {}
    }
}

static void display_task(void *unused)
{
    (void)unused;
    for (;;) {
        draw();
        vTaskDelay(pdMS_TO_TICKS(EOS_DISPLAY_PERIOD_MS));
    }
}

static void ui_task(void *unused)
{
    (void)unused;
    TickType_t release = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&release, pdMS_TO_TICKS(EOS_UI_PERIOD_MS));
        buttons_poll();
        write_settings();
    }
}

static void report_task(void *unused)
{
    (void)unused;
    static char stats[640];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        report();
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
    result_lock = xSemaphoreCreateMutex();
    require_success(result_lock ? pdPASS : pdFAIL);
    /* A mutex, because settings have an owner that a reader may need to wait
     * for; inheritance is what bounds that wait. */
    settings_lock = xSemaphoreCreateMutex();
    require_success(settings_lock ? pdPASS : pdFAIL);
    /* [6] Priorities. DSP is highest, display medium, UI lowest: the classic
     * three-task inversion layout for the settings lock. */
    /* The marker pins are shared with the analyzer; the switch hook writes the
     * running task's number to them. */
    const unsigned marker_pins[] = { 16, 19, 20 };
    for (unsigned i = 0; i < sizeof marker_pins / sizeof marker_pins[0]; ++i) {
        gpio_init(marker_pins[i]);
        gpio_set_dir(marker_pins[i], GPIO_OUT);
    }
    task_marker_publish(TASK_ID_IDLE);

    require_success(xTaskCreate(dsp_task, "dsp", 2048, NULL, 3, &dsp_handle));
    require_success(xTaskCreate(display_task, "display", 1024, NULL, 2,
                                &display_handle));
    require_success(xTaskCreate(ui_task, "ui", 512, NULL, 1, &ui_handle));
    require_success(xTaskCreate(report_task, "report", 1024, NULL, 1,
                                &report_handle));

    /* Numbers are what the marker pins and the report use to identify a task;
     * the idle task keeps number 0. */
    vTaskSetTaskNumber(dsp_handle, TASK_ID_DSP);
    vTaskSetTaskNumber(display_handle, TASK_ID_DISPLAY);
    vTaskSetTaskNumber(ui_handle, TASK_ID_UI);
    vTaskSetTaskNumber(report_handle, TASK_ID_REPORT);
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
        if (!process_one()) tight_loop_contents();
        const uint64_t now = time_us_64();
        if (now >= display_due) {
            draw();
            display_due = time_us_64() + EOS_DISPLAY_PERIOD_MS * 1000ull;
        }
        if (now >= ui_due) {
            buttons_poll();
            write_settings();
            ui_due = time_us_64() + EOS_UI_PERIOD_MS * 1000ull;
        }
        if (now >= report_due) {
            report();
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
