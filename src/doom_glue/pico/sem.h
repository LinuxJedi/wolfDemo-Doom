/*
 * Stand-in for the Pico SDK's pico/sem.h.
 *
 * doom/src/i_system.h does (when DOOM_TINY=1):
 *   #include "pico/sem.h"
 *   extern semaphore_t render_frame_ready, display_frame_freed;
 *
 * Those semaphores coordinate the producer/consumer handoff between
 * the two RP2040 cores (one rasterising scanlines, one feeding the PIO
 * scanvideo). The wolfDemo board is single-core; the engine will
 * always run with the rendering and display on the same thread, so
 * these are effectively unused. We just need a struct definition that
 * makes the `extern semaphore_t ...` declarations well-formed.
 *
 * If/when the engine actually calls sem_init / sem_acquire_blocking on
 * these, fill in real implementations using __WFE / SEV or a simple
 * counter; for the bring-up the link should never resolve those calls.
 */

#ifndef WOLFDEMO_PICO_SEM_H
#define WOLFDEMO_PICO_SEM_H

#include <stdint.h>

typedef struct {
    int permits;
    int max_permits;
} semaphore_t;

#endif /* WOLFDEMO_PICO_SEM_H */
