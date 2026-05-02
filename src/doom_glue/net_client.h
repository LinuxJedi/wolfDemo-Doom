/*
 * Shadow of doom/src/net_client.h tailored for our NO_USE_NET=1 build.
 *
 * The upstream header turns net_client_connected into `#define ... false`
 * when NO_USE_NET=1, which breaks d_loop.c:706 (a DOOM_TINY-only branch
 * that does `net_client_connected = false; piconet_stop();` without a
 * NO_USE_NET gate -- the macro substitution turns it into `false = false`).
 *
 * `#include "..."` searches the including file's directory first, so a
 * file under src/doom_glue/ never wins over doom/src/net_client.h. To
 * actually shadow, this header is force-loaded via `-include` from the
 * Makefile. By taking the NET_CLIENT_H include guard up front, any
 * later `#include "net_client.h"` from engine code becomes a no-op.
 *
 * Kept minimal (no upstream header dependencies) so it can be safely
 * pre-included into every TU, including our own main.c.
 */

/* The file is force-loaded by `-include` from the Makefile, so it
 * doubles as the bare-metal compatibility prelude for the entire
 * engine build. Beyond shadowing net_client.h, it also provides a few
 * Pico SDK typedefs and small helpers that the engine assumes are in
 * scope (the Pico SDK itself injects these via pico/types.h). */

#ifndef NET_CLIENT_H
#define NET_CLIENT_H

#include <stdbool.h>

/* Pico SDK convenience typedef. r_state.h / v_video.c use it bare. */
typedef unsigned int uint;

/* Pico SDK noreturn used by v_video.c and friends in unreachable
 * branches. Routes to our panic() so a bug surfaces on UART. */
extern void panic_unsupported(void) __attribute__((noreturn));

/* RP2040 fast hardware multiply intrinsic. The Cortex-M33 has a
 * single-cycle 32x32 multiplier, so a plain * is just as fast. */
static inline int __fast_mul(int a, int b) { return a * b; }

/* Pico SDK assertion that traps on failure even in release builds. */
#define hard_assert(cond) \
    do { if (!(cond)) panic("hard_assert: %s", #cond); } while (0)

extern bool net_client_connected;
extern bool drone;

static inline void piconet_stop(void) { }

/* m_menu.c references the OPTIONS-menu enum value `networkgame` inside
 * a DOOM_TINY-only branch that isn't gated by NET_MENU. Without
 * NET_MENU=1 the enum constant doesn't exist, so we fill it in as 0
 * (corresponding to the position the menu item would have taken). The
 * upstream enum is preprocessor-gated out under our config, so the
 * macro doesn't collide with anything. */
#define networkgame 0

#endif /* NET_CLIENT_H */
