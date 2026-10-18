#include "trace.h"
#include "pico/stdlib.h"
#include "hardware/sync.h"

volatile sig_event_t g_sig_trace[SIG_TRACE_CAPACITY];
volatile uint32_t g_sig_trace_metadata[3];
_Static_assert(sizeof(sig_event_t) == 24, "Host decoder expects 24-byte records");

uint64_t sig_trace(uint32_t event, uint32_t job, uint32_t arg, uint32_t aux)
{
    const uint32_t saved = save_and_disable_interrupts();
    const uint64_t now = time_us_64();
    if (g_sig_trace_frozen) {
        restore_interrupts(saved);
        return now;
    }
    if (g_sig_trace_count < SIG_TRACE_CAPACITY) {
        g_sig_trace[g_sig_trace_count++] = (sig_event_t){
            now, event, job, arg, aux
        };
    } else {
        /* The window closed at capacity. Stop rather than count losses: the
         * dump then holds a complete, bounded prefix instead of a holey tail. */
        g_sig_trace_truncated = 1;
        g_sig_trace_frozen = 1;
    }
    restore_interrupts(saved);
    return now;
}

void sig_trace_freeze(void)
{
    const uint32_t saved = save_and_disable_interrupts();
    g_sig_trace_frozen = 1;
    restore_interrupts(saved);
}
