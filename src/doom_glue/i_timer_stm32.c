/*
 * STM32 timer glue. Doom's tic timer wants 35 Hz (TICRATE).
 * SysTick is already incrementing systick_ms at 1 kHz in main.c.
 */
#include <stdint.h>

extern uint32_t millis(void);

#define TICRATE 35

int I_GetTime(void)
{
    /* Convert ms to game tics: ticks = ms * TICRATE / 1000. */
    uint32_t ms = millis();
    return (int)((ms / 1000u) * TICRATE
                 + ((ms % 1000u) * TICRATE) / 1000u);
}

int I_GetTimeMS(void)
{
    return (int)millis();
}

void I_Sleep(int ms)
{
    uint32_t target = millis() + (uint32_t)ms;
    while ((int32_t)(target - millis()) > 0) { __asm volatile ("wfi"); }
}

void I_InitTimer(void) { /* SysTick already running */ }
void I_WaitVBL(int count) { I_Sleep((count * 1000) / 70); }
