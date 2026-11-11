#include <stdint.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"

/* Cortex-M33 debug registers; this experiment runs on core 0, without an RTOS. */
#define DEMCR      (*(volatile uint32_t *)0xE000EDFCu)
#define DWT_CTRL   (*(volatile uint32_t *)0xE0001000u)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)

/* Probe pin on EP-0172; the analyzer watches the same pulse. */
enum { PROBE = 18 };

/* Two requested spans, one long and one short, so the same fixed error can be
 * compared at two scales. */
enum {
    REQUEST_SHORT_US = 10,
    REQUEST_LONG_US = 500,
    EMPTY_SAMPLES = 100,
    REPORT_PERIOD_MS = 1000,
};

/* Retained for inspection after a run; never time an interval while halted. */
volatile struct {
    uint32_t clock_hz;
    uint32_t dwt_ok;
    uint32_t empty_min;
    uint32_t empty_max;
    uint32_t requested_us;
    uint32_t raw_cycles;
    uint64_t raw_us;
} g_calibration;

int main(void)
{
    stdio_init_all();

    gpio_init(PROBE);
    gpio_put(PROBE, 0);
    gpio_set_dir(PROBE, GPIO_OUT);

    /* TRCENA enables the trace/debug blocks; NOCYCCNT says the counter is
     * missing, and a counter that does not move is treated the same way. */
    DEMCR |= 1u << 24;
    bool dwt_ok = !(DWT_CTRL & (1u << 25));
    if (dwt_ok) {
        DWT_CYCCNT = 0;
        DWT_CTRL |= 1u;

        const uint32_t before = DWT_CYCCNT;
        busy_wait_us_32(1);
        dwt_ok = DWT_CYCCNT != before;
    }

    for (uint32_t run = 0;; ++run) {
        /* Back-to-back counter reads: the harness's own baseline, measured,
         * not assumed. */
        uint32_t minimum = UINT32_MAX;
        uint32_t maximum = 0;
        for (unsigned i = 0; dwt_ok && i < EMPTY_SAMPLES; ++i) {
            const uint32_t start = DWT_CYCCNT;
            const uint32_t end = DWT_CYCCNT;
            const uint32_t span = end - start;
            if (span < minimum) {
                minimum = span;
            }
            if (span > maximum) {
                maximum = span;
            }
        }

        /* Alternate the requested length; the timer reads sit outside the DWT
         * window, so the two instruments measure different boundaries. */
        const uint32_t requested = (run & 1u) ? REQUEST_LONG_US : REQUEST_SHORT_US;
        const uint64_t us_start = time_us_64();
        const uint32_t start = dwt_ok ? DWT_CYCCNT : 0;

        gpio_put(PROBE, 1);
        busy_wait_us_32(requested);
        gpio_put(PROBE, 0);

        const uint32_t end = dwt_ok ? DWT_CYCCNT : 0;
        const uint64_t us_end = time_us_64();

        g_calibration.clock_hz = clock_get_hz(clk_sys);
        g_calibration.dwt_ok = dwt_ok;
        g_calibration.empty_min = dwt_ok ? minimum : 0;
        g_calibration.empty_max = maximum;
        g_calibration.requested_us = requested;
        g_calibration.raw_cycles = end - start;
        g_calibration.raw_us = us_end - us_start;

        printf("cal clock_hz=%lu dwt_ok=%u empty_min=%lu empty_max=%lu "
               "request_us=%lu raw_cycles=%lu outer_us=%llu\n",
               (unsigned long)g_calibration.clock_hz, (unsigned)dwt_ok,
               (unsigned long)g_calibration.empty_min,
               (unsigned long)g_calibration.empty_max,
               (unsigned long)g_calibration.requested_us,
               (unsigned long)g_calibration.raw_cycles,
               (unsigned long long)g_calibration.raw_us);

        /* Reporting and waiting are outside all measured spans. */
        sleep_ms(REPORT_PERIOD_MS);
    }
}
