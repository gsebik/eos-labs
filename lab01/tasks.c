#include <stdint.h>
#include <stdio.h>

#include "pico/stdlib.h"

#include "FreeRTOS.h"
#include "task.h"

/* EP-0172 LEDs used throughout EOS. */
enum { LED_A = 16, LED_B = 17 };

/* Two visible rates, one per task, and one stack size for both. */
enum {
    FAST_PERIOD_MS = 300,
    SLOW_PERIOD_MS = 700,
    TASK_STACK_WORDS = 512,
};

/* Equal priority on purpose: the point is the rate difference, not the
 * scheduler's choice between them. Both tasks block between toggles. */
enum { FAST_PRIORITY = 1, SLOW_PRIORITY = 1 };

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    (void)name;
    taskDISABLE_INTERRUPTS();
    for (;;) {
    }
}

void vApplicationMallocFailedHook(void)
{
    taskDISABLE_INTERRUPTS();
    for (;;) {
    }
}

static void blink_fast(void *arg)
{
    (void)arg;

    for (;;) {
        gpio_xor_mask(1u << LED_A);
        vTaskDelay(pdMS_TO_TICKS(FAST_PERIOD_MS));
    }
}

static void blink_slow(void *arg)
{
    (void)arg;

    for (;;) {
        gpio_xor_mask(1u << LED_B);
        vTaskDelay(pdMS_TO_TICKS(SLOW_PERIOD_MS));
    }
}

int main(void)
{
    stdio_init_all();

    /* Let a monitor attach before the one-time banner. */
    sleep_ms(2000);

    gpio_init(LED_A);
    gpio_set_dir(LED_A, GPIO_OUT);
    gpio_init(LED_B);
    gpio_set_dir(LED_B, GPIO_OUT);

    printf("EOS L1 two tasks: start\n");

    configASSERT(xTaskCreate(blink_fast, "fast", TASK_STACK_WORDS, NULL, FAST_PRIORITY,
                             NULL) == pdPASS);
    configASSERT(xTaskCreate(blink_slow, "slow", TASK_STACK_WORDS, NULL, SLOW_PRIORITY,
                             NULL) == pdPASS);

    vTaskStartScheduler();

    /* The scheduler does not return while the tasks run. */
    for (;;) {
    }
}
