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
#include "opl3.h"

#define MUSIC_SAMPLE_RATE 11025u

/* Shared state. */
static opl3_chip            opl_chip;
static opl_callback_queue_t *callback_queue;
static uint64_t             current_time_us;
static uint64_t             pause_offset_us;
static int                  opl_paused;
static int                  music_initialized;
static int                  music_volume_pct = 100;

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
    OPL3_Reset(&opl_chip, MUSIC_SAMPLE_RATE);
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
    OPL3_WriteRegBuffered(&opl_chip, (Bit16u)reg, (Bit8u)value);
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
void music_render_chunk(int16_t *out, int n)
{
    if (!music_initialized) {
        memset(out, 0, n * sizeof(int16_t));
        return;
    }

    int filled = 0;
    int16_t stereo[2];

    while (filled < n) {
        int chunk = n - filled;

        /* Limit this slice to the time of the next pending callback,
         * so callbacks fire close to their target time even within
         * one mixer chunk. */
        if (!opl_paused && !OPL_Queue_IsEmpty(callback_queue)) {
            uint64_t next_t = OPL_Queue_Peek(callback_queue) + pause_offset_us;
            if (next_t > current_time_us) {
                uint64_t until_us = next_t - current_time_us;
                /* (until_us * MUSIC_SAMPLE_RATE + 999999) / 1000000,
                 * but we want at least 1 sample so we always advance. */
                uint64_t until_samples =
                    (until_us * MUSIC_SAMPLE_RATE) / 1000000ull;
                if (until_samples == 0) until_samples = 1;
                if (until_samples < (uint64_t)chunk) chunk = (int)until_samples;
            } else {
                /* Already past due; service immediately. */
                chunk = 1;
            }
        }

        for (int i = 0; i < chunk; i++) {
            OPL3_GenerateResampled(&opl_chip, stereo);
            /* Doom music is mono; the OPL driver pans left=right
             * anyway for non-spatial sources, so just take left. */
            out[filled + i] = stereo[0];
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

/* Runtime music enable. The OPL2 synth + MIDI sequencer averages
 * ~50%+ CPU at 160 MHz once you account for the 4.5x decimation from
 * 49716 Hz native to 11025 Hz output, with 18 slot-generates per
 * native sample. Even with the music synth deferred to PendSV at
 * lowest priority, that leaves the main render loop too little
 * headroom to produce frames at a watchable rate. Default music OFF
 * until the synth is optimised (DSP intrinsics, lower internal rate,
 * or per-channel skip-when-silent). The OPL code stays linked so
 * the toggle is a one-bool flip. */
static int music_enabled = 0;

void I_EnableMusic(int enable)
{
    if (enable && !music_initialized) {
        if (music_opl_module.Init()) {
            music_initialized = 1;
        }
    } else if (!enable && music_initialized) {
        music_opl_module.Shutdown();
        music_initialized = 0;
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
