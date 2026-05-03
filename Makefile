# Makefile for the wolfDemo Doom port (STM32U585CIT6).
#
# Default target builds the bare-metal firmware skeleton.
# `make flash`     -> stm32flash over USART1 (USB-UART bridge)
# `make flash-swd` -> openocd over SWD (Tag-Connect)

PROJECT := wolfDemo-doom

CROSS    := arm-none-eabi-
CC       := $(CROSS)gcc
AS       := $(CROSS)gcc -x assembler-with-cpp
LD       := $(CROSS)gcc
OBJCOPY  := $(CROSS)objcopy
SIZE     := $(CROSS)size

BUILD_DIR := build

# ---- Source layout ---------------------------------------------------
C_SRCS := \
    src/main.c \
    src/clock.c \
    src/uart.c \
    src/spi.c \
    src/st7789.c \
    src/syscalls.c \
    src/system_stm32u5xx.c \
    src/doom_glue/i_video_stm32.c \
    src/doom_glue/i_input_stm32.c \
    src/doom_glue/i_sound_stm32.c \
    src/doom_glue/i_timer_stm32.c \
    src/doom_glue/i_system_stm32.c \
    src/doom_glue/pd_stubs.c \
    src/doom_glue/engine_stubs.c

# Engine sources from rp2040-doom. We add files incrementally as the
# build link errors converge.
DOOM_SRCS := \
    doom/src/m_misc.c \
    doom/src/m_argv.c \
    doom/src/m_bbox.c \
    doom/src/m_fixed.c \
    doom/src/sha1.c \
    doom/src/tables.c \
    doom/src/z_zone.c \
    doom/src/memio.c \
    doom/src/w_file.c \
    doom/src/w_file_memory.c \
    doom/src/w_wad.c \
    doom/src/w_main.c \
    doom/src/w_checksum.c \
    doom/src/w_merge.c \
    doom/src/tiny_huff.c \
    doom/src/image_decoder.c \
    doom/src/musx_decoder.c \
    doom/src/aes_prng.c \
    doom/src/d_event.c \
    doom/src/d_iwad.c \
    doom/src/d_loop.c \
    doom/src/d_mode.c \
    doom/src/deh_str.c \
    doom/src/gusconf.c \
    doom/src/midifile.c \
    doom/src/mus2mid.c \
    doom/src/m_cheat.c \
    doom/src/m_config.c \
    doom/src/m_controls.c \
    doom/src/v_diskicon.c \
    doom/src/v_video.c \
    doom/src/doom/am_map.c \
    doom/src/doom/d_items.c \
    doom/src/doom/d_main.c \
    doom/src/doom/d_net.c \
    doom/src/doom/doomdef.c \
    doom/src/doom/doomstat.c \
    doom/src/doom/dstrings.c \
    doom/src/doom/f_finale.c \
    doom/src/doom/f_wipe.c \
    doom/src/doom/g_game.c \
    doom/src/doom/hu_lib.c \
    doom/src/doom/hu_stuff.c \
    doom/src/doom/info.c \
    doom/src/doom/m_menu.c \
    doom/src/doom/m_random.c \
    doom/src/doom/p_ceilng.c \
    doom/src/doom/p_doors.c \
    doom/src/doom/p_enemy.c \
    doom/src/doom/p_floor.c \
    doom/src/doom/p_inter.c \
    doom/src/doom/p_lights.c \
    doom/src/doom/p_map.c \
    doom/src/doom/p_maputl.c \
    doom/src/doom/p_mobj.c \
    doom/src/doom/p_plats.c \
    doom/src/doom/p_pspr.c \
    doom/src/doom/p_saveg.c \
    doom/src/doom/p_setup.c \
    doom/src/doom/p_sight.c \
    doom/src/doom/p_spec.c \
    doom/src/doom/p_switch.c \
    doom/src/doom/p_telept.c \
    doom/src/doom/p_tick.c \
    doom/src/doom/p_user.c \
    doom/src/doom/r_bsp.c \
    doom/src/doom/r_data.c \
    doom/src/doom/r_data_whd.c \
    doom/src/doom/r_draw.c \
    doom/src/doom/r_main.c \
    doom/src/doom/r_plane.c \
    doom/src/doom/r_segs.c \
    doom/src/doom/r_sky.c \
    doom/src/doom/r_things.c \
    doom/src/doom/s_sound.c \
    doom/src/doom/sounds.c \
    doom/src/doom/st_lib.c \
    doom/src/doom/st_stuff.c \
    doom/src/doom/statdump.c \
    doom/src/doom/wi_stuff.c

C_SRCS += $(DOOM_SRCS)

ASM_SRCS := startup/startup_stm32u585citx.s

# WHD-compressed WAD blob is wrapped into a relocatable object via objcopy
# and dropped into the .wad section in flash.
WAD_BIN := wad/doom1.whx
WAD_OBJ := $(BUILD_DIR)/wad.o

INCLUDES := \
    -Isrc \
    -Isrc/doom_glue \
    -Idoom/src \
    -Idoom/src/doom \
    -Idoom/opl \
    -Icmsis/Include \
    -Icmsis/Device/ST/STM32U5xx/Include

LDSCRIPT := STM32U585CITX_FLASH.ld

# ---- Toolchain flags -------------------------------------------------
CPU := -mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16

DEFINES := -DSTM32U585xx -DHSE_VALUE=8000000U

# ---- Doom engine compile-time configuration --------------------------
# Mirrors the doom_tiny + small_doom_common + tiny_settings target_compile_
# definitions blocks in doom/src/CMakeLists.txt. Applied to all C sources;
# the engine reads them, our glue/HAL files mostly ignore them.
DOOM_DEFINES := \
    -DDOOM_TINY=1 \
    -DDOOM_ONLY=1 \
    -DDOOM_SMALL=1 \
    -DDOOM_CONST=1 \
    -DPICO_DOOM=1 \
    -DUSE_WHD=1 \
    -DUSE_MEMORY_WAD=1 \
    -DDEMO1_ONLY=1 \
    -DWHD_SUPER_TINY=1 \
    -DUSE_SINGLE_IWAD=1 \
    -DSHRINK_MOBJ=1 \
    -DSOUND_LOW_PASS=1 \
    -DNUM_SOUND_CHANNELS=8 \
    -DUSE_FLAT_MAX_256=1 \
    -DUSE_MEMMAP_ONLY=1 \
    -DUSE_LIGHTMAP_INDEXES=1 \
    -DUSE_ERASE_FRAME=1 \
    -DUSE_RAW_MAPNODE=1 \
    -DUSE_RAW_MAPVERTEX=1 \
    -DUSE_RAW_MAPSEG=1 \
    -DUSE_RAW_MAPLINEDEF=1 \
    -DUSE_RAW_MAPTHING=1 \
    -DUSE_INDEX_LINEBUFFER=1 \
    -DUSE_THINKER_POOL=1 \
    -DUSE_CONST_SFX=1 \
    -DUSE_CONST_MUSIC=1 \
    -DUSE_VANILLA_KEYBOARD_MAPPING_ONLY=1 \
    -DUSE_DIRECT_MIDI_LUMP=1 \
    -DUSE_FPS=1 \
    -DUSE_MUSX=1 \
    -DMUSX_COMPRESSED=1 \
    -DSAVE_COMPRESSED=1 \
    -DLOAD_COMPRESSED=1 \
    -DFIXED_SCREENWIDTH=1 \
    -DPD_DRAW_COLUMNS=1 \
    -DPD_DRAW_MARKERS=1 \
    -DPD_DRAW_PLANES=1 \
    -DPD_SCALE_SORT=1 \
    -DPD_CLIP_WALLS=1 \
    -DPD_QUANTIZE=1 \
    -DPD_SANITY=1 \
    -DPD_COLUMNS=1 \
    -DNO_DRAW_MID=1 \
    -DNO_DRAW_TOP=1 \
    -DNO_DRAW_BOTTOM=1 \
    -DNO_DRAW_MASKED=1 \
    -DNO_DRAW_SKY=1 \
    -DNO_DRAW_PSPRITES=1 \
    -DNO_VISPLANE_GUTS=1 \
    -DNO_VISPLANE_CACHES=1 \
    -DNO_USE_DS_COLORMAP=1 \
    -DNO_USE_DC_COLORMAP=1 \
    -DNO_USE_ZLIGHT=1 \
    -DNO_Z_ZONE_ID=1 \
    -DNO_USE_NET=1 \
    -DNO_USE_SAVE=1 \
    -DNO_USE_LOAD=1 \
    -DNO_USE_ARGS=1 \
    -DNO_USE_DEH=1 \
    -DNO_USE_FLOAT=1 \
    -DNO_FILE_ACCESS=1 \
    -DNO_IERROR=1 \
    -DNO_USE_MUSIC_PACKS=1 \
    -DNO_USE_TIMIDITY=1 \
    -DNO_USE_GUS=1 \
    -DNO_USE_LIBSAMPLERATE=1 \
    -DNO_USE_JOYSTICK=1 \
    -DNO_USE_SAVE_CONFIG=1 \
    -DNO_USE_BOUND_CONFIG=1 \
    -DNO_USE_RELOAD=1 \
    -DNO_USE_CHECKSUM=1 \
    -DNO_USE_FINALE_CAST=1 \
    -DNO_USE_FINALE_BUNNY=1 \
    -DNO_USE_LOADING_DISK=1 \
    -DNO_USE_WIPE=1 \
    -DNO_USE_EXIT=1 \
    -DNO_USE_STATE_MISC=1 \
    -DNO_INTERCEPTS_OVERRUN=1 \
    -DNO_RDRAW=1 \
    -DNO_SCREENSHOT=1 \
    -DNO_DEMO_RECORDING=1 \
    -DNO_ZONE_DEBUG=1 \
    -DNO_Z_MALLOC_USER_PTR=1 \
    -DTEMP_IMMUTABLE_DISABLED=1 \
    -DPICO_NO_TIMING_DEMO=1 \
    -DZ_MALOOC_EXTRA_DATA=1

CFLAGS  := $(CPU) $(DEFINES) $(DOOM_DEFINES) $(INCLUDES) \
           -include src/doom_glue/net_client.h \
           -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
           -Wno-unused-variable -Wno-unused-but-set-variable \
           -Wno-sign-compare -Wno-format-truncation \
           -Wno-missing-field-initializers \
           -ffunction-sections -fdata-sections \
           -fno-common \
           -flto \
           -std=gnu11 -Os -g3

ASFLAGS := $(CPU) -Wall -g3

LDFLAGS := $(CPU) -T$(LDSCRIPT) \
           -Wl,--gc-sections \
           -Wl,-Map=$(BUILD_DIR)/$(PROJECT).map \
           --specs=nano.specs --specs=nosys.specs \
           -flto \
           -u _printf_float

LIBS := -lc -lm -lnosys

# ---- Build rules -----------------------------------------------------
OBJS := $(addprefix $(BUILD_DIR)/,$(C_SRCS:.c=.o)) \
        $(addprefix $(BUILD_DIR)/,$(ASM_SRCS:.s=.o)) \
        $(WAD_OBJ)

ELF := $(BUILD_DIR)/$(PROJECT).elf
BIN := $(BUILD_DIR)/$(PROJECT).bin
HEX := $(BUILD_DIR)/$(PROJECT).hex

.PHONY: all clean flash flash-swd size

all: $(BIN) $(HEX) size

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.s
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -c $< -o $@

# Render hot path: build with -O2 instead of -Os. The default size
# optimization avoids unrolling and aggressive scheduling that the
# per-pixel loops in pd_stubs and r_*.c need to hit the 35 Hz target.
HOT_CFLAGS := $(filter-out -Os,$(CFLAGS)) -O2

HOT_C_SRCS := \
    src/doom_glue/pd_stubs.c \
    doom/src/m_fixed.c \
    doom/src/doom/r_draw.c \
    doom/src/doom/r_main.c \
    doom/src/doom/r_plane.c \
    doom/src/doom/r_segs.c \
    doom/src/doom/r_things.c \
    doom/src/doom/r_bsp.c

HOT_OBJS := $(addprefix $(BUILD_DIR)/,$(HOT_C_SRCS:.c=.o))

$(HOT_OBJS): $(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(HOT_CFLAGS) -c $< -o $@

# Wrap the WAD blob into an ELF object whose data lives in section .wad.
# The objcopy --rename-section moves the default .data into our linker's
# .wad output section. Symbols _binary_*_start/_end are defined automatically.
$(WAD_OBJ): $(WAD_BIN)
	@mkdir -p $(dir $@)
	$(OBJCOPY) -I binary -O elf32-littlearm -B arm \
	    --rename-section .data=.wad,alloc,load,readonly,data,contents \
	    $< $@

$(ELF): $(OBJS) $(LDSCRIPT)
	$(LD) $(OBJS) $(LDFLAGS) $(LIBS) -o $@

$(BIN): $(ELF)
	$(OBJCOPY) -O binary $< $@

$(HEX): $(ELF)
	$(OBJCOPY) -O ihex $< $@

size: $(ELF)
	@$(SIZE) -A -d $(ELF) | head -20
	@$(SIZE) $(ELF)

clean:
	rm -rf $(BUILD_DIR)

# ---- Flashing --------------------------------------------------------
# Default tty - override with `make flash TTY=/dev/...`. The board exposes
# its FT231X-equivalent USB-UART bridge as /dev/cu.usbserial-* on macOS.
TTY ?= $(shell ls /dev/cu.usbserial-* 2>/dev/null | head -1)
BAUD ?= 921600

flash: $(BIN)
	@if [ -z "$(TTY)" ]; then \
	    echo "no usbserial tty found; pass TTY=/dev/cu.usbserial-XXXX"; exit 1; fi
	@echo "Flashing $(BIN) to $(TTY) via stm32flash."
	@echo "Hold BOOT0, tap RESET, release BOOT0 before this command runs."
	stm32flash -b $(BAUD) -w $(BIN) -v -g 0x08000000 $(TTY)

flash-swd: $(ELF)
	openocd -f interface/stlink.cfg -f target/stm32u5x.cfg \
	    -c "program $(ELF) verify reset exit"

monitor:
	@if [ -z "$(TTY)" ]; then \
	    echo "no usbserial tty found; pass TTY=/dev/cu.usbserial-XXXX"; exit 1; fi
	@echo "Opening $(TTY) at $(BAUD); ctrl-A k to quit screen."
	screen $(TTY) $(BAUD)
