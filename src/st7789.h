#ifndef WOLFDEMO_ST7789_H
#define WOLFDEMO_ST7789_H

#include <stdint.h>
#include <stddef.h>

#define ST7789_W 240
#define ST7789_H 240

/* RGB565 helpers */
#define RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3)))

void st7789_init(void);
void st7789_backlight(int on);
void st7789_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void st7789_fill(uint16_t color);
void st7789_blit(const uint16_t *pixels, size_t count);

/* Begin / end a continuous RAMWR write. After st7789_set_window /
 * RAMWR is set up, st7789_blit_begin asserts D-C high and CS low and
 * leaves them; the caller can then push raw byte streams (eg via
 * spi_dma_blit_start) without the panel seeing CS edges between
 * scan-lines. st7789_blit_end releases CS. */
void st7789_blit_begin(void);
void st7789_blit_end(void);

#endif /* WOLFDEMO_ST7789_H */
