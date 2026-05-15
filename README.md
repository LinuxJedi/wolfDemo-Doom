# wolfDemo Doom

id Software's 1993 Doom running on the wolfDemo board
(STM32U585CIT6, 2 MB flash, 768 KB SRAM, Cortex-M33 @ 160 MHz)
through a MIKROE-6078 IPS Display 2 click in mikroBus 1, with SFX
and OPL music out of DAC1/PA4, a 4-LED VU meter on PB12..PB15,
and optional QwSTPad / seesaw joystick input over I2C.

The port is based on `kilograham/rp2040-doom`, which already runs
full shareware Doom in 264 KB SRAM and 2 MB flash on a Pi Pico.
The wolfDemo board exceeds the Pico on every relevant axis (3x the
SRAM, faster M33 with FPU/DSP/cache, same 2 MB flash).

## What it does

The current firmware:
- runs the full Doom engine (`D_DoomMain`) in 256 KB of zone SRAM,
  with two 96 KB RGB565 ping-pong buffers and in-place wipe melt
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
  toggle via SW2), drawn with a 3x-scaled bitmap
  font over a black inset for legibility on the click panel
- emits **Doom SFX and OPL music** through DAC1 / PA4 at 11025 Hz. The TIM6
  update event raises an IRQ; the IRQ handler writes one sample
  to `DAC1->DHR12R1` per tick, and at chunk boundaries pends
  the low-priority software mixer (up to 8 SFX channels plus OPL).
  The four LEDs (PB12..PB15)
  act as a cumulative VU meter on the post-mix peak per chunk.
- pushes frames through `I_FinishUpdate` -> ISR-driven background DMA
  on GPDMA1 channel 0 -> ST7789 SPI in continuous mode at 40 MHz.
  Render and blit run in parallel with no 8 bpp -> RGB565 conversion
  pass: per-frame budget collapses from `render + blit` to
  `max(render, blit)`. Hits the
  engine's 35 Hz target in typical scenes; large open areas with
  many sprites/visplanes drop to ~12-15 FPS (see Perf below)

Known issues (see "Open work items" for details):
- **floor sky** (F_SKY1 *floors* still draw as flat silhouette;
  only ceilings dispatch through PDCOL_SKY).

### Perf

Hits the engine's 35 Hz target in typical scenes. Heavy open areas
with many visplanes / sprites still drop to ~12-15 FPS. Render and
SPI blit run in parallel via GPDMA1 channel 0 in continuous mode
plus a second 96 KB RGB565 buffer; per-frame budget is
`max(render, blit ~19 ms)`.

Optimizations already in:

- **Inlined `FixedMul`** for Cortex-M33. `m_fixed.h` short-circuits
  the function call to a `static inline` that GCC lowers to a single
  `smull` + shift; eliminates ~160k function calls per frame in the
  plane drawer alone.
- **Patch decode early-out**. `decode_patch_column` takes a
  `max_row` cap. Wall, sprite, and composite paths all compute the
  highest row their per-pixel sample loop will touch and pass it in;
  typical slivers skip 80-90% of the Huffman decode work.
- **Per-column plane K cache**. `pd_add_plane_column` caches
  `cos(angle(x)) * dscale(x)` and `sin(angle(x)) * dscale(x)` per
  column for the frame; multiple visplanes touching the same x
  reuse the cached value. Drops emission setup from 2 LUT lookups
  + 3 muls to 2 muls.
- **Sky pre-decode cache**. `paint_sky_column` decodes each sky
  column once on first use into a 256x128 = 32 KB cache, then
  serves subsequent frames as a memcpy-style index lookup. Stable
  scenes pay zero Huffman decode for sky after warmup.
- **`-O2` on the render hot path**. `pd_stubs.c`, `m_fixed.c`, and
  `r_*.c` build with `-O2` instead of the project default `-Os`.
- **LTO** across the whole build. Cross-TU inlining + dead-code
  elimination; text actually shrinks because the linker can drop
  unused config-flag dead branches across the engine.
- **8-slot flat LRU** (was 4). Eliminates eviction churn in scenes
  with many unique flats.

Skipped (intentional):

- **SPI 80 MHz overclock**. `SYSCLK / 2 = 80 MHz` exceeds the
  ST7789V2 datasheet's 62.5 MHz max. Render is the binding
  constraint above the SPI floor of 19 ms in heavy scenes anyway,
  so the win would only show in lighter scenes already at 35 Hz.
  Worth revisiting if the heavy-scene render time ever drops below
  the SPI floor.

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

Audio takes one extra pin off the mikroBus connector:

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
      i_video_stm32.c       RGB565 ping-pong framebuffer, I_SetPalette,
                            I_SetPaletteNum, I_FinishUpdate (320 -> 240
                            column drop, SPI blit)
      i_input_stm32.c       SW2/SW4 buttons plus QwSTPad / seesaw joystick
      i_sound_stm32.c       SFX mixer + DAC1/PA4 output via TIM6
                            update IRQ at 11025 Hz
      i_music_stm32.c       OPL2 music driver feeding the SFX mixer
      i_timer_stm32.c       SysTick @ 1 kHz -> TICRATE
      i_system_stm32.c      I_Error / I_Quit / panic / I_ZoneBase
                            (256 KB static zone), I_Realloc, etc.
      pd_stubs.c            stand-in for pd_render.cpp: WHD splash
                            decoder for GS_DEMOSCREEN, textured
                            wall / plane / sprite painters, melt
                            wipe, sprite + psprite drain queues
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

## Open work items

- **Floor sky**. `F_SKY1` ceilings dispatch through the sky texture
  cache, but `F_SKY1` floors still draw as a flat silhouette.
- **Framebuffer clear validation**. The default build skips the old
  defensive 96 KB level-frame clear (`CLEAR_LEVEL_FRAMEBUFFER=0`) to
  save a few milliseconds per frame. If hardware testing shows stale
  pixels, rebuild with `make CLEAR_LEVEL_FRAMEBUFFER=1`.
- **More FPS in heavy scenes**. Typical scenes hit the 35 Hz cap;
  open areas with many sprites / visplanes still drop to ~12-15
  FPS. The remaining lever the Perf section calls out is
  overclocking SPI to 80 MHz (`SYSCLK / 2`, exceeds the ST7789V2
  spec of 62.5 MHz but often tolerated), worthwhile only once the
  heavy-scene render time drops below the SPI floor.

## License

This project is GPL-2.0-or-later. See [LICENSE](LICENSE) for the
full text.

The `doom/` submodule and `wad/doom1.whx` are GPL-2.0+
(id Software / Chocolate Doom / Graham Sanderson) and dictate the
licensing of the whole derivative work. Vendored CMSIS headers
under `cmsis/` are Apache 2.0 (ST / Arm), which is GPL-compatible.
The wolfDemo-specific glue (`Makefile`, `STM32U585CITX_FLASH.ld`,
`startup/`, `src/`) is GPL-2.0-or-later.
