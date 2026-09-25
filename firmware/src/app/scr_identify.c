/** Identify: park on the marker, measure, then say what it looks like. */
#include <string.h>

#include "app_config.h"
#include "ble_scan.h"
#include "classify.h"
#include "power.h"
#include "scanner.h"
#include "screens.h"
#include "ui.h"

// Identify capture buffer, 2KB of the 58KB we have spare
static burst_t m_bursts[256];
static verdict_t m_verdict;
static bool m_ble_confirmed;
static uint16_t m_ble_confirm_packets;

static void run_identify(void)
{
    uint16_t mhz = scr_scanner_marker_mhz();
    park_stats_t stats;
    uint16_t total = 0;

    // Four 250ms windows so the progress bar moves and the watchdog is fed
    park_stats_t acc;
    memset(&acc, 0, sizeof(acc));
    acc.floor_rssi = RSSI_INVALID; // "nothing measured", not 0dBm
    acc.peak_rssi = RSSI_INVALID;

    const uint16_t capacity = (uint16_t)(sizeof(m_bursts) / sizeof(m_bursts[0]));

    for (int i = 0; i < 4; i++)
    {
        // A full buffer makes scanner_park return at once with no window
        if (total >= capacity)
            break;

        ui_identify_progress(mhz, (uint8_t)(i * 25));
        memset(&stats, 0, sizeof(stats)); // a failed window must not add stale numbers
        uint16_t n = scanner_park(mhz, 250, &m_bursts[total], (uint16_t)(capacity - total),
                                  &stats);
        power_watchdog_feed();

        // No window means nothing was measured: merging the zeroed stats
        // would turn floor and peak into 0dBm
        if (stats.window_us == 0)
            continue;

        // Burst timestamps restart with every window, so shift them
        for (uint16_t k = 0; k < n; k++)
            m_bursts[total + k].start_us += (uint32_t)i * 250000u;

        total = (uint16_t)(total + n);
        acc.window_us += stats.window_us;
        acc.on_us += stats.on_us;
        acc.bursts = (uint16_t)(acc.bursts + stats.bursts);
        acc.dropped = (uint16_t)(acc.dropped + stats.dropped);
        acc.floor_rssi = stats.floor_rssi;
        if (stats.peak_rssi < acc.peak_rssi)
            acc.peak_rssi = stats.peak_rssi;
    }

    classify_run(mhz, m_bursts, total, &acc, &m_verdict);

    // On an advertising channel we do not have to guess: receive the packets
    m_ble_confirmed = false;
    m_ble_confirm_packets = 0;
    if (mhz == 2402 || mhz == 2426 || mhz == 2480)
    {
        ui_identify_progress(mhz, 90);
        ble_scan_reset();
        m_ble_confirm_packets = ble_scan_run(600);
        m_ble_confirmed = m_ble_confirm_packets > 0;
    }
}

static void identify_tick(uint32_t now)
{
    if (app_take_redraw())
    {
        run_identify();
        ui_identify(scr_scanner_marker_mhz(), &m_verdict, m_ble_confirmed,
                    m_ble_confirm_packets);
    }

    if (app_ok())
    {
        app_back();
        return;
    }
    if (app_left() || app_right())
        app_redraw(); // measure again
}

const app_screen_t scr_identify = {
    .name = "Identify",
    .group = APP_GROUP_RECEIVE,
    .tick = identify_tick,
    .busy = true,
};
