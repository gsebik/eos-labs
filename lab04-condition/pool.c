/* Sample-buffer ownership states: implementation.
 *
 * Both delivery paths share the same state machine but not the same queue:
 * the foreground target uses the READY ring in kit_pool_t, while the RTOS
 * target delivers descriptors through a FreeRTOS queue and moves one named
 * buffer at a time with kit_pool_ready/begin/release.
 *
 * The RTOS build is SMP: the DMA ISR runs on core 0 and a processing task may
 * run on either core, so the state machine is protected by a cross-core
 * critical section. The foreground build pays the same short lock; it is small.
 */
#include "pool.h"
#include <string.h>
#include "pico/sync.h"

static critical_section_t pool_lock;

void kit_pool_lock_init(void)
{
    critical_section_init(&pool_lock);
}

void kit_pool_init(kit_pool_t *pool)
{
    critical_section_enter_blocking(&pool_lock);
    memset(pool, 0, sizeof *pool);
    for (unsigned i = 0; i < KIT_POOL_SIZE; ++i) pool->state[i] = KIT_FREE;
    critical_section_exit(&pool_lock);
}

/* First FREE buffer becomes the DMA destination. Linear scan is fine: the pool
 * is three entries and the function runs in an interrupt. */
int kit_pool_dma(kit_pool_t *pool)
{
    critical_section_enter_blocking(&pool_lock);
    int result = -1;
    for (unsigned i = 0; i < KIT_POOL_SIZE; ++i) {
        if (pool->state[i] == KIT_FREE) {
            pool->state[i] = KIT_DMA;
            result = (int)i;
            break;
        }
    }
    critical_section_exit(&pool_lock);
    return result;
}

bool kit_pool_complete(kit_pool_t *pool, unsigned id)
{
    critical_section_enter_blocking(&pool_lock);
    bool ok = !(id >= KIT_POOL_SIZE || pool->state[id] != KIT_DMA
                || pool->count >= KIT_POOL_SIZE);
    if (ok) {
        pool->state[id] = KIT_READY;
        pool->queue[pool->write] = id;
        pool->write = (pool->write + 1) % KIT_POOL_SIZE;
        pool->count++;
    }
    critical_section_exit(&pool_lock);
    return ok;
}

int kit_pool_take(kit_pool_t *pool)
{
    critical_section_enter_blocking(&pool_lock);
    int result = -1;
    if (pool->count) {
        const unsigned id = pool->queue[pool->read];
        if (id >= KIT_POOL_SIZE || pool->state[id] != KIT_READY) {
            result = -2;
        } else {
            pool->read = (pool->read + 1) % KIT_POOL_SIZE;
            pool->count--;
            pool->state[id] = KIT_PROCESSING;
            result = (int)id;
        }
    }
    critical_section_exit(&pool_lock);
    return result;
}

bool kit_pool_release(kit_pool_t *pool, unsigned id)
{
    critical_section_enter_blocking(&pool_lock);
    bool ok = !(id >= KIT_POOL_SIZE || pool->state[id] != KIT_PROCESSING);
    if (ok) pool->state[id] = KIT_FREE;
    critical_section_exit(&pool_lock);
    return ok;
}

/* RTOS delivery: the DMA finished this buffer, so it becomes READY. The ring
 * in kit_pool_t is not used here; the descriptor is the queue entry. */
bool kit_pool_ready(kit_pool_t *pool, unsigned id)
{
    critical_section_enter_blocking(&pool_lock);
    bool ok = !(id >= KIT_POOL_SIZE || pool->state[id] != KIT_DMA);
    if (ok) {
        pool->state[id] = KIT_READY;
        pool->count++;
    }
    critical_section_exit(&pool_lock);
    return ok;
}

/* A waiting reader has taken the descriptor, so the buffer is now in use. */
bool kit_pool_begin(kit_pool_t *pool, unsigned id)
{
    critical_section_enter_blocking(&pool_lock);
    bool ok = !(id >= KIT_POOL_SIZE || pool->state[id] != KIT_READY);
    if (ok) {
        pool->state[id] = KIT_PROCESSING;
        pool->count--;
    }
    critical_section_exit(&pool_lock);
    return ok;
}
