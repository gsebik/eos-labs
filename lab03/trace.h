/* Bounded on-target event recorder.
 *
 * The instrument timestamps its own progress into a fixed array in RAM, which
 * is later dumped with a debugger and decoded on the host. Recording costs a
 * few microseconds per event, and that cost is part of what is being measured.
 *
 * The window is deliberately bounded: it ends at the first deadline miss
 * (frozen) or when the array is full (truncated). A dump therefore holds the
 * events that led to the interesting moment, rather than the newest events
 * with a hole in the middle.
 */
#pragma once
#include <stdint.h>
#include "workload.h"

enum { SIG_TRACE_CAPACITY = 2048 };

/* Event kinds, written as the `event` field of a record.
 *
 * Events 1-4 belong to one input buffer and define its lifecycle. Event 11
 * closes the separate publication obligation and is deliberately not part of
 * buffer ownership; 5-10 are supporting observations. A reader must not
 * reconstruct one buffer's lifetime from records of different jobs.
 */
enum {
    SIG_BLOCK_READY = 1,     /* the DMA interrupt recorded a finished block */
    SIG_WORK_START,          /* processing begins this block */
    SIG_WORK_COMPLETE,       /* FFT and the final sample read have ended */
    SIG_BLOCK_RELEASE,       /* the sample buffer is FREE again */
    SIG_DROP,                /* a block was lost, with the reason in `arg` */
    SIG_DISPLAY_START,       /* display task begins a frame */
    SIG_DISPLAY_DONE,        /* frame transfer finished */
    SIG_SETTINGS_WRITE,      /* settings task published new values */
    SIG_SETTINGS_WAIT,       /* a task waited for the settings lock */
    SIG_PUBLISH_WAIT,        /* a task waited for the spectrum lock */
    SIG_RESULT_PUBLISHED,    /* the result is copied and the lock released */
    /* Who owned the spectrum lock, and for how long. `job` carries the task
     * number of the owner, `arg` the hold time in microseconds, so a wait can
     * be attributed to a task instead of guessed from the source. */
    SIG_SPECTRUM_TAKE,
    SIG_SPECTRUM_GIVE
};

/* One record: 24 bytes, little-endian, as the host decoder expects. */
typedef struct {
    uint64_t t_us;   /* time_us_64() at the call */
    uint32_t event;  /* one of the SIG_* event kinds above */
    uint32_t job;    /* block number this record belongs to, 0 if none */
    uint32_t arg;    /* per-event payload: buffer id, duration, reason */
    uint32_t aux;    /* secondary payload, or 1 for a wait record */
} sig_event_t;

/* Metadata words, dumped with the buffer. Read them to know what the dump is:
 * how many records are valid, whether the array filled, and whether recording
 * stopped on a deliberate freeze. */
enum { SIG_TRACE_COUNT, SIG_TRACE_TRUNCATED, SIG_TRACE_FROZEN };
extern volatile sig_event_t g_sig_trace[SIG_TRACE_CAPACITY];
extern volatile uint32_t g_sig_trace_metadata[3];
#define g_sig_trace_count g_sig_trace_metadata[SIG_TRACE_COUNT]
#define g_sig_trace_truncated g_sig_trace_metadata[SIG_TRACE_TRUNCATED]
#define g_sig_trace_frozen g_sig_trace_metadata[SIG_TRACE_FROZEN]

/* Append one record and return the timestamp it was given.
 *
 * Safe to call from the DMA interrupt and from task context: interrupts are
 * masked only around the array update. Once the window is closed the call
 * still returns a timestamp, so callers can keep measuring, but nothing more
 * is stored.
 *
 * @param event one of the SIG_* event kinds
 * @param job block number, or 0 for events that belong to no single block
 * @param arg per-event payload
 * @param aux secondary payload
 * @return the microsecond timestamp written into the record
 */
uint64_t sig_trace(uint32_t event, uint32_t job, uint32_t arg, uint32_t aux);

/* Close the window and keep what has been recorded.
 *
 * Called when a deadline miss is detected, so a later dump ends at the events
 * that led to it instead of being overwritten by blocks after it.
 */
void sig_trace_freeze(void);
