# wolfDemo Doom

An experiment to run id Software's 1993 Doom on the wolfDemo board
(STM32U585CIT6, 2 MB flash, 768 KB SRAM, Cortex-M33 @ 160 MHz) using a
MIKROE-6078 IPS Display 2 click in mikroBus 1 as the only display
output. Audio and input are out of scope for the first cut; the goal
is to render the title screen and the built-in attract-mode demos.

The port is based on `kilograham/rp2040-doom`, which already runs full
shareware Doom in 264 KB SRAM and 2 MB flash on a Pi Pico. The
wolfDemo board exceeds the Pico on every relevant axis (3x the SRAM,
faster M33 with FPU/DSP/cache, same 2 MB flash), so the precedent is
strong.

## Status

| Phase | What | State |
|-------|------|-------|
| 0 | mikroBus 1 pin map confirmed | done |
| 1 | bare-metal Makefile project, 160 MHz HSE/PLL clock, USART1 at 115200 | done |
| 2 | ST7789V2 driver, displaying RGB565 colour cycle on the panel | **done, hardware confirmed** |
| 3 | WHD-compressed shareware WAD (1.8 MB) embedded in `.wad` flash section | done |
| 4 | rp2040-doom engine integrated, RP2040 platform layer replaced | **done, hardware confirmed** |
| 5 | first boot: title screen + DEMO1 attract-mode | **done, hardware confirmed** (silhouette rendering only) |
| 6 | textured walls + flats + sky + status bar | **done, hardware confirmed** (with caveats below) |
| 7a | composite walls (multi-patch) | **done, hardware confirmed** |
| 7b | sprites + masked columns + player weapon | **done, hardware confirmed** (with caveats below) |
| 7c | wipe (melt) animation | **done, hardware confirmed** |
| 7d | plane projection perf (column-constant hoist + lighting cache) | **done, hardware confirmed** |
| 7d.5 | per-row batched SPI + GPDMA1 ping-pong blit | **done, hardware confirmed** (~10 FPS) |
| 7d.6 | skip rendering of x % 4 == 3 columns the SPI converter drops | **done, hardware confirmed** (~15 FPS) |
| 7d.7 | -O3 on `pd_stubs.c` only | **done, hardware confirmed** |
| 7d.8 | full-frame DMA mirror + ISR-driven background blit | **done, hardware confirmed** (~20-22 FPS) |
| FPS HUD | engine `show_fps` widget + SW2/SW4 button toggle (off by default) | **done, hardware confirmed** |
| 7e | sprite occlusion at central pillars / ledges | **done, hardware confirmed** (vissprites + drawsegs z-sort re-enabled; ~64 KB BSS) |
| 8  | SFX playback (DAC1 + TIM6 update IRQ) with 4-LED VU meter | **done, hardware confirmed** |

The current `build/wolfDemo-doom.elf` (~2.00 MB / 609 KB BSS):
- runs the full Doom engine (`D_DoomMain`) in 256 KB of zone SRAM,
  with a 96 KB RGB565 mirror buffer and 128 KB of wipe snapshots
  (~140 KB of the 768 KB SRAM still free)
- renders **TITLEPIC**, **CREDIT**, **HELP** etc. pixel-accurate from
  the WHD-compressed splash lumps with the active PLAYPAL palette
- ticks the demo loop: TITLEPIC -> DEMO1 -> CREDIT -> DEMO2 -> ...
- melts between gamestate transitions via the local
  `F_WipeDoMelt`-equivalent in `pd_stubs.c`
- during demos renders **textured** walls including **multi-patch
  composites** (BIGDOOR1, TEKWALL4, COMPSTA1, ...) via the per-
  segment `WHD_COL_SEG_*` walk, **textured floors and ceilings** with
  depth-cued lighting (per-visplane `level[y]` cache, no per-pixel
  divide), **textured sky** ceilings, the **status bar** HUD (face,
  ammo, health, armour, keys) via the vpatchlists patchlist machinery,
  **enemy and decoration sprites** (imps, zombies, barrels, dead
  bodies, projectiles, blood splats) with player-colour translation,
  and the **player weapon** overlay
- has an optional **FPS counter** at top-right (off by default;
  toggle via SW2 / SW4 buttons)
- emits **Doom SFX** through DAC1 / PA4 at 11025 Hz. The TIM6
  update event raises an IRQ; the IRQ handler writes one sample
  to `DAC1->DHR8R1` per tick, and at chunk boundaries kicks the
  software mixer (up to 8 channels, same ADPCM-decode + IIR
  low-pass path as `i_picosound.c`). The four LEDs (PB12..PB15)
  act as a cumulative VU meter on the post-mix peak per chunk.
- pushes frames through `I_FinishUpdate` -> ISR-driven background DMA
  on GPDMA1 channel 0 -> ST7789 SPI in continuous mode at 40 MHz.
  Render and blit run in parallel: per-frame budget collapses from
  `render + blit` to `max(render, blit) + 1.4 ms convert`. Steady
  state ~20-22 FPS during attract-mode demos vs the engine's 35 Hz
  target (see Perf below)

What's still stubbed / known issues:
- **music** stays stubbed. MUS / OPL emulation is too expensive for
  the M33's remaining budget and was scoped out of Phase 8. The
  `I_*Music` calls in `i_sound_stm32.c` still land in no-op stubs.
- **input** (intentionally stubbed; demo plays itself). The two
  user buttons (SW2/PB4, SW4/PB5) are wired only to toggle the
  FPS counter, not to player input.
- **floor sky** (F_SKY1 *floors* still draw as flat silhouette; only
  ceilings dispatch through PDCOL_SKY)
- **masked mid-textures** (door bars, fences, transparent walls) do
  not render. Phase 7e re-enabled `R_StoreWallRange`'s drawseg
  silhouette path for sprite-vs-wall clipping, but upstream
  `R_RenderMaskedSegRange` is hard-wired to non-WHD struct layout
  (direct `curline->frontsector`, `curline->sidedef->midtexture`,
  the legacy `texturetranslation` / `textureheight` arrays, and the
  upstream `R_GetColumn(int)` / `R_DrawMaskedColumn(column_t*)`
  signatures) that the `SHRINK_MOBJ` / `USE_RAW_MAPSEG` / WHD
  framedrawable model has restructured. The function is stubbed in
  `doom/src/doom/r_segs.c`; the upstream body is preserved inside
  `#if 0` for a future port. Sprite occlusion against walls works
  correctly via the drawseg silhouettes - this caveat only affects
  transparent walls themselves.

### Perf

Currently ~20-22 FPS against the engine's 35 Hz target. The journey
from the first hardware boot (silhouettes, ~10 FPS) to the current
state landed five distinct multipliers - see Phase 7d.* sections
below for the per-step writeup. Summary of the path that worked:

| Step | Change | FPS observed |
|------|--------|--------------|
| baseline | textured walls + planes + sprites | ~6-7 |
| 7d   | hoist column-constant Kx/Ky + per-visplane lighting LUT | (no measurable jump - see 7d.5) |
| 7d.5 | per-row batched SPI + GPDMA1 ping-pong | ~10 |
| 7d.6 | skip the 25% of columns the SPI converter drops | ~15 |
| 7d.7 | -O3 on the renderer TU only | (folded into 7d.6 measurement) |
| 7d.8 | full-frame DMA mirror + ISR-driven background blit | ~20-22 |

The single most surprising lesson was that all of the algorithmic
plane-projection work in 7d was invisible until 7d.5 because the
per-pixel SPI transactions in `st7789_blit` were saturating the frame
at ~96 ms before any rendering started to matter. Always profile the
output stage first.

Remaining headroom toward the 35 Hz target, roughly in order of
expected payoff:

1. **Patch decode early-out**. `paint_textured_column` always decodes
   the full 128-row `col_buf[]` even when only a small vertical slice
   of the column is visible (top/bottom seams). Capping decode to the
   actual sample range would help the most for the typical Doom
   geometry that's mostly slivers. The Huffman decode is sequential
   from index 0, so "stop early" is the only handle - skipping ahead
   isn't possible without re-encoding.
2. **Precompute per-column plane K-coefficients per visplane**.
   `pd_add_plane_column` redoes `finecosine`, `finesine`, `distscale`
   for every column emission; a single visplane visits each column
   once but multiple visplanes (typically floor + ceiling) share
   x ranges and could share the column-constant Kx/Ky computation.
3. **DSP intrinsics for `FixedMul`**. The Cortex-M33 in the U585
   has the DSP extension (`-mcpu=cortex-m33` already enables it).
   `__SMMUL` / `__SMMLAR` give single-cycle Q31 mul-and-shift; not a
   1:1 swap for FixedMul but worth probing the per-pixel inner loops.
4. **Bump SPI to 80 MHz**. `SYSCLK / 2 = 80 MHz` exceeds the ST7789V2
   datasheet's 62.5 MHz max but the panel often tolerates it. Halves
   the SPI bandwidth floor from 19 ms to ~10 ms - if render ever
   drops below that floor again the SPI re-becomes the bottleneck.
5. **Sliver flat decode**. Same idea as #1 for the 64x64 flat
   decoder: most visible plane spans hit only a small fraction of
   the flat. Already cached in a 4-slot LRU; further wins need
   row-level granularity in the Huffman decode.

## Pin map (board.h)

| Display signal | mikroBus pin | wolfDemo pin | Notes |
|----------------|--------------|--------------|-------|
| SPI SCK        | SCK          | PA1 / SPI1_SCK   | AF5 |
| SPI MOSI (SDA) | MOSI         | PA7 / SPI1_MOSI  | AF5 |
| SPI MISO       | MISO         | PA6              | not connected on this click |
| CS             | CS           | PA0              | active low |
| RST            | RST          | system NRST      | shared; we use SWRESET command |
| D/C            | INT          | PB1              | repurposed mikroBus INT |
| BL (backlight) | AN           | PA2              | repurposed mikroBus AN |

Audio (Phase 8) takes one extra pin off the mikroBus connector:

| Audio signal | wolfDemo pin | Notes |
|--------------|--------------|-------|
| DAC1 OUT1    | PA4          | analog mode, DAC normal mode (output buffer enabled), into a passive RC + speaker |

## Building and flashing

Toolchain: `arm-none-eabi-gcc` 15.2 confirmed working,
`stm32flash` and `openocd`. On macOS via Homebrew:
`brew install --cask gcc-arm-embedded && brew install stm32flash openocd`.

Clone with submodules so the patched rp2040-doom checkout lands
under `doom/`:

```
git clone --recurse-submodules https://github.com/LinuxJedi/wolfDemo-Doom
# or, after a bare clone:
git submodule update --init --recursive
```

Build / flash:

```
make                  # builds build/wolfDemo-doom.{elf,bin,hex}
make flash            # UART programming (default; see below)
make flash-swd        # SWD programming via Tag-Connect
make monitor          # `screen` on the USB-UART at 115200
make clean
```

### UART flashing

Hold `BOOT0`, tap `RESET`, release `BOOT0`. Then `make flash`.
Auto-detects `/dev/cu.usbserial-*`; pass `TTY=/dev/cu.usbserial-XXXX`
to override.

After flashing, open the serial monitor at **115200 8N1** (not 8E1
which is what stm32flash uses while in bootloader mode).

## Bring-up bugs we hit and fixed

For the next time someone does this on a U5, here is the full list:

1. **`HSE_VALUE` was set to 16 MHz** in the build defines. The actual
   wolfDemo crystal is 8 MHz. Symptom: post-PLL UART baud rate was 2x
   off, all post-stage-0 logs were garbled. Fix: `-DHSE_VALUE=8000000U`
   in the Makefile.
2. **PLL register field encoding**. `M`, `N`, `R` fields in
   `RCC_PLL1CFGR`/`PLL1DIVR` store `value-1`, not the divider value
   directly. HAL macros confirmed this at
   `stm32u5xx_hal_rcc.c:1275-1283`.
3. **EPOD booster sequence**. `BOOSTRDY` never asserts if `VOS` and
   `BOOSTEN` are written separately. They must be programmed in a
   single register write (matches HAL `pwr_ex.c:371`), and
   `BOOSTRDY` is checked **after** the PLL is configured (matches
   HAL `rcc.c:1441-1450`).
4. **D/C pin**: assumed PB5 from the schematic position read; actually
   PB1. The user caught this with a logic probe.
5. **SPI1 kernel clock**. STM32U5 peripherals have a kernel clock
   selector independent of the APB enable bit. Without setting
   `RCC_CCIPR1.SPI1SEL`, the bit shifter has no clock and bytes go
   into the FIFO and never come out. Set to SYSCLK to match HAL.
6. **SPI `EOT` does not refire reliably on chunked transfers**.
   Polling `CTSIZE == 0` instead of `EOT` worked for completion;
   `TXC` for the final-bit settle.
7. **Tight SPE off/on cycle**. With BATCH=7 (=14 bytes/chunk) the
   240x240 fill cycled SPE 8228 times and the peripheral got into a
   stuck state where CSTART stopped re-arming. Bumping BATCH to 256
   (=225 chunks for a full fill) fixed it.
8. **Mode fault auto-clears MASTER**. The single hardest bug. On
   STM32U5 the SPI's MODF condition silently auto-clears
   `CFG2.MASTER`, dropping the peripheral into slave mode where it
   stops generating SCK and stops driving MOSI. The fix: rewrite
   `CFG2 = MASTER | SSM | COMM_0` and `IFCR_ALL` (which clears
   MODFC) at the **start of every chunk**. Symptom: a brief burst
   of SCK during init followed by silence, MOSI stuck at 0x00.

## Layout

```
wolfDemo-doom/
  Makefile                  bare-metal arm-none-eabi-gcc build
  STM32U585CITX_FLASH.ld    linker script + .wad section
  startup/
    startup_stm32u585citx.s vector table + Reset_Handler
  cmsis/                    vendored CMSIS-Core + STM32U5xx headers
  src/
    main.c                  bring-up: clock, UART, ST7789 -> D_DoomMain
    board.h                 pin map + system clock target
    clock.c                 HSE 8 MHz -> PLL1 -> 160 MHz on VOS1 + EPOD
    uart.c                  polled USART1 at 115200 (PA9/PA10, AF7)
    spi.c                   polled SPI1 master, 40 MHz, with the
                            CFG2 re-assert per chunk that fixes MODF
    st7789.c                ST7789V2 driver, full init incl. gamma
    syscalls.c              newlib stubs; _write -> uart
    system_stm32u5xx.c      ST CMSIS SystemInit
    doom_glue/              everything that connects rp2040-doom to STM32
      config.h              hand-written replacement for cmake-generated
                            config.h; also force-included via -include
      net_client.h          shadow header (force-included) providing
                            uint, panic_unsupported, hard_assert,
                            __fast_mul, networkgame -- one-stop shim for
                            the Pico SDK / NO_USE_NET gating bugs
      pico/sem.h            semaphore_t placeholder for DOOM_TINY paths
      SDL_endian.h          minimal SDL_SwapLE16/32 macros
      tiny.whd.h            aliases tiny_whd to _binary_wad_doom1_whx_start
      i_video_stm32.c       framebuffer (320x200 8bpp), I_SetPalette,
                            I_SetPaletteNum, I_FinishUpdate (palette ->
                            RGB565, 320 -> 240 column drop, SPI blit)
      i_input_stm32.c       all inputs report no events (attract mode)
      i_sound_stm32.c       no audio, no music
      i_timer_stm32.c       SysTick @ 1 kHz -> TICRATE
      i_system_stm32.c      I_Error / I_Quit / panic / I_ZoneBase
                            (256 KB static zone), I_Realloc, etc.
      pd_stubs.c            stand-in for pd_render.cpp: WHD splash
                            decoder for GS_DEMOSCREEN, silhouette
                            painter for pd_add_column, wipe-state
                            neutraliser, framedrawables reset
      engine_stubs.c        small symbols (I_BindSoundVariables,
                            I_GetSfxLumpNum, snd_pitchshift, usegamma,
                            th_bit_overrun, etc.)
  wad/
    doom1.whx               1.8 MB WHD-compressed shareware DOOM1.WAD
  doom/                     git submodule of kilograham/rp2040-doom
                            (with a small wolfDemo-specific patch
                            branch; see License section for what's
                            modified)
  build/                    output (ELF, bin, hex, .map)
```

## Phase 4 / 5 notes (what actually happened)

- The Makefile wires in ~85 engine TUs from `doom/src/` and
  `doom/src/doom/`. We deliberately skip `i_main.c` (we own
  `main.c`), the `i_*.c` files we replaced, the `net_*.c` chain
  (`NO_USE_NET=1`), `pd_render.cpp` (Pico scanvideo), and the
  `deh_*.c` group (`NO_USE_DEH=1`).

- `config.h` is hand-written. It also forward-declares `panic` and
  `I_Error`; a couple of engine files use them without including the
  header that declares them.

- `src/doom_glue/net_client.h` is a one-stop **bare-metal compatibility
  prelude**, force-included via `-include` from the Makefile. It
  shadows the upstream `net_client.h` (defeating the
  `#define net_client_connected false` macro that breaks an
  unguarded `net_client_connected = false` in `d_loop.c`), and it
  also provides the `uint` typedef, `panic_unsupported`,
  `hard_assert`, `__fast_mul`, and the `networkgame` constant that
  `m_menu.c` references inside an unguarded DOOM_TINY branch. Force-
  including a single small header was much cleaner than patching
  half a dozen upstream files.

- `pico/sem.h`, `SDL_endian.h`, and `tiny.whd.h` are all minimal
  shims under `src/doom_glue/`. The objcopy step in the Makefile
  exposes the WAD blob as `_binary_wad_doom1_whx_start`; the
  `tiny.whd.h` shim aliases it to the `tiny_whd` symbol the engine
  expects.

- `pd_stubs.c` replaces `pd_render.cpp`. It ports
  `draw_splash` for `GS_DEMOSCREEN` (pixel-accurate TITLEPIC,
  CREDIT, HELP via the WHD Huffman decoder). For `GS_LEVEL` it
  paints flat-colour silhouettes per `pd_add_column` /
  `pd_add_plane_column` callback. `wipestate` is pinned to
  `WIPESTATE_NONE` so the engine's `do { D_Display(); } while
  (wipestate);` loop runs once per frame.

- The engine zone is a 256 KB static buffer in BSS via
  `I_ZoneBase` in `i_system_stm32.c`. Final RAM use: 360 KB / 768 KB.
  Final flash use: ~1.99 MB / 2.0 MB (cutting it close; the WAD
  alone is 1.8 MB).

### Bring-up bugs we hit during Phase 4 / 5

- `gcc 14+` promotes implicit function declarations to errors. Engine
  files that call `panic`, `I_Error`, `piconet_stop`, `panic_unsupported`,
  `__fast_mul`, `hard_assert` without the right include surface as
  link errors -- forward-declared everything in the prelude header.

- `#include "..."` searches the **including file's directory first**.
  Putting a shadow header under `src/doom_glue/` doesn't override
  `doom/src/net_client.h` no matter how the `-I` order goes. The
  fix is `-include`, which loads the shadow first and lets its
  include guard suppress the upstream.

- `pd_begin_frame` must call `reset_framedrawables()` every frame.
  Skip it and `num_framedrawables` walks past `MAX_FRAME_DRAWABLES =
  128` after a few seconds and trips a `hard_assert`.

- WHD splash decoder buffers must be sized for
  `read_raw_pixels_decoder_c3`, which packs 3 bytes per symbol with
  up to 263 symbols (~792 bytes minimum tmp buffer). 512 bytes
  triggers an assert mid-decode.

- `i_sound.c` and `i_oplmusic.c` from upstream conflict with our
  `i_sound_stm32.c` stubs (duplicate `I_*Sound` / `I_*Music`
  symbols). Drop them from the build -- our stubs satisfy every
  call site the rest of the engine makes.

- `st7789.h` had a header guard `ST7789_H` that collided with its
  own `#define ST7789_H 240` panel-height constant. Renamed the
  guard to `WOLFDEMO_ST7789_H`.

## Phase 7 progress

7a through 7e are all hardware-confirmed. Phase 8 (audio) is too.

### 7a. Composite-column walls - done

Multi-patch wall textures (BIGDOOR1, TEKWALL4, COMPSTA1, ...) now
render via `paint_composite_column` in `src/doom_glue/pd_stubs.c`,
which walks the `WHD_COL_SEG_*` segment stream after `patch_table`,
identifies the per-column range covering `dc_source.col`, and applies
each segment (patch decode or memcpy, with optional `EXPLICIT_Y` /
`MEMCPY_IS_BACKWARDS` flags) into a 129-byte pixel buffer with
upstream's minimal boundary fixup. The xoff math is `pcol = (uint8_t)
(target_col - base + metadata[2])` with strict uint8 wrap (matches
the encoder rewrite at `whd_gen.cpp:4167`).

### 7b. Sprite rendering - done

`pd_add_masked_columns` now buffers each column-draw request into a
per-frame queue (split into `sprite_queue` for enemies/masked-mid-
textures and `psprite_queue` for the player weapon, distinguished by
`pd_flag & 2`). Drain happens in `pd_end_frame` after BSP +
`R_DrawPlanes` complete: the enemy queue drains in reverse order
(BSP enqueues closer-first, so reverse gives back-to-front), the
player-weapon queue drains forward so the pistol sits on top. This
fixes the visplane-overpaints-sprite issue.

Also fixed during this phase:
- `decode_patch_column` now handles `encoding == 0` (single-byte
  Huffman, no delta) - previously bailed to `memset(0)` which painted
  most sprites and some composite wall patches as solid black
  silhouettes. Splash assets all use `encoding == 1` so the splash
  decoder's `assert(false)` was misleading.
- `Makefile` removes `-DNO_MASKED_FLOOR_CLIP=1` (was set upstream
  because the Pico's deferred z-sort renderer doesn't need it -
  ours does); re-enables `R_DrawSpriteEarly`'s `mfloorclip = floorclip;
  mceilingclip = ceilingclip;` assignment so masked-column clipping
  actually runs.
- Per-segment offset-aware re-clip in `pd_add_masked_columns` -
  `r_things.c:R_DrawMaskedColumn` clips with `yh >= floorclip` instead
  of `yh >= floorclip - FLOOR_CEILING_CLIP_OFFSET`, which lets the
  sprite bleed one row into bottom walls when offset=1. We re-clip
  at queue time using the live `floorclip[]` / `ceilingclip[]` arrays
  to get the correct boundary. `base_off` is left untouched (it's in
  col_buf space, not screen space; the frac/fracstep iterator
  naturally advances correctly with the clipped yl).

Known issue (not fixed): sprite occlusion at walls in the middle of
rooms - see "Known issues" in the Status section. Two reproducible
patterns:
- A solid pillar/column in a room. Enemies/decorations behind it
  show through where the pillar covers them.
- A raised ledge with things on top. The bottom-texture wall doesn't
  fully clip the lower halves of those things.

### 7c. Wipe animation - done

Local F_WipeDoMelt-equivalent in `pd_stubs.c`: two 64 KB buffers
(`wipe_from_buf`, `wipe_to_buf`) snapshot the pre- and post-transition
frames, a per-column `wipe_y[320]` slides per loop iteration, and a
column-major composite mixes the two buffers into `I_VideoBuffer`.
`pd_begin_frame` no longer pins `wipestate = WIPESTATE_NONE`; instead
`pd_end_frame` advances the state itself: on the first wipe frame it
saves the new frame as "to", restores the old frame from the previous
end-of-frame snapshot, initialises `wipe_y[]`, and runs one melt step.
Subsequent iterations of the engine's `do { D_Display(); } while
(wipestate);` loop skip rendering (gated naturally by `wipestate !=
NONE`) and just advance the melt until every column hits `DOOM_H`.
Cost: 128 KB BSS for the two snapshot buffers, plus a 64 KB memcpy at
the end of every normal frame to keep the "from" snapshot fresh.

Either user button (SW2/PB4 or SW4/PB5) toggles the engine's
built-in FPS counter (`show_fps`); it's off by default - press to
turn it on. `pd_end_frame` calls `ST_FpsDrawer(fps)` whenever
`show_fps` is set so the widget at (318, 2) shows a 16-frame moving
average. The four LEDs are reserved for the Phase 8 VU meter; no
LED is toggled on button press.

### 7d. Plane projection performance - done

`pd_add_plane_column` in `pd_stubs.c` now:
1. Hoists the column-constant `Kx = cos_a * plane_h * dscale` and
   `Ky = sin_a * plane_h * dscale` out of the per-pixel loop. Per
   pixel the world-space sample is `world_x = viewx + FixedMul(Kx,
   yslope[y])` (and `world_y` symmetrically), dropping the per-pixel
   mul count from 4 to 2.
2. Caches the lighting `level[y]` profile per `fd_num` (single slot,
   reset every frame in `pd_begin_frame`). For a given visplane both
   `startmap` and `plane_h` are constant, so the per-pixel
   `(SCREENWIDTH/4)/(zindex+1)` divide collapses into a 168-entry
   `uint8_t` table that's filled once per visplane visit and indexed
   per pixel.

Combined: per pixel goes from `4 FixedMul + 1 udiv` to `2 FixedMul + 1
LDR`. The DSP-intrinsic exploration (`__SMMUL`, `__SMMLA`) is still
available as a follow-up if needed.

### 7d.5 SPI blit (the actual bottleneck) - done

The first hardware FPS measurement showed only ~6-7 FPS even after
the plane-projection wins above. Root cause: `st7789_blit` was issuing
a full SPI transaction per pixel (240 transactions per row x 200 rows
= 48000 SPE off/on cycles per frame, ~2 us each = ~96 ms/frame just on
SPI overhead). Two changes flatten that:

1. **Per-row batched SPI**: `i_video_stm32.c::I_FinishUpdate` now
   converts a Doom row to 240 byte-swapped RGB565 pixels in a
   480-byte scan-line buffer and ships the whole row in one transfer.
   The palette LUT is pre-byte-swapped at `I_SetPalette` time so a
   uint16_t scan_out[] can be sent directly via SPI as bytes - the
   ST7789 expects MSB first per pixel and a little-endian uint16_t
   that holds the byte-swapped RGB565 value lands in memory as
   `[hi, lo]`, exactly the wire order. Brings the per-frame SPI cost
   from ~96 ms to ~19 ms (the 40 MHz SPI bandwidth floor for a
   240x200x16 frame).
2. **GPDMA1 channel 0 ping-pong**: `spi_dma_blit_start` /
   `spi_dma_blit_wait` (in `spi.c`) drive a dedicated GPDMA channel
   wired to SPI1_TX request line 7. The video glue holds two
   scan_out buffers and pipelines them - while DMA pumps row N out,
   the CPU prepares row N+1 in the other buffer. Removes the ~7 us
   of per-row palette-conversion idle time and hides any future
   per-row CPU work up to the ~96 us SPI clocking budget.

A `dsb` is issued before enabling the DMA channel so the CPU's store
buffer is drained - otherwise a tight `prepare_scanline` -> `dma_start`
sequence can race the GPDMA reading stale memory. STM32U5 DCACHE1 is
write-through by default, so no cache clean is needed for DMA reads
of SRAM.

### 7d.6 Skip rendering of dropped-by-SPI columns - done

The display is 240 px wide; `I_FinishUpdate` scales the 320-wide
framebuffer down by dropping every 4th column (`x % 4 == 3`). Those
columns are never sent to the panel, so painting them is pure waste.
`pd_add_column`, `pd_add_plane_column`, and `pd_add_masked_columns`
now early-return on `(x & 3) == 3`, removing ~25% of the per-frame
column work. The SPI converter's existing column-drop discards the
black gaps left in `I_VideoBuffer`, so there is no visible loss - and
because the wipe melt also column-stripes from the framebuffer through
the same SPI path, the wipe stays artifact-free too.

### 7d.7 -O3 on the renderer - done

`pd_stubs.c` is now built with `#pragma GCC optimize("O3")`,
overriding the global `-Os`. Costs ~2 KB of code; pays back on the
per-pixel loops in `paint_textured_column`, `paint_composite_column`,
and `pd_add_plane_column` where the compiler can now unroll, hoist
loop invariants, and keep the colormap base register-resident. Other
TUs stay on `-Os` to keep flash budget in hand.

### 7d.8 Background blit (full-frame DMA mirror) - done

`I_FinishUpdate` no longer blocks on the SPI. The flow is now:

1. `spi_blit_async_wait()` - drain any DMA still in flight from the
   PREVIOUS frame.
2. `st7789_blit_end()` - raise CS, ending the previous RAMWR.
3. Convert all 200 rows of the 8 bpp `doom_fb` into a 240 px x 200
   row `scan_full_buf` RGB565 mirror (96 KB BSS, byte-swapped so the
   bytes land in wire order). ~1.4 ms.
4. `st7789_set_window` + `st7789_blit_begin` open a fresh RAMWR.
5. `spi_blit_async_start(scan_full_buf, 96000)` kicks an asynchronous
   blit and returns. SPI runs in continuous mode (`TSIZE = 0`) so it
   keeps requesting bytes from GPDMA1 channel 0. The channel TC
   interrupt (`GPDMA1_Channel0_IRQHandler`) re-arms the channel for
   the next chunk - the SPI's `TSIZE` 16-bit limit forces 2 chunks
   per frame, but the panel sees one unbroken RAMWR sequence because
   continuous mode hides the chunk boundary from SPI's framing logic.

While the SPI is clocking, the engine is already running BSP +
`R_RenderPlayerView` + sprite drain + `ST_Drawer` for the NEXT frame
into `doom_fb`. The mirror buffer decouples the two, so writes to
`doom_fb` don't race the in-flight DMA reading `scan_full_buf`.
Per-frame budget collapses from `render + blit` to roughly
`max(render, blit) + 1.4 ms` for the conversion. With ~46 ms render
and ~19 ms blit, expected steady-state ~20-22 FPS. Cost: 96 KB BSS
for the mirror buffer (current total ~609 KB / 768 KB available,
~140 KB headroom remaining).

Wipes also benefit - mid-melt iterations are now bounded by
`max(composite ~1 ms, blit ~19 ms)`, dropping the wipe duration
from ~2.5 s to ~500 ms.

### 7e. Sprite occlusion - done

Sprites in sectors that span multiple subsectors used to bleed
through walls because `R_AddSprites` runs only on the first visited
subsector (per-sector `validcount`), and the inline
`R_DrawSpriteEarly` path re-clipped against the global
`floorclip[]`/`ceilingclip[]` arrays - which haven't yet been
updated by walls of later-visited subsectors. A prior smaller
"reorder `R_AddSprites` after `R_AddLine`" fix caused
over-clipping by walls of the sprite's own (convex) subsector that
sit behind the sprite, and was rolled back.

RP2040-doom solves this with a depth-sorted per-column linked-list
rasterizer (`pd_render.cpp:push_down_x`) in which every column emit
self-orders by `scale`; porting that wholesale would mean rewriting
`pd_stubs.c`. Instead we re-enabled upstream Chocolate Doom's
existing z-sort by dropping three Makefile macros:

- `-DNO_VISSPRITES=1` -> `R_AddSprites` now allocates real
  `vissprite_t` entries instead of inline-painting; `R_DrawMasked`
  sorts them by scale and walks back-to-front at end of frame.
- `-DNO_DRAWSEGS=1` -> `R_StoreWallRange` records per-segment
  drawseg silhouettes during the *full* BSP traversal, independent
  of subsector visit order. `R_DrawSprite` clips each vissprite
  per-x against the drawsegs that are in front of it.
- `-DFLOOR_CEILING_CLIP_8BIT=1` -> required because `r_segs.c:911`
  does `memcpy(lastopening, ceilingclip + start, sizeof(*lastopening) * count)`,
  with `lastopening` declared as `short *` and `ceilingclip` as
  `uint8_t[]` under 8BIT, which would over-read two bytes per source
  element and garble the silhouette. Dropping 8BIT widens the four
  320-byte clip arrays to shorts (+1.3 KB BSS) and turns
  `FLOOR_CEILING_CLIP_OFFSET` into 0 across `r_things.c` and
  `r_segs.c`.

Smaller changes also went in:

- `r_defs.h` and `r_things.c`: widened `#if !DOOM_TINY` guards on
  the `vissprite_t.gx/gy/gz/gzt` fields and their stores to
  `#if !DOOM_TINY || !NO_DRAWSEGS`. `R_DrawSprite` reads those at
  `r_things.c:1058,1072,1075` for `R_PointOnSegSide` and silhouette
  height gating.
- `r_things.c::R_DrawSpriteEarly`: gated body to
  `#if PICO_DOOM && NO_VISSPRITES` so the unconditional call at the
  end of `R_ProjectSprite` becomes a no-op when the deferred
  `R_DrawMasked` path is active. Otherwise sprites paint twice.
- `Makefile`: dropped `-DNO_DRAW_SPRITES=1`. That macro initialises
  the runtime flag `no_draw_sprites=1`, which gates out the entire
  vissprite drain in `R_DrawMasked`. Invisible under
  `NO_VISSPRITES=1` (no vissprites to drain) but load-bearing once
  vissprites are real.
- `pd_stubs.c`: drain order flipped from reverse to forward. With
  vissprites, `R_DrawMasked` walks `vsprsortedhead.next` back-to-
  front itself, so columns arrive at the queue already in the
  correct paint order. Reverse iteration would invert it. Player
  weapon sprites stay forward (psprite queue is independent and
  drains last so the gun sits on top).
- `pd_stubs.c::pd_add_masked_columns`: secondary re-clip gate
  changed from `!(pd_flag & 2)` to `(pd_flag & 1)`. With vissprites
  on, `R_DrawMaskedColumn` already clamps yl/yh against per-sprite
  `mfloorclip[]`/`mceilingclip[]` (which point at the per-sprite
  `clipbot[]`/`cliptop[]` arrays computed from drawsegs in
  `R_DrawSprite`). The legacy secondary re-clip used the global
  `floorclip[]`/`ceilingclip[]`, which would over-clip back-sprites
  by walls of intervening sectors.

BSS cost: `vissprites[128]` (~7 KB), `drawsegs[256]` (~16 KB),
`openings[SCREENWIDTH * 64]` (~40 KB), wider clip arrays (~1.3 KB),
and `vissprite_t.gx/gy/gz/gzt` (~2 KB) = ~66 KB. Final BSS is
~673 KB / 768 KB (~95 KB free).

Caveat: `R_RenderMaskedSegRange` (the masked-mid-texture renderer
for door bars, fences, transparent walls) is stubbed because its
upstream body is incompatible with our `SHRINK_MOBJ` /
`USE_RAW_MAPSEG` / WHD framedrawable surface. The drawseg
silhouette work that `R_DrawSprite` needs is independent
(`R_StoreWallRange`, lower in the same file) and is intact.
Re-implementing the masked-mid path against the WHD accessors is
follow-up work; sprite-vs-wall occlusion - the actual 7e fix - is
unaffected.

### 8. SFX with a 4-LED VU meter - done

`src/doom_glue/i_sound_stm32.c` now plays real SFX. Pipeline:

1. **DAC1 channel 1 on PA4** runs in normal mode (output buffer
   enabled, `MCR.MODE1=000`, `MCR.HFSEL=01` for the 80..160 MHz AHB
   band). No trigger, no DMA - the CPU writes `DHR8R1` directly
   from the timer IRQ. The pin drives a passive RC + speaker on
   the user's audio test rig.
2. **TIM6** is clocked from PCLK1 (= SYSCLK = 160 MHz) at PSC=0,
   ARR=`(160e6 / 11025) - 1` = 14511, `DIER.UIE=1`. Each update
   event raises `TIM6_IRQn`.
3. **`TIM6_IRQHandler`** reads one byte from a 1024-sample circular
   ring buffer and writes it to `DAC1->DHR8R1`. Steady-state path
   is ~50 cycles (~0.34% CPU). At sample HALF_SAMPLES-1 and
   RING_SAMPLES-1 the IRQ also runs the mixer to refill the half
   just consumed (~250 us, well inside the 46 ms half-period).
4. **Mixer** iterates up to 8 channels per chunk. Each channel
   decompresses one 128-byte ADPCM block at a time into a 249-byte
   scratch (matching the WHD `whd_gen` block layout), advances
   through it via a 16.16 fractional `offset` for pitch, IIR-
   low-passes against the previous sample (same single-pole filter
   as `i_picosound.c`, with `alpha256 = 256 * 201 * sample_freq /
   (201 * sample_freq + 64 * AUDIO_SAMPLE_RATE)`), and accumulates
   into a per-sample int32 mix buffer scaled by the channel's
   volume (0..127 derived from `(left + right) / 4`). After all
   channels mix, the chunk is shifted right by 7, clamped, and
   offset by 0x80 for unsigned DAC output.
5. **4-LED VU meter on LED1..LED4**: the mixer scans the chunk
   for the peak abs amplitude (post-scale, pre-offset; range
   0..127) and lights LED1..LED4 cumulatively at thresholds
   3 / 12 / 32 / 72. Silence keeps all LEDs dark; loud / clipping
   sounds light all four.

The four LEDs are dedicated to the VU meter: the prior LED1
button-press confirmation in `i_input_stm32.c` was removed and
`main.c`'s stage-4 indicator now turns LED4 off at engine handoff
so the meter starts from a clean state.

Music (MUS / OPL) remains stubbed. The OPL emulator alone is too
hot for the M33's remaining budget while sustaining 35 Hz render,
and the upstream MUS converter pulls in pico_audio I2S code that
doesn't apply.

### Phase 8 bring-up notes

We originally planned a DMA-driven design (TIM6_TRGO -> DAC -> DMA
request line -> ring buffer). It ran into bus errors on this
silicon that we couldn't resolve in-session, so the final
implementation uses a TIM6 update IRQ that writes each sample to
`DHR8R1` directly. Trail of attempts:

1. **GPDMA1 channel 1, REQSEL=2 (DAC1_CH1)** latched DTEF
   immediately. GPDMA1 is on AHB1; DAC1 is on AHB3. GPDMA1's two
   AHB master ports don't bridge into the AHB3 sub-bus, so any
   write to `DAC1->DHR8R1` returns DTEF on the first attempt.
2. **LPDMA1 channel 0, REQSEL=8 (DAC1_CH1 in LPDMA1's table)**.
   LPDMA1 is co-resident with DAC1 on AHB3, so this should have
   worked - but still latched DTEF. Promoting the channel to
   secure (`SECCFGR.SEC0` + `CTR1.SSEC|DSEC`), enabling
   `RCC_AHB3ENR.SRAM4EN` explicitly, and moving the ring buffer
   between main SRAM and SRAM4 all changed nothing. Whatever was
   rejecting the bus access on this specific silicon wasn't
   visible in the documentation we had access to.
3. **TIM6 update IRQ writing `DHR8R1` directly** worked first
   try. CPU writes to the DAC have always succeeded (we use them
   during `audio_hw_init` to pre-load mid-rail), so this sidesteps
   the DMA mystery entirely. The 0.34% CPU cost is negligible
   compared to the renderer's budget.

Other tuning in passing:

- **DAC `MCR.HFSEL`** is set to `01` (the 80..160 MHz AHB band).
  Default `00` is rated only up to 80 MHz; our AHB1 runs at 160
  MHz, so leaving HFSEL at the reset value left the DAC silent.
- **Mixer output shift**: an early `>>9` made even loud single-
  channel SFX peak around 5..7, below any sensible VU threshold.
  Now `>>7`, with thresholds 3 / 12 / 32 / 72 across LED1..LED4.
- **DTEF visibility**: the diagnostic LED encoding we used during
  bring-up turned the four LEDs into a one-LED-per-error display
  before the engine drew a frame. That code is gone now that the
  pipeline works, but the pattern of "use unused indicators to
  decode a runtime-only error register" is worth keeping in mind
  for future bring-ups.

Open caveats once the rig is back up:

1. **Pitch shift accuracy** - the per-channel `step` formula uses a
   64-bit divide for `pitch != NORM_PITCH`. Cheap enough for the
   ~21 Hz `I_StartSound` rate, but could be tabled if profiling
   shows it.
2. **Channel volume race** - `I_UpdateSoundParams` writes left/right
   while the mixer reads them. Volume changes mid-chunk produce at
   most one 46 ms chunk of intermediate volume; not worth a lock.
3. **No rumble / surround** - intentional.

## Open work items

- **Music (MUS / OPL)**. Out of scope so far. Would need OPL
  emulation in a TIM-driven mixer, plus MUS-to-MIDI plumbing.
  Likely cuts deeply into the FPS budget.
- **Masked mid-textures**. `R_RenderMaskedSegRange` is stubbed (see
  the 7e caveat above); door bars and fences will not render until
  the function is rewritten against the WHD framedrawable
  accessors.
- **More FPS**. The Perf section lists the ranked remaining wins.
  The biggest unexplored ones are patch-decode early-out (slivers
  waste decode work), per-visplane column-K precomputation, DSP
  intrinsics for `FixedMul`, and overclocking SPI to 80 MHz
  (`SYSCLK / 2`, exceeds the ST7789V2 spec of 62.5 MHz but often
  tolerated).

## License

This project is GPL-2.0-or-later. See [LICENSE](LICENSE) for the
full text.

The `doom/` submodule and `wad/doom1.whx` are GPL-2.0+
(id Software / Chocolate Doom / Graham Sanderson) and dictate the
licensing of the whole derivative work. Vendored CMSIS headers
under `cmsis/` are Apache 2.0 (ST / Arm), which is GPL-compatible.
The wolfDemo-specific glue (`Makefile`, `STM32U585CITX_FLASH.ld`,
`startup/`, `src/`) is GPL-2.0-or-later.
