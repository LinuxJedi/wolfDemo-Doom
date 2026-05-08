#include "stm32u585xx.h"
#include "board.h"
#include "spi.h"

/*
 * Polled SPI1 driver for the ST7789 display.
 *
 * Pins (board.h): PA1=SCK, PA6=MISO, PA7=MOSI, all on AF5.
 * Master, 8-bit, mode 0, software NSS (display CS is a GPIO on PA0).
 * Baud = SYSCLK / 4 = 40 MHz at 160 MHz SYSCLK.
 *
 * U5 SPI requires TSIZE != 0 to make EOT/TXC fire. With TSIZE = 0
 * (continuous) we'd have to use CSUSP to end transfers, which is
 * fiddly; setting TSIZE per transfer and waiting for EOT is simpler
 * and reliable. TSIZE is 16 bits so transfers > 65535 frames are
 * chunked here.
 */

#define TSIZE_MAX 65535u

/* Halfword-DMA byte cap. GPDMA's CBR1.BNDT is a 16-bit byte count;
 * with halfword src/dst widths it must be a multiple of 2, so the
 * largest legal chunk is 65534 bytes. */
#define DMA_HW_BYTES_MAX 65534u

void spi_init(void)
{
    /* Select SYSCLK (160 MHz) as the SPI1 kernel clock source
     * (RCC_CCIPR1.SPI1SEL = 01). Without this the peripheral may not
     * actually clock data out the wire even though APB enable is set;
     * this matches what wolfTPM-bench's HAL_SPI_MspInit does and is
     * the missing step that left init/fill bytes stuck in the FIFO. */
    uint32_t ccipr1 = RCC->CCIPR1;
    ccipr1 &= ~RCC_CCIPR1_SPI1SEL;
    ccipr1 |=  (1u << RCC_CCIPR1_SPI1SEL_Pos);
    RCC->CCIPR1 = ccipr1;

    /* Clock SPI1 and GPIOA */
    RCC->AHB2ENR1 |= RCC_AHB2ENR1_GPIOAEN;
    RCC->APB2ENR  |= RCC_APB2ENR_SPI1EN;
    (void)RCC->APB2ENR;

    /* PA1, PA6, PA7 as alternate function (mode 10), AF5 */
    uint32_t moder = SPI_PINS_PORT->MODER;
    moder &= ~((3u << (SPI_SCK_PIN  * 2)) |
               (3u << (SPI_MISO_PIN * 2)) |
               (3u << (SPI_MOSI_PIN * 2)));
    moder |=  ((2u << (SPI_SCK_PIN  * 2)) |
               (2u << (SPI_MISO_PIN * 2)) |
               (2u << (SPI_MOSI_PIN * 2)));
    SPI_PINS_PORT->MODER = moder;

    /* High-speed slew for ~40 MHz SCK */
    uint32_t ospeedr = SPI_PINS_PORT->OSPEEDR;
    ospeedr |=  ((3u << (SPI_SCK_PIN  * 2)) |
                 (3u << (SPI_MISO_PIN * 2)) |
                 (3u << (SPI_MOSI_PIN * 2)));
    SPI_PINS_PORT->OSPEEDR = ospeedr;

    uint32_t afrl = SPI_PINS_PORT->AFR[0];
    afrl &= ~((0xFu << (SPI_SCK_PIN  * 4)) |
              (0xFu << (SPI_MISO_PIN * 4)) |
              (0xFu << (SPI_MOSI_PIN * 4)));
    afrl |=  (((uint32_t)SPI_AF) << (SPI_SCK_PIN  * 4)) |
             (((uint32_t)SPI_AF) << (SPI_MISO_PIN * 4)) |
             (((uint32_t)SPI_AF) << (SPI_MOSI_PIN * 4));
    SPI_PINS_PORT->AFR[0] = afrl;

    /* Disable, configure, leave disabled (each xfer flips SPE on/off
     * since TSIZE must be set before SPE is asserted). */
    SPI1->CR1  = 0;
    SPI1->CR2  = 0;
    /* CFG1: 8-bit data (DSIZE = 7), MBR = 001 (SYSCLK / 4 = 40 MHz at
     * 160 MHz SYSCLK). The ST7789V2 datasheet rates the write clock
     * up to 62.5 MHz; 40 MHz is the practical max for this click. */
    SPI1->CFG1 = (7u << SPI_CFG1_DSIZE_Pos) | (1u << SPI_CFG1_MBR_Pos);
    /* CFG2: master, software NSS, simplex TX-only (COMM=01), mode 0,
     * MSB first. TX-only avoids the RX FIFO needing to be drained. */
    SPI1->CFG2 = SPI_CFG2_MASTER | SPI_CFG2_SSM | SPI_CFG2_COMM_0;
    /* SSI = 1 keeps the internal NSS high so master is "selected" */
    SPI1->CR1 = SPI_CR1_SSI;
}

/*
 * Send `len` bytes, optionally repeating a single value when
 * `repeat_value >= 0`. Returns when all bytes have been clocked out.
 */
/* Public so the caller can detect a SPI timeout instead of hanging. */
volatile uint32_t spi_last_sr;

/* Mask covering every IFCR write-1-to-clear flag on STM32U5 SPI. */
#define SPI_IFCR_ALL ( SPI_IFCR_EOTC | SPI_IFCR_TXTFC | SPI_IFCR_UDRC  | \
                       SPI_IFCR_OVRC | SPI_IFCR_CRCEC | SPI_IFCR_TIFREC | \
                       SPI_IFCR_MODFC | SPI_IFCR_SUSPC )

static int spi_xfer_chunk(const uint8_t *data, size_t len, int repeat_value)
{
    /* HAL order: CR2 (TSIZE), enable SPE, then pre-fill FIFO with as
     * many bytes as fit, then CSTART. With SPE on but CSTART not yet
     * set, the FIFO accepts data without anything being clocked out;
     * once CSTART asserts, the SPI immediately starts shifting from
     * a non-empty FIFO instead of clocking out 0x00 in the gap. */
    SPI1->CR1  = SPI_CR1_SSI;
    /* Re-assert CFG2 every chunk. On STM32U5 a mode fault (MODF)
     * auto-clears the MASTER bit, dropping the peripheral into
     * slave mode where it stops driving SCK and MOSI. We saw the
     * MASTER bit gone in a register dump after init had run many
     * back-to-back transfers, which matches "clock briefly then
     * dead, MOSI flat 0x00" on the logic probe. Clearing MODF
     * via IFCR.MODFC and rewriting CFG2 with MASTER set forces
     * the peripheral back to master each chunk. */
    SPI1->IFCR = SPI_IFCR_ALL;
    SPI1->CFG2 = SPI_CFG2_MASTER | SPI_CFG2_SSM | SPI_CFG2_COMM_0;
    SPI1->CR2  = (uint32_t)len;
    SPI1->CR1  = SPI_CR1_SSI | SPI_CR1_SPE;

    size_t i = 0;
    /* Pre-fill: write as many bytes as TXP allows before starting. */
    while (i < len && (SPI1->SR & SPI_SR_TXP) != 0) {
        uint8_t b = (repeat_value >= 0) ? (uint8_t)repeat_value : data[i];
        *(volatile uint8_t *)&SPI1->TXDR = b;
        i++;
    }

    SPI1->CR1 = SPI_CR1_SSI | SPI_CR1_SPE | SPI_CR1_CSTART;

    for (; i < len; i++) {
        uint32_t guard = 200000u;
        while ((SPI1->SR & SPI_SR_TXP) == 0) {
            if (--guard == 0) {
                spi_last_sr = SPI1->SR;
                SPI1->CR1 = SPI_CR1_SSI;
                return -1;
            }
        }
        uint8_t b = (repeat_value >= 0) ? (uint8_t)repeat_value : data[i];
        *(volatile uint8_t *)&SPI1->TXDR = b;
    }

    /* Just wait for TXC (FIFO empty + last bit shifted out). EOT and
     * CTSIZE-based completion both proved unreliable on subsequent
     * chunks when SPE is toggled. Short timeout: if TXC does not
     * fire we proceed anyway, since the next chunk's TXP polling
     * will block until the FIFO has space. */
    uint32_t guard = 20000u;
    while ((SPI1->SR & SPI_SR_TXC) == 0) {
        if (--guard == 0) {
            spi_last_sr = SPI1->SR;
            break;
        }
    }

    SPI1->IFCR = SPI_IFCR_ALL;
    SPI1->CR1 = SPI_CR1_SSI;  /* SPE off */
    return 0;
}

void spi_write(const uint8_t *data, size_t len)
{
    while (len > 0) {
        size_t chunk = (len > TSIZE_MAX) ? TSIZE_MAX : len;
        spi_xfer_chunk(data, chunk, -1);
        data += chunk;
        len  -= chunk;
    }
}

void spi_write_repeat(uint8_t value, size_t count)
{
    while (count > 0) {
        size_t chunk = (count > TSIZE_MAX) ? TSIZE_MAX : count;
        spi_xfer_chunk(NULL, chunk, value);
        count -= chunk;
    }
}

void spi_write16_repeat(uint16_t value, size_t count)
{
    /* Use a large stack buffer and stream via spi_write. With BATCH
     * = 256 pixels = 512 bytes per spi_write call, a 240x240 fill is
     * only 225 SPI transactions instead of 8228, and each falls well
     * inside the 16-bit TSIZE limit. The earlier 14-byte BATCH made
     * the SPE off/on cycle so frequent that the peripheral stopped
     * generating clocks (logic capture confirmed). */
    enum { BATCH = 256 };
    uint8_t buf[BATCH * 2];
    uint8_t hi = (uint8_t)(value >> 8);
    uint8_t lo = (uint8_t)value;
    for (int i = 0; i < BATCH; i++) {
        buf[i * 2]     = hi;
        buf[i * 2 + 1] = lo;
    }
    while (count > 0) {
        size_t pixels = (count > BATCH) ? BATCH : count;
        spi_write(buf, pixels * 2u);
        count -= pixels;
    }
}

/* ----- DMA-driven blit -------------------------------------------------
 *
 * GPDMA1 channel 0 wired up to SPI1_TX (request line 7). Each blit:
 *   1. CFG2 re-asserted (MODF auto-clears MASTER on U5 - same trick
 *      as the polled path; see big comment in spi_xfer_chunk).
 *   2. SPI configured for the byte count via TSIZE.
 *   3. CFG1.TXDMAEN set so the SPI requests bytes from DMA whenever
 *      the TX FIFO has space (TXP).
 *   4. SPE on, then DMA channel enabled, then CSTART.
 *   5. spi_dma_blit_wait polls the DMA IDLEF then the SPI TXC so we
 *      know the line is fully drained before the caller releases CS.
 *
 * One channel + one outstanding transfer is enough for the i_video
 * use case; the caller does its own ping-pong with two scan_out
 * buffers. We don't enable any DMA interrupts - polling IDLEF is a
 * single-instruction read and the wait is rare relative to the
 * useful CPU work between blits. */
static volatile int spi_dma_in_flight;

static void spi_dma_init_once(void)
{
    static int initialized;
    if (initialized) return;
    initialized = 1;

    /* GPDMA1 lives on AHB1; clock enable bit 0 of RCC_AHB1ENR. */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPDMA1EN;
    (void)RCC->AHB1ENR;

    DMA_Channel_TypeDef *ch = GPDMA1_Channel0;
    /* Disable channel before configuring (defensive; usually idle). */
    ch->CCR = 0;
    /* Clear all the per-channel pending flags. */
    ch->CFCR = 0xFFFFFFFFu;
    /* CTR1: source halfword (2 bytes), increment source; destination
     * halfword, fixed (the SPI TXDR is one register, byte-addressable
     * but accepts a 16-bit write per transfer). All access through AHB
     * port 0 (SAP/DAP both 0). Burst length defaults to 1 (PBL_LOG2 = 0).
     * SDW_LOG2 = DDW_LOG2 = 1 means each transfer moves one 16-bit unit;
     * paired with SPI1 DSIZE = 15 (set per-blit by the start funcs)
     * the SPI shifts out 16 bits per DMA transfer, halving DMA bus
     * traffic and FIFO refill events. */
    ch->CTR1 = DMA_CTR1_SINC
             | (1u << DMA_CTR1_SDW_LOG2_Pos)
             | (1u << DMA_CTR1_DDW_LOG2_Pos);
    /* CTR2: peripheral request, request line 7 = SPI1_TX. DREQ=1
     * means the peripheral signals readiness for each transfer (the
     * destination peripheral controls flow). */
    ch->CTR2 = (7u << DMA_CTR2_REQSEL_Pos) | DMA_CTR2_DREQ;
    /* Destination is the SPI TXDR byte port. */
    ch->CDAR = (uint32_t)(uintptr_t)&SPI1->TXDR;
}

void spi_dma_blit_start(const uint8_t *data, size_t len)
{
    spi_dma_init_once();
    if (len == 0) return;
    /* Caller passes byte count; the SPI runs in 16-bit DSIZE so one
     * frame == 2 bytes. GPDMA CBR1.BNDT caps at 65534 bytes per chunk
     * (halfword-aligned) and the buffer must be even-length. The
     * caller never exceeds 480-byte rows in practice. */
    if (len > DMA_HW_BYTES_MAX || (len & 1u)) {
        spi_write(data, len);
        return;
    }

    /* Defensive: if a previous DMA is still in flight, wait it out
     * before reprogramming. spi_dma_blit_wait is the same operation. */
    if (spi_dma_in_flight) spi_dma_blit_wait();

    /* SPI side: replicate the polled-path init sequence so we recover
     * if MODF auto-cleared MASTER. DSIZE=15 = 16-bit frames; restored
     * to 7 (8-bit) by spi_dma_blit_wait so polled command writes work. */
    SPI1->CR1  = SPI_CR1_SSI;
    SPI1->IFCR = SPI_IFCR_ALL;
    SPI1->CFG2 = SPI_CFG2_MASTER | SPI_CFG2_SSM | SPI_CFG2_COMM_0;
    SPI1->CFG1 = (SPI1->CFG1 & ~SPI_CFG1_DSIZE)
               | (15u << SPI_CFG1_DSIZE_Pos);
    SPI1->CR2  = (uint32_t)(len / 2u);   /* TSIZE counts 16-bit frames */
    SPI1->CFG1 |= SPI_CFG1_TXDMAEN;      /* SPI now sources from DMA */
    SPI1->CR1  = SPI_CR1_SSI | SPI_CR1_SPE;

    /* DMA side: source = caller's buffer, byte count = len, then arm.
     * The DSB ensures any pending CPU writes to *data (the scan-line
     * the caller just prepared) are committed to SRAM before GPDMA
     * starts pulling bytes through it. The M33 store buffer can
     * otherwise reorder a fast sequence of `prepare_scanline` writes
     * past the DMA enable. */
    DMA_Channel_TypeDef *ch = GPDMA1_Channel0;
    ch->CFCR = 0xFFFFFFFFu;
    ch->CSAR = (uint32_t)(uintptr_t)data;
    ch->CBR1 = (uint32_t)len;
    __asm volatile ("dsb" ::: "memory");
    ch->CCR  = DMA_CCR_EN;

    SPI1->CR1 = SPI_CR1_SSI | SPI_CR1_SPE | SPI_CR1_CSTART;
    spi_dma_in_flight = 1;
}

void spi_dma_blit_wait(void)
{
    if (!spi_dma_in_flight) return;

    DMA_Channel_TypeDef *ch = GPDMA1_Channel0;
    /* Wait for DMA to drain. IDLEF asserts when the channel goes back
     * to IDLE state after the block transfer completes. Bounded
     * timeout so a hardware glitch can't hang the renderer. */
    uint32_t guard = 2000000u;
    while ((ch->CSR & DMA_CSR_IDLEF) == 0) {
        if (--guard == 0) {
            spi_last_sr = SPI1->SR;
            break;
        }
    }

    /* Then wait for SPI to finish shifting the last byte out the wire
     * - DMA done only means bytes are in the SPI FIFO. */
    guard = 200000u;
    while ((SPI1->SR & SPI_SR_TXC) == 0) {
        if (--guard == 0) { spi_last_sr = SPI1->SR; break; }
    }

    /* Drop SPE FIRST. STM32U5 SPI locks DSIZE (and most CFG1 bits)
     * while SPE=1, so the DSIZE=7 restore must happen afterwards. */
    SPI1->CR1  = SPI_CR1_SSI;             /* SPE off */
    SPI1->CFG1 &= ~SPI_CFG1_TXDMAEN;
    SPI1->CFG1 = (SPI1->CFG1 & ~SPI_CFG1_DSIZE)
               | (7u << SPI_CFG1_DSIZE_Pos);
    SPI1->IFCR = SPI_IFCR_ALL;
    ch->CCR    = 0;                       /* channel disable */
    ch->CFCR   = 0xFFFFFFFFu;
    spi_dma_in_flight = 0;
}

/* ----- Background ("async") full-frame blit ---------------------------
 *
 * The single-shot spi_dma_blit_* path above blocks the CPU for the
 * full ~19 ms of SPI clocking per frame. The async path runs SPI in
 * continuous mode (TSIZE=0) so it keeps requesting bytes from DMA
 * indefinitely; an ISR on GPDMA1 channel 0's transfer-complete event
 * re-arms the channel for the next chunk without CPU intervention.
 * I_FinishUpdate kicks the blit and returns; the next frame's BSP +
 * column work happens during the SPI clocking. spi_blit_async_wait
 * is called at the start of the next I_FinishUpdate to drain the
 * SPI FIFO and put the peripheral back in a clean idle state before
 * any non-async access.
 *
 * Why two register paths instead of unifying with spi_dma_blit_start:
 * the synchronous path uses TSIZE=len so EOT/TXC fire reliably for
 * the small (CMD/window) transfers that surround a frame; mixing
 * continuous mode into those would require careful CSUSP handling.
 * The async path is only used by I_FinishUpdate. */
static volatile enum {
    ASYNC_IDLE = 0,
    ASYNC_INFLIGHT,        /* DMA actively pushing bytes */
    ASYNC_DRAIN_PENDING,   /* all chunks queued; SPI may still hold bytes */
} async_state;

static const uint8_t *async_data;
static size_t         async_remaining;

static void async_program_chunk(void)
{
    /* Program GPDMA channel 0 with the next chunk of async_data. The
     * SPI is already in continuous mode and SPE-on, so as soon as the
     * channel re-enables it'll start consuming bytes via TXP. Chunks
     * must be halfword-aligned for the 16-bit DMA path. */
    size_t chunk = async_remaining > DMA_HW_BYTES_MAX
                 ? DMA_HW_BYTES_MAX : async_remaining;

    DMA_Channel_TypeDef *ch = GPDMA1_Channel0;
    ch->CFCR = 0xFFFFFFFFu;
    ch->CSAR = (uint32_t)(uintptr_t)async_data;
    ch->CBR1 = (uint32_t)chunk;
    /* Re-enable channel + transfer-complete interrupt. CCR bits get
     * cleared on the previous transfer's completion, so we always
     * write the full set we want here. */
    __asm volatile ("dsb" ::: "memory");
    ch->CCR = DMA_CCR_TCIE | DMA_CCR_EN;

    async_data      += chunk;
    async_remaining -= chunk;
}

void spi_blit_async_start(const uint8_t *data, size_t total_len)
{
    spi_dma_init_once();
    if (total_len == 0) return;

    /* Defensive: a leftover sync blit shouldn't be in flight, but if
     * it is, drain it cleanly before we change SPI modes. */
    if (spi_dma_in_flight) spi_dma_blit_wait();

    /* Drain any prior async transfer too (caller is supposed to call
     * spi_blit_async_wait first, but make this self-correcting). */
    if (async_state != ASYNC_IDLE) spi_blit_async_wait();

    async_data      = data;
    async_remaining = total_len;
    async_state     = ASYNC_INFLIGHT;

    /* Configure SPI for continuous transfer: TSIZE=0 means it keeps
     * clocking until SPE drops or CSUSP fires. CFG2 re-asserted to
     * recover from any MODF that auto-cleared MASTER. DSIZE=15 puts
     * the peripheral into 16-bit frame mode; spi_blit_async_wait
     * restores DSIZE=7 so polled command writes work between blits. */
    SPI1->CR1   = SPI_CR1_SSI;
    SPI1->IFCR  = SPI_IFCR_ALL;
    SPI1->CFG2  = SPI_CFG2_MASTER | SPI_CFG2_SSM | SPI_CFG2_COMM_0;
    SPI1->CFG1  = (SPI1->CFG1 & ~SPI_CFG1_DSIZE)
                | (15u << SPI_CFG1_DSIZE_Pos);
    SPI1->CR2   = 0;                              /* TSIZE = continuous */
    SPI1->CFG1 |= SPI_CFG1_TXDMAEN;
    SPI1->CR1   = SPI_CR1_SSI | SPI_CR1_SPE;

    /* Enable the GPDMA channel-0 NVIC line so our ISR fires on TC. */
    NVIC_ClearPendingIRQ(GPDMA1_Channel0_IRQn);
    NVIC_EnableIRQ(GPDMA1_Channel0_IRQn);

    async_program_chunk();
    SPI1->CR1 = SPI_CR1_SSI | SPI_CR1_SPE | SPI_CR1_CSTART;
}

void spi_blit_async_wait(void)
{
    /* Spin until ISR has queued the last chunk. A bounded guard keeps
     * a stuck channel from hanging the renderer. */
    uint32_t guard = 8000000u;
    while (async_state == ASYNC_INFLIGHT) {
        if (--guard == 0) {
            spi_last_sr = SPI1->SR;
            break;
        }
    }
    if (async_state != ASYNC_DRAIN_PENDING && async_state != ASYNC_IDLE) {
        async_state = ASYNC_IDLE;
        return;
    }

    /* All chunks have been handed to SPI; wait for the last byte to
     * actually shift out the wire (TXC), then take SPE down so the
     * peripheral is parked and any subsequent polled cmd/window write
     * goes via the regular spi_xfer_chunk path cleanly. */
    guard = 200000u;
    while ((SPI1->SR & SPI_SR_TXC) == 0) {
        if (--guard == 0) { spi_last_sr = SPI1->SR; break; }
    }
    /* Drop SPE FIRST. STM32U5 SPI locks DSIZE (and most CFG1 bits)
     * while SPE=1, so the DSIZE=7 restore must happen afterwards. */
    SPI1->CR1   = SPI_CR1_SSI;             /* SPE off */
    SPI1->CFG1 &= ~SPI_CFG1_TXDMAEN;
    SPI1->CFG1  = (SPI1->CFG1 & ~SPI_CFG1_DSIZE)
                | (7u << SPI_CFG1_DSIZE_Pos);
    SPI1->IFCR  = SPI_IFCR_ALL;
    NVIC_DisableIRQ(GPDMA1_Channel0_IRQn);
    async_state = ASYNC_IDLE;
}

int spi_blit_async_busy(void)
{
    return async_state != ASYNC_IDLE;
}

/* GPDMA1 channel 0 transfer-complete ISR. Fires when the current
 * chunk's BNDT counter reaches zero. We acknowledge the flag, then
 * either chain the next chunk (SPI is still SPE-on in continuous
 * mode, so the new chunk's bytes start streaming as soon as we
 * re-enable the channel) or transition to DRAIN_PENDING and let the
 * main thread finish the SPI shutdown. */
void GPDMA1_Channel0_IRQHandler(void)
{
    DMA_Channel_TypeDef *ch = GPDMA1_Channel0;
    /* W1C: write the bits to clear them. Clearing only TCF leaves any
     * concurrent error flag visible. */
    ch->CFCR = DMA_CFCR_TCF;
    /* Channel auto-disables on TC; we re-enable it via CCR for the
     * next chunk. */

    if (async_remaining > 0) {
        async_program_chunk();
    } else {
        async_state = ASYNC_DRAIN_PENDING;
    }
}
