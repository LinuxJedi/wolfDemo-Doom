#ifndef QWSTPAD_H
#define QWSTPAD_H

#include <stdint.h>

/*
 * Pimoroni QwSTPad (TCA9555 I/O expander) driver.
 *
 * qwstpad_read_buttons() returns a 16-bit bitmap where a set bit means
 * the corresponding button is currently pressed. Bit positions match
 * the QwSTPad pin numbering (0x0..0xF) used in the Pimoroni library:
 *
 *    QWSTPAD_BTN_U (0x1)  QWSTPAD_BTN_D (0x4)
 *    QWSTPAD_BTN_L (0x2)  QWSTPAD_BTN_R (0x3)
 *    QWSTPAD_BTN_MINUS (0x5)  QWSTPAD_BTN_PLUS (0xB)
 *    QWSTPAD_BTN_A (0xE)  QWSTPAD_BTN_B (0xC)
 *    QWSTPAD_BTN_X (0xF)  QWSTPAD_BTN_Y (0xD)
 *
 * Returns 0 if the I2C transaction fails (so a missing pad reads as
 * "no buttons pressed"). The caller decides whether to surface that
 * differently.
 */

#define QWSTPAD_BTN_U      0x1u
#define QWSTPAD_BTN_L      0x2u
#define QWSTPAD_BTN_R      0x3u
#define QWSTPAD_BTN_D      0x4u
#define QWSTPAD_BTN_MINUS  0x5u
#define QWSTPAD_BTN_PLUS   0xBu
#define QWSTPAD_BTN_B      0xCu
#define QWSTPAD_BTN_Y      0xDu
#define QWSTPAD_BTN_A      0xEu
#define QWSTPAD_BTN_X      0xFu

#define QWSTPAD_BUTTON_MASK ( (1u << QWSTPAD_BTN_U)     \
                            | (1u << QWSTPAD_BTN_D)     \
                            | (1u << QWSTPAD_BTN_L)     \
                            | (1u << QWSTPAD_BTN_R)     \
                            | (1u << QWSTPAD_BTN_A)     \
                            | (1u << QWSTPAD_BTN_B)     \
                            | (1u << QWSTPAD_BTN_X)     \
                            | (1u << QWSTPAD_BTN_Y)     \
                            | (1u << QWSTPAD_BTN_PLUS)  \
                            | (1u << QWSTPAD_BTN_MINUS))

int      qwstpad_init(void);          /* 0 on success, -1 if no ACK */
uint16_t qwstpad_read_buttons(void);  /* 1 = pressed, 0 on bus error */

#endif /* QWSTPAD_H */
