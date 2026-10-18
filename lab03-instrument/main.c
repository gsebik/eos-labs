#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "capture.h"
#include "panel.h"
#include "trace.h"
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
static volatile uint32_t g_fault;
#if EOS_SIG_RTOS
static TaskHandle_t dsp_handle, display_handle, ui_handle;
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
    if (!sig_capture_take(&block)) return false;
    gpio_put(MARK_WORK, 1);
    const uint64_t start = sig_trace(SIG_WORK_START, block.job, block.id, 0);
    sig_result_t result;
    sig_dsp_process(block.samples, EOS_DSP_REPEATS, &result);
    const uint64_t finish = sig_trace(SIG_WORK_COMPLETE, block.job, block.id, 0);
    const uint32_t compute = (uint32_t)(finish - start);
    publish(&result, threshold);
    sig_capture_release(block.id);
    const uint64_t release = sig_trace(SIG_BLOCK_RELEASE, block.job, block.id, 0);
    gpio_put(MARK_WORK, 0);

    const uint32_t saved = save_and_disable_interrupts();
    processed++;
    const uint32_t response = (uint32_t)(release - block.ready_us);
    const bool late = response > EOS_DEADLINE_US;
    if (late) misses++;
    if (response > largest_response) largest_response = response;
    if (compute > largest_compute) largest_compute = compute;
    restore_interrupts(saved);
    /* Close the trace window at the first miss, so a dump shows the events that
     * led to it instead of being overwritten by later blocks. */
    if (late) sig_trace_freeze();
    return true;
}

/* TODO (2), Task 3: retain the configured preparation workload and a consistent
 * settings update, but bound the work that readers must wait for. Diagnose the
 * longer-write run before editing this function. Do not delete the workload,
 * bypass settings_take/give, or change the threshold merely to pass a test. */
static void write_settings(void)
{
    settings_take();
    const uint64_t start = time_us_64();
    timebase_div = 4 + (timebase_div % 8);
    threshold_q15 = 800u + (timebase_div * 16u);
    volatile uint32_t spin = 0;
    for (uint32_t i = 0; i < EOS_SETTINGS_ITERS; ++i) spin += i;
    settings_give();
    sig_trace(SIG_SETTINGS_WRITE, 0, (uint32_t)(time_us_64() - start), 0);
}

/* [5] Display. ON preserves the supplied faulty comparison. OFF selects the
 * student implementation below, which initially has the SAME fault. The flag
 * selects a source path; it does not certify that the repair is correct. */
static void draw(void)
{
    gpio_put(MARK_DISPLAY, 1);
    sig_panel_set_tone(sig_signal_hz());
    const uint64_t start = sig_trace(SIG_DISPLAY_START, 0, EOS_DISPLAY_ROWS, 0);
    sig_result_t snapshot;
#if EOS_LONG_RESULT_LOCK
    /* The renderer owns the result lock for its whole redraw. This is the
     * deliberately overlong critical section: it delays the DSP's publication. */
    result_take();
    snapshot = latest;
    sig_panel_draw(&snapshot, EOS_DISPLAY_ROWS);
    result_give();
#else
    /* TODO (1), Task 1: display a consistent result without making publication
     * wait for the physical redraw. Keep the same snapshot, renderer, workload
     * and measurement boundaries. This compiles, but is not repaired yet. */
    result_take();
    snapshot = latest;
    sig_panel_draw(&snapshot, EOS_DISPLAY_ROWS);
    result_give();
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
    const uint32_t level = level_q15, hits = detections;
    const float checksum = latest.checksum;
    restore_interrupts(saved);
    printf("sig rtos=%d mutex=%d long_lock=%d blocks=%lu processed=%lu "
           "dropped=%lu overrun=%lu misses_%luus=%lu\n",
           EOS_SIG_RTOS, EOS_SIG_SETTINGS_MUTEX, EOS_LONG_RESULT_LOCK,
           (unsigned long)capture.completed, (unsigned long)done,
           (unsigned long)capture.dropped, (unsigned long)capture.overruns,
           (unsigned long)EOS_DEADLINE_US, (unsigned long)lost);
    printf("response_max_us=%lu compute_max_us=%lu draw_max_us=%lu "
           "settings_wait_max_us=%lu publish_wait_max_us=%lu adc_hz=%lu "
           "spi_hz=%lu tone_hz=%lu rows=%u repeats=%u level=%lu detections=%lu "
           "trace=%lu/%lu/%lu "
           "checksum=%.6f\n",
           (unsigned long)response, (unsigned long)compute,
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
/* [1] DMA completion. The interrupt hands a READY block to the DSP task; it
 * never takes a lock and never processes samples. */
static void ready_isr(void)
{
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(dsp_handle, &woken);
    portYIELD_FROM_ISR(woken);
}

static void dsp_task(void *unused)
{
    (void)unused;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
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
    settings_lock = EOS_SIG_SETTINGS_MUTEX
        ? xSemaphoreCreateMutex() : xSemaphoreCreateBinary();
    require_success(settings_lock ? pdPASS : pdFAIL);
    if (!EOS_SIG_SETTINGS_MUTEX) require_success(xSemaphoreGive(settings_lock));
    /* [6] Priorities. DSP is highest, display medium, UI lowest: the classic
     * three-task inversion layout for the settings lock. */
    require_success(xTaskCreate(dsp_task, "dsp", 2048, NULL, 3, &dsp_handle));
    require_success(xTaskCreate(display_task, "display", 1024, NULL, 2,
                                &display_handle));
    require_success(xTaskCreate(ui_task, "ui", 512, NULL, 1, &ui_handle));
    require_success(xTaskCreate(report_task, "report", 1024, NULL, 1, NULL));
    sig_capture_start(ready_isr);
    vTaskStartScheduler();
    g_fault = 3;
    for (;;) {}
#else
    sig_capture_start(NULL);
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
