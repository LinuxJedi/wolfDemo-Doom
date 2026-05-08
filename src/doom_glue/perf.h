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

#if PERF_INSTRUMENT

/* Window metric: sum for averaging, plus the two largest single-frame
 * samples in the window. max2 (second-largest) is reported as a cheap
 * p95 proxy at ~30 samples per 1 s window. */
typedef struct {
    uint64_t sum;
    uint32_t max1;
    uint32_t max2;
} perf_metric_t;

static inline void perf_metric_track(perf_metric_t *m, uint32_t us)
{
    m->sum += us;
    if (us > m->max1) { m->max2 = m->max1; m->max1 = us; }
    else if (us > m->max2) { m->max2 = us; }
}

static inline void perf_metric_reset(perf_metric_t *m)
{
    m->sum = 0;
    m->max1 = 0;
    m->max2 = 0;
}

/* Per-frame us accumulators, written by the subsystem brackets and
 * consumed (then zeroed) by I_FinishUpdate at end-of-frame. */
extern uint32_t perf_clear_us;
extern uint32_t perf_bsp_us;
extern uint32_t perf_paint_us;   /* subset of bsp_us: pixel paint only */
extern uint32_t perf_planes_us;
extern uint32_t perf_sprites_us;
extern uint32_t perf_hud_us;

/* Silent-failure counters. Free-running uint32_t; written from ISRs
 * and main thread, snapshotted at window emit. Single-writer per
 * counter so naked ++ is safe on M33. */
extern uint32_t perf_tic_skip;
extern uint32_t perf_audio_under;
extern uint32_t perf_dma_err;
extern uint32_t perf_spi_trip;

#endif /* PERF_INSTRUMENT */

#endif
