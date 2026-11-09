/* Supplied bounded recorder. No UART printing in the measured trace window.
 * Switch segments measure scheduled residency (including IRQs charged to the
 * interrupted task), not a proved per-job CPU demand or WCET.
 */
#pragma once
#include <stdint.h>
#include "pico/time.h"
#ifndef EOS_SCHED_TRACE
#define EOS_SCHED_TRACE 0
#endif
enum { SCHED_IN = 1, SCHED_OUT, SCHED_JOB_START, SCHED_JOB_FINISH };
#if EOS_SCHED_MODE && EOS_SCHED_TRACE
void eos_sched_trace_init(void);
void eos_sched_trace_switch(unsigned task, unsigned kind);
uint64_t eos_sched_trace_job(unsigned kind, unsigned task, uint32_t job,
                             uint64_t release_us, uint64_t deadline_us);
void eos_sched_trace_dump(void);
#else
static inline void eos_sched_trace_init(void) {}
static inline void eos_sched_trace_dump(void) {}
static inline uint64_t eos_sched_trace_job(unsigned kind, unsigned task,
        uint32_t job, uint64_t release_us, uint64_t deadline_us)
{
    (void)kind; (void)task; (void)job; (void)release_us; (void)deadline_us;
    return time_us_64();
}
#endif
