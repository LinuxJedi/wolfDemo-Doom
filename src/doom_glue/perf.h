#ifndef PERF_H
#define PERF_H

#include <stdint.h>
#include "stm32u585xx.h"

/* Set to 0 to compile out all instrumentation. */
#define PERF_INSTRUMENT 1

/* Enable the M33 DWT cycle counter. Call once at startup after SYSCLK
 * is at its final rate; perf_us_since assumes 160 MHz. */
static inline void perf_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
}

static inline uint32_t perf_cyc(void)
{
    return DWT->CYCCNT;
}

static inline uint32_t perf_us_since(uint32_t t0)
{
    return (perf_cyc() - t0) / 160u;
}

#endif
