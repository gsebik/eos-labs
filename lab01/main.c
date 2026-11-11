#include <stdint.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"

#include "FreeRTOS.h"
#include "task.h"

#if EOS_LAB_MODE < 1 || EOS_LAB_MODE > 4
#error "Select EOS_LAB_MODE 1..4 through CMake"
#endif

/* EP-0172 only: do not copy these pins to a different carrier.
 * Each probe pin toggles with its LED, so the analyzer sees the same event. */
enum { LED_A = 16, LED_B = 17, PROBE_A = 18, PROBE_B = 19 };

/* Experiment timing and stack sizes, named once so the numbers say what they
 * are. One file carries four modes; the guide explains what each mode changes. */
enum {
    A_PERIOD_MS = 100,         /* mode 1: task A's blocking period */
    B_PERIOD_MS = 100,         /* mode 1: task B's blocking period */
    B_MONITOR_PERIOD_MS = 20,  /* modes 3-4: the monitor task's own rate */
    RELEASE_PERIOD_MS = 10,    /* mode 4: the schedule both variants target */
    REPORTER_PERIOD_MS = 1000, /* the @@CHECK window */
    WORK_SHORT_US = 2000,      /* mode 4: alternating busy work, shorter */
    WORK_LONG_US = 5000,       /* mode 4: alternating busy work, longer */
    TASK_STACK_WORDS = 512,
    REPORTER_STACK_WORDS = 1024,
};

/* Task priorities. Mode 3 lifts A above B and the reporter on purpose. */
enum {
    PRIO_SERVICE = 4,
    PRIO_TASK = 2,
    PRIO_REPORTER_MODE1 = 3,
    PRIO_REPORTER = 1,
};

volatile uint32_t g_a_runs;
volatile uint32_t g_b_runs;
volatile uint32_t g_fault;

TaskHandle_t g_a_task;
TaskHandle_t g_b_task;

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    (void)name;

    g_fault = 1; /* Inspect with GDB; avoid printf on an exhausted stack. */
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
        /* One masked write toggles the LED and the probe together, so the
         * visible and the captured evidence describe the same instant. */
        gpio_xor_mask((1u << LED_A) | (1u << PROBE_A));
        g_a_runs++;

#if EOS_LAB_MODE == 1
        /* Both tasks block, so neither can starve the other. */
        vTaskDelay(pdMS_TO_TICKS(A_PERIOD_MS));

#elif EOS_LAB_MODE == 2
        /* Equal priority and never blocking: only the scheduler rotates them. */
        if (EOS_EXPLICIT_YIELD) {
            taskYIELD();
        }

#elif EOS_LAB_MODE == 3
        /* TODO STARVATION: make this service periodic at one tick.
         * Keep priority 4 and preserve the GPIO/counter work above. */
        (void)release;

#else
        /* Busy work keeps A Ready instead of blocked, which is what makes a
         * relative delay drift. */
        busy_wait_us_32((g_a_runs & 1u) ? WORK_SHORT_US : WORK_LONG_US);
        if (EOS_ABSOLUTE_RELEASE) {
            /* TODO PERIOD: use release and a 10 ms absolute tick schedule. */
            (void)release;
            vTaskDelay(pdMS_TO_TICKS(RELEASE_PERIOD_MS)); /* still relative */
        } else {
            vTaskDelay(pdMS_TO_TICKS(RELEASE_PERIOD_MS));
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
        vTaskDelay(pdMS_TO_TICKS(B_PERIOD_MS));

#elif EOS_LAB_MODE == 2
        gpio_xor_mask((1u << LED_B) | (1u << PROBE_B));
        g_b_runs++;
        if (EOS_EXPLICIT_YIELD) {
            taskYIELD();
        }

#else
        gpio_xor_mask((1u << LED_B) | (1u << PROBE_B));
        g_b_runs++;
        vTaskDelay(pdMS_TO_TICKS(B_MONITOR_PERIOD_MS));
#endif
    }
}

static void reporter(void *arg)
{
    (void)arg;

    TickType_t release = xTaskGetTickCount();
    uint32_t old_a = 0;
    uint32_t old_b = 0;

    for (;;) {
        vTaskDelayUntil(&release, pdMS_TO_TICKS(REPORTER_PERIOD_MS));

        /* Report window deltas, not absolute counts: PASS means "ran inside
         * this window", which is progress evidence, not a deadline proof. */
        const uint32_t a = g_a_runs;
        const uint32_t b = g_b_runs;
        const uint32_t da = a - old_a;
        const uint32_t db = b - old_b;
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

    printf("EOS L1 mode=%d cores=%d tick=%u Hz clock=%lu Hz\n", EOS_LAB_MODE,
           configNUMBER_OF_CORES, (unsigned)configTICK_RATE_HZ,
           (unsigned long)clock_get_hz(clk_sys));

    const UBaseType_t priority_a = EOS_LAB_MODE == 3 ? PRIO_SERVICE : PRIO_TASK;
    configASSERT(xTaskCreate(task_a, "task_a", TASK_STACK_WORDS, NULL, priority_a,
                             &g_a_task) == pdPASS);
    configASSERT(xTaskCreate(task_b, "task_b", TASK_STACK_WORDS, NULL, PRIO_TASK,
                             &g_b_task) == pdPASS);

#if EOS_LAB_MODE != 2
    /* Mode 1's reporter sits above the student tasks, so a missing block stays
     * visible. Mode 3 deliberately starves this lower-priority diagnostic too. */
    const UBaseType_t reporter_priority =
        EOS_LAB_MODE == 1 ? PRIO_REPORTER_MODE1 : PRIO_REPORTER;
    configASSERT(xTaskCreate(reporter, "report", REPORTER_STACK_WORDS, NULL,
                             reporter_priority, NULL) == pdPASS);
#endif

    vTaskStartScheduler();

    /* Reached only if the scheduler returns, which it should not. */
    g_fault = 3;
    for (;;) {
    }
}
