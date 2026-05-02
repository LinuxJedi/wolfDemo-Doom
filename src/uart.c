#include "stm32u585xx.h"
#include "board.h"
#include "uart.h"

/*
 * Bare-metal USART1 driver. PA9 = TX, PA10 = RX, AF7. After reset the
 * STM32U5 runs from MSI at 4 MHz; this driver derives the baud divisor
 * from SystemCoreClock so it stays correct after we switch to PLL/HSE.
 */

extern uint32_t SystemCoreClock;

static void uart_set_baud(unsigned int baud)
{
    /* USART1 is on PCLK2; with prescalers all DIV1 it equals HCLK.
     * BRR = fck / baud (oversampling x16). */
    uint32_t fck = SystemCoreClock;
    uint32_t brr = (fck + baud / 2u) / baud;
    USART1->BRR = brr;
}

void uart_init(unsigned int baud)
{
    /* Enable GPIOA clock and USART1 clock */
    RCC->AHB2ENR1 |= RCC_AHB2ENR1_GPIOAEN;
    RCC->APB2ENR  |= RCC_APB2ENR_USART1EN;
    /* Small barrier so subsequent register access sees the clock */
    (void)RCC->APB2ENR;

    /* Configure PA9, PA10 as alternate function (mode 0b10) AF7 */
    uint32_t moder = GPIOA->MODER;
    moder &= ~((3u << (UART_TX_PIN * 2)) | (3u << (UART_RX_PIN * 2)));
    moder |=  ((2u << (UART_TX_PIN * 2)) | (2u << (UART_RX_PIN * 2)));
    GPIOA->MODER = moder;

    /* AFRH covers pins 8..15 */
    uint32_t afrh = GPIOA->AFR[1];
    afrh &= ~((0xFu << ((UART_TX_PIN - 8) * 4)) |
              (0xFu << ((UART_RX_PIN - 8) * 4)));
    afrh |=  ((UART_AF & 0xFu) << ((UART_TX_PIN - 8) * 4)) |
             ((UART_AF & 0xFu) << ((UART_RX_PIN - 8) * 4));
    GPIOA->AFR[1] = afrh;

    /* Disable, configure, then enable USART1 */
    USART1->CR1 = 0;
    uart_set_baud(baud);
    USART1->CR2 = 0;
    USART1->CR3 = 0;
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;

    /* Wait for transmitter to become idle */
    while ((USART1->ISR & USART_ISR_TEACK) == 0) { }
}

void uart_putc(char c)
{
    while ((USART1->ISR & USART_ISR_TXE_TXFNF) == 0) { }
    USART1->TDR = (uint8_t)c;
}

void uart_write(const char *s, size_t len)
{
    while (len--) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}

void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}
