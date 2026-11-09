#ifndef EOS_SCHED_RELEASE_H
#define EOS_SCHED_RELEASE_H

#include <stdbool.h>
#include <stdint.h>

enum { SCHED_RELEASE_TASKS = 3 };
#ifndef SCHED_RELEASE_CAPACITY
#define SCHED_RELEASE_CAPACITY 1024u
#endif

/* Supplied measurement support, not the student's Task 3 ring exercise.
 * One tick-path producer and one consumer per task. Keep old releases when a
 * job is backlogged; never replace them with its eventual dispatch time.
 */
void eos_sched_release_init(const uint32_t periods[SCHED_RELEASE_TASKS], uint32_t start_tick);
void eos_sched_release_record(uint32_t tick, uint64_t now_us);
bool eos_sched_release_take(unsigned task, uint64_t *release_us);

#endif
