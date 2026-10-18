#pragma once
#include <stdbool.h>
#include <stdint.h>
enum { KIT_POOL_SIZE = 3 };
enum { KIT_FREE, KIT_DMA, KIT_READY, KIT_PROCESSING };
/* Caller serializes operations. Zero-initialize, then call kit_pool_init. */
typedef struct {
    uint32_t state[KIT_POOL_SIZE], queue[KIT_POOL_SIZE], read, write, count;
} kit_pool_t;
void kit_pool_init(kit_pool_t *pool);
int kit_pool_dma(kit_pool_t *pool);
bool kit_pool_complete(kit_pool_t *pool, unsigned id);
int kit_pool_take(kit_pool_t *pool);
bool kit_pool_release(kit_pool_t *pool, unsigned id);
