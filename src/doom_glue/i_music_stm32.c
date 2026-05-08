/*
 * OPL2 music driver for the wolfDemo board.
 *
 * Replaces opl_pico.c: provides the OPL_* low-level API that
 * doom/src/i_oplmusic.c calls (WriteRegister / SetCallback / etc.),
 * and implements the I_*Music engine entry points by delegating to
 * music_opl_module (the music_module_t exported by i_oplmusic.c).
 *
 * Audio output goes through the existing TIM6/DAC1 path in
 * i_sound_stm32.c. Each SFX mixer chunk-refill calls
 * music_render_chunk() which produces N OPL samples and services any
 * MIDI callback events whose deadline lies within the chunk's time
 * window. The OPL emulator runs at our 11025 Hz output rate
 * (OPL3_Reset configures rateratio internally to handle the 49716 Hz
 * native -> 11025 Hz output decimation via linear interpolation).
 *
 * Threading: music_render_chunk runs in TIM6 ISR context (same as
 * the SFX mixer's mix_chunk). All other entry points (I_InitMusic,
 * I_RegisterSong, I_PlaySong, ...) run in main context. The shared
 * state is the callback queue and opl3_chip; we don't take locks
 * because the ISR is single-priority and main-context paths happen
 * between map loads / pause toggles where no playback ISR work is
 * pending. If a torn read ever shows up in practice, gate the ISR
 * with a brief BASEPRI raise around main-context updates.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

#include "opl.h"
#include "opl_queue.h"
#include "emu8950.h"

/* Mono-int32 batch generator. Not in emu8950.h's public surface but
 * implemented at file scope in emu8950.c (line 1727). Skips the
 * duplicate-then-strip pass that OPL_calc_buffer_stereo does for us. */
extern void OPL_calc_buffer_linear(OPL *opl, int32_t *buffer, uint32_t nsamples);

/* Native OPL2 chip clock. emu8950 with EMU8950_NO_RATECONV must run
 * with output rate == clock/72 == 49716 Hz, so we generate music at
 * the native rate then decimate to MUSIC_SAMPLE_RATE in our render
 * loop. The decimation is a simple skip - aliasing is mild because
 * Doom OPL music's spectral energy sits below 5 kHz. */
#define MUSIC_OPL_CLK     3579545u
#define MUSIC_NATIVE_RATE 49716u
#define MUSIC_SAMPLE_RATE 11025u

/* Shared state. */
static OPL                  *emu8950_opl;
static opl_callback_queue_t *callback_queue;
static uint64_t             current_time_us;
static uint64_t             pause_offset_us;
static int                  opl_paused;
static int                  music_initialized;
static int                  music_volume_pct = 100;
static int                  music_muted;        /* SW4 mute flag; see I_EnableMusic */

/* Engine-side externs that i_oplmusic.c reads. snd_samplerate is
 * referenced by I_OPL_InitMusic (line 1808 of i_oplmusic.c) to call
 * OPL_SetSampleRate. We pin it to our output rate. */
int snd_samplerate = MUSIC_SAMPLE_RATE;

/* OPL-side state. opl_io_port is defined inside i_oplmusic.c
 * (isb_int16_t opl_io_port = 0x388) - we don't need our own. */

/* The currently-pending OPL register being written via the legacy
 * port-based OPL_WritePort API (we don't expose that, but
 * OPL_InitRegisters might still touch it indirectly via OPL_Detect).
 * Only the high-level OPL_WriteRegister / OPL_SetCallback paths are
 * actually exercised by i_oplmusic.c. */

/* --- OPL low-level API (consumed by i_oplmusic.c) ---------------- */

opl_init_result_t OPL_Init(unsigned int port_base)
{
    (void)port_base;  /* we have no real OPL hardware */
    if (emu8950_opl == NULL) {
        emu8950_opl = OPL_new(MUSIC_OPL_CLK, MUSIC_NATIVE_RATE);
        if (emu8950_opl == NULL) return OPL_INIT_NONE;
    } else {
        OPL_reset(emu8950_opl);
    }
    if (callback_queue == NULL) {
        callback_queue = OPL_Queue_Create();
    } else {
        OPL_Queue_Clear(callback_queue);
    }
    current_time_us = 0;
    pause_offset_us = 0;
    opl_paused = 0;
    return OPL_INIT_OPL2;
}

void OPL_Shutdown(void)
{
    if (callback_queue != NULL) {
        OPL_Queue_Clear(callback_queue);
    }
}

void OPL_SetSampleRate(unsigned int rate)
{
    /* Engine sets this from snd_samplerate before OPL_Init. We pin
     * to our hardware rate; ignore. */
    (void)rate;
}

void OPL_WritePort(opl_port_t port, unsigned int value)
{
    /* Not used by i_oplmusic.c (it goes through OPL_WriteRegister). */
    (void)port; (void)value;
}

unsigned int OPL_ReadPort(opl_port_t port)
{
    (void)port;
    return 0;
}

unsigned int OPL_ReadStatus(void)
{
    return 0;
}

void OPL_WriteRegister(int reg, int value)
{
    if (emu8950_opl) OPL_writeReg(emu8950_opl, (uint32_t)reg, (uint8_t)value);
}

opl_init_result_t OPL_Detect(void)
{
    return OPL_INIT_OPL2;
}

void OPL_InitRegisters(int opl3)
{
    /* Mirror opl_api.c:OPL_InitRegisters minimal init: silence all
     * voices, set carrier/modulator levels to zero, disable rhythm,
     * waveform select enable. i_oplmusic.c does its own voice setup
     * in I_OPL_InitMusic so this is mostly a safety reset. */
    int i;
    for (i = 0; i < OPL_NUM_VOICES; i++) {
        OPL_WriteRegister(OPL_REGS_FREQ_2 + i, 0);  /* key off */
    }
    for (i = 0; i < OPL_NUM_OPERATORS; i++) {
        OPL_WriteRegister(OPL_REGS_LEVEL + i, 0x3f);  /* max attenuation */
        OPL_WriteRegister(OPL_REGS_TREMOLO + i, 0);
        OPL_WriteRegister(OPL_REGS_ATTACK + i, 0);
        OPL_WriteRegister(OPL_REGS_SUSTAIN + i, 0);
        OPL_WriteRegister(OPL_REGS_WAVEFORM + i, 0);
    }
    OPL_WriteRegister(OPL_REG_FM_MODE, 0);
    OPL_WriteRegister(OPL_REG_WAVEFORM_ENABLE, 0x20);
    if (opl3) {
        OPL_WriteRegister(OPL_REG_NEW, 1);
    }
}

void OPL_SetCallback(uint64_t us, opl_callback_t callback, void *data)
{
    OPL_Queue_Push(callback_queue, callback, data,
                   current_time_us - pause_offset_us + us);
}

void OPL_ClearCallbacks(void)
{
    OPL_Queue_Clear(callback_queue);
}

void OPL_AdjustCallbacks(unsigned int old_tempo, unsigned int new_tempo)
{
    OPL_Queue_AdjustCallbacks(callback_queue, current_time_us,
                              old_tempo, new_tempo);
}

void OPL_Lock(void)   { /* single-priority ISR; no real lock needed */ }
void OPL_Unlock(void) { }

void OPL_SetPaused(int paused)
{
    opl_paused = paused;
}

void OPL_Delay(uint64_t us)
{
    /* i_oplmusic.c never calls this in our build; safe stub. */
    (void)us;
}

/* --- Render path (called by SFX mixer's chunk refill) ------------ */

/*
 * Generate `n` mono OPL samples into `out` (16-bit signed, native
 * range), advancing the music timeline and dispatching any MIDI
 * callbacks whose deadlines fall inside the chunk window. Safe to
 * call from TIM6 ISR context.
 *
 * If music is uninitialised or a song isn't loaded, fills `out` with
 * silence and returns - the SFX mixer can blend zeros in cheaply.
 */
/* Native-rate scratch buffer. emu8950's LINEAR-mode OPL_calc_buffer_stereo
 * writes int32 samples (raw left/right packed); we take the low 16 bits
 * as mono. emu8950's internal lfo_am_buffer is fixed at SAMPLE_BUF_SIZE
 * (1024) native samples per call, so OUTPUT_BATCH_MAX is capped at
 * 224 output samples (224 * 49716/11025 = 1010 native, safely under
 * the limit). The outer music_render_chunk loop iterates as many inner
 * batches as needed for the requested n. */
#define OUTPUT_BATCH_MAX 224
#define NATIVE_BATCH_MAX 1024
static int32_t emu_scratch[NATIVE_BATCH_MAX];

void music_render_chunk(int16_t *out, int n)
{
    if (!music_initialized) {
        memset(out, 0, n * sizeof(int16_t));
        return;
    }

    int filled = 0;

    while (filled < n) {
        int chunk = n - filled;
        if (chunk > OUTPUT_BATCH_MAX) chunk = OUTPUT_BATCH_MAX;

        /* Limit this slice to the time of the next pending callback. */
        if (!opl_paused && !OPL_Queue_IsEmpty(callback_queue)) {
            uint64_t next_t = OPL_Queue_Peek(callback_queue) + pause_offset_us;
            if (next_t > current_time_us) {
                uint64_t until_us = next_t - current_time_us;
                uint64_t until_samples =
                    (until_us * MUSIC_SAMPLE_RATE) / 1000000ull;
                if (until_samples == 0) until_samples = 1;
                if (until_samples < (uint64_t)chunk) chunk = (int)until_samples;
            } else {
                chunk = 1;
            }
        }

        if (music_muted) {
            /* User muted music via SW4. Skip the per-sample emu8950
             * call entirely (saves the bulk of the music CPU cost)
             * and emit silence. Time still advances below; OPL_paused
             * causes pause_offset_us to absorb it so MIDI callback
             * deadlines aren't backed up when the user un-mutes. */
            memset(out + filled, 0, chunk * sizeof(int16_t));
        } else {
            /* Map output samples to native samples: native = output *
             * (49716/11025) = output * 4.5089... The integer ratio is
             * close enough; sample-rate drift accumulates slowly over
             * a track but doesn't cause perceptible pitch error in
             * chiptune-style OPL music. */
            int n_native = (chunk * MUSIC_NATIVE_RATE) / MUSIC_SAMPLE_RATE;
            if (n_native < 1) n_native = 1;
            if (n_native > NATIVE_BATCH_MAX) n_native = NATIVE_BATCH_MAX;

            OPL_calc_buffer_linear(emu8950_opl, emu_scratch, (uint32_t)n_native);

            /* Decimate native -> output via Bresenham-style accumulator
             * (n_native steps of n_native into chunk steps of chunk).
             * Avoids a per-sample integer divide; aliasing is mild because
             * Doom OPL music spectrum sits well below the decimation
             * Nyquist (~5.5 kHz). */
            int idx = 0, err = 0;
            for (int i = 0; i < chunk; i++) {
                out[filled + i] = (int16_t)(emu_scratch[idx] & 0xFFFF);
                err += n_native;
                while (err >= chunk) {
                    err -= chunk;
                    idx++;
                }
            }
        }
        filled += chunk;

        /* Advance time and drain due callbacks. */
        uint64_t us_advanced =
            ((uint64_t)chunk * 1000000ull) / MUSIC_SAMPLE_RATE;
        current_time_us += us_advanced;
        if (opl_paused) pause_offset_us += us_advanced;

        while (!OPL_Queue_IsEmpty(callback_queue) &&
               current_time_us >=
                   OPL_Queue_Peek(callback_queue) + pause_offset_us) {
            opl_callback_t cb;
            void *data;
            if (!OPL_Queue_Pop(callback_queue, &cb, &data)) break;
            cb(data);
        }
    }
}

/* --- I_*Music engine entry points -------------------------------- */

extern const music_module_t music_opl_module;

/* Runtime music enable. With opl3.c built at -O3 and the OPL2-mode
 * short-circuit skipping the 18 unused OPL3-only slots per sample,
 * the synth fits within the available CPU headroom alongside the
 * renderer. Music synth still runs in PendSV at the lowest priority
 * so it can be preempted by SPI-TC, SysTick, and per-sample TIM6
 * firings. */
static int music_enabled = 1;

/* SW4 mutes music via OPL_SetPaused (preserves song iterators in
 * i_oplmusic.c and the emulator voice/envelope state) and by
 * short-circuiting the per-sample emu8950 generator above in
 * music_render_chunk. The earlier I_EnableMusic(0) implementation
 * called music_opl_module.Shutdown() which destroyed num_tracks via
 * I_OPL_StopSong, leaving no path to bring music back on - that's
 * what made the toggle one-way. */
void I_EnableMusic(int enable)
{
    if (enable) {
        /* Initial bring-up if music was never started. */
        if (!music_initialized && music_opl_module.Init()) {
            music_initialized = 1;
        }
        music_muted = 0;
        if (music_initialized) OPL_SetPaused(0);
    } else {
        music_muted = 1;
        if (music_initialized) OPL_SetPaused(1);
    }
    music_enabled = enable ? 1 : 0;
}

void I_InitMusic(void)
{
    if (music_enabled && !music_initialized) {
        if (music_opl_module.Init()) {
            music_initialized = 1;
        }
    }
}

void I_ShutdownMusic(void)
{
    if (!music_initialized) return;
    music_opl_module.Shutdown();
    music_initialized = 0;
}

void I_PlaySong(void *handle, boolean looping)
{
    if (!music_initialized) return;
    music_opl_module.PlaySong(handle, looping);
}

void I_PauseSong(void)
{
    if (!music_initialized) return;
    music_opl_module.PauseMusic();
}

void I_ResumeSong(void)
{
    if (!music_initialized) return;
    music_opl_module.ResumeMusic();
}

void I_StopSong(void)
{
    if (!music_initialized) return;
    music_opl_module.StopSong();
}

void I_UnRegisterSong(void *handle)
{
    if (!music_initialized) return;
    music_opl_module.UnRegisterSong(handle);
}

void *I_RegisterSong(should_be_const void *data, int len)
{
    if (!music_initialized) return NULL;
    return music_opl_module.RegisterSong(data, len);
}

boolean I_MusicIsPlaying(void)
{
    if (!music_initialized) return false;
    return music_opl_module.MusicIsPlaying();
}

void I_SetMusicVolume(int volume)
{
    /* volume arrives 0..127 from S_SetMusicVolume. The OPL driver
     * scales internally; clamp and forward. */
    if (volume < 0) volume = 0;
    if (volume > 127) volume = 127;
    music_volume_pct = (volume * 100) / 127;
    if (!music_initialized) return;
    music_opl_module.SetMusicVolume(volume);
}
