/**
 * Debounced input layer for the three front buttons (active low, pull ups).
 *
 * buttons_poll() samples the pins and updates the state machines, everything
 * else is a cheap query. Events are edge based and one shot: a query returns
 * true exactly once per physical press, so holding a button can never make the
 * UI act twice or leak a press into the next screen.
 */
#ifndef PIXLA_BUTTONS_H
#define PIXLA_BUTTONS_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    BTN_LEFT = 0,
    BTN_MID,
    BTN_RIGHT,
    BTN_COUNT
} button_t;

void buttons_init(void);

// Call once per main loop iteration, before any query below
void buttons_poll(void);

// Debounced level: is the button down right now
bool buttons_down(button_t btn);
bool buttons_any_down(void);

// A press held this long is a long press instead of a click
#define BUTTONS_LONG_MS 600

// One shot press event on the press edge, consumed by the caller
bool buttons_pressed(button_t btn);

// Short click: fires on release, only when the press was shorter than
// BUTTONS_LONG_MS. Use it where the same button also has a long press.
bool buttons_clicked(button_t btn);

// Long press: fires once while still held, when BUTTONS_LONG_MS is crossed.
// The release that follows produces no click.
bool buttons_long(button_t btn);

// Press event plus auto repeat while the button stays down, for navigation
bool buttons_repeat(button_t btn);

// How long the button has been down, 0 when it is up
uint32_t buttons_held_ms(button_t btn);

// Drop pending events and ignore the buttons that are currently down until
// they are released. Use it after a screen change or after waking up, so the
// press that got us here is not seen again by the new screen.
void buttons_flush(void);

#endif // PIXLA_BUTTONS_H
