#include "sched_release.h"
#include <stdatomic.h>

typedef struct {
    uint64_t times[SCHED_RELEASE_CAPACITY];
    _Atomic uint32_t head, tail;
    _Atomic bool overflow;
    uint32_t period, next_tick;
} release_history_t;

static release_history_t history[SCHED_RELEASE_TASKS];
_Static_assert(ATOMIC_INT_LOCK_FREE == 2 && ATOMIC_BOOL_LOCK_FREE == 2,
               "Release history must not lock in the kernel tick path");
_Static_assert(SCHED_RELEASE_CAPACITY > 0, "Release history must have capacity");

void eos_sched_release_init(const uint32_t periods[SCHED_RELEASE_TASKS], uint32_t start_tick)
{
    /* Called before the scheduler starts, with no concurrent readers/writer. */
    for (unsigned i = 0; i < SCHED_RELEASE_TASKS; ++i) {
        release_history_t *h = &history[i];
        atomic_store_explicit(&h->head, 0, memory_order_relaxed);
        atomic_store_explicit(&h->tail, 0, memory_order_relaxed);
        atomic_store_explicit(&h->overflow, false, memory_order_relaxed);
        h->period = periods[i];
        h->next_tick = start_tick + periods[i];
    }
}

void eos_sched_release_record(uint32_t tick, uint64_t now_us)
{
    for (unsigned i = 0; i < SCHED_RELEASE_TASKS; ++i) {
        release_history_t *h = &history[i];
        if (!h->period || tick != h->next_tick) continue;
        h->next_tick += h->period; /* unsigned arithmetic also handles tick wrap */
        const uint32_t head = atomic_load_explicit(&h->head, memory_order_relaxed);
        const uint32_t tail = atomic_load_explicit(&h->tail, memory_order_acquire);
        if (head - tail >= SCHED_RELEASE_CAPACITY) {
            atomic_store_explicit(&h->overflow, true, memory_order_release);
            continue;
        }
        h->times[head % SCHED_RELEASE_CAPACITY] = now_us;
        atomic_store_explicit(&h->head, head + 1u, memory_order_release);
    }
}

bool eos_sched_release_take(unsigned task, uint64_t *release_us)
{
    if (task >= SCHED_RELEASE_TASKS || !release_us) return false;
    release_history_t *h = &history[task];
    if (atomic_load_explicit(&h->overflow, memory_order_acquire)) return false;
    const uint32_t tail = atomic_load_explicit(&h->tail, memory_order_relaxed);
    const uint32_t head = atomic_load_explicit(&h->head, memory_order_acquire);
    if (tail == head) return false;
    *release_us = h->times[tail % SCHED_RELEASE_CAPACITY];
    atomic_store_explicit(&h->tail, tail + 1u, memory_order_release);
    return true;
}
