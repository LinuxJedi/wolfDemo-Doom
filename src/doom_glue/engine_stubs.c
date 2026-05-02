/*
 * Small symbols the engine references that don't have a natural home
 * in any other glue file. Bundled here to keep the file count down.
 */

#include <stdint.h>
#include "doomtype.h"
#include "i_sound.h"
#include "tiny_huff.h"

/* Variable-bind callbacks. The upstream definitions in i_sound.c /
 * i_input.c register console variables for the SDL build; we have
 * neither variables nor a console, so these are no-ops. */
void I_BindSoundVariables(void)  { }
void I_BindInputVariables(void)  { }

/* Sound config and state. snd_pitchshift / usegamma are normally
 * driven by the .cfg loader (which is disabled by NO_USE_SAVE_CONFIG=1)
 * so we just give them defaults. */
isb_int8_t snd_pitchshift = 0;
isb_int8_t usegamma       = 0;

/* I_GetSfxLumpNum lives in i_sound_stm32.c (Phase 8). */

/* Haptic feedback. We have no rumble motor. */
void I_Tactile(int on, int off, int total)
{
    (void)on; (void)off; (void)total;
}

/* tiny_huff.h declares th_bit_overrun as "global and user-provided".
 * It's called when the bitstream reader walks off the end of the
 * compressed lump -- a corrupted-WAD signal. Halt the same way as
 * I_Error so the user sees the failure on UART. */
void th_bit_overrun(th_bit_input *bi)
{
    (void)bi;
    panic("tiny_huff: bit-stream overrun");
}
