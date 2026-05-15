#include <stdio.h>

#include "stm32u585xx.h"
#include "board.h"
#include "i2c.h"
#include "qwstpad.h"        /* QWSTPAD_BTN_* bit names - the seesaw driver
                             * reports state in the same layout so a single
                             * keymap in i_input_stm32.c covers both pads. */
#include "seesaw_joy.h"
#include "doom_glue/perf.h" /* perf_cyc() - DWT cycle counter, enabled at boot */

/* Bring-up diagnostics: 1 = print init centres and per-tick (rate-limited)
 * ADC readings over UART so we can see what the stick is actually doing.
 * Leave at 0 in production; flip to 1 when calibrating a new stick. */
#define SS_DEBUG_PRINT      0

/*
 * Adafruit seesaw protocol: every command starts with a 2-byte register
 * prefix { module base, function } and the read is a separate I2C read
 * transaction after a per-function delay (the seesaw firmware needs the
 * gap to service the request).
 */
#define SEESAW_STATUS_BASE          0x00u
#define SEESAW_GPIO_BASE            0x01u
#define SEESAW_ADC_BASE             0x09u

#define SEESAW_STATUS_HW_ID         0x01u
#define SEESAW_GPIO_DIRCLR_BULK     0x03u
#define SEESAW_GPIO_BULK            0x04u
#define SEESAW_GPIO_BULK_SET        0x05u
#define SEESAW_GPIO_PULLENSET       0x0Bu
#define SEESAW_ADC_CHANNEL_OFFSET   0x07u

/* HW_ID values for the SAMD/ATtiny silicon Adafruit ships seesaw on. The
 * product-5753 board is currently ATtiny8x7 (0x87) but the older runs use
 * ATtiny8x6 (0x86) and SAMD09 (0x55) and 0x84 also appears on some
 * variants; treat 0x84..0x87 as "this is a seesaw" and accept 0x55 too. */
static inline int seesaw_hwid_ok(uint8_t id)
{
    return id == 0x55u || (id >= 0x84u && id <= 0x87u);
}

/* Joystick wiring on the adapter (per Adafruit's PC_Joystick example).
 * Note B3/B4 only carry data on pads that drive all four gameport button
 * lines (BA1/BA2/BB1/BB2). A Gravis Gamepad Pro in legacy 2-button mode
 * leaves B3/B4 idle; flip the pad's mode switch to position 2 to get
 * four independent buttons. GRiP mode (position 3) cannot be decoded
 * through the seesaw because its 20-25 kHz clock outpaces I2C polling. */
#define SS_PIN_B1   3u   /* fire   */
#define SS_PIN_B2   13u  /* use    */
#define SS_PIN_B3   2u   /* strafe */
#define SS_PIN_B4   14u  /* escape */
#define SS_ADC_X1   1u
#define SS_ADC_Y1   15u

#define SS_BTN_MASK ((1u << SS_PIN_B1) | (1u << SS_PIN_B2) | \
                     (1u << SS_PIN_B3) | (1u << SS_PIN_B4))

/* Analog deadzone (10-bit ADC). PC gameport sticks vary wildly in their
 * rest reading - rarely close to the theoretical 512 - so the centres are
 * captured per-axis at init from the actual at-rest ADC value. The
 * deadzone is then scaled per-direction as 1/4 of the available headroom
 * on that side (verified empirically on an Adafruit PC-joystick adapter
 * whose stick rests near ADC ~180 with ~80 counts of swing toward the
 * low rail and ~820 toward the high rail). A floor keeps jitter from
 * triggering on the cramped side; the cap stops the deadzone from
 * eating useful swing on the spacious side. */
#define SS_AXIS_CENTER     512
#define SS_AXIS_DEADZONE   200  /* maximum / default deadzone */
#define SS_AXIS_DZ_FLOOR   15   /* minimum deadzone, regardless of headroom */
#define SS_ADC_MAX         1023

static uint16_t ss_x_center  = SS_AXIS_CENTER;
static uint16_t ss_y_center  = SS_AXIS_CENTER;
static uint16_t ss_x_dz_low  = SS_AXIS_DEADZONE;
static uint16_t ss_x_dz_high = SS_AXIS_DEADZONE;
static uint16_t ss_y_dz_low  = SS_AXIS_DEADZONE;
static uint16_t ss_y_dz_high = SS_AXIS_DEADZONE;

static inline uint16_t ss_clamp_dz(uint16_t headroom)
{
    uint16_t dz = headroom / 4;
    if (dz > SS_AXIS_DEADZONE) dz = SS_AXIS_DEADZONE;
    if (dz < SS_AXIS_DZ_FLOOR) dz = SS_AXIS_DZ_FLOOR;
    return dz;
}

/* SYSCLK is 160 MHz (board.h SYS_CLOCK_HZ), so 1 us = 160 cycles. */
#define SS_CYC_PER_US      160u

static void busy_wait_us(uint32_t us)
{
    uint32_t start = perf_cyc();
    uint32_t target = us * SS_CYC_PER_US;
    while ((perf_cyc() - start) < target) { /* spin */ }
}

/* Write a 2-byte seesaw register prefix followed by `extra_len` data bytes. */
static int ss_write(uint8_t base, uint8_t func,
                    const uint8_t *extra, size_t extra_len)
{
    uint8_t buf[6];
    if (extra_len > sizeof(buf) - 2) return -1;
    buf[0] = base;
    buf[1] = func;
    for (size_t i = 0; i < extra_len; i++) buf[2 + i] = extra[i];
    return i2c_write(SEESAW_I2C_ADDR, buf, 2 + extra_len);
}

/* Two-phase read: write register prefix, busy-wait the seesaw's processing
 * gap, then read `rd_len` bytes. */
static int ss_read(uint8_t base, uint8_t func,
                   uint8_t *rd, size_t rd_len, uint32_t delay_us)
{
    uint8_t prefix[2] = { base, func };
    if (i2c_write(SEESAW_I2C_ADDR, prefix, 2) != 0) return -1;
    busy_wait_us(delay_us);
    return i2c_read(SEESAW_I2C_ADDR, rd, rd_len);
}

static int ss_read_axis(uint8_t channel, uint16_t *out);

int seesaw_joy_probe(void)
{
    i2c_init();
    uint8_t id = 0;
    if (ss_read(SEESAW_STATUS_BASE, SEESAW_STATUS_HW_ID, &id, 1, 500) != 0) {
        return -1;
    }
    return seesaw_hwid_ok(id) ? 0 : -1;
}

int seesaw_joy_init(void)
{
    i2c_init();

    if (seesaw_joy_probe() != 0) return -1;

    /* GPIO mask is sent MSB-first over 4 bytes (seesaw is big-endian on
     * the wire for multi-byte fields). */
    const uint32_t mask = SS_BTN_MASK;
    uint8_t mask_be[4] = {
        (uint8_t)((mask >> 24) & 0xFFu),
        (uint8_t)((mask >> 16) & 0xFFu),
        (uint8_t)((mask >>  8) & 0xFFu),
        (uint8_t)( mask        & 0xFFu),
    };

    /* INPUT_PULLUP: clear direction (input), enable pull, latch HIGH so
     * the pull resolves to pull-up. The chip ignores bits outside `mask`. */
    if (ss_write(SEESAW_GPIO_BASE, SEESAW_GPIO_DIRCLR_BULK, mask_be, 4) != 0) return -1;
    if (ss_write(SEESAW_GPIO_BASE, SEESAW_GPIO_PULLENSET,   mask_be, 4) != 0) return -1;
    if (ss_write(SEESAW_GPIO_BASE, SEESAW_GPIO_BULK_SET,    mask_be, 4) != 0) return -1;

    /* Auto-calibrate axis centres from the at-rest reading. Assumes the
     * stick is mechanically centred when init runs (boot time). If a
     * sample fails, leave the centre at the 512 fallback so the driver
     * still produces sensible (if slightly off) output. Precompute
     * per-direction deadzones from the available headroom toward each
     * ADC rail so a stick resting near 0 or 1023 can still register
     * presses on the cramped side. */
    uint16_t x, y;
    if (ss_read_axis(SS_ADC_X1, &x) == 0) {
        ss_x_center  = x;
        ss_x_dz_low  = ss_clamp_dz(x);
        ss_x_dz_high = ss_clamp_dz((uint16_t)(SS_ADC_MAX - x));
    }
    if (ss_read_axis(SS_ADC_Y1, &y) == 0) {
        ss_y_center  = y;
        ss_y_dz_low  = ss_clamp_dz(y);
        ss_y_dz_high = ss_clamp_dz((uint16_t)(SS_ADC_MAX - y));
    }

#if SS_DEBUG_PRINT
    printf("seesaw init: cx=%u cy=%u dz x[-%u/+%u] y[-%u/+%u]\r\n",
           ss_x_center, ss_y_center,
           ss_x_dz_low, ss_x_dz_high,
           ss_y_dz_low, ss_y_dz_high);
#endif

    return 0;
}

static int ss_read_axis(uint8_t channel, uint16_t *out)
{
    uint8_t rd[2] = { 0, 0 };
    if (ss_read(SEESAW_ADC_BASE,
                (uint8_t)(SEESAW_ADC_CHANNEL_OFFSET + channel),
                rd, 2, 500) != 0) {
        return -1;
    }
    *out = (uint16_t)(((uint16_t)rd[0] << 8) | rd[1]);
    return 0;
}

int seesaw_joy_read_buttons_checked(uint16_t *out)
{
    uint16_t bitmap = 0;

    /* Buttons: one bulk GPIO read returns 32 bits MSB-first. The mapped
     * lines idle high (pull-up) and read low when pressed. */
    uint8_t gpio[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    if (ss_read(SEESAW_GPIO_BASE, SEESAW_GPIO_BULK, gpio, 4, 250) != 0) {
        return -1;
    }
    uint32_t gpio_state = ((uint32_t)gpio[0] << 24) |
                         ((uint32_t)gpio[1] << 16) |
                         ((uint32_t)gpio[2] <<  8) |
                          (uint32_t)gpio[3];
    uint32_t pressed = (~gpio_state) & SS_BTN_MASK;
    if (pressed & (1u << SS_PIN_B1)) bitmap |= (1u << QWSTPAD_BTN_A);
    if (pressed & (1u << SS_PIN_B2)) bitmap |= (1u << QWSTPAD_BTN_B);
    if (pressed & (1u << SS_PIN_B3)) bitmap |= (1u << QWSTPAD_BTN_X);
    if (pressed & (1u << SS_PIN_B4)) bitmap |= (1u << QWSTPAD_BTN_MINUS);

    /* Axes: convert analog 0..1023 to digital direction bits relative to
     * the per-axis centre captured at init. Polarity on the Adafruit PC
     * joystick adapter (verified empirically): pushing the stick LEFT or
     * UP raises the ADC reading; RIGHT or DOWN lowers it. Deadzones are
     * per-direction (precomputed in init from the headroom toward each
     * rail) so a centre near one rail can still register presses on the
     * cramped side. The asymmetric comparison form keeps unsigned
     * subtraction underflow-safe regardless of where the centre lands. */
    uint16_t x = ss_x_center;
    uint16_t y = ss_y_center;
    if (ss_read_axis(SS_ADC_X1, &x) != 0) return -1;
    if (ss_read_axis(SS_ADC_Y1, &y) != 0) return -1;

    if (y > (uint16_t)(ss_y_center + ss_y_dz_high))
        bitmap |= (1u << QWSTPAD_BTN_U);
    else if (y + ss_y_dz_low < ss_y_center)
        bitmap |= (1u << QWSTPAD_BTN_D);

    if (x > (uint16_t)(ss_x_center + ss_x_dz_high))
        bitmap |= (1u << QWSTPAD_BTN_L);
    else if (x + ss_x_dz_low < ss_x_center)
        bitmap |= (1u << QWSTPAD_BTN_R);

#if SS_DEBUG_PRINT
    /* Rate-limited: print roughly once a second (pad_tick fires every render
     * frame, ~35-60 Hz). Emits raw X/Y plus the resulting bitmap so we can
     * see whether right/down actually drives the ADC reading down. */
    static uint16_t dbg_counter;
    if (++dbg_counter >= 45) {
        dbg_counter = 0;
        printf("seesaw: x=%u y=%u bits=0x%04x\r\n",
               x, y, (unsigned)bitmap);
    }
#endif

    *out = bitmap;
    return 0;
}

uint16_t seesaw_joy_read_buttons(void)
{
    uint16_t buttons = 0;
    (void)seesaw_joy_read_buttons_checked(&buttons);
    return buttons;
}
