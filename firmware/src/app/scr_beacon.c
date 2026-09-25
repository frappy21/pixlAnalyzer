/**
 * BLE beacon transmitter screen.
 *
 * Configuration rows: the payload preset (see ble_beacon.h), the power and
 * the advertising interval. Starting is deliberate like the TX test screen:
 * a one second hold of MID. While it runs, any button stops it, it also
 * stops itself after BLE_BEACON_MAX_MS, and leaving the screen by any route
 * goes through beacon_leave() first.
 *
 * The preset and interval persist in the settings, so a lab setup survives
 * a reboot; the power always starts at the lowest step.
 */
#include <string.h>

#include "app.h"
#include "ble_beacon.h"
#include "buttons.h"
#include "screens.h"
#include "settings.h"
#include "tx_test.h"
#include "ui.h"
#include "ui_beacon.h"

enum
{
    ROW_BACK = 0,
    ROW_TYPE,
    ROW_PWR,
    ROW_INTV,
    ROW_COUNT
};

static bool m_active;
static bool m_edit;
static uint8_t m_row_sel;

static uint8_t m_type;
static uint8_t m_power = TX_POWER_MIN;
static uint8_t m_intv_sel;

static uint16_t interval_ms(void)
{
    return ble_beacon_intervals[m_intv_sel < BLE_BEACON_INTV_COUNT ? m_intv_sel : 0];
}

static void beacon_enter(void)
{
    m_active = false;
    m_edit = false;
    m_row_sel = 0;

    m_type = g_settings.beacon_type < BLE_BEACON_TYPE_COUNT ? g_settings.beacon_type
                                                            : BLE_BEACON_NAME;
    m_intv_sel = g_settings.beacon_int < BLE_BEACON_INTV_COUNT ? g_settings.beacon_int : 1;
    m_power = TX_POWER_MIN;
}

static void beacon_leave(void)
{
    if (ble_beacon_active())
    {
        ble_beacon_stop();
        ui_message("BEACON OFF", 0, 600);
    }
    m_active = false;

    if (g_settings.beacon_type != m_type || g_settings.beacon_int != m_intv_sel)
    {
        g_settings.beacon_type = m_type;
        g_settings.beacon_int = m_intv_sel;
        settings_mark_dirty();
    }
    if (settings_dirty())
        settings_save();
}

static void config_tick(void)
{
    if (m_edit)
    {
        if (app_left() || app_right())
        {
            int dir = app_left() ? -1 : 1;
            switch (m_row_sel)
            {
            case ROW_TYPE:
                m_type = (uint8_t)((m_type + BLE_BEACON_TYPE_COUNT + dir) % BLE_BEACON_TYPE_COUNT);
                break;
            case ROW_PWR:
                m_power = (uint8_t)((m_power + TX_POWER_COUNT + dir) % TX_POWER_COUNT);
                break;
            case ROW_INTV:
                m_intv_sel = (uint8_t)((m_intv_sel + BLE_BEACON_INTV_COUNT + dir) % BLE_BEACON_INTV_COUNT);
                break;
            default:
                break;
            }
            app_redraw();
        }
        if (app_ok())
        {
            m_edit = false;
            app_redraw();
        }
        return;
    }

    if (app_left())
    {
        m_row_sel = (uint8_t)((m_row_sel + ROW_COUNT - 1) % ROW_COUNT);
        app_redraw();
    }
    if (app_right())
    {
        m_row_sel = (uint8_t)((m_row_sel + 1) % ROW_COUNT);
        app_redraw();
    }

    if (app_ok())
    {
        if (m_row_sel == ROW_BACK)
        {
            app_back();
            return;
        }
        m_edit = true;
        app_redraw();
        return;
    }

    // Deliberate hold, so a stray press can never start transmitting
    if (buttons_down(BTN_MID) && buttons_held_ms(BTN_MID) > 1000)
    {
        app_note_input();
        if (ble_beacon_start(m_type, m_power, interval_ms(), 0))
        {
            m_active = true;
            buttons_flush();
            app_redraw();
        }
        return;
    }

    if (app_take_redraw())
        ui_beacon_config(m_type, m_row_sel, m_power, interval_ms(), m_edit);
}

static void active_tick(uint32_t now)
{
    ble_beacon_update(now);

    if (!ble_beacon_active())
    {
        ui_message("BEACON OFF", "TIME LIMIT", 800);
        m_active = false;
        app_redraw();
        return;
    }

    if (app_any())
    {
        app_back();
        return;
    }

    (void)app_take_redraw();
    ui_beacon_active(m_type, ble_beacon_sent(), ble_beacon_remaining_ms(now));
}

static void beacon_tick(uint32_t now)
{
    if (m_active)
        active_tick(now);
    else
        config_tick();
}

const app_screen_t scr_beacon = {
    .name = "Beacon TX",
    .group = APP_GROUP_TRANSMIT,
    .enter = beacon_enter,
    .tick = beacon_tick,
    .leave = beacon_leave,
};
