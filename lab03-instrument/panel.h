#pragma once
#include "workload.h"
#include <stdint.h>

void sig_panel_init(void);
void sig_panel_draw(const spectrum_result_t *result, unsigned rows);
/* Report the generated tone so the panel can show it. */
void sig_panel_set_tone(uint32_t hz);
uint32_t sig_panel_spi_hz(void);
