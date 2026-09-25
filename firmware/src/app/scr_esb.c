/** ShockBurst / nRF24 scan: which cheap 2.4GHz mice and keyboards are on air. */
#include "esb_scan.h"
#include "screens.h"
#include "ui_esb.h"

// Scanned in short slices, one per main loop pass, so the buttons are polled
// often enough to debounce a normal click
#define ESB_DWELL_MS 30 // per MHz: 15ms at 2Mbit + 7.5ms at 1Mbit
#define ESB_SLICE_MHZ 1
#define ESB_UPDATE_MHZ 10

// One pass over the band the cheap radios use, in slices so the UI stays alive
static uint16_t m_next_mhz = 2400;

static void esb_enter(void)
{
    esb_scan_reset();
}

static void esb_leave(void)
{
    m_next_mhz = 2400;
}

static void esb_tick(uint32_t now)
{
    static uint8_t since_draw_mhz;

    if (app_take_redraw())
        ui_esb_list(esb_scan_total());

    uint16_t end = m_next_mhz + ESB_SLICE_MHZ - 1;
    if (end > 2483)
        end = 2483;

    esb_scan_run(m_next_mhz, end, ESB_DWELL_MS);
    since_draw_mhz = (uint8_t)(since_draw_mhz + (end - m_next_mhz + 1));
    m_next_mhz = (end >= 2483) ? 2400 : (uint16_t)(end + 1);
    if (since_draw_mhz >= ESB_UPDATE_MHZ || m_next_mhz == 2400)
    {
        since_draw_mhz = 0;
        app_redraw();
    }

    if (app_any())
        app_back();
}

const app_screen_t scr_esb = {
    .name = "Mouse/Kbd",
    .group = APP_GROUP_RECEIVE,
    .enter = esb_enter,
    .tick = esb_tick,
    .leave = esb_leave,
    .busy = true,
};
