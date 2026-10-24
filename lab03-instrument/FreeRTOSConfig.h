/* FreeRTOS configuration for the Lab 3 instrument.
 *
 * Only the settings that differ from the kernel defaults, or that the
 * experiment depends on, are listed. Three of them are not free choices:
 *
 *   - static allocation is on because the ready-descriptor queue is created
 *     statically, so a failed creation cannot be blamed on heap exhaustion;
 *   - run-time statistics use the same microsecond timer as the trace, through
 *     a helper in main.c;
 *   - the task-number field is enabled because the switch hook publishes it on
 *     the marker pins for the logic analyzer.
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H
#include <stdint.h>

/* Scheduling. Priority is pre-emptive; equal priorities are time-sliced. */
#define configUSE_PREEMPTION 1
#define configUSE_TIME_SLICING 1
#define configTICK_RATE_HZ 1000
#define configMAX_PRIORITIES 7
#define configMINIMAL_STACK_SIZE 256
#define configUSE_16_BIT_TICKS 0
#define configIDLE_SHOULD_YIELD 1
#define configUSE_IDLE_HOOK 0
#define configUSE_TICK_HOOK 0

/* Target: single core, no FPU context saving beyond what the port does, no MPU
 * region checks and no TrustZone split. */
#define configENABLE_FPU 1
#define configENABLE_MPU 0
#define configENABLE_TRUSTZONE 0
#define configRUN_FREERTOS_SECURE_ONLY 1
#define configMAX_SYSCALL_INTERRUPT_PRIORITY 16
#define configNUMBER_OF_CORES 1
#define configCPU_CLOCK_HZ 150000000

/* Memory. The kernel heap holds task stacks and the two mutexes; the sample
 * pool, trace and descriptor queue are static. */
#define configSUPPORT_DYNAMIC_ALLOCATION 1
#define configSUPPORT_STATIC_ALLOCATION 1
#define configTOTAL_HEAP_SIZE (64 * 1024)
#define configCHECK_FOR_STACK_OVERFLOW 2
#define configUSE_MALLOC_FAILED_HOOK 1

/* Features used by the instrument or by the inheritance demonstration. */
#define configUSE_TIMERS 0
#define configUSE_MUTEXES 1
#define configUSE_TASK_NOTIFICATIONS 1
#define configUSE_COUNTING_SEMAPHORES 1
#define configQUEUE_REGISTRY_SIZE 8

/* Run-time statistics. The counter is the same microsecond timer the trace
 * uses, through a helper defined in main.c: the SDK's time_us_32 is a static
 * inline and cannot be declared here. */
#define configUSE_TRACE_FACILITY 1
#define configUSE_STATS_FORMATTING_FUNCTIONS 1
#define configGENERATE_RUN_TIME_STATS 1
uint32_t eos_runtime_counter(void);
#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS()
#define portGET_RUN_TIME_COUNTER_VALUE() eos_runtime_counter()

/* The Pico SDK interop layers are not used, so they cost nothing here. */
#define configSUPPORT_PICO_SYNC_INTEROP 0
#define configSUPPORT_PICO_TIME_INTEROP 0

/* Kernel API surface. Only what the instrument calls is included, so the
 * linked kernel stays small enough to reason about. */
#define INCLUDE_vTaskDelay 1
#define INCLUDE_xTaskDelayUntil 1
#define INCLUDE_vTaskSuspend 1
#define INCLUDE_uxTaskPriorityGet 1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
/* The task number is what the switch hook publishes on the marker pins, so a
 * logic analyzer can name the running task. */
#define INCLUDE_uxTaskGetTaskNumber 1
#define INCLUDE_vTaskSetTaskNumber 1

/* A failed assertion stops with interrupts disabled rather than unwinding:
 * there is no console to unwind to, and a halt keeps the state readable. */
#define configASSERT(x) do { if (!(x)) { \
    portDISABLE_INTERRUPTS(); for (;;) {} \
} } while (0)

#endif

/* Task-switch marker hook. Included last so the scheduler types are resolved.
 * Guarded because this configuration is also preprocessed for assembly. */
#ifndef __ASSEMBLER__
#include "task_marker.h"
#endif
