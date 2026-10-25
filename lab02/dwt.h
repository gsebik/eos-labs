#pragma once

#include <stdint.h>

/* ARMv8-M DWT cycle counter addresses used by the RP2350 Cortex-M33.
 * A finite CYCCNT measurement wraps modulo 2^32; keep intervals short or use
 * unsigned subtraction. This does not calibrate clock changes or prove WCET. */
#define EOS_COREDEBUG_DEMCR (*(volatile uint32_t *)0xE000EDFCu)
#define EOS_DWT_CTRL        (*(volatile uint32_t *)0xE0001000u)
#define EOS_DWT_CYCCNT      (*(volatile uint32_t *)0xE0001004u)
#define EOS_DEMCR_TRCENA    (1u << 24)
#define EOS_DWT_CYCCNTENA   (1u << 0)

static inline void eos_dwt_init(void)
{
    EOS_COREDEBUG_DEMCR |= EOS_DEMCR_TRCENA;
    EOS_DWT_CYCCNT = 0u;
    EOS_DWT_CTRL |= EOS_DWT_CYCCNTENA;
}

static inline uint32_t eos_dwt_cycles(void)
{
    return EOS_DWT_CYCCNT;
}

static inline uint32_t eos_dwt_elapsed(uint32_t start, uint32_t end)
{
    return end - start;
}
