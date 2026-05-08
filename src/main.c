#include <stdio.h>
#include <stdint.h>
#include "stm32u585xx.h"
#include "board.h"
#include "uart.h"
#include "st7789.h"
#include "doom_glue/perf.h"

/* The Doom engine entry point. D_DoomMain drives Z_Init, W_AddFile,
 * R_Init, P_Init, and ultimately D_DoomLoop -- it never returns. */
extern void D_DoomMain(void);

int  clock_init_160mhz(void);   /* returns 0 on success, stage code on fail */

extern uint32_t SystemCoreClock;

static volatile uint32_t systick_ms;

void SysTick_Handler(void) { systick_ms++; }
uint32_t millis(void)      { return systick_ms; }

static void delay_loop(volatile uint32_t cycles)
{
    while (cycles--) { __asm volatile ("nop"); }
}

static void crude_delay_ms(uint32_t ms)
{
    /* About 250 cycles per inner iteration. At 4 MHz MSI ~16 iter/ms,
     * at 160 MHz ~640 iter/ms; pick a value that is close at both ends
     * by scaling against SystemCoreClock. */
    uint32_t per_ms = SystemCoreClock / 250u / 1000u;
    if (per_ms == 0) per_ms = 1;
    while (ms--) { delay_loop(per_ms * 1000u); }
}

/* ----- LED helpers (visible even with no UART) ----- */
static void led_init(void)
{
    RCC->AHB2ENR1 |= RCC_AHB2ENR1_GPIOBEN;
    (void)RCC->AHB2ENR1;
    uint32_t moder = LED_PORT->MODER;
    for (int p = LED1_PIN; p <= LED4_PIN; p++) {
        moder &= ~(3u << (p * 2));
        moder |=  (1u << (p * 2));
    }
    LED_PORT->MODER = moder;
    LED_PORT->BSRR = (1u << (LED1_PIN + 16)) | (1u << (LED2_PIN + 16)) |
                     (1u << (LED3_PIN + 16)) | (1u << (LED4_PIN + 16));
}

static void led_set(int idx, int on)
{
    int pin = LED1_PIN + idx;
    LED_PORT->BSRR = on ? (1u << pin) : (1u << (pin + 16));
}

static void panic_blink(int code)
{
    /* Morse-ish: long-pulse `code` times on LED1, repeat forever.
     * Visible even if UART is wrong, so the user can read the
     * stage number from the blink count. */
    while (1) {
        for (int i = 0; i < code; i++) {
            led_set(0, 1); crude_delay_ms(250);
            led_set(0, 0); crude_delay_ms(150);
        }
        crude_delay_ms(900);
    }
}

int main(void)
{
    /* Stage 0: alive at MSI 4 MHz. LED1 on as a "running" indicator. */
    led_init();
    led_set(0, 1);

    /* MSI is the reset default at 4 MHz, so SystemCoreClock is 4M.
     * The UART driver derives BRR from SystemCoreClock, so this works
     * before clock_init_160mhz runs. */
    SystemCoreClock = 4000000u;
    uart_init(115200);
    uart_puts("\r\n");
    uart_puts("[stage 0] alive at MSI 4 MHz\r\n");
    led_set(1, 1);

    /* Stage 1: bring SYSCLK up to 160 MHz from HSE. If anything
     * times out (HSE missing, VOS, EPOD, PLL), we fall back to
     * MSI and report which step failed. */
    int rc = clock_init_160mhz();
    if (rc == 0) {
        /* Re-init UART with the new SystemCoreClock */
        uart_init(115200);
        printf("[stage 1] PLL up; SystemCoreClock = %lu\r\n",
               (unsigned long)SystemCoreClock);
    } else {
        /* Stay at MSI 4 MHz so UART still works */
        SystemCoreClock = 4000000u;
        uart_init(115200);
        printf("[stage 1] clock_init failed at step %d; staying at MSI\r\n", rc);
        printf("  step 1 -> VOSRDY never asserted (VOS encoding wrong)\r\n");
        printf("  step 2 -> BOOSTRDY never asserted (EPOD)\r\n");
        printf("  step 4 -> HSERDY never asserted (HSE crystal missing?)\r\n");
        printf("  step 5/6 -> PLL did not lock\r\n");
        printf("  step 7 -> SWS never reflected new source\r\n");
        /* Keep LED1 blinking the failure code forever. */
        panic_blink(rc);
    }
    led_set(2, 1);

    /* Stage 2: SysTick @ 1 kHz */
    SysTick_Config(SystemCoreClock / 1000u);

    /* DWT cycle counter for perf brackets (perf.h). Safe to leave on
     * permanently; no measurable runtime cost. */
    perf_init();

    /* Stage 3: WAD probe */
    extern const uint8_t _wad_start[], _wad_end[];
    size_t wad_size = (size_t)(_wad_end - _wad_start);
    printf("[stage 2] WAD blob: %u bytes at %p, magic = %c%c%c%c\r\n",
           (unsigned)wad_size, (const void *)_wad_start,
           _wad_start[0], _wad_start[1], _wad_start[2], _wad_start[3]);
    led_set(3, 1);

    /* Stage 3: bring up the panel. The engine's I_InitGraphics expects
     * the ST7789 to already be initialised (i_video_stm32.c just sets
     * the window and clears the palette). */
    printf("[stage 3] st7789_init\r\n");
    st7789_init();
    /* Briefly flash the panel black so the user sees a transition from
     * the bootloader / previous-firmware state into "engine starting". */
    st7789_fill(RGB565(0, 0, 0));

    /* Stage 4: hand off to the Doom engine. D_DoomMain runs Z_Init,
     * loads the WAD, initialises everything, and enters D_DoomLoop.
     * It never returns. If anything along the way calls I_Error or
     * panic, the system glue prints "[doom] ..." over UART and halts
     * the core in WFI -- LEDs freeze in whatever state they were in. */
    printf("[stage 4] D_DoomMain (engine handoff)\r\n");
    /* All LEDs off; the SFX VU meter takes over once the engine starts. */
    led_set(0, 0); led_set(1, 0); led_set(2, 0); led_set(3, 0);
    D_DoomMain();
    /* Should be unreachable. */
    printf("[stage 4] D_DoomMain returned (unexpected)\r\n");
    while (1) { __asm volatile ("wfi"); }
}
