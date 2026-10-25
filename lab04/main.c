#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "FreeRTOS.h"
#include "task.h"

#if EOS_LAB_MODE < 1 || EOS_LAB_MODE > 3
#error "EOS_LAB_MODE must be 1 (AMP), 2 (FIFO), or 3 (SMP)"
#endif

enum { PIN_CORE0 = 16, PIN_CORE1 = 17, PIN_REQ = 18, PIN_RESP = 19 };
volatile uint32_t g_fault;

#define EOS_COREDEBUG_DEMCR (*(volatile uint32_t *)0xE000EDFCu)
#define EOS_DWT_CTRL        (*(volatile uint32_t *)0xE0001000u)
#define EOS_DWT_CYCCNT      (*(volatile uint32_t *)0xE0001004u)

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    (void)name;
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

static void cycle_counter_init(void)
{
    EOS_COREDEBUG_DEMCR |= 1u << 24;
    EOS_DWT_CYCCNT = 0;
    EOS_DWT_CTRL |= 1u;
}

#if EOS_LAB_MODE == 1
static void core1_periodic(void)
{
    gpio_init(PIN_CORE1);
    gpio_set_dir(PIN_CORE1, GPIO_OUT);
    absolute_time_t deadline = delayed_by_us(get_absolute_time(), 1000);
    for (;;) {
        gpio_put(PIN_CORE1, 1);
        /* TODO AMP: add the fixed 50-NOP control computation. */
        gpio_put(PIN_CORE1, 0);
        busy_wait_until(deadline);
        deadline = delayed_by_us(deadline, 1000);
    }
}

static void load_task(void *arg)
{
    (void)arg;
    gpio_init(PIN_CORE0);
    gpio_set_dir(PIN_CORE0, GPIO_OUT);
    for (;;) {
        gpio_put(PIN_CORE0, 1);
        busy_wait_us_32(20000);
        gpio_put(PIN_CORE0, 0);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

int main(void)
{
    stdio_init_all();
    multicore_launch_core1(core1_periodic);
#if EOS_CORE0_LOAD
    configASSERT(xTaskCreate(load_task, "load", 512, NULL, 1, NULL) == pdPASS);
#endif
    vTaskStartScheduler();
    for (;;) {}
}

#elif EOS_LAB_MODE == 2
static void core1_fifo_echo(void)
{
    cycle_counter_init();
    gpio_init(PIN_RESP);
    gpio_set_dir(PIN_RESP, GPIO_OUT);
    for (;;) {
        const uint32_t word = multicore_fifo_pop_blocking();
        gpio_put(PIN_RESP, 1);
        multicore_fifo_push_blocking(word + 1u);
        gpio_put(PIN_RESP, 0);
    }
}

static void fifo_ping_task(void *arg)
{
    (void)arg;
    gpio_init(PIN_REQ);
    gpio_set_dir(PIN_REQ, GPIO_OUT);
    for (;;) {
        /* TODO FIFO: use the DWT cycle counter to timestamp the round trip. */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

int main(void)
{
    stdio_init_all();
    cycle_counter_init();
    multicore_launch_core1(core1_fifo_echo);
    configASSERT(xTaskCreate(fifo_ping_task, "ping", 512, NULL, 1, NULL) == pdPASS);
    vTaskStartScheduler();
    for (;;) {}
}

#else
static atomic_uchar seen_cores[4];
static atomic_uchar control_core_mask;
static atomic_uint worst_period_error_us;
static TaskHandle_t control_task_handle;
static TaskHandle_t load_task_handle;

static void load_task(void *arg)
{
    (void)arg;
    gpio_init(PIN_REQ);
    gpio_set_dir(PIN_REQ, GPIO_OUT);
    for (;;) {
        gpio_put(PIN_REQ, 1);
        busy_wait_us_32(20000);
        gpio_put(PIN_REQ, 0);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void worker_task(void *arg)
{
    const unsigned id = (unsigned)(uintptr_t)arg;
    for (;;) {
        /* TODO SMP: record this task's observed core in seen_cores[id]. */
        const unsigned core = portGET_CORE_ID();
        gpio_put(core == 0 ? PIN_CORE0 : PIN_CORE1, 1);
        __asm volatile("nop");
        gpio_put(core == 0 ? PIN_CORE0 : PIN_CORE1, 0);
        taskYIELD();
    }
}

static void control_task(void *arg)
{
    (void)arg;
    gpio_init(PIN_RESP);
    gpio_set_dir(PIN_RESP, GPIO_OUT);
    uint32_t previous = time_us_32();
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(1));
        const uint32_t now = time_us_32();
        const uint32_t period = now - previous;
        previous = now;
        const uint32_t error = period > 1000u ? period - 1000u : 1000u - period;
        /* TODO CONTROL: record which core ran this periodic task. */
        unsigned previous_max = atomic_load_explicit(&worst_period_error_us, memory_order_relaxed);
        while (error > previous_max &&
               !atomic_compare_exchange_weak_explicit(&worst_period_error_us, &previous_max, error,
                                                      memory_order_relaxed, memory_order_relaxed)) {}
        gpio_put(PIN_RESP, 1);
        gpio_put(PIN_RESP, 0);
    }
}

static void report_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const unsigned masks[4] = {
            atomic_load_explicit(&seen_cores[0], memory_order_relaxed),
            atomic_load_explicit(&seen_cores[1], memory_order_relaxed),
            atomic_load_explicit(&seen_cores[2], memory_order_relaxed),
            atomic_load_explicit(&seen_cores[3], memory_order_relaxed),
        };
        const unsigned control_mask = atomic_exchange_explicit(&control_core_mask, 0, memory_order_relaxed);
        const unsigned worst = atomic_exchange_explicit(&worst_period_error_us, 0, memory_order_relaxed);
        printf("worker masks: %u %u %u %u; control mask: %u; worst 1ms period error: %u us\n",
               masks[0], masks[1], masks[2], masks[3], control_mask, worst);
    }
}

int main(void)
{
    stdio_init_all();
    gpio_init(PIN_CORE0);
    gpio_init(PIN_CORE1);
    gpio_init(PIN_REQ);
    gpio_set_dir(PIN_CORE0, GPIO_OUT);
    gpio_set_dir(PIN_CORE1, GPIO_OUT);
    gpio_set_dir(PIN_REQ, GPIO_OUT);
    for (unsigned i = 0; i < 4; ++i) {
        configASSERT(xTaskCreate(worker_task, "worker", 512,
                                 (void *)(uintptr_t)i, 1, NULL) == pdPASS);
    }
    configASSERT(xTaskCreate(load_task, "load", 512, NULL, 1,
                             &load_task_handle) == pdPASS);
    configASSERT(xTaskCreate(control_task, "control", 512, NULL, 2,
                             &control_task_handle) == pdPASS);
    configASSERT(xTaskCreate(report_task, "report", 768, NULL, 2, NULL) == pdPASS);
    vTaskCoreAffinitySet(load_task_handle, (UBaseType_t)(1u << 0));
#if defined(EOS_PIN_CONTROL)
    vTaskCoreAffinitySet(control_task_handle, (UBaseType_t)(1u << 1));
#endif
    vTaskStartScheduler();
    for (;;) {}
}
#endif
