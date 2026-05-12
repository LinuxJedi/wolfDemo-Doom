#ifndef SEESAW_JOY_H
#define SEESAW_JOY_H

#include <stdint.h>

/*
 * Adafruit "PC Joystick to seesaw I2C Adapter" (product 5753) driver.
 *
 * Exposes a DA-15 PC gameport joystick (4 analog axes, 4 digital buttons)
 * over I2C via Adafruit's seesaw protocol on an ATtiny8x7. The 7-bit I2C
 * address is fixed at 0x49 (no jumper).
 *
 * To let the same Doom keymap drive either this adapter or the Pimoroni
 * QwSTPad, seesaw_joy_read_buttons() reports state in the QwSTPad bit
 * layout (see qwstpad.h). The X1/Y1 analog axes are converted to digital
 * direction bits with a fixed deadzone around a per-axis centre captured
 * at init (PC gameport sticks rarely rest at the theoretical 512). X2/Y2
 * are unused. Polarity: pushing left or up raises the ADC reading on the
 * adapter, so the comparisons below use the high side for L/U. Mapping:
 *
 *   Y1 > centre + DEADZONE  -> QWSTPAD_BTN_U     (forward)
 *   Y1 < centre - DEADZONE  -> QWSTPAD_BTN_D     (back)
 *   X1 > centre + DEADZONE  -> QWSTPAD_BTN_L     (turn left)
 *   X1 < centre - DEADZONE  -> QWSTPAD_BTN_R     (turn right)
 *   B1 (seesaw GPIO 3)      -> QWSTPAD_BTN_A     (fire)
 *   B2 (seesaw GPIO 13)     -> QWSTPAD_BTN_B     (use)
 *   B3 (seesaw GPIO 2)      -> QWSTPAD_BTN_PLUS  (menu enter)
 *   B4 (seesaw GPIO 14)     -> QWSTPAD_BTN_MINUS (menu escape)
 */

int      seesaw_joy_probe(void);         /* 0 if HW_ID looks like a seesaw chip, -1 otherwise */
int      seesaw_joy_init(void);          /* 0 on success, -1 on bus error */
uint16_t seesaw_joy_read_buttons(void);  /* QwSTPad-layout bitmap; 0 on bus error */

#endif /* SEESAW_JOY_H */
