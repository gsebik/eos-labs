#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H
#include <stdint.h>
#define configUSE_PREEMPTION 1
#define configUSE_TIME_SLICING 1
#define configTICK_RATE_HZ 1000
#define configMAX_PRIORITIES 7
#define configMINIMAL_STACK_SIZE 256
#define configUSE_16_BIT_TICKS 0
#define configIDLE_SHOULD_YIELD 1
#define configUSE_IDLE_HOOK 0
#define configUSE_TICK_HOOK 0
#define configENABLE_FPU 1
#define configENABLE_MPU 0
#define configENABLE_TRUSTZONE 0
#define configRUN_FREERTOS_SECURE_ONLY 1
#define configMAX_SYSCALL_INTERRUPT_PRIORITY 16
#define configNUMBER_OF_CORES 1
#define configSUPPORT_DYNAMIC_ALLOCATION 1
/* The instrument's ready-descriptor queue is statically allocated, so that a
 * failed creation cannot be blamed on heap exhaustion. */
#define configSUPPORT_STATIC_ALLOCATION 1
#define configTOTAL_HEAP_SIZE (64 * 1024)
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
/* Run-time stats use the same microsecond timer as the trace, through a helper
 * defined in main.c: the SDK's own time_us_32 is a static inline and cannot be
 * declared here. */
uint32_t eos_runtime_counter(void);
#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS()
#define portGET_RUN_TIME_COUNTER_VALUE() eos_runtime_counter()
#define configSUPPORT_PICO_SYNC_INTEROP 0
#define configSUPPORT_PICO_TIME_INTEROP 0
#define configCPU_CLOCK_HZ 150000000
#define INCLUDE_vTaskDelay 1
#define INCLUDE_xTaskDelayUntil 1
#define INCLUDE_vTaskSuspend 1
#define INCLUDE_uxTaskPriorityGet 1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
/* The task-number field is what the switch hook publishes on the marker pins,
 * so that a logic analyzer can name the running task. */
#define INCLUDE_uxTaskGetTaskNumber 1
#define INCLUDE_vTaskSetTaskNumber 1
#define configASSERT(x) do { if (!(x)) { \
    portDISABLE_INTERRUPTS(); for (;;) {} \
} } while (0)
#endif

/* Task-switch marker hook. Included last so the scheduler types are resolved.
 * Guarded because this configuration is also preprocessed for assembly. */
#ifndef __ASSEMBLER__
#include "task_marker.h"
#endif
