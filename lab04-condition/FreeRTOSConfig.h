/* FreeRTOS configuration for the L4 condition monitor prototype.
 *
 * Based on the L3 instrument configuration. The L4 additions are the SMP
 * switches and the core-affinity API used by the placement experiments.
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H
#include <stdint.h>

#ifndef EOS_CORES
#define EOS_CORES 1
#endif

#define configUSE_PREEMPTION 1
#define configUSE_TIME_SLICING 1
#define configTICK_RATE_HZ 1000
#define configMAX_PRIORITIES 7
#define configMINIMAL_STACK_SIZE 256
#define configUSE_16_BIT_TICKS 0
#define configIDLE_SHOULD_YIELD 1
#define configUSE_IDLE_HOOK 0
#define configUSE_TICK_HOOK 0

#if !defined(__ASSEMBLER__) && EOS_SCHED_MODE
/* FreeRTOS V11.3.1 calls this before incrementing xTickCount and before
 * unblocking tasks. Skip the suspended calls: the kernel processes those
 * pended ticks later in xTaskResumeAll. Do not use the end-of-tick hook here:
 * an SMP task may already have been dispatched on the other core by then.
 */
void eos_sched_release_tick(uint32_t tick);
#define traceTASK_INCREMENT_TICK(tick) do { \
    if (uxSchedulerSuspended == 0u) eos_sched_release_tick((uint32_t)(tick) + 1u); \
} while (0)
#endif

#define configENABLE_FPU 1
#define configENABLE_MPU 0
#define configENABLE_TRUSTZONE 0
#define configRUN_FREERTOS_SECURE_ONLY 1
#define configMAX_SYSCALL_INTERRUPT_PRIORITY 16

#if EOS_CORES == 2
#define configNUMBER_OF_CORES 2
#define configUSE_CORE_AFFINITY 1
#define configRUN_MULTIPLE_PRIORITIES 1
#define configUSE_TASK_PREEMPTION_DISABLE 0
#define configUSE_PASSIVE_IDLE_HOOK 0
#else
#define configNUMBER_OF_CORES 1
#endif

#ifndef EOS_TARGET_KHZ
#define EOS_TARGET_KHZ 150000
#endif
#define configCPU_CLOCK_HZ (EOS_TARGET_KHZ * 1000u)

#define configSUPPORT_DYNAMIC_ALLOCATION 1
#define configSUPPORT_STATIC_ALLOCATION 1
#define configTOTAL_HEAP_SIZE (96 * 1024)
#define configCHECK_FOR_STACK_OVERFLOW 2
#define configUSE_MALLOC_FAILED_HOOK 1

#define configUSE_TIMERS 0
#define configUSE_MUTEXES 1
#define configUSE_TASK_NOTIFICATIONS 1
#define configUSE_COUNTING_SEMAPHORES 1
#define configQUEUE_REGISTRY_SIZE 8

#define configUSE_TRACE_FACILITY 1
#define configUSE_STATS_FORMATTING_FUNCTIONS 1
#define configGENERATE_RUN_TIME_STATS 1
uint32_t eos_runtime_counter(void);
void eos_runtime_counter_start(void);
#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS() eos_runtime_counter_start()
#define portGET_RUN_TIME_COUNTER_VALUE() eos_runtime_counter()

#define configSUPPORT_PICO_SYNC_INTEROP 0
#define configSUPPORT_PICO_TIME_INTEROP 0

#define INCLUDE_vTaskDelay 1
#define INCLUDE_xTaskDelayUntil 1
#define INCLUDE_vTaskSuspend 1
#define INCLUDE_uxTaskPriorityGet 1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define INCLUDE_uxTaskGetTaskNumber 1
#define INCLUDE_vTaskSetTaskNumber 1

#define configASSERT(x) do { if (!(x)) { \
    portDISABLE_INTERRUPTS(); for (;;) {} \
} } while (0)

#endif

#ifndef __ASSEMBLER__
#include "task_marker.h"
#endif
