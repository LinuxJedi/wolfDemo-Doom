/*
 * Stand-in for doom/src/pd_render.cpp.
 *
 * pd_render.cpp is the per-scanline renderer that hands buffers to the
 * RP2040 PIO scanvideo system via DMA. We're a single-threaded fb-only
 * port; this file is the corresponding subset:
 *
 *   - pd_begin_frame: clear framebuffer for GS_LEVEL, reset framedrawable
 *     ring (without it num_framedrawables grows past MAX_FRAME_DRAWABLES
 *     and trips a hard_assert), open a fresh patchlist for ST_Drawer/
 *     M_Drawer to push V_DrawPatch entries into.
 *   - pd_end_frame: install PLAYPAL once, decode the WHD splash lump
 *     (TITLEPIC / CREDIT / HELP2) for GS_DEMOSCREEN, run ST_Drawer for
 *     GS_LEVEL to populate the patchlist with status-bar widgets, drain
 *     the patchlist into I_VideoBuffer, blit via I_FinishUpdate.
 *   - Wipe state machine: in-place classic melt using the two RGB565
 *     ping-pong buffers as "from" and "to" snapshots.
 *   - pd_add_column: textured wall sampling for PDCOL_TOP / MID /
 *     BOTTOM / SKY via a single-slot Huffman patch-decoder cache and
 *     per-column decode + iscale/texturemid/colormap sample. Multi-
 *     patch composite textures route through paint_composite_column,
 *     which walks the WHD_COL_SEG_* segment stream to stitch each
 *     contributing patch into one 128-byte sample buffer.
 *   - pd_add_plane_column: textured floor/ceiling spans with flat
 *     decoder + LRU cache and span-major projection.
 *   - pd_add_masked_columns: sprite columns (and masked mid-textures)
 *     decoded through the same patch decoder cache as walls, with
 *     dc_translation_index applied for player-colour remap.
 */

/* The whole file is the rendering hot path - per-pixel loops dominate
 * the frame budget, so override the global -Os with -O3 to let GCC
 * unroll and aggressively register-allocate. Costs a few KB of code
 * size but pays back on inner-loop throughput. */
#pragma GCC optimize("O3")

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "m_fixed.h"
#include "tables.h"
#include "v_video.h"
#include "v_patch.h"
#include "w_wad.h"
#include "tiny_huff.h"
#include "image_decoder.h"
#include "doom/f_wipe.h"
#include "doom/r_data.h"
#include "doom/r_defs.h"
#include "doom/r_draw.h"
#include "doom/r_main.h"
#include "doom/r_plane.h"
#include "doom/r_state.h"
#include "doom/doomdef.h"
#include "doom/doomstat.h"
#include "doom/m_menu.h"
#include "doom/m_random.h"
#include "doom/st_stuff.h"
#include "doom/wi_stuff.h"
#include "doom/f_finale.h"
#include "picodoom.h"

#include "i_video_lut.h"
#include "perf.h"

extern pixel_t *I_VideoBuffer;
extern void I_FinishUpdate(void);
extern void I_SetPalette(const uint8_t *palette);
extern int  I_GetTimeMS(void);
extern const char *pagename;
extern void button_tick(void);

#define DOOM_W      320          /* logical render width (engine geometry) */
#define DOOM_H      200
#define DISP_W      240          /* physical scan-out width (RGB565 buffer) */

#ifndef CLEAR_LEVEL_FRAMEBUFFER
#define CLEAR_LEVEL_FRAMEBUFFER 1
#endif

/* Compact x from 320-space to 240-space by dropping every 4th column.
 * Equivalent to (x * 3) / 4 but avoids the multiply. The (x & 3) == 3
 * columns are filtered out at the head of pd_add_*; for the kept
 * columns this maps 0->0, 1->1, 2->2, 4->3, 5->4, 6->5, 8->6, ...
 * which lays them out contiguously in the 240-wide RGB565 buffer. */
static inline int x_squeeze(int x) { return x - (x >> 2); }

/* pd_render.cpp owns these; types must match the upstream externs. */
int              pd_flag;
fixed_t          pd_scale;
wipestate_t      wipestate;
volatile uint8_t wipe_min;
pre_wipe_state_t pre_wipe_state;
boolean          screenvisible = true;

/* The patchlist machinery in v_video.c expects a single global
 * vpatchlists_t (declared in v_video.h). pd_render.cpp puts it in
 * USB DPRAM scratch on the RP2040; we just give it a BSS slot. The
 * struct is < 0xc00 = 3072 bytes, ~1% of our SRAM budget. */
static vpatchlists_t vpatchlists_storage;
vpatchlists_t       *vpatchlists = &vpatchlists_storage;

/* Silhouette colour palette: rough Doom-palette indices that
 * give visually distinct surfaces without needing texture data.
 * (These are approximate; the real palette is set by PLAYPAL.) */
#define COLOUR_TOP      88   /* upper wall   - dark gray */
#define COLOUR_MID     112   /* middle wall  - mid gray */
#define COLOUR_BOTTOM   64   /* lower wall   - tan */
#define COLOUR_SKY     200   /* sky          - light blue */
#define COLOUR_FLOOR   139   /* floor flat   - brownish */
#define COLOUR_CEILING 105   /* ceiling flat - dim gray */

static inline void paint_column(int x, int yl, int yh, uint8_t colour)
{
    if (yl < 0) yl = 0;
    if (yh > DOOM_H - 1) yh = DOOM_H - 1;
    if (yl > yh || x < 0 || x >= DOOM_W) return;
    uint16_t pix = palette_rgb565[colour];
    pixel_t *dest = &I_VideoBuffer[yl * DISP_W + x_squeeze(x)];
    for (int y = yl; y <= yh; y++) {
        *dest = pix;
        dest += DISP_W;
    }
}

/* Huffman code table + prefix-length LUT shared by the splash decoder
 * (TITLEPIC etc.) and the wall-texture patch decoder cache. Sized for
 * read_raw_pixels_decoder_c3's worst case: up to 263 symbols at 3 bytes
 * each requires ~792 bytes of tmp scratch; we use 1024. The Huffman
 * table itself fits comfortably in 4 KB. */
static uint16_t splash_decoder_buf[2048];
static uint8_t  splash_prefix_lengths[256];
static uint8_t  splash_decoder_tmp[1024];

/* ---------- Patch decoder cache (single-slot LRU) -----------------
 * pd_render.cpp keeps a 1.75 KB circular buffer + 128-slot hash table
 * to amortise Huffman-decoder construction across the multi-core
 * scanvideo renderer. We're single-threaded and decode column-by-column
 * in pd_add_column, so columns from the same texture/patch arrive in
 * runs - a single-slot cache hits 95%+ of the time without the LRU
 * machinery.
 *
 * The decoder buffers are shared with the splash decoder (TITLEPIC etc.):
 * a transition GS_LEVEL -> GS_DEMOSCREEN overwrites them, so splash_draw
 * invalidates the cache. */
static int             cached_patch_num = -1;
static const uint8_t  *cached_patch;
static const uint16_t *cached_col_offsets;
static uint32_t        cached_data_byte_index;
static uint8_t         cached_encoding;
static int             cached_width;
static int             cached_height;

#define WHD_TEXTURE_COL_HEIGHT  128

static void load_patch_decoder(int patch_num)
{
    if (patch_num == cached_patch_num) return;

    const uint8_t *patch =
        (const uint8_t *)W_CacheLumpNum(patch_num, 0 /* PU_CACHE */);
    int data_index = 3 + (patch_has_extra(patch) ? 1 : 0);

    /* Decode the Huffman code table from the patch's metadata block.
     * Identical to splash_draw's prologue. */
    const uint8_t *src = patch + data_index * 2 + 1;
    th_bit_input bi;
    th_bit_input_init(&bi, src);
    int encoding = th_read_bits(&bi, 1);
    if (encoding == 0) {
        if (th_bit(&bi)) {
            th_read_simple_decoder(&bi, splash_decoder_buf,
                                   sizeof(splash_decoder_buf) / 2,
                                   splash_decoder_tmp,
                                   sizeof(splash_decoder_tmp));
        } else {
            read_raw_pixels_decoder(&bi, splash_decoder_buf,
                                    sizeof(splash_decoder_buf) / 2,
                                    splash_decoder_tmp,
                                    sizeof(splash_decoder_tmp));
        }
    } else {
        read_raw_pixels_decoder_c3(&bi, splash_decoder_buf,
                                   sizeof(splash_decoder_buf) / 2,
                                   splash_decoder_tmp,
                                   sizeof(splash_decoder_tmp));
    }
    th_make_prefix_length_table(splash_decoder_buf, splash_prefix_lengths);

    data_index += ((const uint8_t *)patch)[data_index * 2];
    int w = patch_width(patch);
    cached_patch_num = patch_num;
    cached_patch = patch;
    cached_col_offsets = &((const uint16_t *)patch)[data_index];
    cached_data_byte_index = (data_index + w) * 2 + 2;
    cached_encoding = (uint8_t)encoding;
    cached_width = w;
    cached_height = patch_height(patch);
}

/* Decode column `col` of the cached patch into `out`. Output is a
 * 128-byte buffer that R_DrawColumn-style sampling (`& 127`) can index
 * directly. Patches shorter than 128 are wrap-padded by repeating from
 * the start, matching the pd_render.cpp h<127 fixup heuristic.
 *
 * `max_row` is the highest output index the caller will sample
 * (inclusive, [0, 127]). Decode is sequential from row 0 with no skip
 * support, but we can stop early once we've produced all the rows the
 * caller needs - a meaningful win for the typical Doom sliver
 * geometry where only a handful of rows of a 128-row column survive
 * top/bottom seam clipping. */
static void decode_patch_column(int col, uint8_t *out, int max_row)
{
    if (max_row < 0) return;
    if (max_row >= WHD_TEXTURE_COL_HEIGHT) max_row = WHD_TEXTURE_COL_HEIGHT - 1;

    uint16_t col_offset = cached_col_offsets[col];
    if ((col_offset >> 8) == 0xff) {
        col_offset = cached_col_offsets[col_offset & 0xff];
    }
    th_bit_input bi;
    if (patch_byte_addressed(cached_patch)) {
        th_bit_input_init(&bi,
                          cached_patch + cached_data_byte_index + col_offset);
    } else {
        th_bit_input_init_bit_offset(&bi,
                                     cached_patch + cached_data_byte_index,
                                     col_offset);
    }

    int h = cached_height;
    if (h > WHD_TEXTURE_COL_HEIGHT) h = WHD_TEXTURE_COL_HEIGHT;
    int decode_h = (max_row + 1 < h) ? (max_row + 1) : h;

    if (cached_encoding == 0) {
        /* Single-byte Huffman, no delta. The splash decoder asserts
         * here because TITLEPIC / CREDIT / HELP all use encoding 1,
         * but plenty of patches (most sprites in particular, plus
         * some wall patches with low pixel-value entropy) use
         * encoding 0 - bailing to memset(0) was painting them as
         * solid black silhouettes. Mirrors pd_render.cpp's
         * draw_patch_columns encoding==0 path. */
        for (int y = 0; y < decode_h; y++) {
            out[y] = th_decode_table_special(splash_decoder_buf,
                                             splash_prefix_lengths, &bi);
        }
    } else {
        uint8_t prev = 0;
        for (int y = 0; y < decode_h; y++) {
            uint16_t p = th_decode_table_special_16(splash_decoder_buf,
                                                    splash_prefix_lengths, &bi);
            if (p < 256) {
                prev = (uint8_t)p;
            } else {
                prev = (uint8_t)(prev + (p & 0xff) - 3);
            }
            out[y] = prev;
        }
    }
    /* Wrap-pad shorter textures so `& 127` sampling stays sane. Only
     * fills up to max_row - if max_row < h we never enter this loop. */
    for (int y = h; y <= max_row; y++) {
        out[y] = out[y - h];
    }
}

/* Resolve a wall texture's real_id to a single patch_num for the
 * single-patch and pc==1 cases. Composite (pc>=2) cases go through
 * paint_composite_column instead - patch_table[0] alone misses the
 * overlay patches that make doors and computer screens look right. */
static int texture_patch_num(int real_id)
{
    int pc = whd_textures[real_id].patch_count;
    if (pc == 0) {
        /* Single covering opaque patch at (0, 0); patch0 is the lump
         * number directly. */
        return whd_textures[real_id].patch0;
    }
    /* pc == 1: the metadata table's first entry is the only patch.
     * For pc >= 2 the caller should use paint_composite_column; we
     * still return patch_table[0] as a defensive fallback. */
    const uint8_t *patch_table =
        &((const uint8_t *)whd_textures)[whd_textures[real_id].metdata_offset];
    return patch_table[0] | (patch_table[1] << 8);
}

/* ---------- Composite-column wall renderer ----------------------
 * Multi-patch wall textures (BIGDOOR1, TEKWALL4, COMPSTA1, ...) store
 * their column structure as a per-segment byte stream that follows the
 * patch_table. Each composite column may stack 2-4 patches in y, with
 * optional memcpy segments that duplicate y-runs. r_segs.c's
 * pd_add_column2 leaves dc_source.real_id positive for these and
 * defers resolution to the renderer.
 *
 * Mirrors draw_composite_columns in pd_render.cpp - same metadata walk,
 * same segment encoding. The Pico version batches all columns sharing
 * one texture; we only ever resolve a single column per call, so we
 * apply each segment's contribution directly into a 128-byte pixel
 * buffer instead of accumulating runs[] for later replay.
 *
 * The single-slot patch decoder cache is reused per segment - composite
 * columns with two stacked patches will thrash it, but each composite
 * column is a one-time decode regardless. */
static void paint_composite_column(void)
{
    int real_id = dc_source.real_id;
    int target_col = dc_source.col;
    int w = whd_textures[real_id].width;
    int pc = whd_textures[real_id].patch_count;

    const uint8_t *patch_table =
        &((const uint8_t *)whd_textures)[whd_textures[real_id].metdata_offset];
    const uint8_t *metadata = patch_table + pc * 2;

    /* Skip past the non-composite (single-patch) column metadata - the
     * same byte stream pd_add_column2 walks to find single-patch ranges.
     * Each entry: [b]; if b & 0x80, two trailing bytes (patch index +
     * originx); run length is (b & 0x7f) + 1. */
    int xx = 0;
    while (xx < w) {
        uint8_t b = *metadata++;
        xx += (b & 0x7f) + 1;
        if (b & 0x80) metadata += 2;
    }

    /* Match pd_render.cpp's draw_composite_columns: leave pixels[]
     * uninitialized. Solid composite walls cover all hh rows via
     * segments (memcpy or patch decode), so zeroing first would only
     * matter if a row went unfilled - but for solid walls that
     * shouldn't happen, and for transparent texture columns the
     * masked-column path handles them instead. Initialising to 0 had
     * the side effect of painting black where my pcol math was off
     * for any reason; uninitialised matches upstream and avoids the
     * "consistent black border" failure mode. The buffer is sized
     * [129] so the boundary fixup `pixels[hh] = pixels[hh - 1]` for
     * sub-128 textures stays in bounds. */
    uint8_t pixels[WHD_TEXTURE_COL_HEIGHT + 1];

    /* Find the composite range covering target_col, walking ranges in
     * order. A range starting with 0xff is a single-patch run that
     * pd_add_column2 already resolved - skip its 2-byte tail. */
    int base = 0;
    int found = 0;
    while (base < w) {
        int range_len = (int)*metadata++ + 1;
        int limit = base + range_len;
        if (limit > w) limit = w;
        if (metadata[0] == 0xff) {
            metadata += 2;
            base = limit;
            continue;
        }
        if (target_col >= base && target_col < limit) {
            /* Process this range's segment list, applying each segment
             * to pixels[]. Segments are read until m1's high bit marks
             * the last one. */
            int y = 0;
            for (;;) {
                int local_patch = metadata[0];
                int m1 = metadata[1];
                if (local_patch & WHD_COL_SEG_EXPLICIT_Y) {
                    y = metadata[2];
                    metadata++;
                }
                int length = 1 + (m1 & 0x7f);
                int copy_len = length;
                if (y < 0) {
                    copy_len += y;
                    /* y < 0 not expected; clamp defensively */
                    y = 0;
                }
                if (copy_len < 0) copy_len = 0;
                if (y + copy_len > WHD_TEXTURE_COL_HEIGHT) {
                    copy_len = WHD_TEXTURE_COL_HEIGHT - y;
                }
                if (local_patch & WHD_COL_SEG_MEMCPY) {
                    int src = metadata[2];
                    int n = copy_len;
                    if (src + n > WHD_TEXTURE_COL_HEIGHT) {
                        n = WHD_TEXTURE_COL_HEIGHT - src;
                    }
                    if (n > 0) {
                        if (local_patch & WHD_COL_SEG_MEMCPY_IS_BACKWARDS) {
                            for (int yy = n - 1; yy >= 0; yy--) {
                                pixels[y + yy] = pixels[src + yy];
                            }
                        } else {
                            for (int yy = 0; yy < n; yy++) {
                                pixels[y + yy] = pixels[src + yy];
                            }
                        }
                    }
                    metadata += 3;
                } else {
                    int local_patch_index = local_patch & 0x0f;
                    int patch_num =
                        patch_table[local_patch_index * 2]
                        | (patch_table[local_patch_index * 2 + 1] << 8);
                    /* The encoder stores metadata[2] as
                     *   (base - originx) & 0xff   (whd_gen.cpp:4167)
                     * so the decoder gets the right patch column via
                     * unsigned 8-bit arithmetic:
                     *   pcol = (uint8_t)(col - base + metadata[2])
                     *        = (col - originx) mod 256
                     * which naturally lands in [0, patch_width).
                     * Casting to int8_t here is wrong - it sign-flips
                     * values >= 128 and breaks any patch with originx
                     * outside [-127, 127] (shows up as the wrong
                     * patch column being sampled). */
                    uint8_t xoff = metadata[2];
                    int src_yoff = metadata[3];
                    load_patch_decoder(patch_num);
                    if (cached_width > 0) {
                        int pcol = (uint8_t)(target_col - base + xoff);
                        if (pcol >= cached_width) pcol %= cached_width;
                        int n = copy_len;
                        if (src_yoff + n > WHD_TEXTURE_COL_HEIGHT) {
                            n = WHD_TEXTURE_COL_HEIGHT - src_yoff;
                        }
                        uint8_t col_buf[WHD_TEXTURE_COL_HEIGHT];
                        /* Per-segment we only sample [src_yoff,
                         * src_yoff + n - 1] of the patch column. */
                        decode_patch_column(pcol, col_buf,
                                            src_yoff + n - 1);
                        for (int yy = 0; yy < n; yy++) {
                            pixels[y + yy] = col_buf[src_yoff + yy];
                        }
                    }
                    metadata += 4;
                }
                y += length;
                if (m1 & 0x80) break;
            }
            found = 1;
            break;
        }
        /* Range doesn't cover our column - skip its segments. */
        for (;;) {
            int last = metadata[1] & 0x80;
            int has_y = (metadata[0] & WHD_COL_SEG_EXPLICIT_Y) ? 1 : 0;
            if (metadata[0] & WHD_COL_SEG_MEMCPY) {
                metadata += 3 + has_y;
            } else {
                metadata += 4 + has_y;
            }
            if (last) break;
        }
        base = limit;
    }
    if (!found) return;

    /* Boundary fixup matching pd_render.cpp's draw_composite_columns:
     * for sub-128 textures, anchor pixels[127] = pixels[0] (the wrap
     * point sampled when frac advances past the texture) and
     * pixels[hh] = pixels[hh-1] (the bottom edge). Other rows in
     * (hh, 127) are unsampled in practice for the column-frac math
     * Doom uses on solid walls. */
    int hh = whd_textures[real_id].height;
    if (hh != WHD_TEXTURE_COL_HEIGHT && hh > 0 && hh <= WHD_TEXTURE_COL_HEIGHT) {
        pixels[WHD_TEXTURE_COL_HEIGHT - 1] = pixels[0];
        pixels[hh] = pixels[hh - 1];
    }

    int yl = dc_yl, yh = dc_yh;
    if (yl < 0) yl = 0;
    if (yh > DOOM_H - 1) yh = DOOM_H - 1;
    if (yl > yh || dc_x < 0 || dc_x >= DOOM_W) return;

    fixed_t fracstep = dc_iscale;
    fixed_t frac = dc_texturemid + (yl - centery) * fracstep;
    int cmi = dc_colormap_index;
    if (cmi < 0) cmi = 0;
    const uint16_t *clut = lit_lut[cmi];

    pixel_t *dest = &I_VideoBuffer[yl * DISP_W + x_squeeze(dc_x)];
    for (int y = yl; y <= yh; y++) {
        *dest = clut[pixels[(frac >> FRACBITS) & 127]];
        dest += DISP_W;
        frac += fracstep;
    }
}

static void paint_textured_column(uint8_t fallback_colour)
{
    int real_id = dc_source.real_id;
    int patch_num;
    if (real_id < 0) {
        /* r_segs.c:pd_add_column2 rewrote dc_source via lookup_patch,
         * which stores the negated patch lump number in real_id. */
        patch_num = -real_id;
    } else if (real_id > 0) {
        /* Composite textures: pd_add_column2 leaves real_id positive
         * for x-ranges where multiple patches stack in y. Resolve
         * those here via the segment-walk decoder. */
        if (whd_textures[real_id].patch_count >= 2) {
            paint_composite_column();
            return;
        }
        patch_num = texture_patch_num(real_id);
        if (patch_num < 0) {
            paint_column(dc_x, dc_yl, dc_yh, fallback_colour);
            return;
        }
    } else {
        return; /* real_id == 0 - invalid, skip */
    }
    load_patch_decoder(patch_num);
    if (cached_width <= 0) return;

    /* R_GetColumn already masks col by (texture_width - 1); for patch_count
     * <= 1 the patch and texture widths match, so col is in range. Modulo
     * defensively for any non-pow2 corner case. */
    int col = dc_source.col;
    if (col >= cached_width) col %= cached_width;

    int yl = dc_yl, yh = dc_yh;
    if (yl < 0) yl = 0;
    if (yh > DOOM_H - 1) yh = DOOM_H - 1;
    if (yl > yh || dc_x < 0 || dc_x >= DOOM_W) return;

    fixed_t fracstep = dc_iscale;
    fixed_t frac = dc_texturemid + (yl - centery) * fracstep;

    /* Compute the highest col_buf[] row the per-pixel loop will touch
     * so decode_patch_column can stop early. Sampling does
     * `(frac >> FRACBITS) & 127`, so the integer-row range walked from
     * frac0 to frac1 modulo 128 gives us the upper bound. If the span
     * itself reaches 128 rows or it straddles a 128-row boundary,
     * fall back to a full decode. */
    int max_row;
    int span_rows = yh - yl;
    int64_t span_frac = (int64_t)span_rows * (int64_t)fracstep;
    int64_t span_abs = span_frac < 0 ? -span_frac : span_frac;
    if (span_abs >= ((int64_t)WHD_TEXTURE_COL_HEIGHT << FRACBITS)) {
        max_row = WHD_TEXTURE_COL_HEIGHT - 1;
    } else {
        fixed_t frac_end = frac + (fixed_t)span_frac;
        fixed_t lo = (frac < frac_end) ? frac : frac_end;
        fixed_t hi = (frac < frac_end) ? frac_end : frac;
        int lo_row = (int)((lo >> FRACBITS) & (WHD_TEXTURE_COL_HEIGHT - 1));
        int hi_row = (int)((hi >> FRACBITS) & (WHD_TEXTURE_COL_HEIGHT - 1));
        max_row = (lo_row > hi_row) ? (WHD_TEXTURE_COL_HEIGHT - 1)
                                    : hi_row;
    }

    uint8_t col_buf[WHD_TEXTURE_COL_HEIGHT];
    decode_patch_column(col, col_buf, max_row);

    int cmi = dc_colormap_index;
    if (cmi < 0) cmi = 0;
    const uint16_t *clut = lit_lut[cmi];

    pixel_t *dest = &I_VideoBuffer[yl * DISP_W + x_squeeze(dc_x)];
    for (int y = yl; y <= yh; y++) {
        *dest = clut[col_buf[(frac >> FRACBITS) & 127]];
        dest += DISP_W;
        frac += fracstep;
    }
}

/* ---------- Sky pre-decode cache --------------------------------
 * PDCOL_SKY paints ~240 columns per frame, each with up to ~100 rows
 * of sequential Huffman decode in decode_patch_column. The sky
 * texture is constant per level, viewangle rotation just shifts
 * which source columns are sampled, so caching the decoded columns
 * across frames eliminates ~24k Huffman decodes/frame after warmup.
 *
 * Storage budget: SKY_CACHE_MAX_COLS * 128 = 32 KB. Sky patches in
 * vanilla Doom are 256 wide; if anything wider ever appears we just
 * fall back to the per-frame decode path. */
#define SKY_CACHE_MAX_COLS 256
static int     sky_cache_patch = -1;
static int     sky_cache_width;
static uint8_t sky_cache_data[SKY_CACHE_MAX_COLS][WHD_TEXTURE_COL_HEIGHT];
static uint8_t sky_cache_valid[SKY_CACHE_MAX_COLS] __attribute__((section(".sram4")));

static void paint_sky_column(void)
{
    int real_id = dc_source.real_id;
    if (real_id >= 0) {
        /* Sky should always come through pd_add_column2 which negates
         * to a direct patch handle. Defensive fallback. */
        paint_textured_column(COLOUR_SKY);
        return;
    }
    int patch_num = -real_id;

    if (patch_num != sky_cache_patch) {
        load_patch_decoder(patch_num);
        if (cached_width <= 0) return;
        if (cached_width > SKY_CACHE_MAX_COLS) {
            /* Wider than our cache - skip caching, just paint. */
            paint_textured_column(COLOUR_SKY);
            return;
        }
        sky_cache_patch = patch_num;
        sky_cache_width = cached_width;
        memset(sky_cache_valid, 0, sizeof sky_cache_valid);
    }

    int col = dc_source.col;
    if (col >= sky_cache_width) col %= sky_cache_width;

    if (!sky_cache_valid[col]) {
        load_patch_decoder(patch_num);
        decode_patch_column(col, sky_cache_data[col],
                            WHD_TEXTURE_COL_HEIGHT - 1);
        sky_cache_valid[col] = 1;
    }

    int yl = dc_yl, yh = dc_yh;
    if (yl < 0) yl = 0;
    if (yh > DOOM_H - 1) yh = DOOM_H - 1;
    if (yl > yh || dc_x < 0 || dc_x >= DOOM_W) return;

    fixed_t fracstep = dc_iscale;
    fixed_t frac = dc_texturemid + (yl - centery) * fracstep;
    /* Sky always uses colormap 0 (full bright). lit_lut[0] composes
     * the identity colormap with the live palette into RGB565. */
    const uint16_t *clut = lit_lut[0];

    const uint8_t *col_buf = sky_cache_data[col];
    pixel_t *dest = &I_VideoBuffer[yl * DISP_W + x_squeeze(dc_x)];
    for (int y = yl; y <= yh; y++) {
        *dest = clut[col_buf[(frac >> FRACBITS) & 127]];
        dest += DISP_W;
        frac += fracstep;
    }
}

void pd_add_column(pd_column_type type)
{
    /* The display is 240 px wide; I_FinishUpdate scales the 320-wide
     * framebuffer down by dropping every 4th column (x % 4 == 3). Those
     * dropped columns are never sent to the panel, so painting them
     * here is pure waste - skip the per-column patch decode + sample
     * loop entirely. Saves ~25% of wall rendering time at the cost of
     * leaving black gaps in I_VideoBuffer at x = 3, 7, 11, ... that
     * the wipe melt will briefly snapshot (a column of black streaks
     * for a single transition tic - barely perceptible since the panel
     * never sees those columns anyway). */
    if ((dc_x & 3) == 3) return;
#if PERF_INSTRUMENT
    uint32_t perf_paint_t = perf_cyc();
#endif
    switch (type) {
        case PDCOL_TOP:    paint_textured_column(COLOUR_TOP);    break;
        case PDCOL_MID:    paint_textured_column(COLOUR_MID);    break;
        case PDCOL_BOTTOM: paint_textured_column(COLOUR_BOTTOM); break;
        case PDCOL_SKY:
            /* r_segs.c's sky path sets dc_iscale = pspriteiscale (no
             * perspective), dc_colormap_index = 0 (full bright), and
             * dc_texturemid = skytexturemid, then routes through
             * pd_add_column2 which converts dc_source to a negative
             * patch handle. Specialize the paint to use the sky cache
             * - skips the per-frame Huffman decode after warmup. */
            paint_sky_column();
            break;
        default: return; /* MASKED, NONE, etc. -- skip */
    }
#if PERF_INSTRUMENT
    perf_paint_us += perf_us_since(perf_paint_t);
#endif
}

/* ---------- Sprite column queue --------------------------------
 * Sprites and masked mid-textures emit columns via this callback,
 * but rendering immediately would let later visplanes (R_DrawPlanes
 * runs after BSP) and farther walls overwrite them: sprites are
 * drawn during BSP traversal in this build (NO_VISSPRITES=1 routes
 * through R_DrawSpriteEarly, which renders inline rather than
 * deferring to a vissprite list). The Pico tolerates this because
 * its scanvideo renderer z-sorts per-scanline; we don't.
 *
 * The fix: buffer column-draw requests during BSP / R_DrawMasked,
 * then drain in pd_end_frame after all walls and visplanes are in
 * the framebuffer. The engine projects sprites in BSP front-to-back
 * order, so reverse-iterating the queue gives back-to-front draw
 * order without an explicit sort.
 *
 * Player weapon sprites (psprites) go to a separate small queue so
 * they can be drawn last, on top of enemies. The engine flags those
 * via pd_flag bit 1 (set in R_DrawPSprite). */
typedef struct {
    int16_t  x;
    int16_t  real_id;          /* negative = patch lump number */
    fixed_t  iscale;
    fixed_t  scale;            /* pd_scale at enqueue, for depth sort */
    fixed_t  texturemid;
    uint8_t  col;
    int8_t   colormap_index;
    uint8_t  translation_index;
    uint8_t  seg_count;
    uint16_t seg_offset;       /* into the matching seg buffer */
} sprite_queued_column_t;

#define SPRITE_QUEUE_MAX_COLS  512
#define SPRITE_QUEUE_SEG_BYTES 3072
#define PSPRITE_QUEUE_MAX_COLS 128
#define PSPRITE_QUEUE_SEG_BYTES 1024

static sprite_queued_column_t sprite_queue[SPRITE_QUEUE_MAX_COLS];
static uint8_t  sprite_queue_segs[SPRITE_QUEUE_SEG_BYTES];
static uint16_t sprite_queue_count;
static uint16_t sprite_queue_segs_used;

static sprite_queued_column_t psprite_queue[PSPRITE_QUEUE_MAX_COLS];
static uint8_t  psprite_queue_segs[PSPRITE_QUEUE_SEG_BYTES];
static uint16_t psprite_queue_count;
static uint16_t psprite_queue_segs_used;

/* Fuzz pattern: vanilla r_draw.c:309-318. Each entry is +-1 row
 * (the sample apply walks one row up or down). fuzzpos advances per
 * pixel and wraps at 50, deliberately persisting across columns /
 * sprites / frames so the shimmer pattern shifts continuously. The
 * fuzz reads the already-rendered RGB565 neighbour and darkens it
 * via the (rgb >> 1) & 0x7BEF mask (per-channel /2). */
static const int8_t fuzz_dy[50] = {
    +1, -1, +1, -1, +1, +1, -1, +1, +1, -1,
    +1, +1, +1, -1, +1, +1, +1, -1, -1, -1,
    -1, +1, -1, -1, +1, +1, +1, +1, -1, +1,
    +1, -1, -1, +1, +1, -1, -1, -1, -1, +1,
    +1, +1, +1, +1, -1, +1, -1, +1, +1, -1,
};
static int fuzzpos;

void pd_add_masked_columns(uint8_t *ys, int seg_count)
{
    int real_id = dc_source.real_id;
    if (real_id >= 0 || seg_count <= 0) return;
    if (dc_x < 0 || dc_x >= DOOM_W) return;
    /* Same x % 4 == 3 drop as walls and planes. Sprites are usually a
     * small fraction of the frame budget but every queue insert avoided
     * is a queued render call avoided in pd_end_frame's drain too. */
    if ((dc_x & 3) == 3) return;
    if (seg_count > 255) seg_count = 255;

    /* Legacy safety re-clip from the R_DrawSpriteEarly path: that path
     * pointed mfloorclip/mceilingclip at the global floorclip[] /
     * ceilingclip[] arrays, but R_DrawMaskedColumn (r_things.c:517-521)
     * compares as `yh >= mfloorclip[dc_x]` without subtracting
     * FLOOR_CEILING_CLIP_OFFSET, so the sprite could bleed one row into
     * the bottom wall under FLOOR_CEILING_CLIP_8BIT. We re-clipped here
     * with the offset to compensate.
     *
     * With NO_VISSPRITES=0 the engine drives painting through
     * R_DrawMasked -> R_DrawSprite, where mfloorclip/mceilingclip point
     * at per-sprite clipbot[]/cliptop[] arrays computed from drawseg
     * silhouettes, and R_DrawMaskedColumn's own clamp is correct. The
     * global floorclip[]/ceilingclip[] no longer reflect the per-sprite
     * silhouette and would over-clip back-sprites by walls of
     * intervening sectors, so we skip the secondary re-clip in this
     * mode. The gate is `pd_flag & 1`, set only by R_DrawSpriteEarly
     * (now neutered when NO_VISSPRITES=0), so this whole block is dead
     * in the current build but kept as a safety harness. */
    uint8_t local_segs[256 * 3];
    int kept = 0;
    int reclip = (pd_flag & 1);
    int cl_top = reclip ? (int)ceilingclip[dc_x] - FLOOR_CEILING_CLIP_OFFSET : -1;
    int cl_bot = reclip ? (int)floorclip[dc_x]   - FLOOR_CEILING_CLIP_OFFSET : DOOM_H;
    for (int s = 0; s < seg_count; s++) {
        int yl = ys[s * 3];
        int yh = ys[s * 3 + 1];
        int base_off = ys[s * 3 + 2];
        if (yl <= cl_top) yl = cl_top + 1;
        if (yh >= cl_bot) yh = cl_bot - 1;
        if (yl > yh) continue;
        local_segs[kept * 3]     = (uint8_t)yl;
        local_segs[kept * 3 + 1] = (uint8_t)yh;
        /* base_off is the col_buf offset (compressed-pixel space),
         * not screen-y space. Trimming yl in screen space is handled
         * automatically by the frac/fracstep iterator at draw time -
         * advancing yl by N rows advances frac by N*fracstep, which
         * naturally lands on the right col_buf index. Adjusting
         * base_off here would double-count and shift the sampled
         * pixels by N at non-1:1 scaling. */
        local_segs[kept * 3 + 2] = (uint8_t)base_off;
        kept++;
    }
    if (kept == 0) return;
    int seg_bytes = kept * 3;

    sprite_queued_column_t *q;
    uint8_t *segs;
    if (pd_flag & 2) {
        if (psprite_queue_count >= PSPRITE_QUEUE_MAX_COLS) return;
        if (psprite_queue_segs_used + seg_bytes > PSPRITE_QUEUE_SEG_BYTES) return;
        q = &psprite_queue[psprite_queue_count++];
        q->seg_offset = psprite_queue_segs_used;
        segs = &psprite_queue_segs[psprite_queue_segs_used];
        psprite_queue_segs_used += seg_bytes;
    } else {
        if (sprite_queue_count >= SPRITE_QUEUE_MAX_COLS) return;
        if (sprite_queue_segs_used + seg_bytes > SPRITE_QUEUE_SEG_BYTES) return;
        q = &sprite_queue[sprite_queue_count++];
        q->seg_offset = sprite_queue_segs_used;
        segs = &sprite_queue_segs[sprite_queue_segs_used];
        sprite_queue_segs_used += seg_bytes;
    }
    q->x                 = (int16_t)dc_x;
    q->real_id           = (int16_t)real_id;
    q->iscale            = dc_iscale;
    q->scale             = pd_scale;
    q->texturemid        = dc_texturemid;
    q->col               = dc_source.col;
    q->colormap_index    = dc_colormap_index;
    q->translation_index = dc_translation_index;
    q->seg_count         = (uint8_t)kept;
    memcpy(segs, local_segs, seg_bytes);
}

/* Render one queued column. Reuses the single-slot patch decoder
 * cache - successive columns from the same sprite are usually
 * adjacent in the queue, so the cache hits 95%+ of the time. */
static void render_queued_column(const sprite_queued_column_t *q,
                                 const uint8_t *seg_buf)
{
    /* Fuzz path: MF_SHADOW mobjs (Spectres, Partial-Invisibility-affected
     * actors) arrive with colormap_index < 0. Vanilla R_DrawFuzzColumn
     * (r_draw.c:305-383) ignores the patch entirely and resamples a
     * neighbour framebuffer pixel through colormap 6 (the shadow tint),
     * walking fuzz_dy[] to pick +/- 1 row per pixel. drain_sprite_queues
     * runs after walls/floors are painted, so I_VideoBuffer holds the
     * already-rendered scenery underneath. */
    if (q->colormap_index < 0) {
        /* Fuzz / spectre transparency. Vanilla samples a neighbour
         * palette index through colormap 6 (a darkening colormap).
         * In our RGB565 buffer there's no palette index to recover,
         * so darken the neighbour pixel directly: shift every RGB565
         * channel right by one bit (mask 0x7BEF clears the carry from
         * R bit11 -> G bit10 and from G bit5 -> B bit4). Visually
         * close to colormap[6] for the "dark wobble" look. */
        int x = q->x;
        if ((unsigned)x >= (unsigned)DOOM_W) return;
        int xo = x_squeeze(x);
        const uint8_t *ys = &seg_buf[q->seg_offset];
        for (int s = 0; s < q->seg_count; s++) {
            int yl = ys[s * 3];
            int yh = ys[s * 3 + 1];
            if (yh > DOOM_H - 1) yh = DOOM_H - 1;
            for (int y = yl; y <= yh; y++) {
                int sy = y + fuzz_dy[fuzzpos];
                fuzzpos++;
                if (fuzzpos >= 50) fuzzpos = 0;
                if (sy < 0) sy = 0;
                else if (sy > DOOM_H - 1) sy = DOOM_H - 1;
                uint16_t neighbour = I_VideoBuffer[sy * DISP_W + xo];
                I_VideoBuffer[y * DISP_W + xo] =
                    (uint16_t)((neighbour >> 1) & 0x7BEFu);
            }
        }
        return;
    }

    int patch_num = -q->real_id;
    load_patch_decoder(patch_num);
    if (cached_width <= 0) return;

    int col = q->col;
    if (col >= cached_width) col %= cached_width;

    fixed_t fracstep_pre = q->iscale;

    /* Compute the highest col_buf[] row touched across all segments
     * of this queued column. Each segment shifts the texturemid by
     * base_off rows, so each gets its own frac range; we take the
     * union. If any segment wraps the 128-row boundary or spans more
     * than 128 rows, fall back to full decode. */
    int max_row = 0;
    int need_full = 0;
    {
        const uint8_t *ys = &seg_buf[q->seg_offset];
        for (int s = 0; s < q->seg_count; s++) {
            int yl = ys[s * 3];
            int yh = ys[s * 3 + 1];
            int base_off = ys[s * 3 + 2];
            if (yh > DOOM_H - 1) yh = DOOM_H - 1;
            if (yl > yh) continue;
            fixed_t tex_mid_seg =
                q->texturemid - ((fixed_t)base_off << FRACBITS);
            fixed_t frac0 = tex_mid_seg + (yl - centery) * fracstep_pre;
            int span_rows = yh - yl;
            int64_t span_frac = (int64_t)span_rows * (int64_t)fracstep_pre;
            int64_t span_abs = span_frac < 0 ? -span_frac : span_frac;
            if (span_abs >= ((int64_t)WHD_TEXTURE_COL_HEIGHT << FRACBITS)) {
                need_full = 1; break;
            }
            fixed_t frac_end = frac0 + (fixed_t)span_frac;
            fixed_t lo = (frac0 < frac_end) ? frac0 : frac_end;
            fixed_t hi = (frac0 < frac_end) ? frac_end : frac0;
            int lo_row = (int)((lo >> FRACBITS) & (WHD_TEXTURE_COL_HEIGHT - 1));
            int hi_row = (int)((hi >> FRACBITS) & (WHD_TEXTURE_COL_HEIGHT - 1));
            if (lo_row > hi_row) { need_full = 1; break; }
            if (hi_row > max_row) max_row = hi_row;
        }
    }
    if (need_full) max_row = WHD_TEXTURE_COL_HEIGHT - 1;

    uint8_t col_buf[WHD_TEXTURE_COL_HEIGHT];
    decode_patch_column(col, col_buf, max_row);

    /* Player-colour translation - same formula pd_render.cpp:
     * draw_patch_columns uses for the 0x70-range green marine
     * pixels. Bounded by max_row since rows above that aren't
     * decoded and aren't sampled. */
    if (q->translation_index) {
        int tbase = 0x80 - (int)q->translation_index * 0x20;
        for (int yy = 0; yy <= max_row; yy++) {
            if ((col_buf[yy] >> 4) == 7) {
                col_buf[yy] = (uint8_t)(tbase + (col_buf[yy] & 0x0f));
            }
        }
    }

    fixed_t fracstep = q->iscale;
    /* Fuzz (colormap_index < 0) is handled via the early return at the
     * top of this function; here colormap_index is guaranteed >= 0. */
    const uint16_t *clut = lit_lut[q->colormap_index];
    int x = q->x;
    int xo = x_squeeze(x);
    const uint8_t *ys = &seg_buf[q->seg_offset];

    for (int s = 0; s < q->seg_count; s++) {
        int yl = ys[s * 3];
        int yh = ys[s * 3 + 1];
        int base_off = ys[s * 3 + 2];
        if (yh > DOOM_H - 1) yh = DOOM_H - 1;
        if (yl > yh) continue;

        fixed_t tex_mid_seg = q->texturemid - ((fixed_t)base_off << FRACBITS);
        fixed_t frac = tex_mid_seg + (yl - centery) * fracstep;
        pixel_t *dest = &I_VideoBuffer[yl * DISP_W + xo];
        for (int y = yl; y <= yh; y++) {
            *dest = clut[col_buf[(frac >> FRACBITS) & 127]];
            dest += DISP_W;
            frac += fracstep;
        }
    }
}

/* Drain both queues. R_DrawMasked queues vissprite columns first
 * (back-to-front by R_SortVisSprites) and then walks drawsegs in
 * reverse to queue masked-midtex columns - so a midtex column lands
 * in the queue AFTER every sprite column, even when the midtex wall
 * is further from the camera than the sprite. A naive forward drain
 * paints the wall over a closer sprite (lamps/imps clipped by the
 * diagonal-stripe walls in E1M1's exit antechamber).
 *
 * Fix: stable-sort sprite_queue[] by `scale` ascending (lowest scale
 * = furthest, painted first). pd_scale is captured per column in
 * pd_add_masked_columns from the upstream globals set in
 * R_DrawVisSprite (r_things.c:612) and R_RenderMaskedSegRange
 * (r_segs.c:192) under PD_SCALE_SORT=1. R_SortVisSprites itself
 * orders vissprites by ascending scale, so within equal scales the
 * stable sort preserves the engine's left-to-right column emit
 * order. The patch-decoder cache stays warm within each sprite
 * because all columns of one vissprite share vis->scale and remain
 * adjacent after sorting. Player weapon sprites stay last unsorted
 * so they sit on top of everything else but the HUD. */
static uint16_t sprite_queue_order[SPRITE_QUEUE_MAX_COLS];
static uint16_t sprite_queue_scratch[SPRITE_QUEUE_MAX_COLS];

static void sort_sprite_queue_by_scale(void)
{
    int n = sprite_queue_count;
    for (int i = 0; i < n; i++) sprite_queue_order[i] = (uint16_t)i;

    uint16_t *src = sprite_queue_order;
    uint16_t *dst = sprite_queue_scratch;
    for (int width = 1; width < n; width *= 2) {
        for (int i = 0; i < n; i += 2 * width) {
            int left = i;
            int right = i + width;
            int end = i + 2 * width;
            if (right > n) right = n;
            if (end > n) end = n;
            int l = left, r = right, o = left;
            while (l < right && r < end) {
                /* Stable: take left when scales are equal. */
                if (sprite_queue[src[r]].scale < sprite_queue[src[l]].scale) {
                    dst[o++] = src[r++];
                } else {
                    dst[o++] = src[l++];
                }
            }
            while (l < right) dst[o++] = src[l++];
            while (r < end)   dst[o++] = src[r++];
        }
        uint16_t *tmp = src; src = dst; dst = tmp;
    }
    if (src != sprite_queue_order) {
        memcpy(sprite_queue_order, src, n * sizeof(uint16_t));
    }
}

static void drain_sprite_queues(void)
{
    sort_sprite_queue_by_scale();
    for (int i = 0; i < sprite_queue_count; i++) {
        render_queued_column(&sprite_queue[sprite_queue_order[i]],
                             sprite_queue_segs);
    }
    for (int i = 0; i < psprite_queue_count; i++) {
        render_queued_column(&psprite_queue[i], psprite_queue_segs);
    }
    sprite_queue_count = 0;
    sprite_queue_segs_used = 0;
    psprite_queue_count = 0;
    psprite_queue_segs_used = 0;
}

/* ---------- Flat decoder + LRU cache -----------------------------
 * WHD flats are 64x64 = 4096-pixel tiles, Huffman-encoded. Decoding
 * one is much cheaper than a multi-patch texture but still ~10K cycles.
 * E1M1's view typically shows 2-3 distinct flats at a time (CEIL5_1
 * + FLOOR4_8 + FLOOR0_1 ish), so the 7-slot LRU keeps the steady state
 * decode-free. */
/* Flat decoder LRU cache. Each slot is one 64x64 = 4096-byte tile.
 * The RGB565 framebuffer refactor freed 64 KB (doom_fb removed) but
 * spent ~16.5 KB on lit_lut and ~64 KB on the doubled wipe buffers,
 * netting +16 KB BSS. We pay for it by dropping one flat slot. The
 * E1M1 typical view shows 2-3 flats simultaneously, so 7 slots is
 * still well above the steady-state miss boundary. */
#define FLAT_CACHE_SLOTS 7
#define FLAT_PIXELS      4096

static uint8_t flat_cache[FLAT_CACHE_SLOTS][FLAT_PIXELS];
static int     flat_cache_picnum[FLAT_CACHE_SLOTS] =
    {-1, -1, -1, -1, -1, -1, -1};
static uint8_t flat_cache_lru[FLAT_CACHE_SLOTS] =
    {0, 1, 2, 3, 4, 5, 6};

static void decode_flat(int picnum, uint8_t *dst)
{
    /* The shared splash_decoder_buf holds the Huffman code table;
     * splash_decoder_tmp doubles as the prefix-length scratch when the
     * WHD flat path uses th_decode_table_special (8-bit decode). The
     * patch-decoder cache is now stale - invalidate. */
    cached_patch_num = -1;

    const uint8_t *sourcez =
        (const uint8_t *)W_CacheLumpNum(firstflat + picnum, 0 /* PU_STATIC */);
    th_bit_input bi;
    th_bit_input_init(&bi, sourcez);
    if (th_bit(&bi)) {
        th_read_simple_decoder(&bi, splash_decoder_buf,
                               sizeof(splash_decoder_buf) / 2,
                               splash_decoder_tmp,
                               sizeof(splash_decoder_tmp));
    } else {
        read_raw_pixels_decoder(&bi, splash_decoder_buf,
                                sizeof(splash_decoder_buf) / 2,
                                splash_decoder_tmp,
                                sizeof(splash_decoder_tmp));
    }
    /* The flat decoder's prefix table also lives in splash_decoder_tmp
     * (per pd_render.cpp:decode_flat_to_slot). Sized at 256 bytes; our
     * 1024-byte tmp buffer is fine. */
    th_make_prefix_length_table(splash_decoder_buf, splash_decoder_tmp);

    bool have_same = th_bit(&bi);
    if (!have_same) {
        for (int y = 0; y < FLAT_PIXELS; y++) {
            dst[y] = th_decode_table_special(splash_decoder_buf,
                                             splash_decoder_tmp, &bi);
        }
    } else {
        /* Per-column with a back-reference shortcut: a column may say
         * "I'm the same as column xf" instead of carrying its own data. */
        for (int x = 0; x < 64; x++) {
            uint8_t *p = &dst[x * 64];
            if (th_bit(&bi)) {
                unsigned bits = bitcount8(x);
                unsigned xf = bits ? th_read_bits(&bi, bits) : 0;
                /* xf < x; copy column xf into column x. */
                memcpy(p, &dst[xf * 64], 64);
            } else {
                for (int y = 0; y < 64; y++) {
                    *p++ = th_decode_table_special(splash_decoder_buf,
                                                   splash_decoder_tmp, &bi);
                }
            }
        }
    }
}

static const uint8_t *load_flat(int picnum)
{
    /* Hit? Promote to MRU and return. */
    for (int i = 0; i < FLAT_CACHE_SLOTS; i++) {
        if (flat_cache_picnum[i] == picnum) {
            /* Move flat_cache_lru's reference to the head. */
            for (int j = 0; j < FLAT_CACHE_SLOTS; j++) {
                if (flat_cache_lru[j] == i) {
                    while (j > 0) {
                        flat_cache_lru[j] = flat_cache_lru[j - 1];
                        j--;
                    }
                    flat_cache_lru[0] = (uint8_t)i;
                    break;
                }
            }
            return flat_cache[i];
        }
    }
    /* Miss: evict the LRU slot and decode into it. */
    int slot = flat_cache_lru[FLAT_CACHE_SLOTS - 1];
    decode_flat(picnum, flat_cache[slot]);
    flat_cache_picnum[slot] = picnum;
    /* Promote slot to MRU. */
    for (int j = FLAT_CACHE_SLOTS - 1; j > 0; j--) {
        flat_cache_lru[j] = flat_cache_lru[j - 1];
    }
    flat_cache_lru[0] = (uint8_t)slot;
    return flat_cache[slot];
}

/* Plane lighting profile. `level[y] = clamp(startmap -
 * (SCREENWIDTH/4)/(zindex+1), 0, NUMCOLORMAPS-1)` with zindex =
 * (plane_h * yslope[y]) >> LIGHTZSHIFT. startmap and plane_h are
 * per-visplane constants, so the entire level[y] table is. We
 * recompute it on the first access for a visplane (either from
 * pd_finalize_planes or from the overlap fallback) and reuse it
 * across every span / column emitted for that same visplane.
 *
 * The cache is a single slot keyed by fd_num because both the BSP-walk
 * column emissions and the finalize span emissions cluster by fd_num.
 * Reset to -1 in pd_begin_frame so frame-to-frame fd_num reuse can't
 * serve stale data. yslope[] is sized MAIN_VIEWHEIGHT (168) under
 * DOOM_TINY; status-bar rows never receive plane spans. */
static int     plane_light_cached_fd = -1;
static uint8_t plane_light_level[MAIN_VIEWHEIGHT] __attribute__((section(".sram4")));

static void plane_light_refresh(int fd_num, fixed_t plane_h)
{
    if (fd_num == plane_light_cached_fd) return;
    int light = (visplanes[fd_num].lightlevel >> LIGHTSEGSHIFT) + extralight;
    if (light < 0)             light = 0;
    if (light >= LIGHTLEVELS)  light = LIGHTLEVELS - 1;
    int startmap = ((LIGHTLEVELS - 1 - light) * 2) * NUMCOLORMAPS / LIGHTLEVELS;
    for (int yy = 0; yy < MAIN_VIEWHEIGHT; yy++) {
        fixed_t distance = FixedMul(plane_h, yslope[yy]);
        unsigned zindex  = (unsigned)distance >> LIGHTZSHIFT;
        int scale_l      = (SCREENWIDTH / 4) / (int)(zindex + 1);
        int level        = startmap - scale_l;
        if (level < 0)             level = 0;
        if (level >= NUMCOLORMAPS) level = NUMCOLORMAPS - 1;
        plane_light_level[yy] = (uint8_t)level;
    }
    plane_light_cached_fd = fd_num;
}

/* Plane buffering for span-major draw. pd_add_plane_column is called
 * column-by-column from r_segs.c during BSP traversal; rather than
 * rasterise each column with two FixedMul-per-pixel (the classic
 * column-major R_MapPlane), we record the (top, bot) row range per
 * (visplane, x) here and walk it as horizontal spans in
 * pd_finalize_planes. Along a span (constant y) the world-space
 * delta per pixel is constant, so the inner loop drops to two
 * adds + one flat sample + one LUT lookup per pixel - roughly 2.5x
 * the column-major cost on Cortex-M33.
 *
 * Storage: 128 visplanes * 320 columns * 2 bytes (top + bot) = 80 KB
 * main BSS, plus ~768 B of per-visplane bookkeeping in SRAM4. The
 * sentinel for "column not touched" is plane_top[x] = 0xff; bot is
 * undefined for untouched columns and is never read.
 *
 * fd_num is bounded by MAXVISPLANES (128) and clamped on entry to
 * pd_add_plane_column. The visplane layer hands out indices densely
 * from 0 within a frame (R_ClearPlanes resets lastvisplane). */
/* Sentinels in plane_top[]:
 *   PD_PLANE_UNTOUCHED (0xff) - column not emitted this frame.
 *   PD_PLANE_INLINE_DRAWN (0xfe) - both emissions for this column were
 *     painted column-major in BSP order (overlap fallback). Treated as
 *     untouched by the makespans walker so no span paints over them.
 * Real top values are in [0, MAIN_VIEWHEIGHT - 1] = [0, 167], so 0xfe
 * and 0xff cannot collide with a legitimate value. */
#define PD_PLANE_UNTOUCHED    0xff
#define PD_PLANE_INLINE_DRAWN 0xfe
static uint8_t plane_top[MAXVISPLANES][DOOM_W];
static uint8_t plane_bot[MAXVISPLANES][DOOM_W];

/* Per-frame visplane bookkeeping, sized for MAXVISPLANES = 128 entries.
 * plane_used_list keeps fd_nums in touch order so finalize iterates
 * only over touched visplanes (typically << 128). plane_used_bit makes
 * the dedup check O(1) on insert. */
static int16_t plane_minx[MAXVISPLANES] __attribute__((section(".sram4")));
static int16_t plane_maxx[MAXVISPLANES] __attribute__((section(".sram4")));
static uint8_t plane_used_bit[MAXVISPLANES] __attribute__((section(".sram4")));
static uint8_t plane_used_list[MAXVISPLANES] __attribute__((section(".sram4")));
static int     plane_used_count;

/* spanstart[y] - x where the currently-open span at row y began. Reused
 * across visplanes since every open span is closed before the next
 * visplane starts. */
static uint16_t plane_spanstart[MAIN_VIEWHEIGHT] __attribute__((section(".sram4")));

/* Per-column cos(angle(x)) * distscale(x) and sin(angle(x)) * distscale(x).
 * Frame-constant (depend only on viewangle and x); shared across every
 * visplane that touches column x via the overlap-fallback path. The
 * span drawer uses the basexscale / baseyscale form instead so it
 * doesn't read this cache, but on heavy zig-zag geometry many overlap
 * fallbacks land on the same x with different fd_nums, where caching
 * the per-x trig pays off. Filled lazily; valid bitmap cleared in
 * pd_begin_frame. ~2.9 KB in SRAM4. */
static fixed_t plane_xcos[DOOM_W] __attribute__((section(".sram4")));
static fixed_t plane_xsin[DOOM_W] __attribute__((section(".sram4")));
static uint8_t plane_xcache_valid[DOOM_W] __attribute__((section(".sram4")));

/* Self-contained column-major fallback for the rare overlap case in
 * pd_add_plane_column (see comment there). No global caches: lighting,
 * cos/sin/dscale, and flat are all derived per call. Slow per pixel
 * vs. the span drawer, but only fires when the engine reuses a visplane
 * for two stacked emissions at the same x - typically a handful of
 * columns per frame in zig-zag floor geometry. */
static void paint_plane_column_immediate(int x, int yl, int yh, int fd_num)
{
    int picnum = visplanes[fd_num].picnum;
    if (picnum == skyflatnum) {
        paint_column(x, yl, yh, COLOUR_SKY);
        return;
    }
    if (whd_flattospecial[picnum] != 0xff) {
        picnum = whd_specialtoflat[whd_flattranslation[whd_flattospecial[picnum]]];
    }
    const uint8_t *flat = load_flat(picnum);
    if (!flat) {
        paint_column(x, yl, yh,
                     (visplanes[fd_num].height < viewz) ? COLOUR_FLOOR : COLOUR_CEILING);
        return;
    }

    fixed_t plane_h = abs(visplanes[fd_num].height - viewz);
    plane_light_refresh(fd_num, plane_h);

    if (!plane_xcache_valid[x]) {
        angle_t angle  = (viewangle + x_to_viewangle(x)) >> ANGLETOFINESHIFT;
        fixed_t cos_a  = finecosine(angle);
        fixed_t sin_a  = finesine(angle);
        fixed_t dscale = distscale(x);
        plane_xcos[x] = FixedMul(cos_a, dscale);
        plane_xsin[x] = FixedMul(sin_a, dscale);
        plane_xcache_valid[x] = 1;
    }
    fixed_t kx = FixedMul(plane_xcos[x], plane_h);
    fixed_t ky = FixedMul(plane_xsin[x], plane_h);

    pixel_t *dest = &I_VideoBuffer[yl * DISP_W + x_squeeze(x)];
    for (int y = yl; y <= yh; y++) {
        fixed_t world_x = viewx  + FixedMul(kx, yslope[y]);
        fixed_t world_y = -viewy - FixedMul(ky, yslope[y]);
        unsigned u = ((unsigned)world_x >> FRACBITS) & 63;
        unsigned v = ((unsigned)world_y >> FRACBITS) & 63;
        const uint16_t *clut = lit_lut[plane_light_level[y]];
        *dest = clut[flat[(v << 6) | u]];
        dest += DISP_W;
    }
}

void pd_add_plane_column(int x, int yl, int yh, fixed_t scale,
                         int floor, int fd_num)
{
    (void)scale;
    (void)floor;

    if (yl > yh) return;
    if (yl < 0) yl = 0;
    /* Cap to MAIN_VIEWHEIGHT - 1 so the per-row `yslope[y]` access in
     * pd_finalize_planes stays in bounds. Plane columns never need to
     * reach into the status bar region (rows 168..199); the engine
     * emits ceiling / floor spans only across the view area. */
    if (yh > MAIN_VIEWHEIGHT - 1) yh = MAIN_VIEWHEIGHT - 1;
    if (x < 0 || x >= DOOM_W) return;
    /* Same x % 4 == 3 drop as pd_add_column - the SPI converter never
     * ships these columns to the panel, so the per-pixel projection
     * + flat sample work is pure waste. */
    if ((x & 3) == 3) return;

    if (fd_num < 0 || fd_num >= MAXVISPLANES) return;
    int picnum = visplanes[fd_num].picnum;

    /* Sky flat: paint immediately as a colour stripe. Sky doesn't go
     * through the span drawer (no flat sampling); also lets us skip
     * adding sky visplanes to the touch list. */
    if (picnum == skyflatnum) {
        paint_column(x, yl, yh, COLOUR_SKY);
        return;
    }

    /* Overlap case: under NO_VISPLANE_GUTS=1 the engine has no
     * R_CheckPlane to split a visplane when its silhouette overlaps,
     * so two stacked wall segments sharing (height, picnum, light)
     * can both emit at the same x for the same fd_num. With the
     * column-major path each call painted directly in BSP order and
     * later emissions overdrew earlier ones in any overlapping pixel;
     * here we'd lose the buffered one. Reproduce the old behaviour:
     * paint BOTH the buffered range and the new one column-major,
     * then mark the column INLINE_DRAWN so the makespans walk skips
     * it (preventing spans painting over the inline pixels). */
    uint8_t prev_top = plane_top[fd_num][x];
    if (prev_top != PD_PLANE_UNTOUCHED) {
        if (prev_top != PD_PLANE_INLINE_DRAWN) {
            /* First overlap: also flush the previously-buffered range
             * inline so paint order matches the column-major code path
             * (buffered emission drawn first, then this one on top). */
            paint_plane_column_immediate(x, prev_top, plane_bot[fd_num][x], fd_num);
        }
        paint_plane_column_immediate(x, yl, yh, fd_num);
        plane_top[fd_num][x] = PD_PLANE_INLINE_DRAWN;
        return;
    }

    /* First emission for this (fd, x): buffer the row range. Defer all
     * of flat load, animation translation, lighting profile, and
     * per-pixel sampling to pd_finalize_planes which can hoist them
     * per-span / per-visplane instead of paying per-column. */
    plane_top[fd_num][x] = (uint8_t)yl;
    plane_bot[fd_num][x] = (uint8_t)yh;
    if (!plane_used_bit[fd_num]) {
        plane_used_bit[fd_num] = 1;
        plane_used_list[plane_used_count++] = (uint8_t)fd_num;
        plane_minx[fd_num] = (int16_t)x;
        plane_maxx[fd_num] = (int16_t)x;
    } else {
        if (x < plane_minx[fd_num]) plane_minx[fd_num] = (int16_t)x;
        if (x > plane_maxx[fd_num]) plane_maxx[fd_num] = (int16_t)x;
    }
}

/* Span drawer for the buffered plane path. Along a span at row y, the
 * world-space step per x-pixel is constant: ds_xstep = distance *
 * basexscale, ds_ystep = distance * baseyscale, with distance =
 * plane_h * yslope[y]. So the inner loop is two adds + one flat sample
 * + one LUT lookup, no FixedMul.
 *
 * 3-of-4 unroll: the (x & 3) == 3 columns are dropped at SPI conversion
 * time. Hoisting that test out of the per-pixel loop keeps the inner
 * body branch-free in the common 4-aligned middle of the span. */
static void paint_flat_span(int y, int xl, int xh, fixed_t plane_h,
                            const uint8_t *flat)
{
    if (xl > xh) return;

    fixed_t distance = FixedMul(plane_h, yslope[y]);
    fixed_t xstep    = FixedMul(distance, basexscale);
    fixed_t ystep    = FixedMul(distance, baseyscale);
    fixed_t length   = FixedMul(distance, distscale(xl));
    angle_t angle    = (viewangle + x_to_viewangle(xl)) >> ANGLETOFINESHIFT;
    fixed_t xfrac    = viewx  + FixedMul(finecosine(angle), length);
    fixed_t yfrac    = -viewy - FixedMul(finesine(angle), length);

    const uint16_t *clut = lit_lut[plane_light_level[y]];
    pixel_t *dest = &I_VideoBuffer[y * DISP_W + x_squeeze(xl)];

    int x = xl;
    /* Head: align x to a 4-pixel boundary. */
    while ((x & 3) != 0 && x <= xh) {
        if ((x & 3) != 3) {
            unsigned u = ((unsigned)xfrac >> FRACBITS) & 63;
            unsigned v = ((unsigned)yfrac >> FRACBITS) & 63;
            *dest++ = clut[flat[(v << 6) | u]];
        }
        xfrac += xstep; yfrac += ystep; x++;
    }
    /* Body: 4-pixel groups, write 3, skip 1. */
    while (x + 3 <= xh) {
        unsigned u, v;
        u = ((unsigned)xfrac >> FRACBITS) & 63;
        v = ((unsigned)yfrac >> FRACBITS) & 63;
        *dest++ = clut[flat[(v << 6) | u]];
        xfrac += xstep; yfrac += ystep;
        u = ((unsigned)xfrac >> FRACBITS) & 63;
        v = ((unsigned)yfrac >> FRACBITS) & 63;
        *dest++ = clut[flat[(v << 6) | u]];
        xfrac += xstep; yfrac += ystep;
        u = ((unsigned)xfrac >> FRACBITS) & 63;
        v = ((unsigned)yfrac >> FRACBITS) & 63;
        *dest++ = clut[flat[(v << 6) | u]];
        xfrac += xstep + xstep; yfrac += ystep + ystep;
        x += 4;
    }
    /* Tail. */
    while (x <= xh) {
        if ((x & 3) != 3) {
            unsigned u = ((unsigned)xfrac >> FRACBITS) & 63;
            unsigned v = ((unsigned)yfrac >> FRACBITS) & 63;
            *dest++ = clut[flat[(v << 6) | u]];
        }
        xfrac += xstep; yfrac += ystep; x++;
    }
}

/* Walk the touched visplanes, convert their (top[x], bot[x]) silhouettes
 * to horizontal spans (vanilla R_MakeSpans algorithm), and paint via
 * paint_flat_span. Called from pd_end_frame before sprite drain so the
 * planes appear under sprites in the framebuffer. */
static void pd_finalize_planes(void)
{
    for (int idx = 0; idx < plane_used_count; idx++) {
        int fd_num = plane_used_list[idx];
        plane_used_bit[fd_num] = 0;

        int picnum = visplanes[fd_num].picnum;
        /* Sky was painted immediately in pd_add_plane_column. */
        if (picnum == skyflatnum) goto cleanup_columns;

        /* Animated/translated flats (NUKAGE, FWATER, etc.). */
        if (whd_flattospecial[picnum] != 0xff) {
            picnum = whd_specialtoflat[whd_flattranslation[whd_flattospecial[picnum]]];
        }

        const uint8_t *flat   = load_flat(picnum);
        fixed_t        plane_h = abs(visplanes[fd_num].height - viewz);
        uint8_t        fb_colour = (visplanes[fd_num].height < viewz)
                                       ? COLOUR_FLOOR : COLOUR_CEILING;

        plane_light_refresh(fd_num, plane_h);

        int minx = plane_minx[fd_num];
        int maxx = plane_maxx[fd_num];

        /* R_MakeSpans sweep: walk x from minx to maxx+1; the +1 step
         * forces every still-open span to close. Sentinels for an
         * untouched column: t = MAIN_VIEWHEIGHT (> any real bot),
         * b = -1 (< any real top), so the makespans loops fire only
         * on real transitions. */
        int prev_t = MAIN_VIEWHEIGHT, prev_b = -1;
        for (int x = minx; x <= maxx + 1; x++) {
            int orig_t, orig_b;
            uint8_t cell = (x > maxx) ? PD_PLANE_UNTOUCHED : plane_top[fd_num][x];
            if (cell >= PD_PLANE_INLINE_DRAWN) {
                /* Untouched or inline-drawn: no span emission for this x. */
                orig_t = MAIN_VIEWHEIGHT;
                orig_b = -1;
            } else {
                orig_t = cell;
                orig_b = plane_bot[fd_num][x];
            }
            int t = orig_t, b = orig_b;

            if (flat) {
                while (prev_t < t && prev_t <= prev_b) {
                    paint_flat_span(prev_t, plane_spanstart[prev_t],
                                    x - 1, plane_h, flat);
                    prev_t++;
                }
                while (prev_b > b && prev_b >= prev_t) {
                    paint_flat_span(prev_b, plane_spanstart[prev_b],
                                    x - 1, plane_h, flat);
                    prev_b--;
                }
            } else {
                /* No flat data - paint a flat colour stripe for each
                 * closing run. Mirrors the legacy fallback in the
                 * column-major path. */
                while (prev_t < t && prev_t <= prev_b) {
                    int xl = plane_spanstart[prev_t], xh = x - 1;
                    pixel_t pix = palette_rgb565[fb_colour];
                    pixel_t *dst = &I_VideoBuffer[prev_t * DISP_W];
                    for (int xx = xl; xx <= xh; xx++) {
                        if ((xx & 3) != 3)
                            dst[x_squeeze(xx)] = pix;
                    }
                    prev_t++;
                }
                while (prev_b > b && prev_b >= prev_t) {
                    int xl = plane_spanstart[prev_b], xh = x - 1;
                    pixel_t pix = palette_rgb565[fb_colour];
                    pixel_t *dst = &I_VideoBuffer[prev_b * DISP_W];
                    for (int xx = xl; xx <= xh; xx++) {
                        if ((xx & 3) != 3)
                            dst[x_squeeze(xx)] = pix;
                    }
                    prev_b--;
                }
            }
            while (t < prev_t && t <= b) {
                plane_spanstart[t] = (uint16_t)x;
                t++;
            }
            while (b > prev_b && b >= t) {
                plane_spanstart[b] = (uint16_t)x;
                b--;
            }
            prev_t = orig_t;
            prev_b = orig_b;
        }

cleanup_columns:
        /* Reset the touched columns so the buffer is clean for the next
         * frame. Walk minx..maxx only - untouched columns kept their
         * sentinel from the previous reset. */
        for (int x = plane_minx[fd_num]; x <= plane_maxx[fd_num]; x++) {
            plane_top[fd_num][x] = PD_PLANE_UNTOUCHED;
        }
    }
    plane_used_count = 0;
}

/* ---------- Wipe (melt) animation -------------------------------
 * The classic Doom screen melt, restored after the RGB565 ping-pong
 * refactor. Standard implementations need two snapshot buffers
 * ("from" and "to") plus a composite destination = 3 * 96 KB. We
 * don't have the budget. Instead we exploit two facts about the
 * ping-pong layout to skip extra storage entirely:
 *
 *   1. At wipe-start the previously-displayed buffer (front_buf)
 *      already holds the "from" image; the just-rendered buffer
 *      (back_buf) holds the "to" image. They're our snapshots.
 *   2. The melt is monotonic - wipe_y[i] only ever advances, so a
 *      pixel that has shifted from "from" to "to" never reverts.
 *
 * That lets us composite IN PLACE on the "from" buffer: each step
 * overwrites rows [0, wipe_y[i]) with pixels from "to". The "from"
 * pixels we destroy are exactly the ones that no longer need to be
 * read.
 *
 * During the wipe the engine's render gate is closed (D_Display only
 * runs pd_begin_frame + pd_end_frame, no R_RenderPlayerView), so the
 * buffers stay quiescent except for our composite + DMA. We use a
 * synchronous DMA path during wipe (see i_video_begin_wipe /
 * i_video_end_wipe in i_video_stm32.c) since the wipe is short and
 * the simpler control flow is worth not having to track an in-flight
 * DMA across composite steps.
 *
 * D_Display's `do { D_Display(); } while (wipestate);` loop in
 * d_main.c:515 keeps re-entering as long as wipestate != NONE. We
 * set wipestate to WIPESTATE_SKIP1 on the first wipe frame and clear
 * it back to NONE when wipe_step_melt reports done. */
static int16_t wipe_y[DISP_W];

extern void           i_video_begin_wipe(void);
extern void           i_video_end_wipe(void);
extern pixel_t       *i_video_wipe_dest(void);
extern const pixel_t *i_video_wipe_to(void);

static void wipe_init_melt(void)
{
    /* Each column starts at a small random negative offset so the
     * top edges of the new screen creep down at slightly staggered
     * times. Smoothing between adjacent columns avoids a strobing
     * look. Same constants as vanilla Doom's wipe_initMelt. */
    wipe_y[0] = -(int16_t)(M_Random() % 16);
    for (int i = 1; i < DISP_W; i++) {
        int r = (M_Random() % 3) - 1;
        wipe_y[i] = wipe_y[i - 1] + (int16_t)r;
        if (wipe_y[i] > 0)         wipe_y[i] = 0;
        else if (wipe_y[i] == -16) wipe_y[i] = -15;
    }
}

/* Advance one tic. Returns 1 when every column has scrolled all
 * 200 rows ("from" fully replaced by "to"). */
static int wipe_step_melt(void)
{
    int done = 1;
    for (int i = 0; i < DISP_W; i++) {
        if (wipe_y[i] < 0) {
            wipe_y[i]++;
            done = 0;
        } else if (wipe_y[i] < DOOM_H) {
            int dy = (wipe_y[i] < 16) ? wipe_y[i] + 1 : 8;
            if (wipe_y[i] + dy >= DOOM_H) dy = DOOM_H - wipe_y[i];
            wipe_y[i] += (int16_t)dy;
            done = 0;
        }
    }
    return done;
}

/* In-place composite: for each column i, overwrite rows
 * [0, wipe_y[i]) of `composite` with the same rows from `to`. Rows
 * below wipe_y[i] are left as-is (they're "from" content). */
static void wipe_composite_inplace(pixel_t *composite, const pixel_t *to)
{
    for (int i = 0; i < DISP_W; i++) {
        int yi = wipe_y[i];
        if (yi <= 0) continue;
        if (yi > DOOM_H) yi = DOOM_H;
        pixel_t       *dst = &composite[i];
        const pixel_t *src = &to[i];
        for (int y = 0; y < yi; y++) {
            *dst = *src;
            dst += DISP_W;
            src += DISP_W;
        }
    }
}

void pd_begin_frame(void)
{
    /* One-time init of the patchlist header: header.max bounds how many
     * V_DrawPatch entries V_DrawPatchN will accept before silently
     * dropping the rest. BSS-zeroed by default, so we set it on the
     * first frame. show_fps stays BSS-zero (off by default); the user
     * turns it on via SW2/SW4. */
    static int patchlist_initialized = 0;
    if (!patchlist_initialized) {
        vpatchlists->framebuffer[0].header.max = VPATCHLIST_COUNT_FRAMEBUFFER;
        /* Plane buffering uses 0xff as the "column not touched" sentinel.
         * BSS zero-init would seed every column to 0 and confuse the span
         * walker into reading row 0 spans for untouched columns. Prime
         * the buffer once so the per-frame cleanup invariant
         * (touched columns get cleared back to 0xff) holds from frame 1. */
        memset(plane_top, PD_PLANE_UNTOUCHED, sizeof plane_top);
        patchlist_initialized = 1;
    }

    /* Sync v_video.c's dest_screen to the current I_VideoBuffer.
     * I_VideoBuffer flips between two ping-pong buffers each frame;
     * dest_screen was set once at startup by V_RestoreBuffer and would
     * otherwise stay stuck on whichever buffer was current at init. */
    extern void V_RestoreBuffer(void);
    V_RestoreBuffer();
    /* No view-sized clipping; ST_Drawer / M_Drawer write across the
     * full 320x200 frame. */
    vpatch_clip_top    = 0;
    vpatch_clip_bottom = DOOM_H;
    /* Reset patchlist size to 1 (header). All V_DrawPatch calls during
     * this frame land in vpatchlists->framebuffer until V_DrawPatchList
     * runs in pd_end_frame. */
    V_BeginPatchList(vpatchlists->framebuffer);

    reset_framedrawables();

    /* During mid-wipe loop iterations the engine's render gate is
     * closed, so the framebuffer holds our melt composite from the
     * previous iteration. Don't clear it. Only clear on normal
     * GS_LEVEL frames where the column callbacks will repaint
     * everything.
     *
     * Note: this memset has a 4-30 ms range. The floor (~3.6 ms) is
     * the native AHB-bound time for a 96 KB clear; the spikes are
     * PendSV preemption from the audio mix_chunk path (TIM6 pends
     * PendSV at chunk boundaries every ~46 ms; mix_chunk runs the
     * OPL synth for 5-30 ms). Moving the memset doesn't help since
     * PendSV is independent of frame phase. The real fix would be a
     * GPDMA m2m clear running in background, but that's deferred. */
    if (CLEAR_LEVEL_FRAMEBUFFER
        && gamestate == GS_LEVEL
        && wipestate == WIPESTATE_NONE) {
        /* RGB565 0x0000 == black, so a byte-zero clear covers the
         * whole 240*200 uint16_t buffer. */
#if PERF_INSTRUMENT
        uint32_t perf_t = perf_cyc();
#endif
        memset(I_VideoBuffer, 0, DISP_W * DOOM_H * sizeof(pixel_t));
#if PERF_INSTRUMENT
        perf_clear_us += perf_us_since(perf_t);
#endif
    }
    /* Touched-visplane bookkeeping is reset by pd_finalize_planes when
     * it drains the buffer; nothing to do here on the BSP-emit side. */

    /* Plane lighting cache is keyed on visplane index; the index is
     * reused across frames for unrelated visplanes, so drop the cache
     * on every new frame to avoid serving stale level[y] profiles. */
    plane_light_cached_fd = -1;

    /* Per-column cos*dscale / sin*dscale cache depends on viewangle,
     * which can change every frame. Drop validity. */
    memset(plane_xcache_valid, 0, sizeof plane_xcache_valid);

    wipe_min = 0;
}

/* ---------- WHD splash decoder ---------- */
/*
 * Draw a full-screen 320x200 WHD-compressed patch into `dest` between
 * source rows [top, bottom). Mirrors pd_render.cpp:draw_splash().
 *
 * The WHD patch encodes columns as Huffman-compressed deltas. Each
 * column's data offset is in col_offsets[col] (with a deduplication
 * trick: a high byte of 0xff means "use col_offsets[low_byte]" instead).
 * The decoder produces pixel values 0..255, or 256..262 which means
 * "previous pixel + (val & 0xff) - 3".
 *
 * Overwrites splash_decoder_buf / splash_prefix_lengths, so the
 * patch-decoder cache is invalidated.
 */
/* `dest_buf` is the RGB565 scan-out buffer (DISP_W stride). top/bottom
 * are source rows in the 320x200 patch coordinate space; this function
 * walks the whole 320 columns and writes only the kept ones into
 * dest_buf at x_squeeze(col) per row. */
static void splash_draw(int patch_num, int top, int bottom, pixel_t *dest_buf)
{
    cached_patch_num = -1;
    const uint8_t *patch =
        (const uint8_t *)W_CacheLumpNum(patch_num, 0 /* PU_CACHE */);

    int data_index = 3 + (patch_has_extra(patch) ? 1 : 0);

    /* Read the encoding mode + Huffman code table from the patch's
     * decoder metadata block. The block is bit-packed; tiny_huff
     * reads it via th_bit_input_init. */
    const uint8_t *src = patch + data_index * 2 + 1;
    th_bit_input bi;
    th_bit_input_init(&bi, src);
    int encoding = th_read_bits(&bi, 1);
    if (encoding == 0) {
        if (th_bit(&bi)) {
            th_read_simple_decoder(&bi, splash_decoder_buf,
                                   sizeof(splash_decoder_buf) / 2,
                                   splash_decoder_tmp,
                                   sizeof(splash_decoder_tmp));
        } else {
            read_raw_pixels_decoder(&bi, splash_decoder_buf,
                                    sizeof(splash_decoder_buf) / 2,
                                    splash_decoder_tmp,
                                    sizeof(splash_decoder_tmp));
        }
    } else {
        read_raw_pixels_decoder_c3(&bi, splash_decoder_buf,
                                   sizeof(splash_decoder_buf) / 2,
                                   splash_decoder_tmp,
                                   sizeof(splash_decoder_tmp));
    }
    th_make_prefix_length_table(splash_decoder_buf, splash_prefix_lengths);

    /* Skip past the decoder metadata block to reach the column-offset
     * table. patch[data_index*2] holds the byte length of that block. */
    data_index += ((const uint8_t *)patch)[data_index * 2];
    const uint16_t *col_offsets = &((const uint16_t *)patch)[data_index];

    int w = patch_width(patch);
    int h = patch_height(patch);
    if (w != DOOM_W || h != DOOM_H) {
        /* Splash patches are always full-screen. Bail rather than
         * scribble outside I_VideoBuffer. */
        return;
    }
    /* +2 = the one extra column-offset slot the encoder writes for the
     * "look-back" trick (a column whose high-byte offset is 0xff
     * indexes into col_offsets again). */
    uint data_byte_index = (data_index + w) * 2 + 2;

    /* For each source column we compute its squeezed dest x; the
     * (col & 3) == 3 columns are not written but their data must
     * still be consumed from the stream so the bit reader stays in
     * sync with the next column's start. */
    for (int col = 0; col < w; col++) {
        uint16_t col_offset = col_offsets[col];
        if ((col_offset >> 8) == 0xff) {
            col_offset = col_offsets[col_offset & 0xff];
        }
        if (patch_byte_addressed(patch)) {
            th_bit_input_init(&bi, patch + data_byte_index + col_offset);
        } else {
            th_bit_input_init_bit_offset(&bi, patch + data_byte_index,
                                         col_offset);
        }

        if (encoding == 0) {
            /* This branch's th_decode_table_special path was already
             * stubbed out (assert(false)) in upstream draw_splash. We
             * never see it for the Doom shareware splash lumps. */
            return;
        }

        uint8_t prev_pixel = 0;
        int y;
        for (y = 0; y < top; y++) {
            uint16_t p = th_decode_table_special_16(splash_decoder_buf,
                                                   splash_prefix_lengths, &bi);
            if (p < 256) {
                prev_pixel = (uint8_t)p;
            } else {
                prev_pixel = (uint8_t)(prev_pixel + (p & 0xff) - 3);
            }
        }

        int keep = ((col & 3) != 3);
        pixel_t *dest = keep ? &dest_buf[top * DISP_W + x_squeeze(col)]
                             : NULL;
        for (; y < bottom; y++) {
            uint16_t p = th_decode_table_special_16(splash_decoder_buf,
                                                   splash_prefix_lengths, &bi);
            uint8_t pi;
            if (p < 256) {
                pi = (uint8_t)p;
            } else {
                pi = (uint8_t)(prev_pixel + (p & 0xff) - 3);
            }
            prev_pixel = pi;
            if (keep) {
                *dest = palette_rgb565[pi];
                dest += DISP_W;
            }
        }
    }
}

/* Install palette 0 from the WAD's PLAYPAL lump. PLAYPAL holds 14
 * 256-entry palettes (normal + damage tints + bonus + power-ups);
 * index 0 is the natural one used outside gameplay. */
static void install_playpal(void)
{
    static int installed = 0;
    if (installed) return;
    int pnum = W_CheckNumForName("PLAYPAL");
    if (pnum < 0) return;
    const uint8_t *pal = (const uint8_t *)W_CacheLumpNum(pnum, 0);
    I_SetPalette(pal);
    installed = 1;
}

/* ---------- Large FPS overlay -----------------------------------
 * Engine ST_FpsDrawer paints 4-px-tall digits at (318, 2) which is
 * unreadable on the click panel. Overlay our own 5x7 bitmap font
 * scaled 3x at the top-right after V_DrawPatchList drains; backed
 * by a 1-px black inset for contrast against any sky/wall colour.
 * Drawn into the 320-wide framebuffer; the SPI converter drops one
 * column in four, but at 3x scale each glyph cell is 3 source cols
 * so character pixels stay visible (worst case 2/3 source cols
 * reach the panel per glyph cell, plenty for legibility). */
extern boolean show_fps;

static const uint8_t fps_font[10][7] = {
    /* 0 */ { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },
    /* 1 */ { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
    /* 2 */ { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },
    /* 3 */ { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
    /* 4 */ { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },
    /* 5 */ { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
    /* 6 */ { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },
    /* 7 */ { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
    /* 8 */ { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },
    /* 9 */ { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
};

#define FPS_GLYPH_SCALE 3
#define FPS_GLYPH_W     (5 * FPS_GLYPH_SCALE)
#define FPS_GLYPH_H     (7 * FPS_GLYPH_SCALE)
#define FPS_GLYPH_GAP   FPS_GLYPH_SCALE
#define FPS_DIGIT_FG    4    /* PLAYPAL: bright off-white */
#define FPS_DIGIT_BG    0    /* PLAYPAL: black */

static void fps_paint_digit(int x, int y, int d)
{
    if (d < 0 || d > 9) return;
    uint16_t fg = palette_rgb565[FPS_DIGIT_FG];
    for (int row = 0; row < 7; row++) {
        uint8_t bits = fps_font[d][row];
        for (int col = 0; col < 5; col++) {
            if (!(bits & (1 << (4 - col)))) continue;
            int px = x + col * FPS_GLYPH_SCALE;
            int py = y + row * FPS_GLYPH_SCALE;
            for (int sy = 0; sy < FPS_GLYPH_SCALE; sy++) {
                int yy = py + sy;
                if ((unsigned)yy >= (unsigned)DOOM_H) continue;
                pixel_t *p = &I_VideoBuffer[yy * DISP_W];
                for (int sx = 0; sx < FPS_GLYPH_SCALE; sx++) {
                    int xx = px + sx;
                    if ((unsigned)xx >= (unsigned)DOOM_W) continue;
                    if ((xx & 3) == 3) continue;   /* dropped column */
                    p[x_squeeze(xx)] = fg;
                }
            }
        }
    }
}

static void draw_big_fps(int fps)
{
    if (fps < 0)   fps = 0;
    if (fps > 999) fps = 999;
    int n = (fps >= 100) ? 3 : (fps >= 10) ? 2 : 1;
    int total_w = n * FPS_GLYPH_W + (n - 1) * FPS_GLYPH_GAP;

    int x = DOOM_W - 4 - total_w;
    int y = 2;

    uint16_t bg = palette_rgb565[FPS_DIGIT_BG];

    /* Black inset for contrast - 1 px past each edge of the glyph
     * cluster. */
    for (int yy = y - 1; yy < y + FPS_GLYPH_H + 1; yy++) {
        if ((unsigned)yy >= (unsigned)DOOM_H) continue;
        pixel_t *row = &I_VideoBuffer[yy * DISP_W];
        for (int xx = x - 2; xx < x + total_w + 2; xx++) {
            if ((unsigned)xx >= (unsigned)DOOM_W) continue;
            if ((xx & 3) == 3) continue;
            row[x_squeeze(xx)] = bg;
        }
    }

    int cx = x;
    int divisors[3] = {100, 10, 1};
    int start = 3 - n;
    for (int i = start; i < 3; i++) {
        fps_paint_digit(cx, y, (fps / divisors[i]) % 10);
        cx += FPS_GLYPH_W + FPS_GLYPH_GAP;
    }
}

void pd_end_frame(int wipe_start)
{
    install_playpal();

    if (wipestate == WIPESTATE_NONE) {
        /* Normal (non-wipe) frame path. Render the new content for
         * this gamestate, possibly start a wipe at the end. */
        if (gamestate == GS_DEMOSCREEN && pagename) {
            int pnum = W_CheckNumForName(pagename);
            if (pnum >= 0) {
                splash_draw(pnum, 0, DOOM_H, I_VideoBuffer);
            }
        }

        /* Intermission summary. WI_Start (called from G_DoCompleted)
         * loads wi_background_patch_num (WIMAP1/2/3 or INTERPIC); we
         * decode it into the framebuffer here, then re-arm the patchlist
         * and let WI_Drawer emit the level name, "Finished!", and stat
         * widgets via V_DrawPatch. The existing V_DrawPatchList drain
         * below renders them on top of the background. The upstream
         * D_Display switch that calls WI_Drawer is compiled out under
         * PD_COLUMNS=1, so this dispatch is the only path. */
        if (gamestate == GS_INTERMISSION && wi_background_patch_num) {
            splash_draw(wi_background_patch_num, 0, DOOM_H, I_VideoBuffer);
            V_BeginPatchList(vpatchlists->framebuffer);
            WI_Drawer();
        }

        /* End-of-episode finale screen. Under DOOM_TINY F_TextWrite
         * doesn't redraw the tiled flat each frame - it only
         * V_DrawPatches the last few characters and assumes the
         * framebuffer already has the background. Paint the flat once
         * on transition into GS_FINALE; subsequent frames let the
         * patchlist drain accumulate text on top. F_ArtScreenDrawer
         * issues a fullscreen V_DrawPatch for HELP2 (E1 shareware),
         * which overwrites the text when F_Ticker advances stages -
         * no special handling needed for that transition. */
        {
            if (gamestate == GS_FINALE) {
                /* Re-tile the finaleflat every frame: with ping-pong
                 * back-buffers the previously-tiled buffer is only
                 * one of two, so we redraw to keep both in sync. The
                 * tile is cheap (~0.6 ms; 240*200 LUT loads). */
                if (finaleflat) {
                    int flat_lump = W_CheckNumForName(finaleflat);
                    if (flat_lump >= 0) {
                        int picnum = flat_lump - firstflat;
                        if (picnum >= 0) {
                            const uint8_t *src = load_flat(picnum);
                            for (int y = 0; y < DOOM_H; y++) {
                                pixel_t *dst = I_VideoBuffer + y * DISP_W;
                                const uint8_t *row = src + ((y & 63) << 6);
                                for (int x = 0; x < DOOM_W; x++) {
                                    if ((x & 3) == 3) continue;
                                    dst[x_squeeze(x)] =
                                        palette_rgb565[row[x & 63]];
                                }
                            }
                        }
                    }
                }
                V_BeginPatchList(vpatchlists->framebuffer);
                F_Drawer();
            }
        }

        /* Drain the buffered visplanes via the span-major drawer. Walls
         * have already rasterised during BSP traversal; planes need to
         * fill the silhouette below the wall sweeps. Sprites then
         * draw on top of both. */
        if (gamestate == GS_LEVEL) {
#if PERF_INSTRUMENT
            uint32_t perf_t = perf_cyc();
#endif
            pd_finalize_planes();
#if PERF_INSTRUMENT
            perf_planes_us += perf_us_since(perf_t);
#endif
        }

        /* Drain sprites here, after R_RenderPlayerView has emitted all
         * walls + visplanes + masked columns into our queues. Enemies
         * sit on top of the world geometry; the player weapon sprite
         * sits on top of enemies. The HUD (ST_Drawer / V_DrawPatchList)
         * draws after this, on top of everything. */
        if (gamestate == GS_LEVEL) {
#if PERF_INSTRUMENT
            uint32_t perf_t = perf_cyc();
#endif
            drain_sprite_queues();
#if PERF_INSTRUMENT
            perf_sprites_us += perf_us_since(perf_t);
#endif
        }

        /* Status bar widgets (face, ammo, health, armour, keys, and the
         * STBAR background patch itself - DOOM_TINY ST_drawWidgets emits
         * sbar at ST_FACESY=168 directly). They land in the patchlist set
         * up by pd_begin_frame; V_DrawPatchList below renders them on top
         * of the silhouette. The fullscreen=false argument flips
         * st_statusbaron on so the widgets actually emit. */
        if (gamestate == GS_LEVEL) {
#if PERF_INSTRUMENT
            uint32_t perf_t = perf_cyc();
#endif
            ST_Drawer(false, true);
#if PERF_INSTRUMENT
            perf_hud_us += perf_us_since(perf_t);
#endif
        }
        /* Menu (only renders when menuactive); no-op in attract demo. */
#if PERF_INSTRUMENT
        {
            uint32_t perf_t = perf_cyc();
            M_Drawer();
            perf_hud_us += perf_us_since(perf_t);
        }
#else
        M_Drawer();
#endif

        /* Advance the pre-wipe state machine one frame. g_game.c
         * holds back ga_newgame / ga_loadlevel / ga_completed /
         * ga_worlddone gameactions for one render frame so the menu
         * (or last-frame HUD) gets drawn once with the action pending,
         * then the gameaction runs on the *next* tic. The original
         * pd_render.cpp advances NEEDED -> DONE here; without this
         * the menu's "select skill" never closes into the level. */
        if (pre_wipe_state == PRE_WIPE_EXTRA_FRAME_NEEDED) {
            pre_wipe_state = PRE_WIPE_EXTRA_FRAME_DONE;
        }

        /* Poll the wolfDemo user buttons every render frame. I_StartTic
         * also polls them, but only fires from the engine's tic builder
         * - per-frame polling here guarantees the press is caught even
         * when tic builds drop out (long wipe loops, frame stalls). */
        button_tick();

        /* FPS overlay. We compute fps ourselves over a 1-second window
         * and feed it both to the engine widget (small digits at
         * (318, 2)) and to our larger top-right overlay drawn after
         * V_DrawPatchList. ST_FpsDrawer's auto-mode (fps == -1) caps
         * frame deltas at < 100 ms, which silently rejects samples at
         * our ~28 ms/frame rate, so we always pass the value
         * explicitly. The drawer is a no-op when show_fps is false. */
        static int fps_value;
        {
            static int frames_in_window;
            static int window_start_ms;
            int now = I_GetTimeMS();
            frames_in_window++;
            int elapsed = now - window_start_ms;
            if (elapsed >= 1000) {
                fps_value = (frames_in_window * 1000) / elapsed;
                frames_in_window = 0;
                window_start_ms  = now;
            }
            ST_FpsDrawer(fps_value);
        }

        /* Drain the patchlist into I_VideoBuffer (dest_screen was pointed
         * here by V_RestoreBuffer in d_main.c before the game loop). */
#if PERF_INSTRUMENT
        {
            uint32_t perf_t = perf_cyc();
            V_DrawPatchList(vpatchlists->framebuffer);
            perf_hud_us += perf_us_since(perf_t);
        }
#else
        V_DrawPatchList(vpatchlists->framebuffer);
#endif

        /* Large FPS overlay on top of everything. The engine's built-in
         * widget at (318, 2) is too small to read on the click panel;
         * paint our own 3x-scaled bitmap font at the top-right. */
        if (show_fps) {
            draw_big_fps(fps_value);
        }

        if (wipe_start) {
            /* Gamestate just changed. The previously-displayed
             * front_buf in i_video_stm32.c holds the "from" image;
             * back_buf holds the new "to" image we just rendered.
             * Hand both to the video glue, init the per-column melt
             * y-table, and run the first composite step. */
            i_video_begin_wipe();
            wipe_init_melt();
            wipestate = WIPESTATE_SKIP1;
            wipe_step_melt();
            wipe_composite_inplace(i_video_wipe_dest(),
                                   i_video_wipe_to());
        }
    } else {
        /* Mid-wipe iteration. Engine's render gate is closed so
         * I_VideoBuffer hasn't been touched. Step the melt one tic
         * and re-composite into the same buffer. */
        int done = wipe_step_melt();
        wipe_composite_inplace(i_video_wipe_dest(),
                               i_video_wipe_to());
        if (done) {
            wipestate = WIPESTATE_NONE;
            i_video_end_wipe();
        }
    }

    /* Always blit. For GS_LEVEL the silhouette renderer has already
     * painted into I_VideoBuffer via the pd_add_* callbacks; for
     * GS_INTERMISSION the WIMAP background plus WI_Drawer's patchlist
     * widgets land here via the block above; for GS_FINALE the tiled
     * finaleflat (painted on entry) plus F_Drawer's accumulated text
     * patches land via the block above. */
    I_FinishUpdate();
}
