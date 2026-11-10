#include <stdint.h>
#include <stdio.h>

#include "pico/stdlib.h"

#include "FreeRTOS.h"
#include "task.h"

/* EP-0172 LEDs used throughout EOS. */
enum { LED_A = 16, LED_B = 17 };

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
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

static void blink_slow(void *arg)
{
    (void)arg;
    for (;;) {
        gpio_xor_mask(1u << LED_B);
        vTaskDelay(pdMS_TO_TICKS(700));
    }
}

int main(void)
{
    stdio_init_all();
    /* Pause so the monitor can attach before the one-time banner. */
    sleep_ms(2000);
    gpio_init(LED_A);
    gpio_set_dir(LED_A, GPIO_OUT);
    gpio_init(LED_B);
    gpio_set_dir(LED_B, GPIO_OUT);

    printf("EOS L1 two tasks: start\n");
    xTaskCreate(blink_fast, "fast", 512, NULL, 1, NULL);
    xTaskCreate(blink_slow, "slow", 512, NULL, 1, NULL);
    vTaskStartScheduler();

    for (;;) {
    }
}
