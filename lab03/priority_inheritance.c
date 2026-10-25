/* Priority-inheritance demonstration for Lab 3, Task 2.
 *
 * This is a separate program from the spectrum instrument. It exists to make one
 * scheduling dependency observable: a high-priority task waits for a resource
 * owned by a low-priority task, while an unrelated medium-priority task is Ready.
 *
 * Three tasks share one resource:
 *
 *   H (priority 3) requests the resource that L already owns.
 *   M (priority 2) is Ready with unrelated, bounded CPU work.
 *   L (priority 1) owns the resource and still has bounded work to finish.
 *
 * Staging is deterministic. L takes the resource before the gate opens, so H
 * always meets an owned resource. The gate is opened once for all three tasks,
 * which keeps the measured interval free of any deliberate suspension.
 *
 * EOS_INHERITANCE_MUTEX selects the kind of object that protects the resource:
 * a mutex (ON) or a binary semaphore (OFF). Everything else is identical.
 *
 * All work is synthetic and bounded. Nothing is printed inside the measured
 * interval, because printing would itself disturb the schedule being measured.
 */
#include <stdio.h>
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

/* Synthetic CPU demand, in microseconds. The program reports what it actually
 * measured rather than what was requested. */
enum {
    H_WORK_US = 1000,
    M_WORK_US = 3000,
    L_WORK_US = 2000,
    PRIORITY_H = 3,
    PRIORITY_M = 2,
    PRIORITY_L = 1,
    /* Above H, so the gate is fully open before any measured task runs. The
     * controller blocks immediately afterwards and never disturbs the interval. */
    PRIORITY_CONTROLLER = 4,
};

static SemaphoreHandle_t resource;   /* mutex, or binary semaphore when OFF */
static SemaphoreHandle_t gate;       /* counting: opens the measured interval  */
static TaskHandle_t h_handle, m_handle, l_handle, controller_handle;

/* One recorded observation. Kept in a small fixed array so the report needs no
 * allocation and the order of events is preserved. */
typedef struct {
    const char *task;
    const char *event;
    uint32_t us;          /* microseconds since the interval opened */
    uint32_t priority;    /* task priority at that moment, where relevant */
} observation_t;

static observation_t observations[16];
static volatile unsigned observation_count;
/* The highest priority the kernel gave L while it owned the resource. This is
 * the direct evidence that inheritance happened, so it is kept separately from
 * the event list rather than inferred from its last entry. */
static volatile uint32_t l_highest_priority;
static volatile uint64_t interval_start;

/* Append one observation, timed from the moment the interval opened.
 * Silently drops the entry when the array is full, so recording can never
 * change the schedule it is observing. */
static void record(const char *task, const char *event, uint32_t priority)
{
    if (observation_count >= sizeof observations / sizeof observations[0]) return;
    observation_t *entry = &observations[observation_count];
    entry->task = task;
    entry->event = event;
    entry->us = (uint32_t)(time_us_64() - interval_start);
    entry->priority = priority;
    observation_count++;
}

/* Bounded synthetic CPU work: the task is genuinely busy, not sleeping, so the
 * priorities are what decide who runs. @param microseconds busy duration. */
static void spin_us(uint32_t microseconds)
{
    const uint64_t until = time_us_64() + microseconds;
    while (time_us_64() < until) tight_loop_contents();
}

/* H: request the resource L owns, use it, then wake the controller. */
static void high_task(void *unused)
{
    (void)unused;
    xSemaphoreTake(gate, portMAX_DELAY);
    record("H", "request", PRIORITY_H);
    xSemaphoreTake(resource, portMAX_DELAY);
    record("H", "acquire", PRIORITY_H);
    spin_us(H_WORK_US);
    xSemaphoreGive(resource);
    record("H", "complete", PRIORITY_H);
    /* H is the last task to finish, so it releases the reporter. The reporter
     * stays Blocked for the whole interval and cannot disturb the schedule. */
    xTaskNotifyGive(controller_handle);
    vTaskSuspend(NULL);
}

/* M: unrelated work at a priority between H and L. With a plain semaphore this
 * is what postpones L's release; with an inheritance mutex it cannot. */
static void medium_task(void *unused)
{
    (void)unused;
    xSemaphoreTake(gate, portMAX_DELAY);
    record("M", "start", PRIORITY_M);
    spin_us(M_WORK_US);
    record("M", "end", PRIORITY_M);
    vTaskSuspend(NULL);
}

/* L: own the resource first, then finish the protected work in slices while
 * sampling the priority the kernel actually gives it. */
static void low_task(void *unused)
{
    (void)unused;
    /* Own the resource before the interval opens, so H meets an owned resource. */
    xSemaphoreTake(resource, portMAX_DELAY);
    xSemaphoreTake(gate, portMAX_DELAY);

    /* Finish the protected work in slices and note the priority the kernel is
     * actually giving us. With an inheritance mutex this rises to H's priority
     * while H waits; with a binary semaphore it stays at the base priority. */
    uint32_t highest = uxTaskPriorityGet(NULL);
    const uint32_t slice_us = 100;
    for (uint32_t done = 0; done < L_WORK_US; done += slice_us) {
        spin_us(slice_us);
        const uint32_t now = uxTaskPriorityGet(NULL);
        if (now > highest) highest = now;
    }
    l_highest_priority = highest;
    /* Record before releasing: the give unblocks H, which preempts L at once,
     * so a record placed after the call would carry H's completion time. */
    record("L", "release", highest);
    xSemaphoreGive(resource);
    vTaskSuspend(NULL);
}

/* The kernel configuration enables run-time statistics, static allocation and
 * the malloc/stack failure hooks. Each target must supply them, because the
 * instrument's own copies live in main.c and are not linked here. */
uint32_t eos_runtime_counter(void) { return time_us_32(); }

static volatile unsigned fault;

void vApplicationMallocFailedHook(void)
{
    fault = 1;
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    (void)name;
    fault = 2;
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}

/* Static allocation is enabled for the instrument's ready-descriptor queue, so
 * every target in this project must supply the idle task's storage. */
static StaticTask_t idle_task_control;
static StackType_t idle_task_stack[configMINIMAL_STACK_SIZE];

void vApplicationGetIdleTaskMemory(StaticTask_t **control, StackType_t **stack,
                                   configSTACK_DEPTH_TYPE *size)
{
    *control = &idle_task_control;
    *stack = idle_task_stack;
    *size = configMINIMAL_STACK_SIZE;
}

/* Print the configuration, the recorded events and the two derived numbers:
 * L's highest observed priority, and H's request-to-completion response. */
static void report(void)
{
    printf("inheritance demo: resource=%s H=%d M=%d L=%d work_us=%u/%u/%u\n",
           EOS_INHERITANCE_MUTEX ? "mutex" : "binary-semaphore",
           PRIORITY_H, PRIORITY_M, PRIORITY_L, H_WORK_US, M_WORK_US, L_WORK_US);
    for (unsigned i = 0; i < observation_count; ++i) {
        const observation_t *entry = &observations[i];
        printf("  %-2s %-9s t=%6lu us  priority=%lu\n", entry->task, entry->event,
               (unsigned long)entry->us, (unsigned long)entry->priority);
    }
    printf("  L highest observed priority = %lu (base %d)\n",
           (unsigned long)l_highest_priority, PRIORITY_L);
    for (unsigned i = 0; i + 1 < observation_count; ++i) {
        if (observations[i].task[0] == 'H' && observations[i].event[0] == 'r') {
            const uint32_t request = observations[i].us;
            for (unsigned j = i + 1; j < observation_count; ++j) {
                if (observations[j].task[0] == 'H'
                    && observations[j].event[0] == 'c') {
                    printf("  H response = %lu us (request to completion)\n",
                           (unsigned long)(observations[j].us - request));
                }
            }
        }
    }
}

/* Opens the measured interval, then waits to be told it is over. It runs at the
 * lowest application priority and is Blocked throughout the interval, so it
 * never competes with L for CPU service. */
static void controller_task(void *unused)
{
    (void)unused;
    /* L is created first, so it owns the resource before this point. */
    printf("demo: controller running\n");
    vTaskDelay(pdMS_TO_TICKS(50));
    interval_start = time_us_64();
    printf("demo: interval opens\n");
    for (int i = 0; i < 3; ++i) xSemaphoreGive(gate);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    printf("demo: interval closed\n");
    report();
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

/* Create the tasks in an order that fixes the initial state — L owns the
 * resource before the gate opens — then hand control to the scheduler. */
int main(void)
{
    stdio_init_all();
    sleep_ms(500);
    printf("demo: boot\n");

    gate = xSemaphoreCreateCounting(3, 0);
    hard_assert(gate != NULL);
#if EOS_INHERITANCE_MUTEX
    resource = xSemaphoreCreateMutex();
#else
    resource = xSemaphoreCreateBinary();
    hard_assert(resource != NULL);
    xSemaphoreGive(resource);
#endif
    hard_assert(resource != NULL);

    xTaskCreate(low_task, "L", 512, NULL, PRIORITY_L, &l_handle);
    xTaskCreate(medium_task, "M", 512, NULL, PRIORITY_M, &m_handle);
    xTaskCreate(high_task, "H", 512, NULL, PRIORITY_H, &h_handle);
    xTaskCreate(controller_task, "ctl", 512, NULL, PRIORITY_CONTROLLER,
                &controller_handle);

    printf("demo: tasks created\n");
    /* main() must not return: the reporting task ends this program's output. */
    vTaskStartScheduler();
    for (;;) tight_loop_contents();
}
