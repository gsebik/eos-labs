/* L4 scheduling preset: three periodic tasks with fixed priorities.
 *
 * A = (C 2 ms, T 5 ms, D 5 ms)   priority 3
 * B = (C 4 ms, T 10 ms, D 10 ms) priority 2
 * C = (C 8 ms, T 20 ms, D 20 ms) priority 1
 * Total U = 1.2: impossible on one core, feasible partitioned as
 * A+B on core 0 (U=0.8) and C on core 1 (U=0.4).
 *
 * EOS_SCHED_PLACEMENT 0 = one core, 1 = partitioned, 2 = global.
 * This is a scheduler demonstration, not the instrument and not a WCET proof.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"

static uint32_t rt_epoch;
uint32_t eos_runtime_counter(void) { return time_us_32() - rt_epoch; }
void eos_runtime_counter_start(void) { rt_epoch = time_us_32(); }

static StaticTask_t idle_control;
static StackType_t idle_stack[configMINIMAL_STACK_SIZE];
void vApplicationGetIdleTaskMemory(StaticTask_t **control, StackType_t **stack,
                                   configSTACK_DEPTH_TYPE *size)
{
    *control = &idle_control;
    *stack = idle_stack;
    *size = configMINIMAL_STACK_SIZE;
}

#if configNUMBER_OF_CORES > 1
static StaticTask_t passive_control;
static StackType_t passive_stack[configMINIMAL_STACK_SIZE];
void vApplicationGetPassiveIdleTaskMemory(StaticTask_t **control,
                                          StackType_t **stack,
                                          configSTACK_DEPTH_TYPE *size,
                                          BaseType_t index)
{
    (void)index;
    *control = &passive_control;
    *stack = passive_stack;
    *size = configMINIMAL_STACK_SIZE;
}
#endif

#ifndef EOS_CORES
#define EOS_CORES 1
#endif
#ifndef EOS_SCHED_PLACEMENT
#define EOS_SCHED_PLACEMENT 0
#endif
#ifndef EOS_SCHED_LOOP_PER_MS
#define EOS_SCHED_LOOP_PER_MS 15000u
#endif

typedef struct {
    const char *name;
    uint32_t c_us, t_ms, d_ms;
    UBaseType_t priority;
    UBaseType_t core_mask;
    TaskHandle_t handle;
    volatile uint32_t n, miss, max_r_us, max_exec_us;
} sched_task_t;

enum { A_C_US = 2000, B_C_US = 4000, C_C_US = 8000 };
enum { A_T_MS = 5, B_T_MS = 10, C_T_MS = 20 };

#if EOS_SCHED_PLACEMENT == 1 && EOS_CORES == 2
#define MASK_AB (1u << 0)
#define MASK_C  (1u << 1)
#else
#define MASK_AB 3u
#define MASK_C  3u
#endif

static volatile uint32_t hb_count;
static void hb_task(void *arg)
{
    (void)arg;
    for (;;) {
        ++hb_count;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static sched_task_t tasks[] = {
    { "A", A_C_US, A_T_MS, A_T_MS, 3, MASK_AB, NULL, 0, 0, 0, 0 },
    { "B", B_C_US, B_T_MS, B_T_MS, 2, MASK_AB, NULL, 0, 0, 0, 0 },
    { "C", C_C_US, C_T_MS, C_T_MS, 1, MASK_C,  NULL, 0, 0, 0, 0 },
};

static void sched_task(void *arg)
{
    sched_task_t *t = (sched_task_t *)arg;
    if (t->name[0] == 'C') printf("dbg,C,task-start\n");
    const TickType_t period = pdMS_TO_TICKS(t->t_ms);
    TickType_t expected = xTaskGetTickCount() + period;
    for (;;) {
        if (t->name[0] == 'C' && t->n < 3) printf("dbg,C,before-delay,n=%lu\n", (unsigned long)t->n);
        vTaskDelay(period);
        if (t->name[0] == 'C' && t->n < 3) printf("dbg,C,after-delay,n=%lu\n", (unsigned long)t->n);
        TickType_t actual = xTaskGetTickCount();
        int32_t late_ticks = (int32_t)(actual - expected);
        uint32_t late_us = late_ticks > 0
            ? (uint32_t)late_ticks * 1000000u / configTICK_RATE_HZ : 0;
        uint64_t start = time_us_64();
        const uint32_t iterations = t->c_us * EOS_SCHED_LOOP_PER_MS / 1000u;
        for (volatile uint32_t i = 0; i < iterations; ++i) {
            __asm volatile("nop");
        }
        uint64_t finish = time_us_64();
        uint32_t exec_us = (uint32_t)(finish - start);
        if (exec_us > t->max_exec_us) t->max_exec_us = exec_us;
        if (late_us > t->max_r_us) t->max_r_us = late_us;
        if (late_us > 0) t->miss += (uint32_t)late_ticks;
        ++t->n;
        expected = actual + period;   /* resynchronise; count skipped releases */
    }
}

static void report_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        for (unsigned i = 0; i < sizeof tasks / sizeof tasks[0]; ++i) {
            sched_task_t *t = &tasks[i];
            printf("sched,hb=%lu\n", (unsigned long)hb_count);
            printf("sched,task=%s,n=%lu,miss=%lu,max_late_us=%lu,max_exec_us=%lu,mask=%u,core=%u\n",
                   t->name, (unsigned long)t->n, (unsigned long)t->miss,
                   (unsigned long)t->max_r_us, (unsigned long)t->max_exec_us,
                   (unsigned)t->core_mask, (unsigned)get_core_num());
            t->n = 0;
            t->miss = 0;
            t->max_r_us = 0;
            t->max_exec_us = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task; (void)name;
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}

void vApplicationMallocFailedHook(void)
{
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}

static void require(BaseType_t r)
{
    if (r != pdPASS) {
        taskDISABLE_INTERRUPTS();
        for (;;) {}
    }
}

int main(void)
{
    stdio_init_all();
    printf("sched cores=%d placement=%d U=1.2\n", EOS_CORES, EOS_SCHED_PLACEMENT);
#if EOS_CORES == 2
    require(xTaskCreateAffinitySet(sched_task, "A", 512, &tasks[0], tasks[0].priority,
                                   tasks[0].core_mask, &tasks[0].handle));
    require(xTaskCreateAffinitySet(sched_task, "B", 512, &tasks[1], tasks[1].priority,
                                   tasks[1].core_mask, &tasks[1].handle));
    require(xTaskCreateAffinitySet(sched_task, "C", 4096, &tasks[2], tasks[2].priority,
                                   tasks[2].core_mask, &tasks[2].handle));
#else
    require(xTaskCreate(sched_task, "A", 512, &tasks[0], tasks[0].priority, &tasks[0].handle));
    require(xTaskCreate(sched_task, "B", 512, &tasks[1], tasks[1].priority, &tasks[1].handle));
    require(xTaskCreate(sched_task, "C", 1024, &tasks[2], tasks[2].priority, &tasks[2].handle));
#endif
    require(xTaskCreate(report_task, "report", 512, NULL, 1, NULL));
#if EOS_CORES == 2
    require(xTaskCreateAffinitySet(hb_task, "hb", 512, NULL, 1, (UBaseType_t)(1u << 1), NULL));
#endif
    vTaskStartScheduler();
    for (;;) {}
}
