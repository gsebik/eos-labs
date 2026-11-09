/* Task 1: bit 0 permits core 0, bit 1 permits core 1.
 * 1 = core 0, 2 = core 1, 3 = either. Eligibility is not a CPU reservation.
 */
#include "placement.h"

/* Supplied candidate: A/B on core 0, C on core 1.
 * TODO Task 1: after analysing your alternative, change these three entries.
 * EOS_SCHED_PLACEMENT=1 selects this table. Leave the supplied drivers alone.
 */
static const uint32_t partition[SCHED_TASK_COUNT] = {
    [SCHED_A] = 1u,
    [SCHED_B] = 1u,
    [SCHED_C] = 2u,
};

uint32_t eos_scheduler_mask(unsigned task)
{
#if EOS_CORES == 1
    (void)task;
    return 1u;
#else
    return EOS_SCHED_PLACEMENT == 1 ? partition[task] : 3u;
#endif
}
