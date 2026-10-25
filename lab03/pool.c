/* Sample-buffer ownership states: implementation.
 *
 * Both delivery paths share the same state machine but not the same queue:
 * the foreground target uses the READY ring in kit_pool_t, while the RTOS
 * target delivers descriptors through a FreeRTOS queue and moves one named
 * buffer at a time with kit_pool_ready/begin/release.
 *
 * None of these functions mask interrupts or take a lock; the caller does.
 */
#include "pool.h"
#include <string.h>

void kit_pool_init(kit_pool_t *pool)
{
    memset(pool, 0, sizeof *pool);
    for (unsigned i = 0; i < KIT_POOL_SIZE; ++i) pool->state[i] = KIT_FREE;
}

/* First FREE buffer becomes the DMA destination. Linear scan is fine: the pool
 * is three entries and the function runs in an interrupt. */
int kit_pool_dma(kit_pool_t *pool)
{
    for (unsigned i = 0; i < KIT_POOL_SIZE; ++i) {
        if (pool->state[i] == KIT_FREE) {
            pool->state[i] = KIT_DMA;
            return (int)i;
        }
    }
    return -1;
}

bool kit_pool_complete(kit_pool_t *pool, unsigned id)
{
    if (id >= KIT_POOL_SIZE || pool->state[id] != KIT_DMA
        || pool->count >= KIT_POOL_SIZE) return false;
    pool->state[id] = KIT_READY;
    pool->queue[pool->write] = id;
    pool->write = (pool->write + 1) % KIT_POOL_SIZE;
    pool->count++;
    return true;
}

int kit_pool_take(kit_pool_t *pool)
{
    if (!pool->count) return -1;
    const unsigned id = pool->queue[pool->read];
    if (id >= KIT_POOL_SIZE || pool->state[id] != KIT_READY) return -2;
    pool->read = (pool->read + 1) % KIT_POOL_SIZE;
    pool->count--;
    pool->state[id] = KIT_PROCESSING;
    return (int)id;
}

bool kit_pool_release(kit_pool_t *pool, unsigned id)
{
    if (id >= KIT_POOL_SIZE || pool->state[id] != KIT_PROCESSING) return false;
    pool->state[id] = KIT_FREE;
    return true;
}

/* RTOS delivery: the DMA finished this buffer, so it becomes READY. The ring
 * in kit_pool_t is not used here; the descriptor is the queue entry. */
bool kit_pool_ready(kit_pool_t *pool, unsigned id)
{
    if (id >= KIT_POOL_SIZE || pool->state[id] != KIT_DMA) return false;
    pool->state[id] = KIT_READY;
    pool->count++;
    return true;
}

/* A waiting reader has taken the descriptor, so the buffer is now in use. */
bool kit_pool_begin(kit_pool_t *pool, unsigned id)
{
    if (id >= KIT_POOL_SIZE || pool->state[id] != KIT_READY) return false;
    pool->state[id] = KIT_PROCESSING;
    pool->count--;
    return true;
}
