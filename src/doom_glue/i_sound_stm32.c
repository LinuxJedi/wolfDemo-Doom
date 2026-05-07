/*
 * STM32 sound glue for the Doom port.
 *
 * Provides Phase 8 SFX playback. Music remains stubbed - MUS / OPL
 * emulation is out of scope for this port.
 *
 * --- Output stage ----------------------------------------------------
 * DAC1 channel 1 on PA4 (analog mode, output buffer enabled, normal
 * mode) drives an external amplified speaker. TIM6 generates an
 * update interrupt at 11025 Hz; the TIM6_IRQHandler reads the next
 * sample from a 1024-sample (16-bit) circular ring buffer and writes
 * it to DAC1->DHR12R1. We use 12-bit mode (vs 8-bit) so that low Doom
 * volumes still have enough resolution to avoid audible quantization
 * grit; combined with the >>MIX_OUTPUT_SHIFT master attenuation below,
 * it brings the line-level swing down to a sane figure for an external
 * amp. When the read pointer crosses the half-buffer or full-buffer
 * boundary, the IRQ also runs the mixer to refill the consumed half.
 *
 * We landed on this IRQ-per-sample approach after multiple DMA
 * attempts ran into bus errors:
 *  - GPDMA1 (on AHB1) returned DTEF whenever its channel tried to
 *    write to DAC1 at 0x46021810: GPDMA1's two AHB master ports do
 *    not bridge into the AHB3 sub-bus where DAC1 lives.
 *  - LPDMA1 (on AHB3, co-resident with DAC1) ALSO returned DTEF on
 *    the first transfer attempt, with and without the channel
 *    promoted to secure (SECCFGR.SEC0 + CTR1.SSEC|DSEC) and with
 *    the source buffer placed in SRAM4 vs main SRAM. The bus
 *    rejected the access for some reason we couldn't pin down on
 *    this board.
 *
 * The CPU itself can write DAC1->DHR12R1 fine (we use it during
 * audio_hw_init to pre-load mid-rail), so an IRQ-driven path that
 * just writes one register per timer tick avoids the whole DMA
 * mystery. Cost: 11025 IRQs/sec * ~50 cycles ~= 550K cycles/s ~=
 * 0.34% of the 160 MHz core. Negligible compared to the renderer.
 *
 * Within the TIM6 IRQ:
 *   - Write ring_buf[ring_rd] to DAC.
 *   - Increment ring_rd (mod RING_SAMPLES).
 *   - If we just consumed sample HALF_SAMPLES-1, mix the next
 *     HALF_SAMPLES into ring_buf[0..HALF). At sample
 *     RING_SAMPLES-1, mix into ring_buf[HALF..RING) and toggle the
 *     LED4 heartbeat.
 *
 * The mixer chunk takes ~250 us; that's a few sample periods, so
 * the per-IRQ time spikes around chunk boundaries. As long as the
 * spike is shorter than HALF_SAMPLES * (1/11025 Hz) = 46 ms, no
 * sample is missed. (250 us << 46 ms by 200x.) The TIM6 IRQ runs
 * at default NVIC priority; since the SPI background blit ISR
 * also runs at default priority, we lose at most a few audio
 * samples to the worst-case SPI ISR latency, which is inaudible.
 *
 * --- Mixer -----------------------------------------------------------
 * Up to NUM_SOUND_CHANNELS (=8) channels mix simultaneously. Each
 * channel decompresses one ADPCM block at a time into a 249-byte
 * scratch (matching the WHD encoder block size); the mixer steps
 * through the block sample by sample with a fixed-point fractional
 * offset (channel->offset is a 16.16 index so playback rate can be
 * pitched up or down). Samples are signed 8-bit; per-channel volume
 * is 0..127. The mixer sums into an int32 accumulator, shifts right
 * by MIX_OUTPUT_SHIFT to scale into the 12-bit DAC range (with
 * built-in master attenuation), and stores uint16 = clamped + 0x800.
 *
 * The same single-pole IIR low-pass filter as i_picosound.c is
 * applied per-channel (alpha256 cached at StartSound time based on
 * the SFX source rate) to soften the upsampled aliasing. The IIR
 * state restarts at each chunk; with 11025 Hz output and ~46 ms
 * chunks the boundary discontinuity is inaudible.
 *
 * --- VU meter --------------------------------------------------------
 * After mixing each chunk, the DMA ISR scans the chunk for the peak
 * absolute amplitude (post-mix, post-scale) and buckets it into four
 * thresholds. LEDs PB12..PB15 light cumulatively: LED1 below the
 * lowest threshold means silence, LED4 lit means clipping. The peak
 * scan is a single linear pass over 512 bytes - well under 100 us
 * on the M33, fitting comfortably inside the chunk-period of 46 ms.
 *
 * --- Threading -------------------------------------------------------
 * I_StartSound / I_StopSound run from the main game thread; the
 * mixer runs from the TIM6 update IRQ. Channel state is
 * volatile and mutated atomically (decompressed_size = 0 to mark
 * stopped). The ISR reads volume/step/data fields without taking a
 * lock; a torn read produces at most one chunk of glitched audio,
 * which is acceptable for SFX. I_UpdateSound is a no-op since the
 * mixer is interrupt-driven.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>

#include "stm32u585xx.h"
#include "../board.h"

#include "doomtype.h"
#include "i_sound.h"
#include "deh_str.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"
#include "doom/sounds.h"

#define AUDIO_SAMPLE_RATE   11025u
#define HALF_SAMPLES        512
#define RING_SAMPLES        (HALF_SAMPLES * 2)

#define ADPCM_BLOCK_SIZE              128
#define ADPCM_SAMPLES_PER_BLOCK_SIZE  249
#define MIX_VOL_DIVISOR               4   /* (left+right)/4 -> 0..127 */
/* Output shift: scale the int32 accumulator into the 12-bit DAC range
 * with a master digital attenuator built in.
 *
 * Max single-channel mix = (sample max) * (vol max) = 127 * 127 ~= 16K.
 * Max 8-channel = ~128K. The 12-bit signed range is +-2048.
 *
 * - >>3 lands a single channel at full amplitude near the top of the
 *   12-bit range (16K/8 = 2K) - rail-to-rail, same loudness as the
 *   prior 8-bit setup.
 * - >>6 cuts that to ~12.5% of full scale (16K/64 = 256 -> ~0.4 V peak
 *   on a 3.3 V DAC), which is in line-level territory and well-matched
 *   to an external amplified speaker.
 *
 * Default >>6 trades absolute loudness for headroom; because the DAC
 * is now 12-bit, low Doom volumes still have ~16x more codes than
 * before so quantization grit is gone even with the digital attenuation.
 * Drop to >>5 for ~6 dB louder, raise to >>7 for ~6 dB quieter. */
#define MIX_OUTPUT_SHIFT              6
#define DAC_BIAS_12BIT                0x800u   /* 12-bit mid-rail */
#define DAC_PEAK_12BIT                0x7FFu   /* +-2047 around bias */

typedef struct channel_s {
    const uint8_t *data;        /* current ADPCM read pointer */
    const uint8_t *data_end;    /* one-past-last ADPCM byte */
    uint32_t       offset;      /* 16.16 index into decompressed[] */
    uint32_t       step;        /* 16.16 advance per output sample */
    uint8_t        left, right; /* per-channel volume 0..255 */
    uint16_t       alpha256;    /* IIR coefficient, 0..256 */
    volatile uint8_t decompressed_size;  /* 0 = stopped */
    int8_t         decompressed[ADPCM_SAMPLES_PER_BLOCK_SIZE];
} channel_t;

static channel_t channels[NUM_SOUND_CHANNELS];
static bool      sound_initialized;
static bool      use_sfx_prefix;

/* Sample ring buffer.
 *
 * The TIM6 IRQ reads sequentially; each chunk-boundary crossing
 * triggers a mix into the consumed half. ring_rd is incremented
 * inside the IRQ only - main-thread writers (I_StartSound etc.)
 * don't touch it. Samples are 12-bit unsigned (DAC code), biased at
 * DAC_BIAS_12BIT for silence. */
static uint16_t           ring_buf[RING_SAMPLES] __attribute__((aligned(4)));
static volatile uint16_t  ring_rd;

/* --- ADPCM decode (lifted from doom/src/pico/i_picosound.c) ------- */
static const uint16_t step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};
static const int8_t index_table[8] = {
    -1, -1, -1, -1, 2, 4, 6, 8
};

static int adpcm_decode_block_s8(int8_t *outbuf, const uint8_t *inbuf, int inbufsize)
{
    if (inbufsize < 4) return 0;

    int32_t pcmdata = (int16_t)(inbuf[0] | (inbuf[1] << 8));
    *outbuf++ = (int8_t)(pcmdata >> 8);
    int index = inbuf[2];
    if (index < 0 || index > 88 || inbuf[3]) return 0;

    inbufsize -= 4;
    inbuf     += 4;

    int chunks  = inbufsize / 4;
    int samples = 1 + chunks * 8;

    while (chunks--) {
        for (int i = 0; i < 4; ++i) {
            int step  = step_table[index];
            int delta = step >> 3;
            if (*inbuf & 1) delta += (step >> 2);
            if (*inbuf & 2) delta += (step >> 1);
            if (*inbuf & 4) delta +=  step;
            if (*inbuf & 8) delta = -delta;
            pcmdata += delta;
            index   += index_table[*inbuf & 0x7];
            if (index < 0)   index = 0;
            else if (index > 88) index = 88;
            if (pcmdata < -32768) pcmdata = -32768;
            else if (pcmdata > 32767) pcmdata = 32767;
            outbuf[i * 2] = (int8_t)(pcmdata >> 8);

            step  = step_table[index];
            delta = step >> 3;
            if (*inbuf & 0x10) delta += (step >> 2);
            if (*inbuf & 0x20) delta += (step >> 1);
            if (*inbuf & 0x40) delta +=  step;
            if (*inbuf & 0x80) delta = -delta;
            pcmdata += delta;
            index   += index_table[(*inbuf >> 4) & 0x7];
            if (index < 0)   index = 0;
            else if (index > 88) index = 88;
            if (pcmdata < -32768) pcmdata = -32768;
            else if (pcmdata > 32767) pcmdata = 32767;
            outbuf[i * 2 + 1] = (int8_t)(pcmdata >> 8);
            inbuf++;
        }
        outbuf += 8;
    }
    return samples;
}

static void decompress_buffer(channel_t *ch)
{
    if (ch->data == ch->data_end) {
        ch->decompressed_size = 0;
        return;
    }
    int avail = ch->data_end - ch->data;
    int block = (avail < ADPCM_BLOCK_SIZE) ? avail : ADPCM_BLOCK_SIZE;
    int n = adpcm_decode_block_s8(ch->decompressed, ch->data, block);
    ch->data += block;
    if (n <= 0 || n > ADPCM_SAMPLES_PER_BLOCK_SIZE) {
        ch->decompressed_size = 0;
        return;
    }
    ch->decompressed_size = (uint8_t)n;
}

/* --- VU meter ------------------------------------------------------ */
/* Four-bucket VU meter on LED1..LED4 (cumulative). Each chunk's peak
 * abs amplitude (post-shift, pre-clamp; range 0..2047 in the 12-bit
 * domain) is bucketed into four thresholds and the LEDs light
 * progressively with louder mixes. Silence = no LEDs lit. The
 * thresholds are 2x the prior 8-bit values so that a single channel
 * at full volume still lights LED4 (single-channel peak with the
 * default >>6 master shift = ~256, so 144 lights LED4). */
static void vu_set(int peak)
{
    uint32_t bsrr = 0;
    int leds_lit = 0;
    if (peak >=   6) leds_lit = 1;
    if (peak >=  24) leds_lit = 2;
    if (peak >=  64) leds_lit = 3;
    if (peak >= 144) leds_lit = 4;
    for (int i = 0; i < 4; i++) {
        int pin = LED1_PIN + i;
        if (i < leds_lit) bsrr |= (1u << pin);
        else              bsrr |= (1u << (pin + 16));
    }
    LED_PORT->BSRR = bsrr;
}

/* --- Mixer ---------------------------------------------------------- */
static void mix_chunk(uint16_t *out, int n)
{
    int32_t mix_buf[HALF_SAMPLES];
    for (int s = 0; s < n; s++) mix_buf[s] = 0;

    for (int ci = 0; ci < NUM_SOUND_CHANNELS; ci++) {
        channel_t *ch = &channels[ci];
        if (ch->decompressed_size == 0) continue;

        int vol = (ch->left + ch->right) / MIX_VOL_DIVISOR;  /* 0..127 */
        if (vol == 0) continue;

        uint32_t offset     = ch->offset;
        uint32_t step       = ch->step;
        uint32_t offset_end = (uint32_t)ch->decompressed_size << 16;
        if (offset >= offset_end) { ch->decompressed_size = 0; continue; }

        int alpha256 = ch->alpha256;
        int beta256  = 256 - alpha256;
        int sample   = ch->decompressed[offset >> 16];

        for (int s = 0; s < n; s++) {
            int cur = ch->decompressed[offset >> 16];
            sample = (beta256 * sample + alpha256 * cur) >> 8;
            mix_buf[s] += sample * vol;
            offset += step;
            if (offset >= offset_end) {
                offset -= offset_end;
                ch->offset = offset;
                decompress_buffer(ch);
                if (ch->decompressed_size == 0) goto next_channel;
                offset_end = (uint32_t)ch->decompressed_size << 16;
                if (offset >= offset_end) {
                    ch->decompressed_size = 0;
                    goto next_channel;
                }
            }
        }
        ch->offset = offset;
    next_channel: ;
    }

    int peak = 0;
    for (int s = 0; s < n; s++) {
        int v = mix_buf[s] >> MIX_OUTPUT_SHIFT;
        int a = v < 0 ? -v : v;
        if (a > peak) peak = a;
        if (v < -(int)(DAC_PEAK_12BIT + 1)) v = -(int)(DAC_PEAK_12BIT + 1);
        else if (v > (int)DAC_PEAK_12BIT)   v =  (int)DAC_PEAK_12BIT;
        out[s] = (uint16_t)(v + (int)DAC_BIAS_12BIT);
    }
    vu_set(peak);
}

/* --- Hardware setup ------------------------------------------------- */
static void audio_hw_init(void)
{
    /* PA4 -> analog mode (DAC1_OUT1). */
    RCC->AHB2ENR1 |= RCC_AHB2ENR1_GPIOAEN;
    (void)RCC->AHB2ENR1;
    GPIOA->MODER  |=  (3u << (4 * 2));   /* MODER4 = 11 (analog) */
    GPIOA->PUPDR  &= ~(3u << (4 * 2));   /* no pull */

    /* DAC1 kernel clock source: select HCLK via RCC_CCIPR3.ADCDACSEL.
     * On STM32U5, peripherals with analog blocks (ADC, DAC, comparators)
     * have a kernel clock selector independent of the AHB enable bit.
     * The DAC won't actually convert without a kernel clock even though
     * the AHB3 enable lets us write to registers. Same gotcha that bit
     * us with SPI1SEL during bring-up. */
    uint32_t ccipr3 = RCC->CCIPR3;
    ccipr3 &= ~RCC_CCIPR3_ADCDACSEL;
    /* 000 = HCLK (160 MHz). Plenty of clock for an 11025 Hz output. */
    RCC->CCIPR3 = ccipr3;

    /* DAC1 clock + reset (AHB3). */
    RCC->AHB3ENR  |= RCC_AHB3ENR_DAC1EN;
    (void)RCC->AHB3ENR;
    RCC->AHB3RSTR |=  RCC_AHB3RSTR_DAC1RST;
    RCC->AHB3RSTR &= ~RCC_AHB3RSTR_DAC1RST;

    /* DAC channel 1: normal mode (MCR.MODE1 = 000 = output buffer
     * enabled, connected to external pin), no trigger, no DMA. We
     * write DHR8R1 directly from the TIM6 IRQ at 11025 Hz.
     *
     * MCR.HFSEL must match the DAC AHB clock band: 00 is rated only
     * up to 80 MHz, 01 covers 80..160 MHz, 10 covers up to 200 MHz.
     * Our AHB1 runs at SYSCLK = 160 MHz, so HFSEL=01. */
    DAC1->MCR = (1u << DAC_MCR_HFSEL_Pos);
    DAC1->CR  = 0;                          /* disable while configuring */
    DAC1->CR  = 0;                          /* TEN1=0: no trigger; the
                                               next DHR8R1 write takes
                                               effect immediately */
    /* Pre-load mid-rail so the speaker is biased before the IRQ runs. */
    DAC1->DHR12R1 = DAC_BIAS_12BIT;
    DAC1->CR |= DAC_CR_EN1;
    __asm volatile ("dsb" ::: "memory");
    for (volatile int i = 0; i < 1000; i++) { __asm volatile (""); }

    /* Pre-fill the ring with silence so the IRQ has good data the
     * moment TIM6 starts firing. */
    for (int i = 0; i < RING_SAMPLES; i++) ring_buf[i] = DAC_BIAS_12BIT;

    /* TIM6: APB1 timer clock = SYSCLK (160 MHz with our PLL). Period
     * = 160e6 / 11025 ~= 14512.5 -> ARR = 14512 - 1, fclk ~= 11025.5 Hz.
     * DIER.UIE=1 so the update event raises an NVIC IRQ instead of
     * just a TRGO. */
    RCC->APB1ENR1 |= RCC_APB1ENR1_TIM6EN;
    (void)RCC->APB1ENR1;
    TIM6->CR1   = 0;
    TIM6->PSC   = 0;
    TIM6->ARR   = (SYS_CLOCK_HZ / AUDIO_SAMPLE_RATE) - 1u;
    TIM6->CNT   = 0;
    TIM6->EGR   = TIM_EGR_UG;          /* force a register update */
    TIM6->SR    = 0;                   /* clear the UG-induced UIF */
    TIM6->DIER  = TIM_DIER_UIE;        /* update interrupt enable */

    NVIC_ClearPendingIRQ(TIM6_IRQn);
    NVIC_EnableIRQ(TIM6_IRQn);

    __asm volatile ("dsb" ::: "memory");
    /* Start TIM6 -> first IRQ in ~90 us, and every ~90 us thereafter. */
    TIM6->CR1 = TIM_CR1_CEN;
}

/* TIM6 update IRQ. Fires at AUDIO_SAMPLE_RATE Hz; pushes one sample
 * into DAC1->DHR12R1 and advances the ring read pointer. At chunk
 * boundaries (sample HALF_SAMPLES-1 or RING_SAMPLES-1) it kicks the
 * mixer to refill the half that was just consumed. mix_chunk also
 * updates the LED VU meter for the chunk just produced. */
void TIM6_IRQHandler(void)
{
    /* Always clear the update flag first so we don't re-enter. */
    TIM6->SR = 0;

    uint16_t rd = ring_rd;
    DAC1->DHR12R1 = ring_buf[rd];
    rd++;
    if (rd == HALF_SAMPLES) {
        mix_chunk(&ring_buf[0], HALF_SAMPLES);
    } else if (rd == RING_SAMPLES) {
        mix_chunk(&ring_buf[HALF_SAMPLES], HALF_SAMPLES);
        rd = 0;
    }
    ring_rd = rd;
}

/* --- Doom interface ------------------------------------------------- */

static void GetSfxLumpName(should_be_const sfxinfo_t *sfx, char *buf, size_t buf_len)
{
    if (sfx->link != NULL) sfx = sfx->link;
    if (use_sfx_prefix)
        M_snprintf(buf, buf_len, "ds%s", DEH_String(sfx->name));
    else
        M_StringCopy(buf, DEH_String(sfx->name), buf_len);
}

int I_GetSfxLumpNum(should_be_const sfxinfo_t *sfx)
{
    char namebuf[9];
    GetSfxLumpName(sfx, namebuf, sizeof(namebuf));
    return W_GetNumForName(namebuf);
}

void I_InitSound(boolean _use_sfx_prefix)
{
    if (sound_initialized) return;
    use_sfx_prefix = _use_sfx_prefix ? true : false;
    for (int i = 0; i < NUM_SOUND_CHANNELS; i++) {
        channels[i].decompressed_size = 0;
    }
    audio_hw_init();
    sound_initialized = true;
}

void I_ShutdownSound(void)
{
    if (!sound_initialized) return;
    /* Park: silence all channels, stop TIM6 so the IRQ stops firing.
     * Leave DAC1 enabled at mid-rail so the speaker bias doesn't pop. */
    for (int i = 0; i < NUM_SOUND_CHANNELS; i++) {
        channels[i].decompressed_size = 0;
    }
    NVIC_DisableIRQ(TIM6_IRQn);
    TIM6->CR1  = 0;
    TIM6->DIER = 0;
    DAC1->DHR12R1 = DAC_BIAS_12BIT;
    sound_initialized = false;
}

void I_UpdateSound(void)
{
    /* Mixing is interrupt-driven; nothing to do per-tic. */
}

void I_UpdateSoundParams(int handle, int vol, int sep)
{
    if (!sound_initialized || handle < 0 || handle >= NUM_SOUND_CHANNELS) return;
    int left  = ((254 - sep) * vol) / 127;
    int right = ((sep)       * vol) / 127;
    if (left < 0) left = 0; else if (left > 255) left = 255;
    if (right < 0) right = 0; else if (right > 255) right = 255;
    channels[handle].left  = (uint8_t)left;
    channels[handle].right = (uint8_t)right;
}

int I_StartSound(should_be_const sfxinfo_t *sfx, int handle, int vol, int sep, int pitch)
{
    if (!sound_initialized || handle < 0 || handle >= NUM_SOUND_CHANNELS) return -1;

    channel_t *ch = &channels[handle];
    /* Atomic stop before reconfiguring. The DMA ISR sees
     * decompressed_size==0 and skips the channel. */
    ch->decompressed_size = 0;

    int lumpnum = sfx_mut(sfx)->lumpnum;
    int lumplen = W_LumpLength(lumpnum);
    if (lumplen < 8) return -1;
    const uint8_t *data = W_CacheLumpNum(lumpnum, PU_STATIC);
    /* Header [0]=0x03, [1]=0x80 (compressed). Vanilla DOOM SFX with
     * 0x00 in [1] are uncompressed PCM; whd_gen rewrites those to
     * 0x80 + ADPCM. If we ever boot against a non-WHD WAD, treat
     * "not compressed" as a soft failure. */
    if (data[0] != 0x03 || data[1] != 0x80) return -1;

    uint32_t sample_freq = ((uint32_t)data[3] << 8) | data[2];
    if (sample_freq == 0) return -1;

    ch->data     = data + 8;
    ch->data_end = data + lumplen;
    ch->offset   = 0;
    /* step in 16.16 = source_freq / output_freq. NORM_PITCH (=128)
     * means no pitch shift; other values rescale by pitch/NORM_PITCH. */
    if (pitch == NORM_PITCH || pitch <= 0) {
        ch->step = (sample_freq << 16) / AUDIO_SAMPLE_RATE;
    } else {
        ch->step = (uint32_t)(((uint64_t)sample_freq * pitch * 65536u) /
                              ((uint64_t)NORM_PITCH * AUDIO_SAMPLE_RATE));
    }
    /* Same IIR coefficient i_picosound.c uses: a single-pole low pass
     * tuned to roll off a bit above the source-rate Nyquist. Empirically
     * good for 11025 Hz Doom SFX. */
    ch->alpha256 = (uint16_t)(256u * 201u * sample_freq /
                              (201u * sample_freq + 64u * AUDIO_SAMPLE_RATE));

    /* Pre-decode the first ADPCM block so the ISR has data the moment
     * we mark the channel playing. */
    decompress_buffer(ch);
    if (ch->decompressed_size == 0) return -1;

    I_UpdateSoundParams(handle, vol, sep);
    return handle;
}

void I_StopSound(int handle)
{
    if (!sound_initialized || handle < 0 || handle >= NUM_SOUND_CHANNELS) return;
    channels[handle].decompressed_size = 0;
}

boolean I_SoundIsPlaying(int handle)
{
    if (!sound_initialized || handle < 0 || handle >= NUM_SOUND_CHANNELS) return false;
    return channels[handle].decompressed_size != 0;
}

void I_PrecacheSounds(should_be_const sfxinfo_t *sounds, int num_sounds)
{
    /* No-op: the WAD blob lives in flash, lump pointers stay valid
     * for the lifetime of the program. */
    (void)sounds; (void)num_sounds;
}

/* --- Music: still stubbed. ----------------------------------------- */
void    I_InitMusic(void)                                       { }
void    I_ShutdownMusic(void)                                   { }
void    I_PlaySong(void *handle, boolean looping)               { (void)handle; (void)looping; }
void    I_PauseSong(void)                                       { }
void    I_ResumeSong(void)                                      { }
void    I_StopSong(void)                                        { }
void    I_UnRegisterSong(void *h)                               { (void)h; }
void   *I_RegisterSong(should_be_const void *d, int l)          { (void)d; (void)l; return (void *)1; }
boolean I_MusicIsPlaying(void)                                  { return false; }
void    I_SetMusicVolume(int v)                                 { (void)v; }
void    I_SetOPLDriverVer(opl_driver_ver_t ver)                 { (void)ver; }
