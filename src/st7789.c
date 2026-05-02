#include "stm32u585xx.h"
#include "board.h"
#include "spi.h"
#include "st7789.h"

/*
 * ST7789V2 driver for the MIKROE-6078 IPS Display 2 click in mikroBus 1.
 *
 * Init sequence is patterned after Floyd-Fish/ST7789-STM32 (the
 * reference the user pointed us at), with one wolfDemo-specific
 * difference: the click's RST line is wired to system NRST on this
 * board, so we cannot pulse it. Instead we issue SWRESET (0x01).
 * That puts the controller in a known state from any power-on or
 * brownout that left it in an odd register state.
 *
 * Pin map (board.h):
 *   SPI1 SCK/MOSI -> PA1, PA7  (AF5)
 *   CS  -> GPIO PA0 (active low)
 *   D/C -> GPIO PB5 (mikroBus INT pin, repurposed)
 *   BL  -> GPIO PA2 (mikroBus AN pin, simple on/off)
 */

/* ST7789V2 commands */
#define ST_NOP        0x00
#define ST_SWRESET    0x01
#define ST_SLPOUT     0x11
#define ST_NORON      0x13
#define ST_INVOFF     0x20
#define ST_INVON      0x21
#define ST_DISPOFF    0x28
#define ST_DISPON     0x29
#define ST_CASET      0x2A
#define ST_RASET      0x2B
#define ST_RAMWR      0x2C
#define ST_MADCTL     0x36
#define ST_COLMOD     0x3A

#define DC_PORT       DISPLAY_DC_PORT
#define DC_PIN        DISPLAY_DC_PIN
#define CS_PORT       DISPLAY_CS_PORT
#define CS_PIN        DISPLAY_CS_PIN
#define BL_PORT       DISPLAY_BL_PORT
#define BL_PIN        DISPLAY_BL_PIN

static inline void cs_low(void)  { CS_PORT->BSRR = (1u << (CS_PIN + 16)); }
static inline void cs_high(void) { CS_PORT->BSRR = (1u <<  CS_PIN); }
static inline void dc_cmd(void)  { DC_PORT->BSRR = (1u << (DC_PIN + 16)); }
static inline void dc_data(void) { DC_PORT->BSRR = (1u <<  DC_PIN); }

static void delay_loop(volatile uint32_t cycles)
{
    while (cycles--) { __asm volatile ("nop"); }
}

static void delay_ms_blocking(uint32_t ms)
{
    /* Roughly tuned for 160 MHz; harmless to run a bit fast or slow. */
    while (ms--) { delay_loop(16000); }
}

static void gpio_init_pin(GPIO_TypeDef *port, int pin, int mode_pp, int high_speed)
{
    uint32_t moder = port->MODER;
    moder &= ~(3u << (pin * 2));
    moder |=  ((uint32_t)mode_pp << (pin * 2));
    port->MODER = moder;

    if (high_speed) {
        uint32_t ospeedr = port->OSPEEDR;
        ospeedr |= (3u << (pin * 2));
        port->OSPEEDR = ospeedr;
    }
}

static void enable_gpio_clock(GPIO_TypeDef *port)
{
    if (port == GPIOA) RCC->AHB2ENR1 |= RCC_AHB2ENR1_GPIOAEN;
    else if (port == GPIOB) RCC->AHB2ENR1 |= RCC_AHB2ENR1_GPIOBEN;
    else if (port == GPIOC) RCC->AHB2ENR1 |= RCC_AHB2ENR1_GPIOCEN;
    (void)RCC->AHB2ENR1;
}

static void write_cmd(uint8_t cmd)
{
    dc_cmd();
    cs_low();
    spi_write(&cmd, 1);
    cs_high();
}

static void write_data(const uint8_t *data, size_t len)
{
    dc_data();
    cs_low();
    spi_write(data, len);
    cs_high();
}

static void write_data_byte(uint8_t b)
{
    write_data(&b, 1);
}

void st7789_backlight(int on)
{
    BL_PORT->BSRR = on ? (1u << BL_PIN) : (1u << (BL_PIN + 16));
}

void st7789_init(void)
{
    /* GPIOs for CS, D/C, BL */
    enable_gpio_clock(CS_PORT);
    enable_gpio_clock(DC_PORT);
    enable_gpio_clock(BL_PORT);

    gpio_init_pin(CS_PORT, CS_PIN, 1, 1);
    gpio_init_pin(DC_PORT, DC_PIN, 1, 1);
    gpio_init_pin(BL_PORT, BL_PIN, 1, 0);

    cs_high();
    dc_data();
    st7789_backlight(0);

    spi_init();

    delay_ms_blocking(10);
    write_cmd(ST_SWRESET);
    delay_ms_blocking(150);

    write_cmd(ST_SLPOUT);
    delay_ms_blocking(120);

    write_cmd(ST_COLMOD);
    write_data_byte(0x55);

    /* Porch control */
    write_cmd(0xB2);
    {
        const uint8_t d[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
        write_data(d, sizeof d);
    }
    /* MADCTL = 0x00 (RGB order, no flips). If colors look swapped
     * try 0x60 (BGR + portrait) or 0xC0 (rotated 180). */
    write_cmd(ST_MADCTL);
    write_data_byte(0x00);

    /* Internal LCD voltage generator settings, copied from Floyd-Fish */
    write_cmd(0xB7); write_data_byte(0x35); /* Gate control */
    write_cmd(0xBB); write_data_byte(0x19); /* VCOM 0.725 V */
    write_cmd(0xC0); write_data_byte(0x2C); /* LCMCTRL */
    write_cmd(0xC2); write_data_byte(0x01); /* VDV/VRH cmd enable */
    write_cmd(0xC3); write_data_byte(0x12); /* VRH +-4.45 V */
    write_cmd(0xC4); write_data_byte(0x20); /* VDV default */
    write_cmd(0xC6); write_data_byte(0x0F); /* Frame rate 60 Hz */
    write_cmd(0xD0);                         /* Power control */
    {
        const uint8_t d[] = {0xA4, 0xA1};
        write_data(d, sizeof d);
    }

    write_cmd(0xE0);
    {
        const uint8_t d[] = {0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F,
                             0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23};
        write_data(d, sizeof d);
    }
    write_cmd(0xE1);
    {
        const uint8_t d[] = {0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F,
                             0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23};
        write_data(d, sizeof d);
    }

    write_cmd(ST_INVON);
    delay_ms_blocking(10);
    write_cmd(ST_NORON);
    delay_ms_blocking(10);
    write_cmd(ST_DISPON);
    delay_ms_blocking(10);

    st7789_set_window(0, 0, ST7789_W, ST7789_H);
    st7789_fill(0x0000);
    st7789_backlight(1);
}

void st7789_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    uint16_t x1 = x + w - 1;
    uint16_t y1 = y + h - 1;
    uint8_t buf[4];

    write_cmd(ST_CASET);
    buf[0] = x  >> 8; buf[1] = x  & 0xFF;
    buf[2] = x1 >> 8; buf[3] = x1 & 0xFF;
    write_data(buf, 4);

    write_cmd(ST_RASET);
    buf[0] = y  >> 8; buf[1] = y  & 0xFF;
    buf[2] = y1 >> 8; buf[3] = y1 & 0xFF;
    write_data(buf, 4);

    write_cmd(ST_RAMWR);
}

void st7789_fill(uint16_t color)
{
    st7789_set_window(0, 0, ST7789_W, ST7789_H);
    dc_data();
    cs_low();
    spi_write16_repeat(color, (size_t)ST7789_W * ST7789_H);
    cs_high();
}

void st7789_blit(const uint16_t *pixels, size_t count)
{
    /* Single-shot polled blit. Kept for callers that hand us a small
     * buffer of native-endian uint16_t pixels (e.g. the engine's
     * st7789_fill path used to). The display blit hot path lives in
     * i_video_stm32.c::I_FinishUpdate and uses spi_dma_blit_*. */
    dc_data();
    cs_low();
    static uint8_t tx_buf[ST7789_W * 2];
    while (count > 0) {
        size_t chunk = (count > ST7789_W) ? ST7789_W : count;
        for (size_t i = 0; i < chunk; i++) {
            tx_buf[i * 2]     = (uint8_t)(pixels[i] >> 8);
            tx_buf[i * 2 + 1] = (uint8_t)pixels[i];
        }
        spi_write(tx_buf, chunk * 2u);
        pixels += chunk;
        count  -= chunk;
    }
    cs_high();
}

/* Open / close a streaming RAMWR window. The caller (i_video_stm32) is
 * responsible for set_window'ing first; these just drive D-C and CS so
 * a sequence of spi_dma_blit_start calls can run back-to-back without
 * the panel seeing a CS edge between them. */
void st7789_blit_begin(void)
{
    dc_data();
    cs_low();
}

void st7789_blit_end(void)
{
    cs_high();
}
