/**
 * Screen registry and the few hooks screens share with each other.
 *
 * The registry itself (screens.c) is the list of every screen and action item
 * in menu order. To add a screen: define a const app_screen_t in its own
 * scr_*.c file and add one line for it to APP_SCREEN_LIST in screens.c.
 */
#ifndef PIXLA_SCREENS_H
#define PIXLA_SCREENS_H

#include <stdint.h>

#include "app.h"

extern const app_screen_t *const g_app_screens[];
extern const uint8_t g_app_screen_count;

// Opened by name from main.c and the menu
extern const app_screen_t scr_scanner; // home
extern const app_screen_t scr_menu;    // top level menu
extern const app_screen_t scr_group;   // submenu of one group

// Scanner state other screens work from (scr_scanner.c)
uint16_t scr_scanner_marker_mhz(void);
void scr_scanner_apply_band(void);

#endif // PIXLA_SCREENS_H
