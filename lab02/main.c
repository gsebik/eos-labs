#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "dwt.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/* L1's EP-0172 mapping; do not reuse unchanged on another carrier. */
enum { LED_A = 16, LED_B = 17, PROBE_A = 18, PROBE_B = 19 };
TaskHandle_t g_tasks[2];
volatile uint32_t g_fault;
volatile uint32_t g_phase[2];
volatile uint32_t g_rounds[2];

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    (void)name;
    g_fault = 1;
    taskDISABLE_INTERRUPTS();
    for (;;) {
    }
}

void vApplicationMallocFailedHook(void)
{
    g_fault = 2;
    taskDISABLE_INTERRUPTS();
    for (;;) {
    }
}

#if EOS_LAB_MODE == 1
typedef struct {
    uint32_t sequence;
    uint32_t value;
    uint32_t created_us;
    uint32_t created_cycles;
    uint32_t checksum;
} message_t;
_Static_assert(sizeof(message_t) == 20, "This lab records five 32-bit fields");

QueueHandle_t g_samples;
volatile uint32_t g_attempted, g_sent, g_dropped, g_received;
volatile uint32_t g_bad_items, g_sequence_gaps, g_age_last_us;
volatile uint32_t g_age_last_cycles, g_age_min_cycles = UINT32_MAX;
volatile uint32_t g_age_max_cycles, g_first_drop_us, g_clock_hz;
volatile uint32_t g_dwt_running, g_dwt_read_span;
static uint32_t run_start_us;

static void accept_item(const message_t *item)
{
    static bool have_previous;
    static uint32_t previous;
    const uint32_t received_cycles = eos_dwt_cycles();
    const uint32_t received_us = time_us_32();
    gpio_xor_mask((1u << LED_B) | (1u << PROBE_B));

    g_age_last_us = received_us - item->created_us;
    g_age_last_cycles = eos_dwt_elapsed(item->created_cycles, received_cycles);
    if (g_age_last_cycles < g_age_min_cycles) {
        g_age_min_cycles = g_age_last_cycles;
    }
    if (g_age_last_cycles > g_age_max_cycles) {
        g_age_max_cycles = g_age_last_cycles;
    }

    if (item->checksum != (item->sequence ^ item->value ^ 0x5a5a5a5au)) {
        g_bad_items++;
    }
    if (have_previous) {
        g_sequence_gaps += item->sequence - previous - 1u;
    }
    previous = item->sequence;
    have_previous = true;
    g_received++;
}

static void producer(void *arg)
{
    (void)arg;
    uint32_t sequence = 0;
    TickType_t release = xTaskGetTickCount();

    for (;;) {
        message_t item = {
            .sequence = sequence,
            .value = 1000u + sequence,
            .created_us = time_us_32(),
            .created_cycles = eos_dwt_cycles(),
        };
        item.checksum = item.sequence ^ item.value ^ 0x5a5a5a5au;
        sequence++;
        g_attempted++;
        gpio_xor_mask((1u << LED_A) | (1u << PROBE_A));

        if (xQueueSend(g_samples, &item, 0) == pdPASS) {
            g_sent++;
        } else {
            if (g_dropped == 0u) {
                g_first_drop_us = time_us_32() - run_start_us;
            }
            g_dropped++;
        }
        vTaskDelayUntil(&release, pdMS_TO_TICKS(20));
    }
}

static void consumer(void *arg)
{
    (void)arg;
    for (;;) {
        message_t item = {0};
        /* TODO QUEUE: receive into item; block while the queue is empty. */
        BaseType_t got = pdFALSE;

        if (got == pdPASS) {
            accept_item(&item);
            if (EOS_SLOW_CONSUMER) {
                vTaskDelay(pdMS_TO_TICKS(80));
            }
        } else {
            /* Buildable placeholder; do not spin while TODO QUEUE is empty. */
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

#elif EOS_LAB_MODE == 2
SemaphoreHandle_t g_signal;
volatile uint32_t g_notification_first, g_notification_left;
volatile uint32_t g_binary_accepted, g_binary_rejected;

static void signal_probe(void *arg)
{
    (void)arg;
    for (;;) {
        g_binary_accepted = 0;
        g_binary_rejected = 0;
        for (unsigned i = 0; i < 3; ++i) {
            xTaskNotifyGive(g_tasks[0]);
            if (xSemaphoreGive(g_signal) == pdPASS) {
                g_binary_accepted++;
            } else {
                g_binary_rejected++;
            }
        }

        /* TODO NOTIFY: drain the whole notification count in one take.
         * Keep the next take as a diagnostic for anything left behind. */
        g_notification_first = ulTaskNotifyTake(pdFALSE, 0);
        g_notification_left = ulTaskNotifyTake(pdTRUE, 0);
        configASSERT(xSemaphoreTake(g_signal, 0) == pdPASS);

        printf("notify_first=%lu notify_left=%lu binary_ok=%lu binary_full=%lu\n",
               (unsigned long)g_notification_first,
               (unsigned long)g_notification_left,
               (unsigned long)g_binary_accepted,
               (unsigned long)g_binary_rejected);
        gpio_xor_mask((1u << LED_A) | (1u << PROBE_A));
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

#elif EOS_LAB_MODE == 3
typedef struct {
    uint32_t value;
    uint32_t inverse;
} config_t;

SemaphoreHandle_t g_config_mutex;
volatile config_t g_config = { .value = 0, .inverse = UINT32_MAX };
volatile uint32_t g_snapshots, g_inconsistent;

static void config_lock(void)
{
    if (EOS_PROTECT_CONFIG) {
        /* TODO LOCK: take g_config_mutex with a blocking wait; check success. */
    }
}

static void config_unlock(void)
{
    if (EOS_PROTECT_CONFIG) {
        /* TODO UNLOCK: give g_config_mutex; check success. */
    }
}

static void writer(void *arg)
{
    (void)arg;
    uint32_t generation = 0;
    for (;;) {
        config_lock();
        g_phase[0] = 1;
        g_config.value = ++generation;
        /* Deliberately stretch the inconsistent window for this experiment. */
        vTaskDelay(1);
        g_config.inverse = ~generation;
        g_phase[0] = 0;
        config_unlock();
        g_rounds[0]++;
        gpio_xor_mask((1u << LED_A) | (1u << PROBE_A));
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void reader(void *arg)
{
    (void)arg;
    for (;;) {
        config_lock();
        const uint32_t value = g_config.value;
        const uint32_t inverse = g_config.inverse;
        config_unlock();

        g_snapshots++;
        if (inverse != ~value) {
            g_inconsistent++;
        }
        gpio_xor_mask((1u << LED_B) | (1u << PROBE_B));
        vTaskDelay(1);
    }
}

#elif EOS_LAB_MODE == 4
SemaphoreHandle_t g_config_mutex, g_logger_mutex;
SemaphoreHandle_t g_first_taken[2];

static void lock_worker(void *arg)
{
    const unsigned id = (unsigned)(uintptr_t)arg;
    for (;;) {
        SemaphoreHandle_t first = id == 0 ? g_config_mutex : g_logger_mutex;
        SemaphoreHandle_t second = id == 0 ? g_logger_mutex : g_config_mutex;
        if (EOS_ORDERED_LOCKS) {
            /* TODO ORDER: both tasks must choose config first, logger second. */
        }

        configASSERT(xSemaphoreTake(first, portMAX_DELAY) == pdPASS);
        g_phase[id] = 1;
        if (!EOS_ORDERED_LOCKS) {
            /* Stage opposite ownership before either task tries its second lock. */
            configASSERT(xSemaphoreGive(g_first_taken[id]) == pdPASS);
            configASSERT(xSemaphoreTake(g_first_taken[1u - id],
                                       portMAX_DELAY) == pdPASS);
        } else {
            /* No cross-task barrier while holding the globally first lock. */
            vTaskDelay(1);
        }

        g_phase[id] = 2;
        configASSERT(xSemaphoreTake(second, portMAX_DELAY) == pdPASS);
        g_phase[id] = 3;
        g_rounds[id]++;
        configASSERT(xSemaphoreGive(second) == pdPASS);
        configASSERT(xSemaphoreGive(first) == pdPASS);
        g_phase[id] = 0;
        gpio_xor_mask(1u << (id == 0 ? PROBE_A : PROBE_B));
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
#else
#error "Select EOS_LAB_MODE 1..4 through CMake"
#endif

#if EOS_LAB_MODE != 2
static void reporter(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
#if EOS_LAB_MODE == 1
        printf("attempt=%lu sent=%lu drop=%lu receive=%lu bad=%lu gaps=%lu age_us=%lu\n",
               (unsigned long)g_attempted, (unsigned long)g_sent,
               (unsigned long)g_dropped, (unsigned long)g_received,
               (unsigned long)g_bad_items, (unsigned long)g_sequence_gaps,
               (unsigned long)g_age_last_us);
        printf("depth=%u first_drop_us=%lu dwt_ok=%lu clock_hz=%lu read_span=%lu\n",
               (unsigned)EOS_QUEUE_DEPTH, (unsigned long)g_first_drop_us,
               (unsigned long)g_dwt_running, (unsigned long)g_clock_hz,
               (unsigned long)g_dwt_read_span);
        printf("age_cycles last=%lu min=%lu max=%lu (receive=%lu)\n",
               (unsigned long)g_age_last_cycles,
               (unsigned long)(g_received ? g_age_min_cycles : 0u),
               (unsigned long)g_age_max_cycles, (unsigned long)g_received);
#elif EOS_LAB_MODE == 3
        printf("write=%lu snapshots=%lu inconsistent=%lu\n",
               (unsigned long)g_rounds[0], (unsigned long)g_snapshots,
               (unsigned long)g_inconsistent);
#else
        printf("phase=%lu/%lu rounds=%lu/%lu state=%d/%d (Blocked=%d)\n",
               (unsigned long)g_phase[0], (unsigned long)g_phase[1],
               (unsigned long)g_rounds[0], (unsigned long)g_rounds[1],
               (int)eTaskGetState(g_tasks[0]), (int)eTaskGetState(g_tasks[1]),
               (int)eBlocked);
#endif
    }
}
#endif

int main(void)
{
    stdio_init_all();
    const unsigned pins[] = { LED_A, LED_B, PROBE_A, PROBE_B };
    for (unsigned i = 0; i < 4; ++i) {
        gpio_init(pins[i]);
        gpio_put(pins[i], 0);
        gpio_set_dir(pins[i], GPIO_OUT);
    }

#if EOS_LAB_MODE == 1
    g_clock_hz = clock_get_hz(clk_sys);
    eos_dwt_init();
    const uint32_t read_start = eos_dwt_cycles();
    g_dwt_read_span = eos_dwt_elapsed(read_start, eos_dwt_cycles());
    const uint32_t check_start = eos_dwt_cycles();
    busy_wait_us_32(10);
    g_dwt_running = eos_dwt_elapsed(check_start, eos_dwt_cycles()) > 0u;
    g_samples = xQueueCreate(EOS_QUEUE_DEPTH, sizeof(message_t));
    configASSERT(g_samples != NULL);
    vQueueAddToRegistry(g_samples, "samples");
    configASSERT(xTaskCreate(producer, "producer", 512, NULL, 2,
                            &g_tasks[0]) == pdPASS);
    configASSERT(xTaskCreate(consumer, "consumer", 512, NULL, 3,
                            &g_tasks[1]) == pdPASS);
#elif EOS_LAB_MODE == 2
    g_signal = xSemaphoreCreateBinary();
    configASSERT(g_signal != NULL);
    configASSERT(xTaskCreate(signal_probe, "signal_probe", 1024, NULL, 2,
                            &g_tasks[0]) == pdPASS);
#elif EOS_LAB_MODE == 3
    g_config_mutex = xSemaphoreCreateMutex();
    configASSERT(g_config_mutex != NULL);
    configASSERT(xTaskCreate(writer, "writer", 512, NULL, 3,
                            &g_tasks[0]) == pdPASS);
    configASSERT(xTaskCreate(reader, "reader", 512, NULL, 2,
                            &g_tasks[1]) == pdPASS);
#else
    g_config_mutex = xSemaphoreCreateMutex();
    g_logger_mutex = xSemaphoreCreateMutex();
    configASSERT(g_config_mutex != NULL && g_logger_mutex != NULL);
    for (unsigned id = 0; id < 2; ++id) {
        g_first_taken[id] = xSemaphoreCreateBinary();
        configASSERT(g_first_taken[id] != NULL);
        configASSERT(xTaskCreate(lock_worker, id == 0 ? "worker_a" : "worker_b",
                                512, (void *)(uintptr_t)id, 2,
                                &g_tasks[id]) == pdPASS);
    }
#endif
#if EOS_LAB_MODE != 2
    configASSERT(xTaskCreate(reporter, "reporter", 1024, NULL, 1, NULL) == pdPASS);
#endif
#if EOS_LAB_MODE == 1
    run_start_us = time_us_32();
#endif
    vTaskStartScheduler();
    g_fault = 3;
    for (;;) {
    }
}
