/**
 * Transmitter test: a confirmation page, then the carrier itself. Both are
 * one screen, so leaving it by any route (a button, global back, timeout)
 * goes through tx_leave() and can never leave a carrier on the air.
 */
#include "buttons.h"
#include "screens.h"
#include "tx_test.h"
#include "ui.h"
#include "ui_tx.h"

static uint16_t m_tx_mhz = 2440;
static uint8_t m_tx_power = TX_POWER_MIN;
static bool m_active;

static void tx_enter(void)
{
    m_tx_mhz = scr_scanner_marker_mhz();
    m_active = false;
}

static void tx_leave(void)
{
    if (tx_test_active())
    {
        tx_test_stop();
        ui_message("TX STOPPED", 0, 600);
    }
    m_active = false;
}

static void tx_confirm_tick(void)
{
    if (app_left())
    {
        m_tx_power = (uint8_t)((m_tx_power + TX_POWER_COUNT - 1) % TX_POWER_COUNT);
        app_redraw();
    }
    if (app_right())
    {
        m_tx_power = (uint8_t)((m_tx_power + 1) % TX_POWER_COUNT);
        app_redraw();
    }

    // Deliberate hold, so a stray press can never put a carrier on the air.
    // A press still held from the menu does not count: the transition that
    // opened this screen flushed it.
    if (buttons_down(BTN_MID) && buttons_held_ms(BTN_MID) > 1000)
    {
        app_note_input();
        if (tx_test_start(m_tx_mhz, m_tx_power))
        {
            // Same flush as a screen change: the MID still held here must
            // not count as the "any button" that stops the carrier
            m_active = true;
            buttons_flush();
            app_redraw();
            return;
        }
        ui_message("TX FAILED", 0, 800);
        app_back();
        return;
    }
    // A short click means "no thanks"
    if (app_ok())
    {
        app_back();
        return;
    }

    if (app_take_redraw())
        ui_tx_confirm(m_tx_mhz, m_tx_power);
}

static void tx_active_tick(uint32_t now)
{
    tx_test_update(now);

    if (!tx_test_active())
    {
        ui_message("TX STOPPED", 0, 600);
        app_back();
        return;
    }

    // Stops on the press edge of any button, not on release: nothing may
    // delay switching the carrier off. tx_leave() does the stopping.
    if (app_any())
    {
        app_back();
        return;
    }

    (void)app_take_redraw(); // redrawn on every pass for the countdown
    ui_tx_active(m_tx_mhz, m_tx_power, tx_test_remaining_ms(now));
}

static void tx_tick(uint32_t now)
{
    if (m_active)
        tx_active_tick(now);
    else
        tx_confirm_tick();
}

const app_screen_t scr_tx = {
    .name = "TX test",
    .group = APP_GROUP_TRANSMIT,
    .enter = tx_enter,
    .tick = tx_tick,
    .leave = tx_leave,
};
