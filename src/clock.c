#include "stm32u585xx.h"
#include "board.h"

/*
 * Clock setup for the wolfDemo Doom port: HSE 8 MHz -> PLL1 -> SYSCLK 160 MHz,
 * VOS Range 1, EPOD booster on, FLASH 4 WS. This mirrors the wolfDemo
 * blinky example (which uses HAL) step-for-step.
 *
 * PLL config (HAL field encoding: register field = HAL_value - 1):
 *   PLLM = 1            -> field 0     (HSE/M = 8 MHz at PFD)
 *   PLLN = 20           -> field 19    (VCO = 8 * 20 = 160 MHz)
 *   PLLR = 1            -> field 0     (SYSCLK = 160 / 1 = 160 MHz)
 *   PLLMBOOST = DIV1    -> field 0     (booster prescaler bypass)
 *   PLLRGE = VCIRANGE_1 -> field 0b11  (PFD in 8-16 MHz range)
 *   PLLSRC = HSE        -> field 0b11
 *
 * Booster sequence quirks gleaned from STM32U5xx_HAL_Driver/Src:
 *   - VOS bits and BOOSTEN must be written in the SAME register write
 *     (stm32u5xx_hal_pwr_ex.c line 371). Setting VOS first and BOOSTEN
 *     second causes BOOSTRDY to never assert on this silicon.
 *   - BOOSTRDY is checked AFTER the PLL is configured and the HSE is
 *     stable, not immediately after BOOSTEN goes high
 *     (stm32u5xx_hal_rcc.c line 1441).
 *
 * Returns 0 on success; on failure, returns the step number that timed
 * out so the caller can stay at MSI and report.
 */

extern uint32_t SystemCoreClock;

#define WAIT_LIMIT 8000000u

static int wait_set(volatile uint32_t *reg, uint32_t mask)
{
    for (uint32_t i = 0; i < WAIT_LIMIT; i++) {
        if ((*reg & mask) != 0) return 1;
    }
    return 0;
}

static int wait_clr(volatile uint32_t *reg, uint32_t mask)
{
    for (uint32_t i = 0; i < WAIT_LIMIT; i++) {
        if ((*reg & mask) == 0) return 1;
    }
    return 0;
}

int clock_init(void)
{
    /* 1. PWR clock + voltage Range 1 + booster enable in one write.
     * HAL pattern: MODIFY_REG(PWR->VOSR, VOS|BOOSTEN, VoltageScaling|BOOSTEN). */
    RCC->AHB3ENR |= RCC_AHB3ENR_PWREN;
    (void)RCC->AHB3ENR;

    PWR->VOSR = (PWR->VOSR & ~(PWR_VOSR_VOS | PWR_VOSR_BOOSTEN)) |
                (3u << PWR_VOSR_VOS_Pos) |
                PWR_VOSR_BOOSTEN;
    if (!wait_set(&PWR->VOSR, PWR_VOSR_VOSRDY)) return 1;

    /* 2. Flash latency: 4 WS for 160 MHz on VOS1. */
    uint32_t acr = FLASH->ACR;
    acr &= ~FLASH_ACR_LATENCY;
    acr |=  (4u << FLASH_ACR_LATENCY_Pos);
    FLASH->ACR = acr;

    /* 3. HSE on. */
    RCC->CR |= RCC_CR_HSEON;
    if (!wait_set(&RCC->CR, RCC_CR_HSERDY)) return 4;

    /* 4. PLL off, configure dividers. HAL toggles BOOSTEN around the
     * MBOOST write; since we leave MBOOST at 0 (DIV1) which is also
     * the reset value, we don't need to bounce BOOSTEN. */
    RCC->CR &= ~RCC_CR_PLL1ON;
    if (!wait_clr(&RCC->CR, RCC_CR_PLL1RDY)) return 5;

    /* PLL1CFGR: SRC=HSE (3), RGE=VCIRANGE_1 (3 << RGE_Pos), M field = M-1 = 0,
     * MBOOST field = 0, enable R output. */
    uint32_t cfgr = 0;
    cfgr |= (3u << RCC_PLL1CFGR_PLL1SRC_Pos);
    cfgr |= (3u << RCC_PLL1CFGR_PLL1RGE_Pos);
    /* PLL1M field = M-1 = 0 (no shift needed) */
    /* PLL1MBOOST field = 0 = DIV1 (no shift needed) */
    cfgr |= RCC_PLL1CFGR_PLL1REN;
    RCC->PLL1CFGR = cfgr;

    /* PLL1DIVR: N field = N-1 = 19, R field = R-1 = 0, P/Q unused. */
    uint32_t divr = 0;
    divr |= (19u << RCC_PLL1DIVR_PLL1N_Pos);
    /* PLL1R field = 0 (R=1) */
    RCC->PLL1DIVR = divr;

    /* 5. PLL on, wait lock. */
    RCC->CR |= RCC_CR_PLL1ON;
    if (!wait_set(&RCC->CR, RCC_CR_PLL1RDY)) return 6;

    /* 6. Now wait for BOOSTRDY (HAL only checks here, after PLL is up). */
    if (!wait_set(&PWR->VOSR, PWR_VOSR_BOOSTRDY)) return 2;

    /* 7. Switch SYSCLK to PLL1R. */
    uint32_t cfgr1 = RCC->CFGR1;
    cfgr1 &= ~RCC_CFGR1_SW;
    cfgr1 |= (3u << RCC_CFGR1_SW_Pos);
    RCC->CFGR1 = cfgr1;
    if (!wait_set(&RCC->CFGR1, (3u << RCC_CFGR1_SWS_Pos))) return 7;

    RCC->CFGR2 = 0;
    RCC->CFGR3 = 0;

    SystemCoreClock = 160000000u;

    /* 8. Caches on (non-fatal if they don't enable). */
    ICACHE->CR = ICACHE_CR_EN;
    DCACHE1->CR = DCACHE_CR_EN;

    return 0;
}

int clock_init_160mhz(void) { return clock_init(); }
