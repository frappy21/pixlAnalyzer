// Host side test of the debounce state machine: drives the pins and the clock
// by hand and checks the event stream the UI would see.
#include <stdio.h>
#include <string.h>
#include "buttons.h"
#include "board_config.h"

static uint32_t g_ms = 0;
static int g_level[32];          // 1 = pressed (pin pulled low)
uint32_t systime_ms(void) { return g_ms; }
void systime_init(void) {}
int host_pin_level(unsigned pin) { return g_level[pin] ? 0 : 1; }

static void press(int pin, int down) { g_level[pin] = down; }
static void advance(uint32_t ms, uint32_t step) {
    for (uint32_t t = 0; t < ms; t += step) { g_ms += step; buttons_poll(); }
}

static int failures = 0;
static void check(const char *what, int got, int want) {
    if (got != want) { printf("  FAIL %-46s got %d want %d\n", what, got, want); failures++; }
    else printf("  ok   %-46s\n", what);
}

int main(void) {
    // Case 1: a bouncing press produces exactly one event
    memset(g_level, 0, sizeof(g_level));
    buttons_init();
    int events = 0;
    for (int i = 0; i < 8; i++) { press(PIN_BTN_MID, i & 1); g_ms += 2; buttons_poll(); if (buttons_pressed(BTN_MID)) events++; }
    press(PIN_BTN_MID, 1);
    advance(100, 5);
    if (buttons_pressed(BTN_MID)) events++;
    check("bouncing press -> exactly one press event", events, 1);

    // Case 2: holding does not produce more events (the menu must not exit)
    advance(3000, 30);
    events = 0;
    for (int i = 0; i < 50; i++) { advance(30, 30); if (buttons_pressed(BTN_MID)) events++; }
    check("holding 4.5s -> no further events", events, 0);
    check("button reads as down while held", buttons_down(BTN_MID), 1);
    check("held time is plausible", buttons_held_ms(BTN_MID) > 3000, 1);

    // Case 3: release, then a second press is seen again
    press(PIN_BTN_MID, 0); advance(100, 5);
    press(PIN_BTN_MID, 1); advance(100, 5);
    check("second press after release", buttons_pressed(BTN_MID), 1);

    // Case 4: a spike shorter than the debounce window is rejected
    press(PIN_BTN_MID, 0); advance(100, 5);
    press(PIN_BTN_MID, 1); g_ms += 8; buttons_poll();
    press(PIN_BTN_MID, 0); g_ms += 8; buttons_poll();
    advance(100, 5);
    check("8ms spike ignored", buttons_pressed(BTN_MID), 0);

    // Case 5: auto repeat on a navigation button
    memset(g_level, 0, sizeof(g_level)); buttons_init();
    press(PIN_BTN_RIGHT, 1); advance(40, 5);
    int repeats = 0;
    if (buttons_repeat(BTN_RIGHT)) repeats++;
    check("first repeat fires immediately", repeats, 1);
    for (int i = 0; i < 100; i++) { advance(10, 5); if (buttons_repeat(BTN_RIGHT)) repeats++; }
    printf("  info repeats within ~1.04s of holding: %d\n", repeats);
    check("auto repeat rate is 4-12 per second", repeats >= 5 && repeats <= 13, 1);

    // Case 6: the wake press is not delivered to the UI after a flush
    memset(g_level, 0, sizeof(g_level));
    press(PIN_BTN_MID, 1);                 // already held when the MCU boots
    buttons_init();
    check("held-at-boot button reads as down (boot gate)", buttons_down(BTN_MID), 1);
    advance(60, 5);
    check("held-at-boot produces no press event", buttons_pressed(BTN_MID), 0);
    buttons_flush();                       // what ui_power_on_sequence() does
    check("after flush the held button reads as up", buttons_down(BTN_MID), 0);
    advance(500, 5);
    check("still no event while it stays held", buttons_pressed(BTN_MID), 0);
    press(PIN_BTN_MID, 0); advance(100, 5);
    check("release after flush produces no event", buttons_pressed(BTN_MID), 0);
    press(PIN_BTN_MID, 1); advance(100, 5);
    check("next real press works", buttons_pressed(BTN_MID), 1);

    // Case 7: a short click fires on release only, never on the press edge
    memset(g_level, 0, sizeof(g_level)); buttons_init();
    press(PIN_BTN_MID, 1); advance(200, 5);
    check("short click: nothing while still held", buttons_clicked(BTN_MID), 0);
    check("short click: no long event while held", buttons_long(BTN_MID), 0);
    press(PIN_BTN_MID, 0); advance(60, 5);
    check("short click fires on release", buttons_clicked(BTN_MID), 1);
    check("short click fires only once", buttons_clicked(BTN_MID), 0);
    check("short click produces no long event", buttons_long(BTN_MID), 0);

    // Case 8: a long press fires once at the threshold and suppresses the click
    press(PIN_BTN_MID, 1);
    int longs = 0;
    uint32_t held_at_long = 0;
    for (int i = 0; i < 400; i++)
    {
        advance(5, 5);
        if (buttons_long(BTN_MID)) { longs++; if (!held_at_long) held_at_long = buttons_held_ms(BTN_MID); }
    }
    check("long press fires exactly once while held 2s", longs, 1);
    printf("  info long event after %u ms of (debounced) holding\n", (unsigned)held_at_long);
    check("long press fires at BUTTONS_LONG_MS",
          held_at_long >= BUTTONS_LONG_MS && held_at_long < BUTTONS_LONG_MS + 10, 1);
    press(PIN_BTN_MID, 0); advance(60, 5);
    check("release after long press: no click", buttons_clicked(BTN_MID), 0);
    check("release after long press: no second long", buttons_long(BTN_MID), 0);

    // Case 9: a press held when the state changes (flush) gives neither event
    press(PIN_BTN_MID, 1); advance(100, 5);
    buttons_flush();
    advance(1000, 5);
    check("flushed press: no long event", buttons_long(BTN_MID), 0);
    press(PIN_BTN_MID, 0); advance(60, 5);
    check("flushed press: no click on release", buttons_clicked(BTN_MID), 0);

    // Case 10: the wake press held at boot is neither long nor a click
    memset(g_level, 0, sizeof(g_level));
    press(PIN_BTN_MID, 1); buttons_init();
    advance(1000, 5);
    check("held-at-boot: no long event", buttons_long(BTN_MID), 0);
    press(PIN_BTN_MID, 0); advance(60, 5);
    check("held-at-boot: no click on release", buttons_clicked(BTN_MID), 0);

    // Case 11: LEFT/RIGHT repeat is unchanged by the click/long logic
    memset(g_level, 0, sizeof(g_level)); buttons_init();
    press(PIN_BTN_LEFT, 1); advance(40, 5);
    repeats = 0;
    if (buttons_repeat(BTN_LEFT)) repeats++;
    check("left: first repeat fires on the press edge", repeats, 1);
    for (int i = 0; i < 100; i++) { advance(10, 5); if (buttons_repeat(BTN_LEFT)) repeats++; }
    check("left: auto repeat rate is 4-12 per second", repeats >= 5 && repeats <= 13, 1);
    press(PIN_BTN_LEFT, 0); advance(60, 5);
    int after = 0;
    for (int i = 0; i < 20; i++) { advance(10, 5); if (buttons_repeat(BTN_LEFT)) after++; }
    check("left: no repeat after release", after, 0);

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASS", failures);
    return failures != 0;
}
