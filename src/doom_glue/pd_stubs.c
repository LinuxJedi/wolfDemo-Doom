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
 *   - Wipe state machine: stubbed to never enter a wipe. The engine's
 *     do/while (wipestate) loop in D_RunFrame would otherwise spin
 *     forever waiting for wipe_min to advance.
 *   - pd_add_column: textured wall sampling for PDCOL_TOP / MID /
 *     BOTTOM / SKY via a single-slot Huffman patch-decoder cache and
 *     per-column decode + iscale/texturemid/colormap sample. Multi-
 *     patch composite textures route through paint_composite_column,
 *     which walks the WHD_COL_SEG_* segment stream to stitch each
 *     contributing patch into one 128-byte sample buffer.
 *   - pd_add_plane_column: textured floor/ceiling spans with flat
 *     decoder + 4-slot LRU cache and per-pixel R_MapPlane projection.
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
#include "picodoom.h"

extern uint8_t *I_VideoBuffer;
extern void I_FinishUpdate(void);
extern void I_SetPalette(const uint8_t *palette);
extern int  I_GetTimeMS(void);
extern const char *pagename;
extern void button_tick(void);

#define DOOM_W      320
#define DOOM_H      200

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
    uint8_t *dest = &I_VideoBuffer[yl * DOOM_W + x];
    for (int y = yl; y <= yh; y++) {
        *dest = colour;
        dest += DOOM_W;
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
 * the start, matching the pd_render.cpp h<127 fixup heuristic. */
static void decode_patch_column(int col, uint8_t *out)
{
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

    if (cached_encoding == 0) {
        /* Single-byte Huffman, no delta. The splash decoder asserts
         * here because TITLEPIC / CREDIT / HELP all use encoding 1,
         * but plenty of patches (most sprites in particular, plus
         * some wall patches with low pixel-value entropy) use
         * encoding 0 - bailing to memset(0) was painting them as
         * solid black silhouettes. Mirrors pd_render.cpp's
         * draw_patch_columns encoding==0 path. */
        for (int y = 0; y < h; y++) {
            out[y] = th_decode_table_special(splash_decoder_buf,
                                             splash_prefix_lengths, &bi);
        }
    } else {
        uint8_t prev = 0;
        for (int y = 0; y < h; y++) {
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
    /* Wrap-pad shorter textures so `& 127` sampling stays sane. */
    for (int y = h; y < WHD_TEXTURE_COL_HEIGHT; y++) {
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
                        uint8_t col_buf[WHD_TEXTURE_COL_HEIGHT];
                        decode_patch_column(pcol, col_buf);
                        int n = copy_len;
                        if (src_yoff + n > WHD_TEXTURE_COL_HEIGHT) {
                            n = WHD_TEXTURE_COL_HEIGHT - src_yoff;
                        }
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
    const lighttable_t *cmap = colormaps + 256 * cmi;

    uint8_t *dest = &I_VideoBuffer[yl * DOOM_W + dc_x];
    for (int y = yl; y <= yh; y++) {
        *dest = cmap[pixels[(frac >> FRACBITS) & 127]];
        dest += DOOM_W;
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

    uint8_t col_buf[WHD_TEXTURE_COL_HEIGHT];
    decode_patch_column(col, col_buf);

    int yl = dc_yl, yh = dc_yh;
    if (yl < 0) yl = 0;
    if (yh > DOOM_H - 1) yh = DOOM_H - 1;
    if (yl > yh || dc_x < 0 || dc_x >= DOOM_W) return;

    fixed_t fracstep = dc_iscale;
    fixed_t frac = dc_texturemid + (yl - centery) * fracstep;
    const lighttable_t *cmap = colormaps + 256 * dc_colormap_index;

    uint8_t *dest = &I_VideoBuffer[yl * DOOM_W + dc_x];
    for (int y = yl; y <= yh; y++) {
        *dest = cmap[col_buf[(frac >> FRACBITS) & 127]];
        dest += DOOM_W;
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
    switch (type) {
        case PDCOL_TOP:    paint_textured_column(COLOUR_TOP);    return;
        case PDCOL_MID:    paint_textured_column(COLOUR_MID);    return;
        case PDCOL_BOTTOM: paint_textured_column(COLOUR_BOTTOM); return;
        case PDCOL_SKY:
            /* r_segs.c's sky path sets dc_iscale = pspriteiscale (no
             * perspective), dc_colormap_index = 0 (full bright), and
             * dc_texturemid = skytexturemid, then routes through
             * pd_add_column2 which converts dc_source to a negative
             * patch handle. paint_textured_column already handles
             * exactly that shape - identical to a wall column with
             * different scale/light. */
            paint_textured_column(COLOUR_SKY);
            return;
        default: return; /* MASKED, NONE, etc. -- skip */
    }
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
    int patch_num = -q->real_id;
    load_patch_decoder(patch_num);
    if (cached_width <= 0) return;

    int col = q->col;
    if (col >= cached_width) col %= cached_width;

    uint8_t col_buf[WHD_TEXTURE_COL_HEIGHT];
    decode_patch_column(col, col_buf);

    /* Player-colour translation - same formula pd_render.cpp:
     * draw_patch_columns uses for the 0x70-range green marine
     * pixels. */
    if (q->translation_index) {
        int tbase = 0x80 - (int)q->translation_index * 0x20;
        for (int yy = 0; yy < WHD_TEXTURE_COL_HEIGHT; yy++) {
            if ((col_buf[yy] >> 4) == 7) {
                col_buf[yy] = (uint8_t)(tbase + (col_buf[yy] & 0x0f));
            }
        }
    }

    fixed_t fracstep = q->iscale;
    /* Fuzz (negative colormap index for partial-invisibility)
     * isn't rendered specially; fall back to fullbright so the
     * sprite is at least visible. */
    int cmi = q->colormap_index;
    if (cmi < 0) cmi = 0;
    const lighttable_t *cmap = colormaps + 256 * cmi;
    int x = q->x;
    const uint8_t *ys = &seg_buf[q->seg_offset];

    for (int s = 0; s < q->seg_count; s++) {
        int yl = ys[s * 3];
        int yh = ys[s * 3 + 1];
        int base_off = ys[s * 3 + 2];
        if (yh > DOOM_H - 1) yh = DOOM_H - 1;
        if (yl > yh) continue;

        fixed_t tex_mid_seg = q->texturemid - ((fixed_t)base_off << FRACBITS);
        fixed_t frac = tex_mid_seg + (yl - centery) * fracstep;
        uint8_t *dest = &I_VideoBuffer[yl * DOOM_W + x];
        for (int y = yl; y <= yh; y++) {
            *dest = cmap[col_buf[(frac >> FRACBITS) & 127]];
            dest += DOOM_W;
            frac += fracstep;
        }
    }
}

/* Drain both queues. With NO_VISSPRITES=0 the engine's R_DrawMasked
 * sorts vissprites by scale and walks vsprsortedhead.next back-to-front
 * before the drawseg post-loop (also walked back-to-front), so columns
 * arrive in the queue already in the correct paint order - drain
 * forward. Player weapon sprites stay last so they sit on top of
 * everything else but the HUD. */
static void drain_sprite_queues(void)
{
    for (int i = 0; i < sprite_queue_count; i++) {
        render_queued_column(&sprite_queue[i], sprite_queue_segs);
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
 * + FLOOR4_8 + FLOOR0_1 ish), so a 4-slot LRU keeps the steady state
 * decode-free. */
#define FLAT_CACHE_SLOTS 4
#define FLAT_PIXELS      4096

static uint8_t flat_cache[FLAT_CACHE_SLOTS][FLAT_PIXELS];
static int     flat_cache_picnum[FLAT_CACHE_SLOTS] = {-1, -1, -1, -1};
static uint8_t flat_cache_lru[FLAT_CACHE_SLOTS] = {0, 1, 2, 3};

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

/* Plane lighting cache. R_MapPlane lighting is `level = clamp(startmap
 * - (SCREENWIDTH/4)/(zindex+1), 0, NUMCOLORMAPS-1)` with `zindex =
 * (plane_h * yslope[y]) >> LIGHTZSHIFT`. For a given visplane both
 * `startmap` (from lightlevel + extralight) and `plane_h` are constant,
 * so the entire `level[y]` profile depends only on the visplane index
 * for the current frame. Caching it avoids the per-pixel divide that
 * NO_USE_ZLIGHT forces inline; one cache slot is sufficient because
 * pd_add_plane_column calls cluster by fd_num within R_DrawPlanes.
 *
 * yslope[] is sized MAIN_VIEWHEIGHT (168) under DOOM_TINY, not full
 * SCREENHEIGHT - the bottom 32 rows are the status bar and never
 * receive plane columns. Sizing the cache to MAIN_VIEWHEIGHT keeps
 * indexing in bounds; pd_add_plane_column never gets a yh past the
 * view region in practice.
 *
 * Invalidated each frame in pd_begin_frame: fd_num is reused across
 * frames for a different visplane, so the cached profile would be
 * stale. */
static int     plane_light_cached_fd = -1;
static uint8_t plane_light_level[MAIN_VIEWHEIGHT];

void pd_add_plane_column(int x, int yl, int yh, fixed_t scale,
                         int floor, int fd_num)
{
    (void)scale;

    if (yl > yh) return;
    if (yl < 0) yl = 0;
    /* Cap to MAIN_VIEWHEIGHT - 1 so the per-pixel `yslope[y]` access
     * stays in bounds. Plane columns never need to reach into the
     * status bar region (rows 168..199); the engine emits ceiling /
     * floor spans only across the view area. */
    if (yh > MAIN_VIEWHEIGHT - 1) yh = MAIN_VIEWHEIGHT - 1;
    if (x < 0 || x >= DOOM_W) return;
    /* Same x % 4 == 3 drop as pd_add_column - the SPI converter never
     * ships these columns to the panel, so the per-pixel projection
     * + flat sample work is pure waste. Plane spans dominate the
     * frame budget on open-area scenes, so this is the biggest single
     * column-skip win. */
    if ((x & 3) == 3) return;

    if (fd_num < 0 || fd_num >= MAXVISPLANES) return;
    int picnum = visplanes[fd_num].picnum;

    /* Sky flat: keep flat-colour stripe; sky needs the texture column
     * path which isn't wired yet. */
    if (picnum == skyflatnum) {
        paint_column(x, yl, yh, COLOUR_SKY);
        return;
    }
    /* Animated/translated flats (NUKAGE, FWATER, etc.) live in a small
     * indirection table - same logic as pd_render.cpp:translate_picnum. */
    if (whd_flattospecial[picnum] != 0xff) {
        picnum = whd_specialtoflat[whd_flattranslation[whd_flattospecial[picnum]]];
    }

    const uint8_t *flat = load_flat(picnum);
    if (!flat) {
        paint_column(x, yl, yh, floor ? COLOUR_FLOOR : COLOUR_CEILING);
        return;
    }

    fixed_t plane_h = abs(visplanes[fd_num].height - viewz);

    /* Refresh the lighting profile when we hit a new visplane. */
    if (fd_num != plane_light_cached_fd) {
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

    /* Hoist the column-constant Kx, Ky out of the per-pixel loop:
     *   length    = plane_h * dscale * yslope[y]    (per pixel)
     *   world_x   = viewx + cos_a * length          = viewx + Kx * yslope[y]
     *   world_y   = -viewy - sin_a * length         = -viewy - Ky * yslope[y]
     * with Kx = cos_a * plane_h * dscale, Ky = sin_a * plane_h * dscale.
     * This drops the per-pixel mul count from 4 (distance, length,
     * world_x, world_y) to 2 (world_x, world_y). distance is no longer
     * needed because the lighting cache above already absorbed it. */
    angle_t angle  = (viewangle + x_to_viewangle(x)) >> ANGLETOFINESHIFT;
    fixed_t cos_a  = finecosine(angle);
    fixed_t sin_a  = finesine(angle);
    fixed_t dscale = distscale(x);
    fixed_t k      = FixedMul(plane_h, dscale);
    fixed_t kx     = FixedMul(cos_a, k);
    fixed_t ky     = FixedMul(sin_a, k);

    uint8_t *dest = &I_VideoBuffer[yl * DOOM_W + x];
    for (int y = yl; y <= yh; y++) {
        fixed_t world_x = viewx  + FixedMul(kx, yslope[y]);
        fixed_t world_y = -viewy - FixedMul(ky, yslope[y]);
        unsigned u = ((unsigned)world_x >> FRACBITS) & 63;
        unsigned v = ((unsigned)world_y >> FRACBITS) & 63;
        const lighttable_t *cmap = colormaps + 256 * plane_light_level[y];
        *dest = cmap[flat[(v << 6) | u]];
        dest += DOOM_W;
    }
}

/* ---------- Wipe (melt) animation -------------------------------
 * The classic Doom screen melt. f_wipe.c is mostly compiled out
 * (NO_USE_WIPE=1), so we implement it locally. Two 64 KB buffers
 * snapshot the "from" (previous frame) and "to" (post-transition
 * frame) images; per-column y[] offsets advance per loop iteration
 * and we composite from + to into I_VideoBuffer based on those
 * offsets.
 *
 * Protocol with the engine:
 *   - D_Display calls pd_begin_frame, optionally renders the new
 *     frame (gated by `!wipestate || wipestate == WIPESTATE_REDRAW1`),
 *     then calls pd_end_frame(wipe). `wipe` is true only on the
 *     first frame after a gamestate change.
 *   - D_RunFrame loops `do { D_Display(); } while (wipestate);` so
 *     setting wipestate to anything non-zero re-enters D_Display.
 *   - At the end of every NORMAL (non-wipe) frame we snapshot
 *     I_VideoBuffer into wipe_from_buf so it's ready when a wipe
 *     suddenly starts on the next frame.
 *   - On the first wipe frame we save the new I_VideoBuffer into
 *     wipe_to_buf, restore wipe_from_buf into I_VideoBuffer, init
 *     the y[] table, and run one melt step. Subsequent loop
 *     iterations skip rendering (engine gate) and run further melt
 *     steps until y[i] >= DOOM_H for every column. */
static uint8_t wipe_from_buf[DOOM_W * DOOM_H];
static uint8_t wipe_to_buf[DOOM_W * DOOM_H];
static int16_t wipe_y[DOOM_W];

static void wipe_init_melt(void)
{
    wipe_y[0] = -(int16_t)(M_Random() % 16);
    for (int i = 1; i < DOOM_W; i++) {
        int r = (M_Random() % 3) - 1;
        wipe_y[i] = wipe_y[i - 1] + (int16_t)r;
        if (wipe_y[i] > 0)        wipe_y[i] = 0;
        else if (wipe_y[i] == -16) wipe_y[i] = -15;
    }
}

/* One tic of the melt. Returns 1 when every column has finished
 * sliding off the bottom (y[i] >= DOOM_H). */
static int wipe_step_melt(void)
{
    int done = 1;
    for (int i = 0; i < DOOM_W; i++) {
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

/* Composite from + to into I_VideoBuffer based on current wipe_y[]:
 * for column i, screen rows [0, y[i]) come from wipe_to_buf and rows
 * [y[i], DOOM_H) come from wipe_from_buf scrolled down by y[i]. */
static void wipe_composite(void)
{
    for (int i = 0; i < DOOM_W; i++) {
        int yi = wipe_y[i];
        if (yi < 0) yi = 0;
        if (yi > DOOM_H) yi = DOOM_H;
        uint8_t *dst = &I_VideoBuffer[i];
        const uint8_t *src_to = &wipe_to_buf[i];
        for (int y = 0; y < yi; y++) {
            *dst = *src_to;
            dst    += DOOM_W;
            src_to += DOOM_W;
        }
        const uint8_t *src_from = &wipe_from_buf[i];
        for (int y = yi; y < DOOM_H; y++) {
            *dst = *src_from;
            dst      += DOOM_W;
            src_from += DOOM_W;
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
        patchlist_initialized = 1;
    }
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
     * everything. */
    if (gamestate == GS_LEVEL && wipestate == WIPESTATE_NONE) {
        memset(I_VideoBuffer, 0, DOOM_W * DOOM_H);
    }
    /* Plane lighting cache is keyed on visplane index; the index is
     * reused across frames for unrelated visplanes, so drop the cache
     * on every new frame to avoid serving stale `level[y]` profiles. */
    plane_light_cached_fd = -1;
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
static void splash_draw(int patch_num, int top, int bottom, uint8_t *dest)
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

    uint32_t row_bytes_skipped = (bottom - top) * DOOM_W - 1;
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
        for (; y < bottom; y++) {
            uint16_t p = th_decode_table_special_16(splash_decoder_buf,
                                                   splash_prefix_lengths, &bi);
            if (p < 256) {
                *dest = (uint8_t)p;
            } else {
                *dest = (uint8_t)(prev_pixel + (p & 0xff) - 3);
            }
            prev_pixel = *dest;
            dest += DOOM_W;
        }
        /* Step back to row `top` of the next column. */
        dest -= row_bytes_skipped;
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

        /* Drain sprites here, after R_RenderPlayerView has emitted all
         * walls + visplanes + masked columns into our queues. Enemies
         * sit on top of the world geometry; the player weapon sprite
         * sits on top of enemies. The HUD (ST_Drawer / V_DrawPatchList)
         * draws after this, on top of everything. */
        if (gamestate == GS_LEVEL) {
            drain_sprite_queues();
        }

        /* Status bar widgets (face, ammo, health, armour, keys, and the
         * STBAR background patch itself - DOOM_TINY ST_drawWidgets emits
         * sbar at ST_FACESY=168 directly). They land in the patchlist set
         * up by pd_begin_frame; V_DrawPatchList below renders them on top
         * of the silhouette. The fullscreen=false argument flips
         * st_statusbaron on so the widgets actually emit. */
        if (gamestate == GS_LEVEL) {
            ST_Drawer(false, true);
        }
        /* Menu (only renders when menuactive); no-op in attract demo. */
        M_Drawer();

        /* Poll the wolfDemo user buttons every render frame. I_StartTic
         * also polls them, but only fires from the engine's tic builder
         * - per-frame polling here guarantees the press is caught even
         * when tic builds drop out (long wipe loops, frame stalls). */
        button_tick();

        /* FPS overlay. ST_FpsDrawer is a no-op when show_fps is false;
         * either user button toggles show_fps via i_input_stm32. The
         * widget lives at (318, 2) - top-right corner above the view.
         *
         * We compute fps ourselves over a 1-second window of frames
         * and pass it explicitly: ST_FpsDrawer's auto-mode (fps == -1)
         * caps individual frame deltas at < 100 ms, which silently
         * rejects every sample at our ~10 fps rate (delta = 100 ms
         * exactly), leaving the widget stuck at 0 forever. */
        {
            static int frames_in_window;
            static int window_start_ms;
            static int fps_value;
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
        V_DrawPatchList(vpatchlists->framebuffer);

        if (wipe_start) {
            /* Gamestate just changed. I_VideoBuffer holds the new
             * frame; wipe_from_buf still holds the snapshot from the
             * end of the previous frame. Save the new frame as "to",
             * restore the old frame into I_VideoBuffer, init the y[]
             * table, and run + blit the first melt step. The engine's
             * do/while loop will keep re-entering D_Display until
             * wipestate returns to NONE. */
            memcpy(wipe_to_buf, I_VideoBuffer, sizeof(wipe_to_buf));
            memcpy(I_VideoBuffer, wipe_from_buf, sizeof(wipe_from_buf));
            wipe_init_melt();
            wipestate = WIPESTATE_SKIP1;
            (void)wipe_step_melt();
            wipe_composite();
        } else {
            /* No wipe pending. Snapshot this frame so it's available
             * as the "from" image if the next frame triggers a wipe. */
            memcpy(wipe_from_buf, I_VideoBuffer, sizeof(wipe_from_buf));
        }
    } else {
        /* Mid-wipe iteration. Engine skipped the render gate, so the
         * framebuffer is whatever we left it (the previous melt
         * composite). Advance one tic and re-composite. */
        int done = wipe_step_melt();
        wipe_composite();
        if (done) {
            wipestate = WIPESTATE_NONE;
            /* Lock in the final image and use it as next frame's
             * "from" snapshot. */
            memcpy(I_VideoBuffer, wipe_to_buf, sizeof(wipe_to_buf));
            memcpy(wipe_from_buf, wipe_to_buf, sizeof(wipe_from_buf));
        }
    }

    /* Always blit. For GS_LEVEL the silhouette renderer has already
     * painted into I_VideoBuffer via the pd_add_* callbacks. For
     * GS_FINALE / GS_INTERMISSION we'll ship whatever's in the
     * buffer (likely the previous frame) until those paths are wired. */
    I_FinishUpdate();
}
