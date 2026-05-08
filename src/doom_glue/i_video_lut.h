#ifndef I_VIDEO_LUT_H
#define I_VIDEO_LUT_H

#include <stdint.h>

#define COLORMAP_LEVELS 33

/* Native-byte-order RGB565 palette LUT, rebuilt on every I_SetPalette /
 * I_SetPaletteNum call. Use directly for full-bright UI/HUD writes. */
extern uint16_t palette_rgb565[256];

/* Composed colormap+palette table: lit_lut[level][palette_index] is the
 * RGB565 pixel for that lit-shade at that palette index. Column drawers
 * with a colormap level (walls, floors, ceilings, sprites) use this so
 * the inner loop is one indexed load per pixel. */
extern uint16_t lit_lut[COLORMAP_LEVELS][256];

#endif
