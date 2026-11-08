/* Scheduling mode inside the working condition-monitor target.
 *
 * Reuses board.c, the same FreeRTOS configuration and the same task-creation
 * path as the instrument. A/B/C are the lab's Task 1 periodic set:
 *   A=(2,5), B=(4,10), C=(8,20) ms, U=1.2.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "FreeRTOS.h"
#include "task.h"

#ifndef EOS_CORES
#define EOS_CORES 1
#endif
#ifndef EOS_SCHED_PLACEMENT
#define EOS_SCHED_PLACEMENT 0
#endif
#ifndef EOS_SCHED_LOOP_PER_MS
#define EOS_SCHED_LOOP_PER_MS 15000u
#endif
#ifndef EOS_SCHED_REPORT
#define EOS_SCHED_REPORT 1
#endif
#ifndef EOS_SCHED_DHALL
#define EOS_SCHED_DHALL 0
#endif

typedef struct {
    const char *name;
    uint32_t c_us, t_ms, d_ms;
    UBaseType_t priority;
    UBaseType_t mask;
    TaskHandle_t handle;
    volatile uint32_t n, miss, max_late_us, max_overrun_us, max_exec_us;
} sched_task_t;

#if EOS_CORES == 2 && EOS_SCHED_PLACEMENT == 1
#define MASK_AB (1u << 0)
#define MASK_C  (1u << 1)
#else
#define MASK_AB 3u
#define MASK_C  3u
#endif

#if EOS_SCHED_DHALL
/* Dhall-style set scaled for the 1 ms tick: A/B = (20,200), C = (195,210) ms.
 * This runs global fixed-priority (RM), which is what FreeRTOS provides.
 * A and B occupy both cores for 20 ms, leaving C only 190 ms before its
 * 210 ms deadline; C needs 195 ms. Partitioned, C has its own core and meets.
 * It is NOT a G-EDF demonstration: FreeRTOS has no EDF scheduler. */
static sched_task_t tasks[] = {
    { "A", 20000, 200, 200, 3, MASK_AB, NULL, 0, 0, 0, 0, 0 },
    { "B", 20000, 200, 200, 2, MASK_AB, NULL, 0, 0, 0, 0, 0 },
    { "C", 195000, 210, 210, 1, MASK_C, NULL, 0, 0, 0, 0, 0 },
};
#else
static sched_task_t tasks[] = {
    { "A", 2000, 5, 5, 3, MASK_AB, NULL, 0, 0, 0, 0, 0 },
    { "B", 4000, 10, 10, 2, MASK_AB, NULL, 0, 0, 0, 0, 0 },
    { "C", 8000, 20, 20, 1, MASK_C, NULL, 0, 0, 0, 0, 0 },
};
#endif

static volatile uint32_t hb_count;

static void sched_task(void *arg)
{
    sched_task_t *t = (sched_task_t *)arg;
    TickType_t last = xTaskGetTickCount();
    const TickType_t start_tick = last;
    const uint64_t start_us = time_us_64();
    const TickType_t period = pdMS_TO_TICKS(t->t_ms);
    TickType_t release_tick = last + period;
    for (;;) {
        vTaskDelayUntil(&last, period);
        uint64_t release_us = start_us
            + (uint64_t)(release_tick - start_tick) * 1000000u / configTICK_RATE_HZ;
        release_tick += period;
        uint64_t start = time_us_64();
        const uint32_t iterations = t->c_us * EOS_SCHED_LOOP_PER_MS / 1000u;
        for (volatile uint32_t i = 0; i < iterations; ++i) {
            __asm volatile("nop");
        }
        uint64_t finish = time_us_64();
        uint32_t exec_us = (uint32_t)(finish - start);
        uint32_t late_start = start > release_us ? (uint32_t)(start - release_us) : 0;
        uint64_t deadline = release_us + (uint64_t)t->d_ms * 1000u;
        uint32_t overrun = finish > deadline ? (uint32_t)(finish - deadline) : 0;
        if (exec_us > t->max_exec_us) t->max_exec_us = exec_us;
        if (late_start > t->max_late_us) t->max_late_us = late_start;
        if (overrun > t->max_overrun_us) t->max_overrun_us = overrun;
        if (overrun > 0) ++t->miss;
        ++t->n;
    }
}

static void hb_task(void *arg)
{
    (void)arg;
    for (;;) {
        ++hb_count;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

#if EOS_SCHED_REPORT
static void report_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        printf("sched,hb=%lu\n", (unsigned long)hb_count);
        for (unsigned i = 0; i < sizeof tasks / sizeof tasks[0]; ++i) {
            sched_task_t *t = &tasks[i];
            printf("sched,task=%s,n=%lu,miss=%lu,max_late_us=%lu,max_overrun_us=%lu,max_exec_us=%lu,mask=%u\n",
                   t->name, (unsigned long)t->n, (unsigned long)t->miss,
                   (unsigned long)t->max_late_us, (unsigned long)t->max_overrun_us,
                   (unsigned long)t->max_exec_us, (unsigned)t->mask);
            t->n = 0;
            t->miss = 0;
            t->max_late_us = 0;
            t->max_overrun_us = 0;
            t->max_exec_us = 0;
        }
    }
}
#endif

static void require(BaseType_t r)
{
    if (r != pdPASS) {
        taskDISABLE_INTERRUPTS();
        for (;;) {}
    }
}

int sched_mode_main(void)
{
    set_sys_clock_khz(150000, true);
    stdio_init_all();
    sleep_ms(1500);
    printf("sched cores=%d placement=%d U=1.2\n", EOS_CORES, EOS_SCHED_PLACEMENT);
#if EOS_CORES == 2
    require(xTaskCreateAffinitySet(sched_task, "A", 512, &tasks[0],
                                   tasks[0].priority, tasks[0].mask, &tasks[0].handle));
    require(xTaskCreateAffinitySet(sched_task, "B", 512, &tasks[1],
                                   tasks[1].priority, tasks[1].mask, &tasks[1].handle));
    require(xTaskCreateAffinitySet(sched_task, "C", 1024, &tasks[2],
                                   tasks[2].priority, tasks[2].mask, &tasks[2].handle));
    require(xTaskCreateAffinitySet(hb_task, "hb", 512, NULL, 1,
                                   (UBaseType_t)(1u << 1), NULL));
#else
    require(xTaskCreate(sched_task, "A", 512, &tasks[0], tasks[0].priority, &tasks[0].handle));
    require(xTaskCreate(sched_task, "B", 512, &tasks[1], tasks[1].priority, &tasks[1].handle));
    require(xTaskCreate(sched_task, "C", 1024, &tasks[2], tasks[2].priority, &tasks[2].handle));
#endif
#if EOS_SCHED_REPORT
    require(xTaskCreate(report_task, "report", 1024, NULL, 1, NULL));
#endif
    vTaskStartScheduler();
    for (;;) {}
}
