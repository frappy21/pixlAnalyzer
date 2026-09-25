/**
 * Screen framework: every screen of the application is an app_screen_t, the
 * open ones sit on a small navigation stack and the main loop ticks the top.
 *
 * Two kinds of entries:
 *  - a screen has tick() (enter/leave optional). Its tick polls its own
 *    buttons through the app_* helpers below and draws when app_take_redraw()
 *    says so or when it has new data.
 *  - an action item has action() and no tick(). The menu runs it and stays
 *    in the menu (Freeze, Set ref, Sleep, ...).
 *
 * Every entry is listed once in screens.c, which is also where the menu gets
 * its rows from.
 *
 * Main screens: the bottom of the stack is one of the main screens (Spectrum,
 * WiFi, BLE, RC, listed in APP_HOME_LIST in screens.c), the carousel. On a
 * main screen the long presses are global (main loop): long LEFT/RIGHT switch
 * to the previous/next main screen, long MID opens the menu. Short clicks
 * belong to the screen, without auto repeat there.
 *
 * Global back: on any other screen, a long LEFT press pops the screen (main
 * loop). A short LEFT click is the screen's own, read with app_left().
 */
#ifndef PIXLA_APP_H
#define PIXLA_APP_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    APP_GROUP_HIDDEN = 0, // never in a menu, opened by another screen
    APP_GROUP_SPECTRUM,  // RF tools: spectrum, meter, identify, top
    APP_GROUP_RECEIVE,   // radios: BLE, Zigbee, ESB, sniffer
    APP_GROUP_RC,        // RC and drones: the dash, the radar
    APP_GROUP_NFC,       // the NFC antenna
    APP_GROUP_TRANSMIT,  // the TX lab
    APP_GROUP_TOOLS,     // sentry, log, presets, help
    APP_GROUP_SYSTEM,    // rows of the top level menu itself
    APP_GROUP_COUNT
} app_group_t;

typedef struct app_screen
{
    const char *name; // menu row label
    uint8_t group;    // app_group_t

    void (*enter)(void);        // opened (pushed), optional
    void (*tick)(uint32_t now); // one main loop pass while on top; NULL for an action item
    void (*leave)(void);        // closed (popped), optional. Stop radios, LEDs, carriers here.
    void (*action)(void);       // action item only: run from the menu
    const char *(*value)(void); // optional right aligned text in the menu row

    // The main loop idles 20ms between ticks unless the screen is busy
    // (sweeping, listening, measuring)
    bool busy;
} app_screen_t;

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

#define APP_STACK_DEPTH 6

// Installs the first main screen as the home screen (bottom of the stack,
// never popped) and calls its enter(). Also starts the uptime and inactivity
// clocks. The main screens come from screens.c.
void app_init(void);

const app_screen_t *app_current(void);
bool app_at_home(void);

// Push a screen and call its enter(). The screen below stays on the stack
// without a leave() and gets no ticks until it is on top again. A full stack
// replaces the top screen instead (leave() is called on it).
// A main screen is never pushed: opening one selects it as the home screen.
void app_open(const app_screen_t *screen);

// Carousel. Both pop everything above the home screen, leave() the old home,
// enter() the new one and show its name for a moment.
void app_home_switch(int direction); // +1 next, -1 previous, wraps around
void app_home_select(const app_screen_t *screen);
bool app_is_main(const app_screen_t *screen);

// Called by the main loop after the tick: expires the name banner
void app_banner_update(uint32_t now);

// Call leave() on the top screen and pop it. The screen revealed below is not
// re-entered, it only gets a redraw. Does nothing on the home screen.
void app_back(void);

// Pop everything down to the home screen
void app_home(void);

// Every transition above flushes the buttons (whatever is still held belongs
// to the screen that was left) and requests a redraw.

// ---------------------------------------------------------------------------
// Redraw request
// ---------------------------------------------------------------------------

void app_redraw(void);

// Returns the pending redraw request and clears it
bool app_take_redraw(void);

// ---------------------------------------------------------------------------
// Input helpers. Each one that returns true also counts as user input
// (undims the display, restarts the inactivity timer).
// ---------------------------------------------------------------------------

bool app_left(void);    // LEFT click, on release. Never fires for a long hold (that is back).
bool app_right(void);   // RIGHT press with auto repeat; a click on a main screen (long is global)
bool app_ok(void);      // MID click, on release
bool app_ok_long(void); // MID long press, fires once while held

// Press edge of any button. Only for screens that close on it: the close
// flushes the held button, so a long LEFT cannot also go back a second level.
bool app_any(void);

// Count something else as user input (hold gestures read with buttons_down)
void app_note_input(void);

// ---------------------------------------------------------------------------
// Housekeeping state, used by main.c
// ---------------------------------------------------------------------------

uint32_t app_boot_ms(void);
uint32_t app_last_input_ms(void);
bool app_dimmed(void);
void app_dim(void);

// Power exits, implemented in main.c. Neither returns.
void go_to_sleep(const char *reason);
void go_to_dfu(uint32_t hold_ms);

// ---------------------------------------------------------------------------
// Shared scratch arena
// ---------------------------------------------------------------------------

// One buffer for screens that are never open at the same time. Its content is
// only valid while the screen that wrote it is open: another screen may have
// used it in between, so a screen initialises what it needs in enter() and
// never expects anything to survive a leave(). A screen that opens another
// screen on top of itself must not keep data here either. Nothing uses it yet,
// so --gc-sections drops it: the 16KB show up in bss with the first user.
#define APP_ARENA_SIZE 16384

extern uint8_t g_app_arena[APP_ARENA_SIZE];

#endif // PIXLA_APP_H
