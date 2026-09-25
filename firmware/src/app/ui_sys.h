/** Rendering of the system screens: boot additions, crash report, help, sentry. */
#ifndef PIXLA_UI_SYS_H
#define PIXLA_UI_SYS_H

#include <stdbool.h>
#include <stdint.h>

// Display overlay for the boot screen: firmware version and build hash in the
// top corners of its frame (the boot screen itself is drawn by ui.c)
void ui_sys_boot_overlay(void);

// Shows a crash record nobody has seen yet, until a button is pressed or
// timeout_ms passes. Runs before the watchdog is started.
void ui_sys_crash_report(uint32_t timeout_ms);

// Help text, scrolled so that line first is at the top
uint8_t ui_help_lines(void);
uint8_t ui_help_rows(void); // lines that fit on the screen
void ui_help(uint8_t first);

typedef struct
{
    bool alert;        // activity seen and not acknowledged yet
    bool dimmed;       // armed and dark
    uint8_t threshold; // dB above the floor
    uint16_t period_ms;
    uint32_t sweeps;
    uint32_t alerts;
    uint16_t last_mhz; // strongest channel of the last alert, 0 before the first
    uint8_t last_db;
    uint32_t last_age_s;
} sentry_view_t;

void ui_sentry(const sentry_view_t *view);

// Banner of a sentry alert, drawn by every flush while the alert stands
void ui_sentry_banner(uint16_t mhz, uint8_t db);

#endif // PIXLA_UI_SYS_H
