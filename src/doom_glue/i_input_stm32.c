/*
 * STM32 input glue for the Doom port.
 *
 * Local input for the Doom port: wolfDemo buttons plus optional I2C
 * joystick controllers on the Qwiic / STEMMA-QT bus.
 *
 * The wolfDemo board has two general-purpose push-buttons wired to
 * PB4 (SW2 / BT1) and PB5 (SW4 / BT2), both with external 10K
 * pull-ups to VDD - so the input idles high and reads low while
 * pressed. SW2 toggles the FPS overlay (show_fps); SW4 toggles OPL
 * music on/off (I_EnableMusic) so the user can trade music for ~5
 * extra FPS when the renderer is the bottleneck. The four LEDs are
 * reserved for the SFX VU meter so we don't piggyback any GPIO
 * confirmation here.
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
#include "../qwstpad.h"
#include "../seesaw_joy.h"

#include "d_event.h"
#include "doomkeys.h"

/* Doom internals we drive directly:
 *  - menuactive: file-scope global in doom/src/doom/m_menu.c, no header
 *    extern. We watch it so a B1 press emits KEY_ENTER while the menu is
 *    up and KEY_RCTRL (fire) otherwise; the seesaw and QwSTPad both
 *    surface "primary action" as QWSTPAD_BTN_A.
 *  - joybspeed: declared in m_controls.h. Setting it >= MAX_JOY_BUTTONS
 *    flips Doom into always-run (see g_game.c, the joybspeed >= 32 check
 *    on the joystick speed key), which frees the pad's fourth button to
 *    do something other than RSHIFT/run. */
extern boolean menuactive;
extern int     joybspeed;

extern int I_GetTimeMS(void);
extern bool show_fps;
extern void I_EnableMusic(int enable);

static int music_on = 1;

static uint8_t  btn_prev_state;  /* bit0 = BTN1 pressed last sample,
                                  * bit1 = BTN2 pressed last sample */
static uint16_t pad_prev_state;  /* QwSTPad button bitmap last sample */

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

/*
 * Pad-bit -> Doom keycode. Mapping mirrors Heretic-era defaults in
 * m_controls.c so the engine sees keys it already binds to game
 * actions (fire = RCTRL, use = SPACE, strafe = RALT, run = RSHIFT,
 * arrows = move/turn). + and - drive ENTER/ESC for menu navigation.
 *
 * Index = QwSTPad pin number (0x0..0xF). Slots that don't correspond
 * to a button are 0 and the polling loop skips them. The Adafruit
 * seesaw joystick driver reports its state in this same bit layout
 * (axes converted to U/D/L/R, B1..B4 mapped to A/B/PLUS/MINUS), so a
 * single keymap serves both supported controllers.
 */
static const uint8_t pad_keymap[16] = {
    [QWSTPAD_BTN_U]     = KEY_UPARROW,
    [QWSTPAD_BTN_D]     = KEY_DOWNARROW,
    [QWSTPAD_BTN_L]     = KEY_LEFTARROW,
    [QWSTPAD_BTN_R]     = KEY_RIGHTARROW,
    [QWSTPAD_BTN_A]     = KEY_RCTRL,
    [QWSTPAD_BTN_B]     = ' ',
    [QWSTPAD_BTN_X]     = KEY_RALT,
    [QWSTPAD_BTN_Y]     = KEY_RSHIFT,
    [QWSTPAD_BTN_PLUS]  = KEY_ENTER,
    [QWSTPAD_BTN_MINUS] = KEY_ESCAPE,
};

/*
 * Two controllers are supported on the same Qwiic / STEMMA-QT bus:
 *
 *   PAD_QWSTPAD  Pimoroni QwSTPad,            TCA9555 at I2C 0x21
 *   PAD_SEESAW   Adafruit PC-joystick adapter, ATtiny8x7 seesaw at 0x49
 *
 * Different I2C addresses make detection unambiguous: probe seesaw via
 * its STATUS_HW_ID register first; if that responds with a recognised
 * silicon ID, use the seesaw driver. Otherwise try the QwSTPad init.
 * If neither answers, leave the kind unset and retry later. A missing
 * device or wedged bus can cost milliseconds per failed I2C transaction,
 * so probing on every tic would destroy frame rate.
 */
enum pad_kind { PAD_NONE = 0, PAD_QWSTPAD, PAD_SEESAW };
static uint8_t pad_kind;
static uint8_t pad_fail_count;
static uint16_t pad_cached_state;
static int pad_next_probe_ms;
static int pad_last_poll_ms = -1000;

#define PAD_REPROBE_MS          1000
#define PAD_POLL_MIN_MS         8
#define PAD_MAX_READ_FAILURES   2

static int32_t elapsed_ms(int now, int then)
{
    return (int32_t)((uint32_t)now - (uint32_t)then);
}

static void pad_init(void)
{
    if (pad_kind != PAD_NONE) return;

    int now = I_GetTimeMS();
    if (elapsed_ms(now, pad_next_probe_ms) < 0) return;

    if (seesaw_joy_probe() == 0) {
        if (seesaw_joy_init() == 0) {
            pad_kind = PAD_SEESAW;
            pad_fail_count = 0;
            pad_last_poll_ms = now - PAD_POLL_MIN_MS;
            return;
        }
        pad_next_probe_ms = now + PAD_REPROBE_MS;
        return;
    }
    if (qwstpad_init() == 0) {
        pad_kind = PAD_QWSTPAD;
        pad_fail_count = 0;
        pad_last_poll_ms = now - PAD_POLL_MIN_MS;
        return;
    }
    pad_next_probe_ms = now + PAD_REPROBE_MS;
}

static int pad_read(uint16_t *state)
{
    switch (pad_kind) {
        case PAD_QWSTPAD: return qwstpad_read_buttons_checked(state);
        case PAD_SEESAW:  return seesaw_joy_read_buttons_checked(state);
        default:
            *state = 0;
            return 0;
    }
}

static void pad_tick(void)
{
    pad_init();

    int now = I_GetTimeMS();
    uint16_t cur = pad_cached_state;

    if (pad_kind == PAD_NONE) {
        cur = 0;
    } else if (elapsed_ms(now, pad_last_poll_ms) >= PAD_POLL_MIN_MS) {
        if (pad_read(&cur) == 0) {
            pad_cached_state = cur;
            pad_fail_count = 0;
        } else {
            cur = 0;
            pad_cached_state = 0;
            if (pad_fail_count < 255) pad_fail_count++;
            if (pad_fail_count >= PAD_MAX_READ_FAILURES) {
                pad_kind = PAD_NONE;
                pad_next_probe_ms = now + PAD_REPROBE_MS;
                pad_fail_count = 0;
            }
        }
        pad_last_poll_ms = now;
    }

    /* B1 is the pad's "primary action" button (QWSTPAD_BTN_A) and we want
     * it to fire in-game (KEY_RCTRL) but confirm in the menu (KEY_ENTER).
     * The keymap binds A->RCTRL and PLUS->ENTER, so while menuactive is
     * set we transpose the bit. If menuactive flips while the button is
     * held the natural xor below sees both bits toggling, which emits a
     * clean release of the old key and a press of the new one - exactly
     * the behaviour we want, no extra bookkeeping required. */
    if (menuactive && (cur & (1u << QWSTPAD_BTN_A))) {
        cur = (uint16_t)((cur & ~(1u << QWSTPAD_BTN_A))
                          | (1u << QWSTPAD_BTN_PLUS));
    }

    uint16_t changed = cur ^ pad_prev_state;
    if (changed == 0) return;

    /* Walk the bits that flipped. For each, post a keydown if newly
     * pressed or keyup if newly released. Doom's MAXEVENTS queue is
     * only 8 entries deep in DOOM_SMALL mode, but realistic input
     * generates at most a handful of edges per frame. */
    while (changed != 0) {
        uint32_t bit = (uint32_t)__builtin_ctz(changed);
        changed &= changed - 1u;
        uint8_t key = pad_keymap[bit];
        if (key == 0) continue;

        event_t ev;
        ev.type  = (cur & (1u << bit)) ? ev_keydown : ev_keyup;
        ev.data1 = key;
        ev.data2 = 0;
        ev.data3 = 0;
        ev.data4 = 0;
        ev.data5 = 0;
        D_PostEvent(&ev);
    }

    pad_prev_state = cur;
}

void button_tick(void)
{
    btn_init();
    uint32_t idr = BTN_PORT->IDR;
    uint8_t  cur = (uint8_t)((((~idr) >> BTN1_PIN) & 1u)        /* bit0 = SW2 */
                           | ((((~idr) >> BTN2_PIN) & 1u) << 1));/* bit1 = SW4 */
    /* Rising edge per button (LOW -> HIGH transition of "pressed"). */
    uint8_t newly_pressed = cur & ~btn_prev_state;
    if (newly_pressed & 0x1) {
        show_fps = !show_fps;
    }
    if (newly_pressed & 0x2) {
        music_on = !music_on;
        I_EnableMusic(music_on);
    }
    btn_prev_state = cur;

    pad_tick();
}

void I_InitInput(void)
{
    btn_init();
    pad_init();
    /* Always-run: any value >= MAX_JOY_BUTTONS (32) makes the engine treat
     * "no joystick speed button" as "speed button always pressed". Frees
     * the pad's fourth button for menu/back instead of RSHIFT. */
    joybspeed = 32;
}
void I_ShutdownInput(void)     { }
void I_StartTextInput(int x1, int y1, int x2, int y2) {
    (void)x1; (void)y1; (void)x2; (void)y2;
}
void I_StopTextInput(void)     { }
void I_GetEvent(void)          { }
void I_GetEventTimeout(int t)  { (void)t; }
void I_StartTic(void)          { button_tick(); }
