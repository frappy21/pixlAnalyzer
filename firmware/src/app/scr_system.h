/** System screens: Presets, Sentry, Help (scr_system.c). */
#ifndef PIXLA_SCR_SYSTEM_H
#define PIXLA_SCR_SYSTEM_H

#include <stdbool.h>

// True while sentry mode is open: it manages the display and the radio duty
// itself, so the main loop leaves out dimming, the screensaver and the
// inactivity sleep
bool scr_sentry_active(void);

#endif // PIXLA_SCR_SYSTEM_H
