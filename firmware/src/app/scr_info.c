/** Info screen: reset reason, uptime, battery and flash diagnostics. */
#include "battery.h"
#include "screens.h"
#include "spectrum.h"
#include "ui_info.h"

static void info_tick(uint32_t now)
{
    if (app_take_redraw())
    {
        battery_update();
        ui_info((now - app_boot_ms()) / 1000u, spectrum_sweeps(), spectrum_history_rows());
    }

    if (app_any())
        app_back();
}

const app_screen_t scr_info = {
    .name = "Info",
    .group = APP_GROUP_SYSTEM,
    .tick = info_tick,
};
