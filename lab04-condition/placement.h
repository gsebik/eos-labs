/* Task 1: allowed-core masks, not CPU reservations. */
#pragma once
#include <stdint.h>
enum { SCHED_A, SCHED_B, SCHED_C, SCHED_TASK_COUNT };
uint32_t eos_scheduler_mask(unsigned task);
