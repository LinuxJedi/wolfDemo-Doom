#include "stm32u585xx.h"
#include "board.h"
#include "i2c.h"
#include "qwstpad.h"

/*
 * TCA9555 register addresses. The chip auto-increments the register
 * pointer on a multi-byte access, so we read INPUT_PORT0 + INPUT_PORT1
 * back-to-back from a single transaction.
 */
#define TCA9555_REG_INPUT0   0x00
#define TCA9555_REG_OUTPUT0  0x02
#define TCA9555_REG_CONFIG0  0x06

/*
 * Pin direction config: 1 = input, 0 = output. The QwSTPad pin
 * numbering puts buttons + LEDs on the same two ports:
 *
 *   port 0 bit 0     reserved (idle high, treat as input)
 *   port 0 bit 1..5  buttons U/L/R/D/-     (input)
 *   port 0 bit 6..7  LED1, LED2            (output)
 *   port 1 bit 0     reserved (idle high, treat as input)
 *   port 1 bit 1..2  LED3, LED4            (output)
 *   port 1 bit 3..7  buttons +/B/Y/A/X     (input)
 *
 * CONFIG bytes: button bits = 1, LED bits = 0. So
 *   port 0: 0011_1111 = 0x3F
 *   port 1: 1111_1001 = 0xF9
 *
 * The LEDs are active-low: driving the output pin LOW lights the LED.
 * To start with all LEDs off we therefore write 1 to each LED output
 * bit. Bits not configured as outputs are ignored by the chip on
 * write, so the non-LED bits in OUTPUT_PORTx don't matter.
 */
#define CONFIG_PORT0_VAL  0x3Fu
#define CONFIG_PORT1_VAL  0xF9u
#define OUTPUT_PORT0_OFF  0xC0u   /* LED1, LED2 high = off */
#define OUTPUT_PORT1_OFF  0x06u   /* LED3, LED4 high = off */

int qwstpad_init(void)
{
    i2c_init();

    /* Park the LED output bits high (off) before switching the pins
     * to outputs, so we don't get a brief on-flash from the default
     * 0x00 OUTPUT register value during the direction change. */
    uint8_t out_buf[3] = { TCA9555_REG_OUTPUT0,
                           OUTPUT_PORT0_OFF, OUTPUT_PORT1_OFF };
    if (i2c_write(QWSTPAD_I2C_ADDR, out_buf, sizeof out_buf) != 0) return -1;

    uint8_t cfg_buf[3] = { TCA9555_REG_CONFIG0,
                           CONFIG_PORT0_VAL, CONFIG_PORT1_VAL };
    if (i2c_write(QWSTPAD_I2C_ADDR, cfg_buf, sizeof cfg_buf) != 0) return -1;

    return 0;
}

int qwstpad_read_buttons_checked(uint16_t *out)
{
    uint8_t reg = TCA9555_REG_INPUT0;
    uint8_t in[2] = { 0xFF, 0xFF };  /* idle-high default = no buttons */

    if (i2c_write_read(QWSTPAD_I2C_ADDR, &reg, 1, in, 2) != 0) {
        return -1;
    }

    /* Buttons read low when pressed, so invert and mask off the LED
     * and reserved bits. */
    uint16_t raw = (uint16_t)in[0] | ((uint16_t)in[1] << 8);
    *out = ((uint16_t)~raw) & (uint16_t)QWSTPAD_BUTTON_MASK;
    return 0;
}

uint16_t qwstpad_read_buttons(void)
{
    uint16_t buttons = 0;
    (void)qwstpad_read_buttons_checked(&buttons);
    return buttons;
}
