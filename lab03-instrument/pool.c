#include "pool.h"
#include <string.h>

void kit_pool_init(kit_pool_t *pool)
{
    memset(pool, 0, sizeof *pool);
    for (unsigned i = 0; i < KIT_POOL_SIZE; ++i) pool->state[i] = KIT_FREE;
}
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
