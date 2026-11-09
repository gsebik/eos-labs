#include "sched_trace.h"
#if EOS_SCHED_MODE && EOS_SCHED_TRACE
#include <stdatomic.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/sync.h"

enum { TRACE_CAPACITY = 1024, TRACE_WINDOW_US = 150000 };
typedef struct {
    uint64_t t_us, release_us, deadline_us;
    uint32_t job;
    uint8_t task, kind;
} sched_event_t;
/* A separate append-only array per core. Local IRQ masking serialises its
 * task events and switch hooks. Publishing count makes complete records
 * available to the later reader. Records are never overwritten in a run.
 */
static sched_event_t events[2][TRACE_CAPACITY];
static _Atomic uint32_t counts[2], drops[2];
static uint64_t begin_us, end_us;
static bool dumped;
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "Trace counters must be lock-free");

void eos_sched_trace_init(void)
{
    begin_us = time_us_64();
    end_us = begin_us + TRACE_WINDOW_US;
}

static uint64_t append(unsigned task, unsigned kind, uint32_t job,
                       uint64_t release, uint64_t deadline)
{
    const uint32_t irq = save_and_disable_interrupts();
    const unsigned core = get_core_num();
    const uint64_t now = time_us_64();
    if (end_us && now >= begin_us && now < end_us) {
        const uint32_t n = atomic_load_explicit(&counts[core], memory_order_relaxed);
        if (n < TRACE_CAPACITY) {
            events[core][n] = (sched_event_t){now, release, deadline, job,
                                            (uint8_t)task, (uint8_t)kind};
            atomic_store_explicit(&counts[core], n + 1u, memory_order_release);
        } else {
            atomic_fetch_add_explicit(&drops[core], 1u, memory_order_relaxed);
        }
    }
    restore_interrupts(irq);
    return now;
}

void eos_sched_trace_switch(unsigned task, unsigned kind)
{
    append(task, kind, 0, 0, 0);
}

uint64_t eos_sched_trace_job(unsigned kind, unsigned task, uint32_t job,
                             uint64_t release, uint64_t deadline)
{
    return append(task, kind, job, release, deadline);
}

void eos_sched_trace_dump(void)
{
    if (dumped || time_us_64() < end_us) return;
    dumped = true;
    static const char *names[] = {"idle", "A", "B", "C", "hb", "report"};
    static const char *kinds[] = {"invalid", "in", "out", "start", "finish"};
    printf("sched_trace,begin_us=%llu,end_us=%llu,cores=%u,capacity_per_core=%u\n",
           (unsigned long long)begin_us, (unsigned long long)end_us,
           (unsigned)EOS_CORES, TRACE_CAPACITY);
    uint32_t total = 0, lost = 0;
    for (unsigned core = 0; core < EOS_CORES; ++core) {
        const uint32_t n = atomic_load_explicit(&counts[core], memory_order_acquire);
        lost += atomic_load_explicit(&drops[core], memory_order_relaxed);
        for (uint32_t i = 0; i < n; ++i) {
            const sched_event_t *e = &events[core][i];
            const char *name = e->task < 6 ? names[e->task] : "unknown";
            printf("sched_event,t_us=%llu,core=%u,seq=%lu,task=%s,kind=%s,job=%lu,release_us=%llu,deadline_us=%llu\n",
                   (unsigned long long)e->t_us, core, (unsigned long)i,
                   name, kinds[e->kind], (unsigned long)e->job,
                   (unsigned long long)e->release_us,
                   (unsigned long long)e->deadline_us);
        }
        total += n;
    }
    printf("sched_trace_end,n=%lu,dropped=%lu\n", (unsigned long)total,
           (unsigned long)lost);
}
#endif
