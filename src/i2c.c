#include "stm32u585xx.h"
#include "board.h"
#include "i2c.h"

/*
 * Polled I2C1 master driver. PB6 = SCL, PB9 = SDA, both AF4.
 *
 * Kernel clock = PCLK1 = SYSCLK = 160 MHz (CCIPR1.I2C1SEL = 00, the
 * reset default). TIMINGR is computed for 100 kHz Standard Mode:
 *
 *   PRESC  = 0xF -> tPRESC = 16 / 160 MHz = 100 ns
 *   SCLDEL = 0x4 -> tSU;DAT = 5 * tPRESC = 500 ns (>= 250 ns)
 *   SDADEL = 0x2 -> tHD;DAT = 2 * tPRESC = 200 ns
 *   SCLH   = 0x31 (49) -> tHIGH = 50 * tPRESC = 5.0 us
 *   SCLL   = 0x31 (49) -> tLOW  = 50 * tPRESC = 5.0 us
 *
 *   tSCL = tHIGH + tLOW = 10 us -> 100 kHz
 *
 * Each public entry point uses a bounded software guard so a stuck
 * line (e.g. an unplugged QwSTPad) returns -1 instead of hanging the
 * render loop. The guard is sized for ~10 ms at 160 MHz; well above a
 * worst-case byte time at 100 kHz (~90 us per byte) but short enough
 * that a missing device costs less than one frame.
 */

#define I2C_TIMINGR_100K_160MHZ  0xF0423131u
#define I2C_GUARD                1600000u

static int wait_flag(volatile uint32_t *isr, uint32_t mask)
{
    for (uint32_t i = 0; i < I2C_GUARD; i++) {
        uint32_t v = *isr;
        if (v & mask) return 0;
        /* NACK ends the transfer early; treat as failure. */
        if (v & I2C_ISR_NACKF) return -1;
    }
    return -1;
}

void i2c_init(void)
{
    static int initialized;
    if (initialized) return;
    initialized = 1;

    /* CCIPR1.I2C1SEL: 00 = PCLK1 (default). Force-write so we don't
     * inherit a stale selection from a previous boot loader. */
    uint32_t ccipr1 = RCC->CCIPR1;
    ccipr1 &= ~RCC_CCIPR1_I2C1SEL;
    RCC->CCIPR1 = ccipr1;

    /* Clock GPIOB and I2C1. */
    RCC->AHB2ENR1  |= RCC_AHB2ENR1_GPIOBEN;
    RCC->APB1ENR1  |= RCC_APB1ENR1_I2C1EN;
    (void)RCC->APB1ENR1;

    /* PB6 / PB9 -> alternate function (10), AF4, open-drain, pull-up. */
    uint32_t moder = I2C_PINS_PORT->MODER;
    moder &= ~((3u << (I2C_SCL_PIN * 2)) | (3u << (I2C_SDA_PIN * 2)));
    moder |=  ((2u << (I2C_SCL_PIN * 2)) | (2u << (I2C_SDA_PIN * 2)));
    I2C_PINS_PORT->MODER = moder;

    I2C_PINS_PORT->OTYPER |= (1u << I2C_SCL_PIN) | (1u << I2C_SDA_PIN);

    uint32_t pupdr = I2C_PINS_PORT->PUPDR;
    pupdr &= ~((3u << (I2C_SCL_PIN * 2)) | (3u << (I2C_SDA_PIN * 2)));
    pupdr |=  ((1u << (I2C_SCL_PIN * 2)) | (1u << (I2C_SDA_PIN * 2)));
    I2C_PINS_PORT->PUPDR = pupdr;

    /* SCL=PB6 is in AFR[0] (pin 6), SDA=PB9 is in AFR[1] (pin 9). */
    uint32_t afrl = I2C_PINS_PORT->AFR[0];
    afrl &= ~(0xFu << (I2C_SCL_PIN * 4));
    afrl |=  ((uint32_t)I2C_AF) << (I2C_SCL_PIN * 4);
    I2C_PINS_PORT->AFR[0] = afrl;

    uint32_t afrh = I2C_PINS_PORT->AFR[1];
    afrh &= ~(0xFu << ((I2C_SDA_PIN - 8) * 4));
    afrh |=  ((uint32_t)I2C_AF) << ((I2C_SDA_PIN - 8) * 4);
    I2C_PINS_PORT->AFR[1] = afrh;

    /* Disable, configure, enable. PE must be 0 while writing TIMINGR. */
    I2C1->CR1     = 0;
    I2C1->TIMINGR = I2C_TIMINGR_100K_160MHZ;
    I2C1->CR2     = 0;
    I2C1->OAR1    = 0;
    I2C1->OAR2    = 0;
    I2C1->CR1     = I2C_CR1_PE;
}

/* Program CR2 for one logical transfer fragment. AUTOEND controls
 * whether STOP is generated automatically after NBYTES bytes. */
static void start_xfer(uint8_t addr7, size_t nbytes, int rd, int autoend)
{
    uint32_t cr2 = ((uint32_t)addr7 << 1)
                 | ((uint32_t)nbytes << I2C_CR2_NBYTES_Pos)
                 | I2C_CR2_START;
    if (rd)      cr2 |= I2C_CR2_RD_WRN;
    if (autoend) cr2 |= I2C_CR2_AUTOEND;
    I2C1->CR2 = cr2;
}

static int wait_stop_clear(void)
{
    if (wait_flag(&I2C1->ISR, I2C_ISR_STOPF) != 0) return -1;
    I2C1->ICR = I2C_ICR_STOPCF | I2C_ICR_NACKCF;
    I2C1->CR2 = 0;
    return 0;
}

int i2c_write(uint8_t addr7, const uint8_t *data, size_t len)
{
    if (len > 255) return -1;
    start_xfer(addr7, len, 0, 1);

    for (size_t i = 0; i < len; i++) {
        if (wait_flag(&I2C1->ISR, I2C_ISR_TXIS) != 0) {
            I2C1->ICR = I2C_ICR_NACKCF | I2C_ICR_STOPCF;
            I2C1->CR2 = 0;
            return -1;
        }
        I2C1->TXDR = data[i];
    }

    return wait_stop_clear();
}

int i2c_read(uint8_t addr7, uint8_t *data, size_t len)
{
    if (len > 255 || len == 0) return -1;
    start_xfer(addr7, len, 1, 1);

    for (size_t i = 0; i < len; i++) {
        if (wait_flag(&I2C1->ISR, I2C_ISR_RXNE) != 0) {
            I2C1->ICR = I2C_ICR_NACKCF | I2C_ICR_STOPCF;
            I2C1->CR2 = 0;
            return -1;
        }
        data[i] = (uint8_t)I2C1->RXDR;
    }

    return wait_stop_clear();
}

int i2c_write_read(uint8_t addr7,
                   const uint8_t *wr, size_t wr_len,
                         uint8_t *rd, size_t rd_len)
{
    if (wr_len > 255 || rd_len > 255 || rd_len == 0) return -1;

    /* Phase 1: write, no AUTOEND so we can issue a repeated START. */
    start_xfer(addr7, wr_len, 0, 0);

    for (size_t i = 0; i < wr_len; i++) {
        if (wait_flag(&I2C1->ISR, I2C_ISR_TXIS) != 0) {
            I2C1->ICR = I2C_ICR_NACKCF | I2C_ICR_STOPCF;
            I2C1->CR2 = 0;
            return -1;
        }
        I2C1->TXDR = wr[i];
    }

    /* Wait for the write phase to complete (TC fires when NBYTES bytes
     * have been transferred and AUTOEND=0). */
    if (wait_flag(&I2C1->ISR, I2C_ISR_TC) != 0) {
        I2C1->ICR = I2C_ICR_NACKCF | I2C_ICR_STOPCF;
        I2C1->CR2 = 0;
        return -1;
    }

    /* Phase 2: repeated START + read. AUTOEND on so STOP fires after
     * the last byte is clocked in. */
    start_xfer(addr7, rd_len, 1, 1);

    for (size_t i = 0; i < rd_len; i++) {
        if (wait_flag(&I2C1->ISR, I2C_ISR_RXNE) != 0) {
            I2C1->ICR = I2C_ICR_NACKCF | I2C_ICR_STOPCF;
            I2C1->CR2 = 0;
            return -1;
        }
        rd[i] = (uint8_t)I2C1->RXDR;
    }

    return wait_stop_clear();
}
