/* EP-0172 display driver for the ST7796 panel on SPI0.
 *
 * The renderer is intentionally blocking: the display task waits for every
 * transfer it starts. That is the behaviour the timing lab measures, not a
 * claim that DMA would be faster.
 *
 * Only the display task may call these functions. Drawing reads a caller-owned
 * snapshot, so no lock is held during the transfer.
 */
#pragma once
#include "workload.h"
#include <stdint.h>

/* Configure SPI0, reset the controller and clear the panel once.
 * Call before the first draw, from the display task or before it starts. */
void sig_panel_init(void);

/* One column of the per-block history strip: which band the block's peak was
 * in, or `present == 0` when that block never arrived. */
enum { SIG_PANEL_HISTORY = 480 };
typedef struct {
    const uint8_t *band;     /* band index per column, 0..SIG_BANDS-1 */
    const uint8_t *present;  /* 1 when the block was processed, 0 when lost */
    unsigned count;          /* number of columns */
    unsigned lost, late;     /* counters shown beside the tone readout */
} sig_panel_history_t;

/* Draw one complete frame from a caller-owned spectrum.
 *
 * `rows` is the tallest bar in pixels and therefore also the plot height and
 * the pixel-transfer budget: doubling it roughly doubles the redraw time. The
 * caller owns `result`; this function only reads it.
 *
 * @param result spectrum to draw, typically a private snapshot
 * @param rows plot height in pixels, clamped to the panel's usable area
 * @param history per-block history and counters, drawn below the bars
 */
void sig_panel_draw(const spectrum_result_t *result, unsigned rows,
                    const sig_panel_history_t *history);

/* Tell the renderer which tone the generator is producing.
 *
 * The value is drawn as the amber marker under the axis and as a readout, so
 * the picture states the frequency it was taken at. @param hz tone in hertz.
 */
void sig_panel_set_tone(uint32_t hz);

/* The SPI clock actually granted by the SDK, in hertz. Reported because the
 * redraw time follows from it, and a lower clock is easy to mistake for
 * rendering work. */
uint32_t sig_panel_spi_hz(void);
