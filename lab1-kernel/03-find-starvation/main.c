#include <stdint.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"
#include "check.h"

#define PIN_FAST     16u
#define PIN_MONITOR  17u
#define PROBE_FAST   18u
#define PROBE_MONITOR 19u

volatile uint32_t g_fast_runs;
volatile uint32_t g_low_runs;
TaskHandle_t g_low_task;

static void fast_task(void *arg)
{
    (void)arg;
    for (;;) {
        gpio_xor_mask((1u << PIN_FAST) | (1u << PROBE_FAST));
        g_fast_runs++;
        /* The supplied activity is fully implemented; diagnose its scheduling effect. */
    }
}

static void monitor_task(void *arg)
{
    (void)arg;
    for (;;) {
        gpio_xor_mask((1u << PIN_MONITOR) | (1u << PROBE_MONITOR));
        g_low_runs++;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void report_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(1000));
        EOS_CHECK_U32("fast_task", g_fast_runs > 0u, g_fast_runs, "runs");
        EOS_CHECK_U32("monitor_task", g_low_runs > 0u, g_low_runs, "runs");
    }
}

int main(void)
{
    stdio_init_all();
    gpio_init(PIN_FAST);
    gpio_set_dir(PIN_FAST, GPIO_OUT);
    gpio_init(PIN_MONITOR);
    gpio_set_dir(PIN_MONITOR, GPIO_OUT);
    gpio_init(PROBE_FAST);
    gpio_set_dir(PROBE_FAST, GPIO_OUT);
    gpio_init(PROBE_MONITOR);
    gpio_set_dir(PROBE_MONITOR, GPIO_OUT);

    puts("EOS L1.03: observe the task states and find why the monitor is silent");
    if (xTaskCreate(fast_task, "fast", configMINIMAL_STACK_SIZE, NULL, 4, NULL) != pdPASS ||
        xTaskCreate(monitor_task, "monitor", configMINIMAL_STACK_SIZE, NULL, 2, &g_low_task) != pdPASS ||
        xTaskCreate(report_task, "report", configMINIMAL_STACK_SIZE, NULL, 1, NULL) != pdPASS) {
        puts("Task creation failed");
        for (;;) { tight_loop_contents(); }
    }

    vTaskStartScheduler();
    for (;;) { tight_loop_contents(); }
}
