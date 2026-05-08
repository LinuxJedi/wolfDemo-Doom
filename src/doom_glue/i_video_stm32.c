/*
 * STM32 video glue for the Doom port.
 *
 * Replaces rp2040-doom's pico/i_video.c. Doom renders DIRECTLY into a
 * 240x200 RGB565 framebuffer (scan_full_buf); on I_FinishUpdate we
 * fire an async GPDMA blit straight from that buffer to the ST7789.
 * The 8 bpp -> 565 conversion pass is gone, doom_fb is gone.
 *
 * Pixels are written through a precomputed 33 x 256 lit_lut: index by
 * the colormap level (dc_colormap_index, 0..32) and palette index
 * (the texel) to get RGB565 directly. The LUT is rebuilt in
 * I_SetPalette / I_SetPaletteNum from the (uint8_t) colormaps lump
 * and the live palette_rgb565 table.
 *
 * Horizontal compaction: Doom's renderer thinks in 320 columns. The
 * panel is 240 wide, so columns where (x & 3) == 3 are dropped at the
 * head of pd_add_* and the remaining columns are written at
 * x_out = x - (x >> 2) into the 240-wide buffer.
 *
 * GPDMA1 channel 0 is configured for 16-bit halfword transfers; SPI1
 * runs at DSIZE=15 during the blit. CFG2.LSBFRST=0 means a uint16_t
 * RGB565 store is clocked out MSB-first naturally; no byte-swap.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <stdio.h>

#include "../board.h"
#include "../st7789.h"
#include "../spi.h"
#include "../uart.h"
#include "perf.h"
#include "w_wad.h"
#include "doomtype.h"
#include "i_video.h"   /* pixel_t, grabmouse_callback_t */
#include "doom/f_wipe.h"  /* wipestate_t, WIPESTATE_NONE */

/* lighttable_t is uint8_t in this port; colormaps[] is the WAD's COLORMAP
 * lump (33 levels * 256 entries of palette indices). Forward-declared
 * here to avoid pulling in r_state.h (which trips other compile-time
 * unrelated to this file). */
extern const uint8_t *colormaps;

#define DOOM_W 320
#define DOOM_H 200
#define DISP_W ST7789_W   /* 240 */
#define DISP_H ST7789_H   /* 240 */

/* Full-frame RGB565 ping-pong buffers. The engine renders into one
 * (the back) while GPDMA reads from the other (the front). On every
 * I_FinishUpdate we wait for the previous DMA, kick a new DMA on
 * the just-rendered buffer, and swap roles. Two 96 KB buffers fit
 * because we removed the doom_fb (8 bpp) buffer and the wipe-melt
 * snapshot buffers. Aligned to 4 (more than halfword-DMA's
 * required 2). */
static uint16_t scan_buf_a[DOOM_H * DISP_W] __attribute__((aligned(4)));
static uint16_t scan_buf_b[DOOM_H * DISP_W] __attribute__((aligned(4)));

/* I_VideoBuffer is the back-buffer pointer (engine writes here). The
 * front-buffer is the one the DMA is currently reading. They flip on
 * every I_FinishUpdate. */
pixel_t *I_VideoBuffer = scan_buf_a;
static uint16_t *front_buf = scan_buf_b;
static uint16_t *back_buf  = scan_buf_a;

/* Wipe-claimed pointers. While wipestate != NONE, ping-pong is
 * suspended: wipe_dest is the in-place composite buffer (started as
 * the "from" image when the wipe began), wipe_src is the read-only
 * "to" snapshot. Synchronous DMA runs on wipe_dest each step. */
static pixel_t *wipe_dest;
static pixel_t *wipe_src;

/* Active 256-entry RGB565 palette in native byte order. SPI MSB-first
 * shifting + halfword DMA = wire-correct without an extra byte-swap.
 * Filled by I_SetPalette / I_SetPaletteNum. Used by the patchlist
 * drain (V_DrawPatchList) and callers that don't have a colormap
 * level (UI/HUD writes are full-bright). */
uint16_t palette_rgb565[256];

/* Precomputed colormap composition table. lit_lut[level][index] is
 * the RGB565 pixel for palette index `index` at colormap level
 * `level`. 33 levels * 256 entries * 2 bytes = 16896 bytes; rebuilt
 * by I_SetPalette/I_SetPaletteNum which both run only on tint
 * transitions (rare). */
#define COLORMAP_LEVELS 33
uint16_t lit_lut[COLORMAP_LEVELS][256];

static void rebuild_lit_lut(void)
{
    /* colormaps may not be loaded yet during the very early palette
     * call; fall back to identity. */
    if (!colormaps) {
        for (int lv = 0; lv < COLORMAP_LEVELS; lv++) {
            for (int i = 0; i < 256; i++) {
                lit_lut[lv][i] = palette_rgb565[i];
            }
        }
        return;
    }
    for (int lv = 0; lv < COLORMAP_LEVELS; lv++) {
        const uint8_t *cmap = (const uint8_t *)colormaps + lv * 256;
        for (int i = 0; i < 256; i++) {
            lit_lut[lv][i] = palette_rgb565[cmap[i]];
        }
    }
}

void I_InitGraphics(void)
{
    /* st7789 was already brought up in main(); leave the panel in
     * window=full-frame, RAMWR-pending state ready for blits. */
    st7789_set_window(0, 0, DISP_W, DISP_H);
    /* Clear palette and LUT to all-black so the first frame isn't random. */
    for (int i = 0; i < 256; i++) palette_rgb565[i] = 0x0000;
    rebuild_lit_lut();
    memset(scan_buf_a, 0, sizeof(scan_buf_a));
    memset(scan_buf_b, 0, sizeof(scan_buf_b));
}

void I_ShutdownGraphics(void) { /* nothing */ }

/* ---- Wipe coordination shims (called from pd_stubs.c::pd_end_frame).
 *
 * At wipe-start: drain the in-flight async DMA so front_buf is
 * read-stable, then claim it as the composite destination ("from")
 * and back_buf as the read-only "to" source. I_VideoBuffer is held
 * at wipe_dest so any defensive write during wipe iterations lands
 * in the composite buffer (the engine's render gate is closed
 * during wipe, so this is mostly a paranoia anchor).
 *
 * At wipe-end: both buffers logically contain the new "to" image
 * (back_buf was the unchanged "to" snapshot; wipe_dest was
 * progressively replaced with "to" rows). Resume ping-pong with the
 * engine writing into the now-stale "to" snapshot for the next
 * frame; spi_blit_async_start in I_FinishUpdate will alternate
 * normally from there. */
void i_video_begin_wipe(void)
{
    spi_blit_async_wait();
    st7789_blit_end();
    wipe_dest = (pixel_t *)front_buf;
    wipe_src  = (pixel_t *)back_buf;
    I_VideoBuffer = wipe_dest;
}

void i_video_end_wipe(void)
{
    front_buf = (uint16_t *)wipe_dest;
    back_buf  = (uint16_t *)wipe_src;
    I_VideoBuffer = (pixel_t *)back_buf;
    wipe_dest = NULL;
    wipe_src  = NULL;
}

pixel_t       *i_video_wipe_dest(void) { return wipe_dest; }
const pixel_t *i_video_wipe_to(void)   { return wipe_src; }

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
        palette_rgb565[i] = rgb565;
    }
    rebuild_lit_lut();
}

void I_SetPalette(const uint8_t *palette)
{
    /* DOOM palette is 768 bytes: 256 entries of (R, G, B) at 8-bit each.
     * Pack into RGB565 in native byte order. The ST7789 expects the
     * high byte first on the wire; with the 16-bit-DSIZE SPI + halfword
     * GPDMA path, a uint16_t store is shifted out MSB-first
     * (CFG2.LSBFRST = 0), which lands the high byte on the wire first
     * naturally. No byte-swap needed. */
    for (int i = 0; i < 256; i++) {
        uint8_t r = palette[i * 3 + 0];
        uint8_t g = palette[i * 3 + 1];
        uint8_t b = palette[i * 3 + 2];
        uint16_t rgb565 = (uint16_t)(((r & 0xF8) << 8) |
                                     ((g & 0xFC) << 3) |
                                     ((b & 0xF8) >> 3));
        palette_rgb565[i] = rgb565;
    }
    rebuild_lit_lut();
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

#if PERF_INSTRUMENT
    static uint32_t perf_frame_t0;          /* set on previous I_FinishUpdate exit */
    static uint64_t perf_total_us, perf_blit_wait_us;
    static uint32_t perf_frames;
    static uint32_t perf_window_start_ms;

    if (perf_frame_t0 != 0) {
        perf_total_us += perf_us_since(perf_frame_t0);
    }
#endif

    /* Drain the previous frame's async blit before doing anything that
     * touches the SPI. On the first call this is a no-op. After the
     * wait, raise CS so set_window's command path can talk to the panel
     * without conflicting with a stale RAMWR. */
#if PERF_INSTRUMENT
    uint32_t perf_t = perf_cyc();
#endif
    spi_blit_async_wait();
    st7789_blit_end();
#if PERF_INSTRUMENT
    perf_blit_wait_us += perf_us_since(perf_t);
#endif

    /* No conversion pass: column drawers wrote RGB565 directly into
     * scan_full_buf via the lit_lut LUT. The GPDMA picks bytes
     * straight off scan_full_buf below.
     *
     * Open a fresh RAMWR window over the doom view region and kick
     * the async blit. spi_blit_async_start chunks `total_len` into
     * pieces internally via the GPDMA1 channel-0 TC interrupt and
     * runs SPI in continuous (TSIZE=0) mode so the panel sees one
     * unbroken RAMWR sequence across all chunks. CS stays low; we
     * lift it in the next call's spi_blit_async_wait. */
    st7789_set_window(0, 20, DISP_W, DOOM_H);
    st7789_blit_begin();

    extern wipestate_t wipestate;
    if (wipestate == WIPESTATE_NONE) {
        /* Normal frame: promote the just-rendered back buffer to
         * front and kick async DMA on it. The OTHER buffer becomes
         * the new back; engine writes its next frame there while
         * DMA reads the new front. The buffers are physically
         * distinct, so there is no race. */
        front_buf = back_buf;
        back_buf  = (back_buf == scan_buf_a) ? scan_buf_b : scan_buf_a;
        I_VideoBuffer = (pixel_t *)back_buf;

        spi_blit_async_start((const uint8_t *)front_buf,
                             (size_t)DOOM_H * DISP_W * 2u);
        /* Returns immediately; SPI clocking continues in background. */
    } else {
        /* Wipe in progress. Don't swap. DMA the in-place composite
         * buffer synchronously so the next composite step doesn't
         * race with DMA reads. */
        spi_blit_async_start((const uint8_t *)wipe_dest,
                             (size_t)DOOM_H * DISP_W * 2u);
        spi_blit_async_wait();
        st7789_blit_end();
    }

#if PERF_INSTRUMENT
    perf_frames++;
    extern int I_GetTimeMS(void);
    int now_ms = I_GetTimeMS();
    if (perf_window_start_ms == 0) perf_window_start_ms = now_ms;
    if ((now_ms - perf_window_start_ms) >= 1000 && perf_frames > 0) {
        uint32_t fps = perf_frames;
        uint32_t blit_wait_avg = (uint32_t)(perf_blit_wait_us / perf_frames);
        uint32_t total_avg     = (uint32_t)(perf_total_us     / perf_frames);
        /* render_us is the rest of the frame: total - blit_wait. */
        uint32_t render_avg = total_avg > blit_wait_avg
            ? total_avg - blit_wait_avg : 0;
        printf("[perf] render=%lu us blit_wait=%lu us total=%lu us fps=%lu\r\n",
               (unsigned long)render_avg,
               (unsigned long)blit_wait_avg,
               (unsigned long)total_avg,
               (unsigned long)fps);
        perf_total_us = perf_blit_wait_us = 0;
        perf_frames = 0;
        perf_window_start_ms = now_ms;
    }
    perf_frame_t0 = perf_cyc();
#endif
}

/* Stubs for things the engine calls but we have no use for. */
void I_StartFrame(void)         { }
void I_UpdateNoBlit(void)       { }
void I_BeginRead(void)          { }
void I_ReadScreen(pixel_t *s)   { (void)s; }
void I_GraphicsCheckCommandLine(void) { }
void I_SetWindowTitle(const char *t)  { (void)t; }
void I_CheckIsScreensaver(void) { }
void I_SetGrabMouseCallback(grabmouse_callback_t cb)  { (void)cb; }
void I_DisplayFPSDots(boolean on)   { (void)on; }
void I_BindVideoVariables(void) { }
void I_InitWindowTitle(void)    { }
void I_InitWindowIcon(void)     { }
void I_EnableLoadingDisk(int x, int y) { (void)x; (void)y; }
void I_GetWindowPosition(int *x, int *y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
}
