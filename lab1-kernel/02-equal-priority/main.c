#include <stdint.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"

#define PIN_A 16u
#define PIN_B 17u
#define PROBE_A 18u
#define PROBE_B 19u

volatile uint32_t g_task_a_runs;
volatile uint32_t g_task_b_runs;

#ifndef EOS_EXPLICIT_YIELD
#define EOS_EXPLICIT_YIELD 0
#endif

static void task_a(void *arg)
{
    (void)arg;
    for (;;) {
        gpio_xor_mask((1u << PIN_A) | (1u << PROBE_A));
        g_task_a_runs++;
#if EOS_EXPLICIT_YIELD
        taskYIELD();
#endif
    }
}

static void task_b(void *arg)
{
    (void)arg;
    for (;;) {
        gpio_xor_mask((1u << PIN_B) | (1u << PROBE_B));
        g_task_b_runs++;
#if EOS_EXPLICIT_YIELD
        taskYIELD();
#endif
    }
}

int main(void)
{
    stdio_init_all();
    gpio_init(PIN_A);
    gpio_set_dir(PIN_A, GPIO_OUT);
    gpio_init(PIN_B);
    gpio_set_dir(PIN_B, GPIO_OUT);
    gpio_init(PROBE_A);
    gpio_set_dir(PROBE_A, GPIO_OUT);
    gpio_init(PROBE_B);
    gpio_set_dir(PROBE_B, GPIO_OUT);
    printf("EOS L1.02: explicit-yield=%d; use the two GPIO traces to compare scheduling\n",
           EOS_EXPLICIT_YIELD);

    if (xTaskCreate(task_a, "task_a", configMINIMAL_STACK_SIZE, NULL, 2, NULL) != pdPASS ||
        xTaskCreate(task_b, "task_b", configMINIMAL_STACK_SIZE, NULL, 2, NULL) != pdPASS) {
        puts("Task creation failed");
        for (;;) { tight_loop_contents(); }
    }
    vTaskStartScheduler();
    for (;;) { tight_loop_contents(); }
}
