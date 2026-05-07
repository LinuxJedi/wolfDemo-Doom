/*
 * STM32 video glue for the Doom port.
 *
 * Replaces rp2040-doom's pico/i_video.c (which uses the RP2040/RP2350
 * PIO scanvideo system to feed VGA timing). Our model is simpler:
 * Doom renders into the 320x200 8 bpp framebuffer I_VideoBuffer; on
 * I_FinishUpdate we convert it to a 240x200 RGB565 mirror buffer and
 * push that to the ST7789 over SPI.
 *
 * History of the hot path (see README "Perf" sections for the math):
 *   - Per-pixel polled SPI: ~96 ms/frame, 6-7 FPS.
 *   - Per-row batched SPI + GPDMA ping-pong: 19 ms/frame for SPI,
 *     ~10 FPS overall (rendering becomes dominant).
 *   - Per-row column-skip in pd_add_*: removes the 25% of column
 *     work that lands on x % 4 == 3 columns the SPI converter
 *     immediately drops. ~15 FPS.
 *   - This file now: full-frame RGB565 mirror + ISR-driven background
 *     DMA. I_FinishUpdate converts the frame, kicks an asynchronous
 *     blit, and returns; the GPDMA1 channel-0 TC interrupt chains the
 *     remaining chunks. Frame N's SPI clocking happens in parallel
 *     with frame N+1's BSP / column work, so the per-frame budget
 *     drops from `render + blit` to `max(render, blit)`. The +96 KB
 *     mirror buffer fits within the ~145 KB of remaining BSS budget.
 *
 * The palette LUT stores byte-swapped RGB565 values so a uint16_t
 * scan-out cell can be DMA'd directly as bytes (ST7789 expects MSB
 * first per pixel; on a little-endian M33 the byte-swapped uint16_t
 * lands in memory as [hi, lo], which is the wire order).
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "../board.h"
#include "../st7789.h"
#include "../spi.h"
#include "../uart.h"
#include "w_wad.h"

#define DOOM_W 320
#define DOOM_H 200
#define DISP_W ST7789_W   /* 240 */
#define DISP_H ST7789_H   /* 240 */

/* Doom's internal framebuffer (8 bpp palette indices). */
static uint8_t doom_fb[DOOM_W * DOOM_H];
uint8_t *I_VideoBuffer = doom_fb;

/* Active 256-entry RGB565 palette, BYTE-SWAPPED so the bytes in memory
 * are already in the order ST7789 expects (high byte first). Filled by
 * I_SetPalette / I_SetPaletteNum. */
static uint16_t palette_rgb565[256];

/* Full-frame RGB565 mirror buffer that the async DMA reads from while
 * the engine renders the next frame into doom_fb. 240 px * 200 rows *
 * 2 bytes = 96000 bytes. Aligned to 4 to keep GPDMA word-burst happy
 * even though we configure byte-wide transfers - costs nothing and
 * future-proofs us if we move to half-word DMA. */
static uint16_t scan_full_buf[DOOM_H * DISP_W] __attribute__((aligned(4)));

void I_InitGraphics(void)
{
    /* st7789 was already brought up in main(); leave the panel in
     * window=full-frame, RAMWR-pending state ready for blits. */
    st7789_set_window(0, 0, DISP_W, DISP_H);
    /* Clear palette to all-black so the first frame is not random. */
    for (int i = 0; i < 256; i++) palette_rgb565[i] = 0x0000;
}

void I_ShutdownGraphics(void) { /* nothing */ }

void I_SetPalette(const uint8_t *palette);

/* The engine's ST_doPaletteStuff picks a tinted palette each tic to
 * drive screen flashes (8 red damage tints, 4 gold bonus-pickup tints,
 * green radsuit). Vanilla Doom stores all 14 768-byte palettes in the
 * PLAYPAL lump, but the WHD generator (whd_gen.cpp:4785) truncates
 * PLAYPAL to a single 768-byte copy of palette 0 because the rest
 * are derivable from it via ColorShiftPalette. We apply the same
 * derivation here on-board: palette 0 is straight PLAYPAL; tinted
 * palettes interpolate each PLAYPAL[0] color toward a target tint
 * by shift/steps. Cache the PLAYPAL pointer on first call. */
void I_SetPaletteNum(int num)
{
    static const uint8_t *cached_playpal;
    if (!cached_playpal) {
        int pnum = W_CheckNumForName("PLAYPAL");
        if (pnum < 0) return;
        cached_playpal = (const uint8_t *)W_CacheLumpNum(pnum, 0);
        if (!cached_playpal) return;
    }
    if (num == 0) {
        I_SetPalette(cached_playpal);
        return;
    }
    int tr, tg, tb, shift, steps;
    if (num < 9)        { tr = 255; tg = 0;   tb = 0;  shift = num;     steps = 9; }
    else if (num < 13)  { tr = 215; tg = 186; tb = 69; shift = num - 8; steps = 8; }
    else                { tr = 0;   tg = 256; tb = 0;  shift = 1;       steps = 8; }
    const uint8_t *src = cached_playpal;
    for (int i = 0; i < 256; i++) {
        int r = src[i * 3 + 0];
        int g = src[i * 3 + 1];
        int b = src[i * 3 + 2];
        r += (tr - r) * shift / steps;
        g += (tg - g) * shift / steps;
        b += (tb - b) * shift / steps;
        if (r < 0) r = 0; else if (r > 255) r = 255;
        if (g < 0) g = 0; else if (g > 255) g = 255;
        if (b < 0) b = 0; else if (b > 255) b = 255;
        uint16_t rgb565 = (uint16_t)(((r & 0xF8) << 8) |
                                     ((g & 0xFC) << 3) |
                                     ((b & 0xF8) >> 3));
        palette_rgb565[i] = (uint16_t)((rgb565 << 8) | (rgb565 >> 8));
    }
}

void I_SetPalette(const uint8_t *palette)
{
    /* DOOM palette is 768 bytes: 256 entries of (R, G, B) at 8-bit each.
     * Pack into RGB565 then byte-swap so a memory-order (LE) uint16_t
     * stores the high byte first - matches ST7789's wire format. */
    for (int i = 0; i < 256; i++) {
        uint8_t r = palette[i * 3 + 0];
        uint8_t g = palette[i * 3 + 1];
        uint8_t b = palette[i * 3 + 2];
        uint16_t rgb565 = (uint16_t)(((r & 0xF8) << 8) |
                                     ((g & 0xFC) << 3) |
                                     ((b & 0xF8) >> 3));
        palette_rgb565[i] = (uint16_t)((rgb565 << 8) | (rgb565 >> 8));
    }
}

/* Convert one Doom row into the mirror buffer at row y. Drops every
 * 4th source column to scale 320 -> 240 (the engine emits these as
 * black via the pd_add_* x%4==3 early-return; we drop them at LUT
 * time anyway so even garbage values would be discarded). 80 groups
 * of 3 kept + 1 dropped, fully unrolled - no per-pixel branch. */
static inline void prepare_scanline(int y)
{
    const uint8_t  *row = &I_VideoBuffer[y * DOOM_W];
    uint16_t       *dst = &scan_full_buf[y * DISP_W];
    for (int g = 0; g < 80; g++) {
        dst[0] = palette_rgb565[row[0]];
        dst[1] = palette_rgb565[row[1]];
        dst[2] = palette_rgb565[row[2]];
        /* row[3] dropped */
        row += 4;
        dst += 3;
    }
}

void I_FinishUpdate(void)
{
    /* Vertical layout: 320x200 -> 240x240 with a 20 px black letterbox
     * top and bottom. Run the letterbox clear once on first call so
     * subsequent frames go straight to the doom window. */
    static uint8_t did_pre = 0;
    if (!did_pre) {
        st7789_set_window(0, 0, DISP_W, 20);
        st7789_fill(0x0000);
        st7789_set_window(0, DISP_H - 20, DISP_W, 20);
        st7789_fill(0x0000);
        did_pre = 1;
    }

    /* Drain the previous frame's async blit before doing anything that
     * touches the SPI. On the first call this is a no-op. After the
     * wait, raise CS so set_window's command path can talk to the panel
     * without conflicting with a stale RAMWR. */
    spi_blit_async_wait();
    st7789_blit_end();

    /* Convert the entire 8 bpp frame into the RGB565 mirror buffer.
     * ~1.4 ms total at 160 MHz - cheap relative to the rendering and
     * blit budgets, and decouples doom_fb from the in-flight DMA so
     * the next frame's render can immediately overwrite doom_fb. */
    for (int y = 0; y < DOOM_H; y++) {
        prepare_scanline(y);
    }

    /* Open a fresh RAMWR window over the doom view region and kick the
     * async blit. spi_blit_async_start chunks `total_len` (>65535) into
     * pieces internally via the GPDMA1 channel-0 TC interrupt and runs
     * SPI in continuous (TSIZE=0) mode so the panel sees one unbroken
     * RAMWR sequence across all chunks. CS stays low - we'll lift it
     * in the next call's spi_blit_async_wait + st7789_blit_end. */
    st7789_set_window(0, 20, DISP_W, DOOM_H);
    st7789_blit_begin();
    spi_blit_async_start((const uint8_t *)scan_full_buf,
                         (size_t)DOOM_H * DISP_W * 2u);
    /* Returns immediately; SPI clocking continues in the background. */
}

/* Stubs for things the engine calls but we have no use for. */
void I_StartFrame(void)         { }
void I_UpdateNoBlit(void)       { }
void I_BeginRead(void)          { }
void I_ReadScreen(uint8_t *s)   { (void)s; }
void I_GraphicsCheckCommandLine(void) { }
void I_SetWindowTitle(const char *t)  { (void)t; }
void I_CheckIsScreensaver(void) { }
void I_SetGrabMouseCallback(void *cb)  { (void)cb; }
void I_DisplayFPSDots(int on)   { (void)on; }
void I_BindVideoVariables(void) { }
void I_InitWindowTitle(void)    { }
void I_InitWindowIcon(void)     { }
void I_EnableLoadingDisk(int x, int y) { (void)x; (void)y; }
void I_GetWindowPosition(int *x, int *y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
}
