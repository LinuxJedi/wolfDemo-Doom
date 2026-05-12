#ifndef BOARD_H
#define BOARD_H

/*
 * wolfDemo board pin map for the Doom port (STM32U585CIT6 LQFP48).
 *
 * Pin assignments are derived from the wolfDemo v1 schematic and the
 * Mikroe IPSDISPLAY2 click driver (mikrosdk_click_v2/clicks/ipsdisplay2),
 * which uses this mikroBus mapping:
 *   CS  -> mikroBus CS
 *   RST -> mikroBus RST   (wolfDemo ties this to system NRST)
 *   D/C -> mikroBus INT
 *   BL  -> mikroBus AN
 *
 * For the wolfDemo v1, mikroBus 1 (the top connector) is used by the
 * MIKROE-6078 IPS Display 2 click. Open question: schematic does not
 * cleanly disambiguate which of PA0/PA3 is MBUS1_CS. PA0 is the
 * placeholder; if the display stays dark at bring-up, swap to PA3.
 */

#include "stm32u585xx.h"

/* ---- USART1: USB-UART bridge (host PC console, 115200) ---- */
#define UART_PINS_PORT          GPIOA
#define UART_TX_PIN             9   /* PA9  USART1_TX */
#define UART_RX_PIN             10  /* PA10 USART1_RX */
#define UART_AF                 7   /* AF7 = USART1 */

/* ---- SPI1: shared mikroBus SPI bus ---- */
#define SPI_PINS_PORT           GPIOA
#define SPI_SCK_PIN             1   /* PA1 SPI1_SCK  */
#define SPI_MISO_PIN            6   /* PA6 SPI1_MISO */
#define SPI_MOSI_PIN            7   /* PA7 SPI1_MOSI */
#define SPI_AF                  5   /* AF5 = SPI1 */

/* ---- MIKROE-6078 IPS Display 2 click in mikroBus 1 ---- */
#define DISPLAY_CS_PORT         GPIOA
#define DISPLAY_CS_PIN          0   /* PA0 - mikroBus 1 CS */

#define DISPLAY_DC_PORT         GPIOB
#define DISPLAY_DC_PIN          1   /* PB1 - mikroBus 1 INT, repurposed as D/C */

#define DISPLAY_BL_PORT         GPIOA
#define DISPLAY_BL_PIN          2   /* PA2 - mikroBus 1 AN, drives backlight */

/* RST is wired to system NRST on the wolfDemo, so display reset is
 * done in software via the ST7789 SWRESET (0x01) command. */

/* ---- LEDs (4) ---- */
#define LED_PORT                GPIOB
#define LED1_PIN                12
#define LED2_PIN                13
#define LED3_PIN                14
#define LED4_PIN                15

/* ---- User push-buttons (active-low, external 10K pull-up to VDD) ----
 * SW1 = system RESET (wired to NRST), SW3 = BOOT0 (PH3, special-purpose).
 * The two general-purpose buttons are SW2 (BT1, PB4) and SW4 (BT2, PB5). */
#define BTN_PORT                GPIOB
#define BTN1_PIN                4   /* PB4 - SW2 / BT1 */
#define BTN2_PIN                5   /* PB5 - SW4 / BT2 */

/* ---- I2C1 on mikroBus 2 (Qwiic / STEMMA-QT bus) ---- */
#define I2C_PINS_PORT           GPIOB
#define I2C_SCL_PIN             6   /* PB6 I2C1_SCL */
#define I2C_SDA_PIN             9   /* PB9 I2C1_SDA */
#define I2C_AF                  4   /* AF4 = I2C1 */

/* Pimoroni QwSTPad (TCA9555 I/O expander). 7-bit I2C address. */
#define QWSTPAD_I2C_ADDR        0x21u

/* Adafruit PC Joystick to seesaw I2C Adapter (product 5753, ATtiny8x7). */
#define SEESAW_I2C_ADDR         0x49u

/* ---- System clock target after PLL ----
 * 160 MHz at VOS Range 1 with EPOD booster, matching the wolfDemo
 * blinky example. PLL: HSE 8 MHz / M=1 * N=20 / R=1 = 160 MHz. */
#define SYS_CLOCK_HZ            160000000UL

#endif /* BOARD_H */
