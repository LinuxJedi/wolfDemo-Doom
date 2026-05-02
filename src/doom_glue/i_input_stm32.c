/*
 * STM32 input glue for the Doom port.
 *
 * No keyboard / joystick yet - the engine sees no input events, so
 * after ~3 s on the title screen it kicks into DEMO1 (attract mode),
 * which is what we want for the first cut.
 *
 * The wolfDemo board has two general-purpose push-buttons wired to
 * PB4 (SW2 / BT1) and PB5 (SW4 / BT2), both with external 10K
 * pull-ups to VDD - so the input idles high and reads low while
 * pressed. Either button toggles the engine's built-in FPS counter
 * (`show_fps`); the four LEDs are reserved for the SFX VU meter so
 * we don't piggyback any GPIO confirmation here.
 *
 * PB4 happens to be the JTAG NJTRST pin (MODER reset value 10 = AF).
 * Writing MODER bits 8-9 to 00 (input) overrides the JTAG TAP and
 * lets us read the line as a regular GPIO. SWD remains live on
 * PA13/PA14 for the Tag-Connect debugger.
 *
 * Polling runs from BOTH I_StartTic (called from the engine's tic
 * builder) and pd_end_frame (every render frame). I_StartTic alone
 * isn't reliable during long wipe spins / frame-pacing dropouts;
 * polling per frame guarantees the press is caught at our minimum
 * service rate. button_tick() is idempotent on the same pin state -
 * it only fires the action on the LOW->HIGH-pressed transition.
 */
#include <stdbool.h>
#include <stdint.h>

#include "stm32u585xx.h"
#include "../board.h"

extern bool show_fps;

static uint8_t btn_prev_state;  /* bit0 = BTN1 pressed last sample,
                                 * bit1 = BTN2 pressed last sample */

static void btn_init(void)
{
    static int initialized;
    if (initialized) return;
    initialized = 1;

    RCC->AHB2ENR1 |= RCC_AHB2ENR1_GPIOBEN;
    (void)RCC->AHB2ENR1;  /* dsb-equivalent - readback to ensure enable
                           * lands before the MODER write */

    /* PB4 (NJTRST) and PB5 - clear MODER to 00 = input mode. The
     * NJTRST default is 10 (AF); switching to 00 disconnects the
     * JTAG TAP and gives us a plain GPIO. */
    uint32_t moder = BTN_PORT->MODER;
    moder &= ~((3u << (BTN1_PIN * 2)) | (3u << (BTN2_PIN * 2)));
    BTN_PORT->MODER = moder;

    /* External 10K pull-ups already idle the lines high; enable
     * internal pull-up too for noise margin. PUPDR field 01 = pull-up. */
    uint32_t pupdr = BTN_PORT->PUPDR;
    pupdr &= ~((3u << (BTN1_PIN * 2)) | (3u << (BTN2_PIN * 2)));
    pupdr |=  ((1u << (BTN1_PIN * 2)) | (1u << (BTN2_PIN * 2)));
    BTN_PORT->PUPDR = pupdr;
}

void button_tick(void)
{
    btn_init();
    uint32_t idr = BTN_PORT->IDR;
    uint8_t  cur = (uint8_t)((((~idr) >> BTN1_PIN) & 1u)        /* bit0 */
                           | ((((~idr) >> BTN2_PIN) & 1u) << 1));/* bit1 */
    /* Either button - rising edge of pressed state. */
    uint8_t newly_pressed = cur & ~btn_prev_state;
    if (newly_pressed) {
        show_fps = !show_fps;
    }
    btn_prev_state = cur;
}

void I_InitInput(void)         { btn_init(); }
void I_ShutdownInput(void)     { }
void I_StartTextInput(int x1, int y1, int x2, int y2) {
    (void)x1; (void)y1; (void)x2; (void)y2;
}
void I_StopTextInput(void)     { }
void I_GetEvent(void)          { }
void I_GetEventTimeout(int t)  { (void)t; }
void I_StartTic(void)          { button_tick(); }
