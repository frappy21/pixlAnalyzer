/**
 * BLE advertising scan: device list (the session log), the detail card of one
 * device, the hunt view that homes in on it, and the menu options of the list.
 *
 * Main screen buttons: LEFT/RIGHT move the selection, MID opens the detail
 * card. Detail card: LEFT/RIGHT scroll, MID starts the hunt. Hunt: MID clears
 * the trend. A long LEFT leaves the detail card and the hunt.
 *
 * The device table lives in the arena while this screen is the home screen,
 * so the log covers the time spent here (menus and sub-screens opened on top
 * included) and is gone after a carousel switch.
 */
#include <string.h>

#include "ble_scan.h"
#include "display.h"
#include "led.h"
#include "screens.h"
#include "settings.h"
#include "ui_ble.h"

// The scan is run in short slices, one per main loop pass, so the buttons are
// polled often enough to debounce a normal click. The screen is updated after
// the same amount of listening as before.
#define BLE_SLICE_MS 30    // 10ms on each advertising channel
#define BLE_UPDATE_MS 300  // listening time between two list updates
#define BLE_DETAIL_UPDATE_MS 600

// Arena layout. This is a main screen, so its table sits in the lower half;
// the detail card and the hunt are pushed on top and use the upper half.
#define BLE_TABLE_CAP 64
#define BLE_DETAIL_LINES 40
#define BLE_HUNT_TREND (DISP_W - 4)
#define BLE_ARENA_TABLE ((ble_dev_t *)(void *)&g_app_arena[0])
#define BLE_ARENA_LINES ((char(*)[BLE_LINE_LEN])(void *)&g_app_arena[8192])
#define BLE_ARENA_TREND ((int8_t *)&g_app_arena[8192 + BLE_DETAIL_LINES * BLE_LINE_LEN])

_Static_assert(BLE_TABLE_CAP * sizeof(ble_dev_t) <= 8192, "BLE table exceeds the main half");
_Static_assert(BLE_DETAIL_LINES * BLE_LINE_LEN + BLE_HUNT_TREND <= 8192,
               "BLE sub-screens exceed the upper half");

// Hunt LED: blink rate range and how long a device may be silent
#define HUNT_LOST_MS 3000
#define HUNT_SAMPLE_MS 250

// Session options, set from the menu while the BLE screen is the home screen
static uint8_t m_filter = BLE_FILTER_ALL;
static uint8_t m_sort = BLE_SORT_RSSI;
static uint8_t m_follow_mode = BLE_FOLLOW_TRACKERS;
static bool m_spam_alert = true;

// Selection, kept by address so it stays on the device when the order changes
static uint8_t m_sel;
static uint8_t m_sel_addr[6];
static uint8_t m_sel_type = 0xFF; // none

static uint8_t m_following_shown;

extern const app_screen_t scr_ble_detail;
extern const app_screen_t scr_ble_hunt;

static void select_at(const uint8_t *idx, uint8_t n, uint8_t pos)
{
    m_sel = pos;
    if (pos < n)
    {
        const ble_dev_t *d = ble_devtab_device(idx[pos]);
        memcpy(m_sel_addr, d->addr, 6);
        m_sel_type = d->addr_type;
    }
}

static const ble_dev_t *selected_device(void)
{
    return m_sel_type == 0xFF ? 0 : ble_devtab_find(m_sel_addr, m_sel_type);
}

// ---------------------------------------------------------------------------
// Device list
// ---------------------------------------------------------------------------

static void ble_enter(void)
{
    ble_scan_reset();
    ble_devtab_attach(BLE_ARENA_TABLE, BLE_TABLE_CAP);
    m_sel = 0;
    m_sel_type = 0xFF;
    m_following_shown = 0;
}

static void ble_leave(void)
{
    ble_devtab_attach(0, 0);
    led_off();
}

// LED while on the list: quick blinks during a spam flood, a slow one while
// something seems to follow
static void alert_led(uint8_t following, const ble_spam_state_t *spam, uint32_t now)
{
    if (spam && spam->active)
        led_set_rate(4);
    else if (following)
        led_set_rate(1);
    else
        led_set_rate(0);
    led_update(now);
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

    uint8_t following = ble_devtab_following(m_follow_mode);
    const ble_spam_state_t *spam = m_spam_alert ? ble_devtab_spam(now) : 0;
    if (following != m_following_shown)
    {
        m_following_shown = following;
        app_redraw();
    }
    alert_led(following, spam, now);

    uint8_t idx[BLE_TABLE_CAP];
    uint8_t n = ble_devtab_sorted(idx, BLE_TABLE_CAP, m_filter, m_sort);

    // Follow the selected device to wherever the sort moved it
    for (uint8_t i = 0; i < n; i++)
    {
        const ble_dev_t *d = ble_devtab_device(idx[i]);
        if (d->addr_type == m_sel_type && memcmp(d->addr, m_sel_addr, 6) == 0)
        {
            m_sel = i;
            break;
        }
    }
    if (m_sel >= n)
        m_sel = n ? n - 1 : 0;

    if (app_left())
    {
        select_at(idx, n, m_sel ? m_sel - 1 : 0);
        app_redraw();
    }
    if (app_right())
    {
        select_at(idx, n, m_sel + 1 < n ? m_sel + 1 : m_sel);
        app_redraw();
    }
    if (app_ok())
    {
        if (n)
        {
            select_at(idx, n, m_sel);
            app_open(&scr_ble_detail);
        }
        return;
    }

    if (app_take_redraw())
    {
        ble_list_view_t view = {
            .idx = idx,
            .n = n,
            .selected = m_sel,
            .filter = m_filter,
            .packets = ble_scan_packets(),
            .following = following,
            .spam = spam,
            .ext = ble_scan_ext_stats(),
        };
        ui_ble_list(&view);
    }
}

const app_screen_t scr_ble = {
    .name = "BLE scan",
    .group = APP_GROUP_RECEIVE,
    .enter = ble_enter,
    .tick = ble_tick,
    .leave = ble_leave,
    .busy = true,
};

// ---------------------------------------------------------------------------
// Device detail, opened from the list
// ---------------------------------------------------------------------------

static uint8_t m_detail_first;

static void ble_detail_enter(void)
{
    m_detail_first = 0;
    led_off(); // the list's alert blinking pauses while it is covered
}

static void ble_detail_tick(uint32_t now)
{
    static uint32_t listened_ms;

    // Keep listening, the card shows live numbers
    ble_scan_run(BLE_SLICE_MS);
    listened_ms += BLE_SLICE_MS;
    if (listened_ms >= BLE_DETAIL_UPDATE_MS)
    {
        listened_ms = 0;
        app_redraw();
    }

    // A page is 7 lines, a step keeps one line of context
    if (app_left())
    {
        m_detail_first = m_detail_first > UI_BLE_DETAIL_ROWS - 1
                             ? (uint8_t)(m_detail_first - (UI_BLE_DETAIL_ROWS - 1))
                             : 0;
        app_redraw();
    }
    if (app_right())
    {
        m_detail_first = (uint8_t)(m_detail_first + UI_BLE_DETAIL_ROWS - 1);
        app_redraw();
    }

    const ble_dev_t *dev = selected_device();
    if (!dev)
    {
        app_back(); // recycled out of the table
        return;
    }

    if (app_ok())
    {
        app_open(&scr_ble_hunt);
        return;
    }

    if (app_take_redraw())
    {
        uint8_t count = ui_ble_detail_lines(dev, now, BLE_ARENA_LINES, BLE_DETAIL_LINES);
        uint8_t last_first = count > UI_BLE_DETAIL_ROWS ? count - UI_BLE_DETAIL_ROWS : 0;
        if (m_detail_first > last_first)
            m_detail_first = last_first;
        ui_ble_detail(BLE_ARENA_LINES, count, m_detail_first);
    }
}

const app_screen_t scr_ble_detail = {
    .name = "BLE device",
    .group = APP_GROUP_HIDDEN,
    .enter = ble_detail_enter,
    .tick = ble_detail_tick,
    .busy = true,
};

// ---------------------------------------------------------------------------
// Hunt: signal level of one device, for walking towards it
// ---------------------------------------------------------------------------

static uint8_t m_trend_len;
static uint16_t m_hunt_packets;
static int16_t m_hunt_avg; // RSSI x4, smoothed over packets

static void ble_hunt_enter(void)
{
    m_trend_len = 0;
    m_hunt_packets = 0;
    m_hunt_avg = 0;
}

static void ble_hunt_leave(void)
{
    led_off();
}

static void ble_hunt_tick(uint32_t now)
{
    static uint32_t last_sample_ms;
    int8_t *trend = BLE_ARENA_TREND;

    ble_scan_run(BLE_SLICE_MS);

    const ble_dev_t *dev = selected_device();
    if (!dev)
    {
        app_back();
        return;
    }

    // Smooth over the packets heard since the last pass
    if (dev->packets != m_hunt_packets)
    {
        m_hunt_avg = m_hunt_avg ? (int16_t)((m_hunt_avg * 3) / 4 + dev->rssi_last)
                                : (int16_t)(dev->rssi_last * 4);
        m_hunt_packets = dev->packets;
    }
    uint32_t age = now - dev->last_ms;
    int8_t rssi = (age < HUNT_LOST_MS && m_hunt_avg) ? (int8_t)(m_hunt_avg / 4) : 0;

    if (now - last_sample_ms >= HUNT_SAMPLE_MS)
    {
        last_sample_ms = now;
        if (m_trend_len == BLE_HUNT_TREND)
        {
            memmove(trend, trend + 1, BLE_HUNT_TREND - 1);
            m_trend_len--;
        }
        trend[m_trend_len++] = rssi;
        app_redraw();
    }

    // LED clicks faster the closer it gets, like the Meter screen
    if (g_settings.led_hunt)
    {
        int rate = rssi ? 1 + ((rssi + 100) * 14) / 70 : 0;
        led_set_rate((uint8_t)(rate < 0 ? 1 : rate > 15 ? 15 : rate));
    }
    else
    {
        led_set_rate(0);
    }
    led_update(now);

    if (app_ok())
    {
        m_trend_len = 0;
        app_redraw();
    }

    if (app_take_redraw())
        ui_ble_hunt(dev, rssi, age, trend, m_trend_len);
}

const app_screen_t scr_ble_hunt = {
    .name = "BLE hunt",
    .group = APP_GROUP_HIDDEN,
    .enter = ble_hunt_enter,
    .tick = ble_hunt_tick,
    .leave = ble_hunt_leave,
    .busy = true,
};

// ---------------------------------------------------------------------------
// Menu options (Receive group), session only
// ---------------------------------------------------------------------------

static void act_filter(void) { m_filter = (uint8_t)((m_filter + 1) % BLE_FILTER_COUNT); }

static const char *val_filter(void)
{
    static const char *const names[BLE_FILTER_COUNT] = {"ALL", "TRACKERS", "FOLLOWING", "EXT"};
    return names[m_filter];
}

static void act_sort(void) { m_sort = (uint8_t)((m_sort + 1) % BLE_SORT_COUNT); }

static const char *val_sort(void)
{
    static const char *const names[BLE_SORT_COUNT] = {"RSSI", "RECENT", "FIRST"};
    return names[m_sort];
}

static void act_follow(void)
{
    m_follow_mode = (uint8_t)((m_follow_mode + 1) % BLE_FOLLOW_MODE_COUNT);
}

static const char *val_follow(void)
{
    static const char *const names[BLE_FOLLOW_MODE_COUNT] = {"OFF", "TRACKERS", "ALL"};
    return names[m_follow_mode];
}

static void act_spam(void) { m_spam_alert = !m_spam_alert; }

static const char *val_spam(void) { return m_spam_alert ? "ON" : "OFF"; }

// Forget the log. Only while the BLE screen holds the table.
static void act_clear(void)
{
    if (ble_devtab_attached())
    {
        ble_devtab_clear();
        ble_scan_reset();
    }
}

static const char *val_clear(void) { return ble_devtab_attached() ? "" : "-"; }

const app_screen_t act_ble_filter = {
    .name = "BLE list",
    .group = APP_GROUP_RECEIVE,
    .action = act_filter,
    .value = val_filter,
};

const app_screen_t act_ble_sort = {
    .name = "BLE sort",
    .group = APP_GROUP_RECEIVE,
    .action = act_sort,
    .value = val_sort,
};

const app_screen_t act_ble_follow = {
    .name = "Follow alert",
    .group = APP_GROUP_RECEIVE,
    .action = act_follow,
    .value = val_follow,
};

const app_screen_t act_ble_spam = {
    .name = "Spam alert",
    .group = APP_GROUP_RECEIVE,
    .action = act_spam,
    .value = val_spam,
};

const app_screen_t act_ble_clear = {
    .name = "BLE clear log",
    .group = APP_GROUP_RECEIVE,
    .action = act_clear,
    .value = val_clear,
};
