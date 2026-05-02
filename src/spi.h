#ifndef SPI_H
#define SPI_H

#include <stdint.h>
#include <stddef.h>

void spi_init(void);
void spi_write(const uint8_t *data, size_t len);
void spi_write_repeat(uint8_t value, size_t count);
void spi_write16_repeat(uint16_t value, size_t count);

/* DMA-driven blit. Caller is responsible for asserting CS / D-C
 * around a sequence of these calls. Each spi_dma_blit_start kicks
 * off a non-blocking transfer of `len` bytes; spi_dma_blit_wait
 * blocks until that transfer (and any prior in-flight one) has
 * fully clocked out. Buffer must remain valid until wait returns.
 *
 * Pattern for ping-pong:
 *   prepare buf[0]
 *   spi_dma_blit_start(buf[0], n);
 *   for (i=1..N-1) {
 *       prepare buf[i&1];
 *       spi_dma_blit_wait();
 *       spi_dma_blit_start(buf[i&1], n);
 *   }
 *   spi_dma_blit_wait();
 *
 * Single-buffer use (no overlap) is also fine - just call
 * start/wait back-to-back. */
void spi_dma_blit_start(const uint8_t *data, size_t len);
void spi_dma_blit_wait(void);

/* Background blit that survives across function returns.
 * spi_blit_async_start kicks off a multi-chunk DMA transfer (the
 * length may exceed SPI's 16-bit TSIZE - the ISR handles chunking)
 * and returns immediately. The TC interrupt on GPDMA1 channel 0
 * programs the next chunk without CPU intervention; once all chunks
 * are queued the channel parks until spi_blit_async_wait is called,
 * which finishes the SPI drain (wait for TXC) and clears SPE.
 *
 * The buffer must remain valid and unmodified for the lifetime of
 * the transfer. CS / D-C are owned by the caller and must stay
 * low/high through the whole asynchronous transfer.
 *
 * SPI is held in continuous mode (TSIZE=0) for the duration so the
 * panel sees one unbroken RAMWR sequence across all chunks. */
void spi_blit_async_start(const uint8_t *data, size_t total_len);
void spi_blit_async_wait(void);
int  spi_blit_async_busy(void);

#endif
