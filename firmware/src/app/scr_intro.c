/**
 * First boot tutorial: three pages of how to live with one joystick, shown
 * once on a fresh device (no settings record yet, or a v3 one without the
 * intro flag) and never again once the user has seen it.
 *
 * A press of any button moves to the next page, the third one exits.
 */
#include <stdbool.h>
#include <stdint.h>

#include "buttons.h"
#include "gfx.h"
#include "power.h"
#include "screens.h"
#include "settings.h"
#include "ui.h"

static const char *const c_pages[3][6] = {
    {"THIS IS A 3 BUTTON", "DEVICE:", "", "LEFT / RIGHT: MOVE,", "SELECT, ADJUST", "(HOLD: FAST)"},
    {"MIDDLE: OPEN, ENTER,", "CONFIRM", "", "HOLD IT 1S: THE TX", "TOOLS ARM THEMSELVES", "LIKE THAT"},
    {"LONG LEFT: ALWAYS GO", "BACK ONE SCREEN", "", "MENU > SYSTEM HAS", "SETTINGS, SLEEP, DFU", "GOOD LUCK!"},
};

static void intro_page(uint8_t page)
{
    display_clear();
    ui_title("HELLO");

    for (uint8_t i = 0; i < 6; i++)
    {
        if (c_pages[page][i][0])
            gfx_text_micro(2, 12 + i * 8, c_pages[page][i]);
    }

    // Page dots
    for (uint8_t i = 0; i < 3; i++)
        gfx_box(58 + i * 5, 60, 3, 2, i == page, true);

    gfx_text_micro(2, 60, "PRESS ANY BUTTON");
    display_flush();
}

void scr_intro_run(void)
{
    for (uint8_t page = 0; page < 3; page++)
    {
        intro_page(page);

        // Any press advances; a held button waits for the release so the
        // next page does not skip
        while (true)
        {
            buttons_poll();
            if (buttons_any_down())
            {
                while (buttons_any_down())
                {
                    buttons_poll();
                    power_watchdog_feed();
                }
                buttons_flush();
                break;
            }
            power_watchdog_feed();
        }
    }

    // Seen it: persist, so it never comes back
    g_settings.intro_done = 1;
    settings_save();
}

const app_screen_t scr_intro = {
    .name = "Intro",
    .group = APP_GROUP_HIDDEN,
};
