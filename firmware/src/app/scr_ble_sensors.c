/**
 * Sensor dashboard: temperature, humidity and battery of the BLE thermometers
 * around, decoded from their adverts (formats in ble_sensor.h).
 *
 * LEFT/RIGHT move the selection, the footer shows its signal and age. A long
 * LEFT goes back. The list lives in the arena while the screen is open.
 */
#include <string.h>

#include "ble_scan.h"
#include "screens.h"
#include "ui_ble.h"

#define SENSOR_SLICE_MS 30
#define SENSOR_UPDATE_MS 600

// Pushed screen: the upper half of the arena
#define SENSOR_CAP 32
#define SENSOR_LIST ((ble_sensor_entry_t *)(void *)&g_app_arena[8192])

_Static_assert(SENSOR_CAP * sizeof(ble_sensor_entry_t) <= 8192, "sensor list exceeds the upper half");

static uint8_t m_count;
static uint8_t m_sel;

static ble_sensor_entry_t *find(const ble_rx_t *rx)
{
    ble_sensor_entry_t *list = SENSOR_LIST;
    for (uint8_t i = 0; i < m_count; i++)
    {
        if (list[i].addr_type == rx->addr_type && memcmp(list[i].addr, rx->addr, 6) == 0)
            return &list[i];
    }
    return 0;
}

static void on_packet(const ble_rx_t *rx)
{
    if (!rx->addr)
        return;

    ble_sensor_entry_t *e = find(rx);

    // A scan response often carries the name of a sensor already listed
    ble_ad_summary_t sum;
    ble_ad_summarize(rx->ad, rx->ad_len, &sum);

    ble_sensor_t s;
    if (ble_sensor_decode(rx->ad, rx->ad_len, &s))
    {
        if (!e)
        {
            if (m_count >= SENSOR_CAP)
                return;
            e = &SENSOR_LIST[m_count++];
            memset(e, 0, sizeof(*e));
            memcpy(e->addr, rx->addr, 6);
            e->addr_type = rx->addr_type;
        }
        // Keep the fields this advert does not carry (BTHome and MiBeacon
        // send temperature and battery in different adverts)
        e->s.fmt = s.fmt;
        if (s.valid & BLE_SENSOR_TEMP)
            e->s.temp = s.temp;
        if (s.valid & BLE_SENSOR_HUM)
            e->s.hum = s.hum;
        if (s.valid & BLE_SENSOR_BATT)
            e->s.batt = s.batt;
        if (s.valid & BLE_SENSOR_MV)
            e->s.mv = s.mv;
        e->s.valid = (uint8_t)((e->s.valid & ~BLE_SENSOR_ENCRYPTED) | s.valid);
    }
    if (!e)
        return;

    e->rssi = rx->rssi;
    e->last_ms = rx->now_ms;
    if (e->packets < 0xFFFF)
        e->packets++;
    if (sum.name && sum.name_len)
    {
        uint8_t n = sum.name_len < sizeof(e->name) - 1 ? sum.name_len : sizeof(e->name) - 1;
        memcpy(e->name, sum.name, n);
        e->name[n] = '\0';
    }
}

static void sensors_enter(void)
{
    m_count = 0;
    m_sel = 0;
    ble_scan_set_listener(on_packet);
}

static void sensors_leave(void)
{
    ble_scan_set_listener(0);
}

static void sensors_tick(uint32_t now)
{
    static uint32_t listened_ms;

    ble_scan_run(SENSOR_SLICE_MS);
    listened_ms += SENSOR_SLICE_MS;
    if (listened_ms >= SENSOR_UPDATE_MS)
    {
        listened_ms = 0;
        app_redraw();
    }

    if (app_left())
    {
        if (m_sel)
            m_sel--;
        app_redraw();
    }
    if (app_right())
    {
        if (m_sel + 1 < m_count)
            m_sel++;
        app_redraw();
    }

    if (app_take_redraw())
        ui_ble_sensors(SENSOR_LIST, m_count, m_sel, now);
}

const app_screen_t scr_ble_sensors = {
    .name = "BLE sensors",
    .group = APP_GROUP_RECEIVE,
    .enter = sensors_enter,
    .tick = sensors_tick,
    .leave = sensors_leave,
    .busy = true,
};
