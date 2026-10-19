#pragma once
#include <stdint.h>
#include "workload.h"

enum { SIG_TRACE_CAPACITY = 2048 };
/* Lifecycle events 1-4 belong to one input buffer. Event 11 closes the separate
 * publication obligation and is deliberately not part of buffer ownership.
 * Events 5-10 are the supporting observations. */
enum {
    SIG_BLOCK_READY = 1, SIG_WORK_START, SIG_WORK_COMPLETE, SIG_BLOCK_RELEASE,
    SIG_DROP, SIG_DISPLAY_START, SIG_DISPLAY_DONE, SIG_SETTINGS_WRITE,
    SIG_SETTINGS_WAIT, SIG_PUBLISH_WAIT, SIG_RESULT_PUBLISHED
};

typedef struct {
    uint64_t t_us;
    uint32_t event, job, arg, aux;
} sig_event_t;

/* Metadata words, dumped with the buffer. The recorder captures a bounded
 * window: it stops at the first miss (frozen) or when the buffer fills
 * (truncated), so a dump always ends at a meaningful point instead of losing
 * the interesting events to overflow. */
enum { SIG_TRACE_COUNT, SIG_TRACE_TRUNCATED, SIG_TRACE_FROZEN };
extern volatile sig_event_t g_sig_trace[SIG_TRACE_CAPACITY];
extern volatile uint32_t g_sig_trace_metadata[3];
#define g_sig_trace_count g_sig_trace_metadata[SIG_TRACE_COUNT]
#define g_sig_trace_truncated g_sig_trace_metadata[SIG_TRACE_TRUNCATED]
#define g_sig_trace_frozen g_sig_trace_metadata[SIG_TRACE_FROZEN]

/* Bounded recorder. It masks interrupts briefly, so it is part of the
 * measurement's disturbance. Returns the serialized timestamp. */
uint64_t sig_trace(uint32_t event, uint32_t job, uint32_t arg, uint32_t aux);

/* Close the window: stop recording and keep what we have. Called when a
 * deadline miss is detected, so the dump contains the events that led to it. */
void sig_trace_freeze(void);
