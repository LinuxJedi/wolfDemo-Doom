/*
 * Minimal SDL_endian.h shim for the bare-metal port.
 *
 * doom/src/i_swap.h does:
 *   #if !PICO_ON_DEVICE
 *   #include "SDL_endian.h"
 *   #else
 *   ...inline little-endian macros using __builtin_bswap*...
 *   #endif
 *
 * We are not on PICO_ON_DEVICE (that macro is owned by the Pico SDK and
 * dragging it in opens the floodgates to pico/stdlib.h, hardware/clocks.h,
 * etc.). Instead we provide just the two SDL_endian symbols i_swap.h
 * actually uses, with the same little-endian-no-op semantics.
 *
 * STM32U5 is always little-endian; if a future port of this glue runs on
 * a big-endian core, fix it then.
 */

#ifndef WOLFDEMO_SDL_ENDIAN_H
#define WOLFDEMO_SDL_ENDIAN_H

#define SDL_LITTLE_ENDIAN 1234
#define SDL_BIG_ENDIAN    4321
#define SDL_BYTEORDER     SDL_LITTLE_ENDIAN

#define SDL_SwapLE16(x) (x)
#define SDL_SwapLE32(x) (x)
#define SDL_SwapBE16(x) __builtin_bswap16(x)
#define SDL_SwapBE32(x) __builtin_bswap32(x)

#endif /* WOLFDEMO_SDL_ENDIAN_H */
