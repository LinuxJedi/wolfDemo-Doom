/*
 * Stand-in for the xxd-generated tiny.whd.h that doom/up.sh produces.
 *
 * The upstream layout (tiny_whd[] as a const uint8_t array literal) is
 * impractical for us: our WHX is 1.8 MB and we already place it in flash
 * via the .wad section. Instead we expose the linker symbol from
 * objcopy --rename-section as `tiny_whd`.
 *
 * doom/src/w_file_memory.c uses tiny_whd only as the base pointer for
 * memcpy()s, so an array alias is enough.
 */

#ifndef WOLFDEMO_TINY_WHD_H
#define WOLFDEMO_TINY_WHD_H

#include <stdint.h>

extern const uint8_t _binary_wad_doom1_whx_start[];
extern const uint8_t _binary_wad_doom1_whx_end[];

#define tiny_whd _binary_wad_doom1_whx_start

#endif /* WOLFDEMO_TINY_WHD_H */
