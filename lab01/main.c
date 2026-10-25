#include <stdint.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"

#include "FreeRTOS.h"
#include "task.h"

#if EOS_LAB_MODE < 1 || EOS_LAB_MODE > 4
#error "Select EOS_LAB_MODE 1..4 through CMake"
#endif

/* EP-0172 only: do not copy these pins to a different carrier. */
enum { LED_A = 16, LED_B = 17, PROBE_A = 18, PROBE_B = 19 };

volatile uint32_t g_a_runs;
volatile uint32_t g_b_runs;
volatile uint32_t g_fault;

TaskHandle_t g_a_task;
TaskHandle_t g_b_task;

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    (void)name;

    g_fault = 1;  /* Inspect with GDB; avoid printf on an exhausted stack. */
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

static void task_a(void *arg)
{
    (void)arg;
#if EOS_LAB_MODE == 3 || EOS_LAB_MODE == 4
    TickType_t release = xTaskGetTickCount();
#endif
    for (;;) {
        gpio_xor_mask((1u << LED_A) | (1u << PROBE_A));
        g_a_runs++;
#if EOS_LAB_MODE == 1
        vTaskDelay(pdMS_TO_TICKS(100));
#elif EOS_LAB_MODE == 2
        if (EOS_EXPLICIT_YIELD) {
            taskYIELD();
        }
#elif EOS_LAB_MODE == 3
        /* TODO STARVATION: make this service periodic at one tick.
         * Keep priority 4 and preserve the GPIO/counter work above. */
        (void)release;
#else
        /* Variable work occupies the CPU rather than blocking the task. */
        busy_wait_us_32((g_a_runs & 1u) ? 2000 : 5000);
        if (EOS_ABSOLUTE_RELEASE) {
            /* TODO PERIOD: use release and a 10 ms absolute tick schedule. */
            (void)release;
            vTaskDelay(pdMS_TO_TICKS(10)); /* Placeholder, still relative. */
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
#endif
    }
}

static void task_b(void *arg)
{
    (void)arg;
    for (;;) {
#if EOS_LAB_MODE == 1
        /* TODO BASELINE: toggle LED_B and PROBE_B and increment g_b_runs. */
        vTaskDelay(pdMS_TO_TICKS(100));
#elif EOS_LAB_MODE == 2
        gpio_xor_mask((1u << LED_B) | (1u << PROBE_B));
        g_b_runs++;
        if (EOS_EXPLICIT_YIELD) {
            taskYIELD();
        }
#else
        gpio_xor_mask((1u << LED_B) | (1u << PROBE_B));
        g_b_runs++;
        vTaskDelay(pdMS_TO_TICKS(20));
#endif
    }
}

static void reporter(void *arg)
{
    (void)arg;
    TickType_t release = xTaskGetTickCount();
    uint32_t old_a = 0, old_b = 0;

    for (;;) {
        vTaskDelayUntil(&release, pdMS_TO_TICKS(1000));

        const uint32_t a = g_a_runs, b = g_b_runs;
        const uint32_t da = a - old_a, db = b - old_b;
        old_a = a;
        old_b = b;

        printf("@@CHECK task_a %s %lu\n", da ? "PASS" : "FAIL", (unsigned long)da);
        printf("@@CHECK task_b %s %lu\n", db ? "PASS" : "FAIL", (unsigned long)db);
    }
}

int main(void)
{
    stdio_init_all();

    const unsigned pins[] = { LED_A, LED_B, PROBE_A, PROBE_B };
    for (unsigned i = 0; i < 4; ++i) {
        gpio_init(pins[i]);
        gpio_put(pins[i], 0);
        gpio_set_dir(pins[i], GPIO_OUT);
    }

    printf("EOS L1 mode=%d cores=%d tick=%u Hz clock=%lu Hz\n",
           EOS_LAB_MODE, configNUMBER_OF_CORES, (unsigned)configTICK_RATE_HZ,
           (unsigned long)clock_get_hz(clk_sys));

    const UBaseType_t priority_a = EOS_LAB_MODE == 3 ? 4 : 2;
    configASSERT(
        xTaskCreate(task_a, "task_a", 512, NULL, priority_a, &g_a_task) == pdPASS);
    configASSERT(xTaskCreate(task_b, "task_b", 512, NULL, 2, &g_b_task) == pdPASS);
#if EOS_LAB_MODE != 2
    /* Mode 1's reporter is above the student task: missing blocking stays visible.
     * Mode 3 deliberately starves this lower-priority diagnostic as well. */
    configASSERT(xTaskCreate(reporter, "report", 1024, NULL,
                            EOS_LAB_MODE == 1 ? 3 : 1, NULL) == pdPASS);
#endif
    vTaskStartScheduler();
    g_fault = 3;
    for (;;) {
    }
}
