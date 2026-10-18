#include <inttypes.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"
#include "check.h"

#define LED_A 16u
#define LED_B 17u
#define PROBE_A 18u
#define PROBE_B 19u

static volatile uint32_t task_a_runs;
static volatile uint32_t task_b_runs;

static void task_a(void *arg)
{
    (void)arg;
    for (;;) {
        gpio_xor_mask((1u << LED_A) | (1u << PROBE_A));
        task_a_runs++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void task_b(void *arg)
{
    (void)arg;
    for (;;) {
        /* TODO: toggle LED_B and PROBE_B, count the run, then block for 100 ms. */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void reporter_task(void *arg)
{
    (void)arg;
    TickType_t last_report = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last_report, pdMS_TO_TICKS(1000));
        EOS_CHECK_U32("task_a", task_a_runs > 0, task_a_runs, "runs");
        EOS_CHECK_U32("task_b", task_b_runs > 0, task_b_runs, "runs");
    }
}

int main(void)
{
    stdio_init_all();
    gpio_init(LED_A);
    gpio_set_dir(LED_A, GPIO_OUT);
    gpio_init(LED_B);
    gpio_set_dir(LED_B, GPIO_OUT);
    gpio_init(PROBE_A);
    gpio_set_dir(PROBE_A, GPIO_OUT);
    gpio_init(PROBE_B);
    gpio_set_dir(PROBE_B, GPIO_OUT);

    printf("EOS L1.01: two-task scheduler baseline\n");
    if (xTaskCreate(task_a, "task_a", configMINIMAL_STACK_SIZE, NULL, 2, NULL) != pdPASS ||
        xTaskCreate(task_b, "task_b", configMINIMAL_STACK_SIZE, NULL, 2, NULL) != pdPASS ||
        xTaskCreate(reporter_task, "report", configMINIMAL_STACK_SIZE, NULL, 1, NULL) != pdPASS) {
        printf("Task creation failed\n");
        for (;;) { tight_loop_contents(); }
    }
    vTaskStartScheduler();
    for (;;) { tight_loop_contents(); }
}
