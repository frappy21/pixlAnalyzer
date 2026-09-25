/** BLE advertising scan: device list and the detail card of one device. */
#include "ble_scan.h"
#include "screens.h"
#include "ui_ble.h"

// The scan is run in short slices, one per main loop pass, so the buttons are
// polled often enough to debounce a normal click. The screen is updated after
// the same amount of listening as before.
#define BLE_SLICE_MS 30    // 10ms on each advertising channel
#define BLE_UPDATE_MS 300  // listening time between two list updates

static uint8_t m_ble_sel;

extern const app_screen_t scr_ble_detail;

// ---------------------------------------------------------------------------
// Device list
// ---------------------------------------------------------------------------

static void ble_enter(void)
{
    ble_scan_reset();
    m_ble_sel = 0;
}

static void ble_tick(uint32_t now)
{
    static uint32_t listened_ms;

    ble_scan_run(BLE_SLICE_MS);
    listened_ms += BLE_SLICE_MS;
    if (listened_ms >= BLE_UPDATE_MS)
    {
        listened_ms = 0;
        app_redraw();
    }

    if (app_left())
    {
        if (m_ble_sel)
            m_ble_sel--;
        app_redraw();
    }
    if (app_right())
    {
        if (m_ble_sel + 1 < ble_scan_count())
            m_ble_sel++;
        app_redraw();
    }
    if (app_ok())
    {
        if (ble_scan_count())
            app_open(&scr_ble_detail);
        else
            app_back();
        return;
    }

    if (app_take_redraw())
        ui_ble_list(m_ble_sel, ble_scan_packets());
}

const app_screen_t scr_ble = {
    .name = "BLE scan",
    .group = APP_GROUP_RECEIVE,
    .enter = ble_enter,
    .tick = ble_tick,
    .busy = true,
};

// ---------------------------------------------------------------------------
// Device detail, opened from the list
// ---------------------------------------------------------------------------

static void ble_detail_tick(uint32_t now)
{
    if (app_take_redraw())
    {
        uint8_t idx[BLE_MAX_DEVICES];
        uint8_t n = ble_scan_sorted(idx, BLE_MAX_DEVICES);
        const ble_dev_t *dev = (m_ble_sel < n) ? ble_scan_device(idx[m_ble_sel]) : 0;
        if (dev)
            ui_ble_detail(dev);
    }

    if (app_any())
        app_back();
}

const app_screen_t scr_ble_detail = {
    .name = "BLE device",
    .group = APP_GROUP_HIDDEN,
    .tick = ble_detail_tick,
};
