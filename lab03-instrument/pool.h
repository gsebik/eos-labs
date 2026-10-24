/* Sample-buffer ownership states.
 *
 * The pool owns storage, not data: it decides which buffer the DMA may write
 * into and which ones a reader currently holds. Every transition is explicit,
 * so a buffer that is being read can never become a DMA destination.
 *
 * The caller serializes all operations (the DMA interrupt and the processing
 * task mask interrupts around each call), so this module holds no lock itself.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { KIT_POOL_SIZE = 3 };
enum {
    KIT_FREE,        /* available to the DMA */
    KIT_DMA,         /* being filled by the DMA */
    KIT_READY,       /* complete, waiting for a reader */
    KIT_PROCESSING,  /* owned by a reader; must not be reassigned */
};

/* State, ring of READY indices, and the ring's read/write positions.
 * Zero-initialize, then call kit_pool_init(). */
typedef struct {
    uint32_t state[KIT_POOL_SIZE];
    uint32_t queue[KIT_POOL_SIZE];
    uint32_t read, write, count;
} kit_pool_t;

/* Mark every buffer FREE and empty the READY ring. */
void kit_pool_init(kit_pool_t *pool);

/* Claim a FREE buffer for the DMA.
 * @return the buffer index, or -1 when every buffer is in use
 */
int kit_pool_dma(kit_pool_t *pool);

/* Publish a buffer the DMA has finished filling (foreground ring path).
 * @return false when the buffer is not in the DMA state or the ring is full
 */
bool kit_pool_complete(kit_pool_t *pool, unsigned id);

/* Take the oldest READY buffer for processing (foreground ring path).
 * @return the buffer index, -1 when the ring is empty, -2 when its state does
 *         not match the ring entry
 */
int kit_pool_take(kit_pool_t *pool);

/* Direct transitions for the queue-based target. That target delivers
 * descriptors through a FreeRTOS queue, so it must not also enqueue into the
 * ring above; these move one named buffer instead. */
bool kit_pool_ready(kit_pool_t *pool, unsigned id);   /* DMA  -> READY      */
bool kit_pool_begin(kit_pool_t *pool, unsigned id);   /* READY -> PROCESSING */
bool kit_pool_release(kit_pool_t *pool, unsigned id); /* PROCESSING -> FREE  */
