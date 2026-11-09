/* Scheduling mode inside the working condition-monitor target.
 *
 * Uses the same FreeRTOS configuration as the instrument. A/B/C are the lab's
 * Task 1 periodic set; student choices live in placement.c:
 *   A=(2,5), B=(4,10), C=(8,20) ms, U=1.2.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "FreeRTOS.h"
#include "task.h"
#include "placement.h"
#include "sched_trace.h"
#include "sched_release.h"
#include "pico/critical_section.h"
#include <stdatomic.h>

#ifndef EOS_CORES
#define EOS_CORES 1
#endif
#ifndef EOS_SCHED_PLACEMENT
#define EOS_SCHED_PLACEMENT 0
#endif
#ifndef EOS_SCHED_LOOP_PER_MS
#define EOS_SCHED_LOOP_PER_MS 13600u
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
    uint32_t n, miss, max_late_us, max_overrun_us, max_exec_us, max_response_us;
} sched_task_t;

#if EOS_SCHED_DHALL
/* Dhall-style set scaled for the 1 ms tick: A/B = (20,200), C = (195,210) ms.
 * This runs global fixed-priority (RM), which is what FreeRTOS provides.
 * A and B occupy both cores for 20 ms, leaving C only 190 ms before its
 * 210 ms deadline; C needs 195 ms. Partitioned, C has its own core and meets.
 * It is NOT a G-EDF demonstration: FreeRTOS has no EDF scheduler. */
static sched_task_t tasks[] = {
    { .name="A", .c_us=20000, .t_ms=200, .d_ms=200, .priority=3 },
    { .name="B", .c_us=20000, .t_ms=200, .d_ms=200, .priority=2 },
    { .name="C", .c_us=195000, .t_ms=210, .d_ms=210, .priority=1 },
};
#else
static sched_task_t tasks[] = {
    { .name="A", .c_us=2000, .t_ms=5, .d_ms=5, .priority=3 },
    { .name="B", .c_us=4000, .t_ms=10, .d_ms=10, .priority=2 },
    { .name="C", .c_us=8000, .t_ms=20, .d_ms=20, .priority=1 },
};
#endif

static _Atomic uint32_t hb_count;
static critical_section_t report_lock;

void eos_sched_release_tick(uint32_t tick)
{
    /* Runs inside the serialised kernel tick path, before any due task can be
     * unblocked. This is the software release observation, not an estimated
     * wall-clock epoch or the physical SysTick edge. No UART in this path.
     */
    eos_sched_release_record(tick, time_us_64());
}

static void sched_task(void *arg)
{
    sched_task_t *t = (sched_task_t *)arg;
    /* All three periodic sequences start at scheduler tick zero. */
    TickType_t last = 0;
    const TickType_t period = pdMS_TO_TICKS(t->t_ms);
    uint32_t job = 0;
    const unsigned id = (unsigned)(t - tasks) + 1u;
    for (;;) {
        vTaskDelayUntil(&last, period);
        uint64_t release_us;
        if (!eos_sched_release_take(id - 1u, &release_us)) {
            printf("sched,error=release_history_invalid,task=%s\n", t->name);
            vTaskSuspend(NULL); /* Do not publish invented timing after overflow. */
            for (;;) {}
        }
        ++job;
        const uint64_t deadline = release_us + (uint64_t)t->d_ms * 1000u;
        uint64_t start = eos_sched_trace_job(SCHED_JOB_START, id, job, release_us, deadline);
        const uint32_t iterations = t->c_us * EOS_SCHED_LOOP_PER_MS / 1000u;
        for (volatile uint32_t i = 0; i < iterations; ++i) {
            __asm volatile("nop");
        }
        uint64_t finish = eos_sched_trace_job(SCHED_JOB_FINISH, id, job, release_us, deadline);
        uint32_t exec_us = (uint32_t)(finish - start);
        uint32_t late_start = start > release_us ? (uint32_t)(start - release_us) : 0;
        uint32_t overrun = finish > deadline ? (uint32_t)(finish - deadline) : 0;
        const uint32_t response = finish > release_us ? (uint32_t)(finish - release_us) : 0;
        critical_section_enter_blocking(&report_lock);
        if (exec_us > t->max_exec_us) t->max_exec_us = exec_us;
        if (late_start > t->max_late_us) t->max_late_us = late_start;
        if (overrun > t->max_overrun_us) t->max_overrun_us = overrun;
        if (response > t->max_response_us) t->max_response_us = response;
        if (overrun > 0) ++t->miss;
        ++t->n;
        critical_section_exit(&report_lock);
    }
}

static void hb_task(void *arg)
{
    (void)arg;
    for (;;) {
        atomic_fetch_add_explicit(&hb_count, 1u, memory_order_relaxed);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

#if EOS_SCHED_REPORT
static void report_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        sched_task_t snapshot[SCHED_TASK_COUNT];
        critical_section_enter_blocking(&report_lock);
        for (unsigned i = 0; i < SCHED_TASK_COUNT; ++i) {
            snapshot[i] = tasks[i];
            tasks[i].n = tasks[i].miss = tasks[i].max_late_us = 0;
            tasks[i].max_overrun_us = tasks[i].max_exec_us = tasks[i].max_response_us = 0;
        }
        const uint64_t report_ms = time_us_64() / 1000u;
        critical_section_exit(&report_lock);
        printf("sched,hb=%lu,t_ms=%llu\n",
               (unsigned long)atomic_load_explicit(&hb_count, memory_order_relaxed),
               (unsigned long long)report_ms);
        for (unsigned i = 0; i < SCHED_TASK_COUNT; ++i) {
            const sched_task_t *t = &snapshot[i];
            printf("sched,task=%s,n=%lu,miss=%lu,max_late_us=%lu,max_overrun_us=%lu,max_exec_us=%lu,mask=%u,max_response_us=%lu\n",
                    t->name, (unsigned long)t->n, (unsigned long)t->miss,
                    (unsigned long)t->max_late_us, (unsigned long)t->max_overrun_us,
                    (unsigned long)t->max_exec_us, (unsigned)t->mask,
                    (unsigned long)t->max_response_us);
        }
        /* Traced builds dump only after recording stops. Ignore later summary
         * windows disturbed by this slow UART transfer; use trace=0 for them. */
        eos_sched_trace_dump();
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
    critical_section_init(&report_lock);
    uint32_t periods[SCHED_RELEASE_TASKS];
    for (unsigned i = 0; i < SCHED_RELEASE_TASKS; ++i)
        periods[i] = pdMS_TO_TICKS(tasks[i].t_ms);
    eos_sched_release_init(periods, 0);
    sleep_ms(1500);
    for (unsigned i = 0; i < SCHED_TASK_COUNT; ++i) {
        tasks[i].mask = eos_scheduler_mask(i);
        if (!tasks[i].mask || (tasks[i].mask & ~((1u << EOS_CORES) - 1u))) {
            printf("sched,error=invalid_mask,task=%s,mask=%u\n", tasks[i].name, (unsigned)tasks[i].mask);
            for (;;) tight_loop_contents();
        }
    }
    unsigned model_permille = 0;
    for (unsigned i = 0; i < SCHED_TASK_COUNT; ++i) model_permille += tasks[i].c_us / tasks[i].t_ms;
    printf("sched cores=%d placement=%d U_model=%u.%03u trace=%d\n", EOS_CORES,
           EOS_SCHED_PLACEMENT, model_permille / 1000u, model_permille % 1000u, EOS_SCHED_TRACE);
#if EOS_CORES == 2
    require(xTaskCreateAffinitySet(sched_task, "A", 512, &tasks[0],
                                   tasks[0].priority, tasks[0].mask, &tasks[0].handle));
    require(xTaskCreateAffinitySet(sched_task, "B", 512, &tasks[1],
                                   tasks[1].priority, tasks[1].mask, &tasks[1].handle));
    require(xTaskCreateAffinitySet(sched_task, "C", 1024, &tasks[2],
                                   tasks[2].priority, tasks[2].mask, &tasks[2].handle));
    TaskHandle_t heartbeat;
    require(xTaskCreateAffinitySet(hb_task, "hb", 512, NULL, 1,
                                   (UBaseType_t)(1u << 1), &heartbeat));
    vTaskSetTaskNumber(heartbeat, 4u);
#else
    require(xTaskCreate(sched_task, "A", 512, &tasks[0], tasks[0].priority, &tasks[0].handle));
    require(xTaskCreate(sched_task, "B", 512, &tasks[1], tasks[1].priority, &tasks[1].handle));
    require(xTaskCreate(sched_task, "C", 1024, &tasks[2], tasks[2].priority, &tasks[2].handle));
#endif
#if EOS_SCHED_REPORT
    TaskHandle_t reporter;
    require(xTaskCreate(report_task, "report", 1024, NULL, 1, &reporter));
    vTaskSetTaskNumber(reporter, 5u);
#endif
    for (unsigned i = 0; i < SCHED_TASK_COUNT; ++i) vTaskSetTaskNumber(tasks[i].handle, i + 1u);
    eos_sched_trace_init();
    vTaskStartScheduler();
    for (;;) {}
}
