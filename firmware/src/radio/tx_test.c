#include "nrf.h"

#include "scanner.h"
#include "tx_test.h"

static bool m_active;
static uint32_t m_started_ms;

static const uint8_t power_reg[TX_POWER_COUNT] = {
    RADIO_TXPOWER_TXPOWER_Neg40dBm,
    RADIO_TXPOWER_TXPOWER_Neg20dBm,
    RADIO_TXPOWER_TXPOWER_Neg8dBm,
    RADIO_TXPOWER_TXPOWER_0dBm,
};

static bool wait_event(volatile uint32_t *event)
{
    for (uint32_t i = 0; i < 200000; i++)
    {
        if (*event)
        {
            *event = 0;
            return true;
        }
    }
    return false;
}

bool tx_test_start(uint16_t mhz, uint8_t power)
{
    if (mhz < 2400 || mhz > 2500 || power >= TX_POWER_COUNT)
        return false;

    tx_test_stop();

    // The HFXO must be running for a clean carrier
    if ((NRF_CLOCK->HFCLKSTAT & CLOCK_HFCLKSTAT_STATE_Msk) == 0)
    {
        NRF_CLOCK->EVENTS_HFCLKSTARTED = 0;
        NRF_CLOCK->TASKS_HFCLKSTART = 1;
        for (uint32_t guard = 0; guard < 1000000; guard++)
        {
            if (NRF_CLOCK->EVENTS_HFCLKSTARTED)
                break;
        }
    }

    NRF_RADIO->SHORTS = 0;
    NRF_RADIO->MODE = RADIO_MODE_MODE_Ble_1Mbit << RADIO_MODE_MODE_Pos;
    NRF_RADIO->FREQUENCY = mhz - 2400;
    NRF_RADIO->TXPOWER = power_reg[power];

    // Ramping up without ever issuing TASKS_START leaves the transmitter in
    // TXIDLE, which is an unmodulated carrier
    NRF_RADIO->EVENTS_READY = 0;
    NRF_RADIO->TASKS_TXEN = 1;
    if (!wait_event(&NRF_RADIO->EVENTS_READY))
        return false;

    m_active = true;
    m_started_ms = 0; // stamped on the first update call
    return true;
}

void tx_test_stop(void)
{
    if (!m_active)
        return;

    NRF_RADIO->EVENTS_DISABLED = 0;
    NRF_RADIO->TASKS_DISABLE = 1;
    wait_event(&NRF_RADIO->EVENTS_DISABLED);
    NRF_RADIO->TXPOWER = RADIO_TXPOWER_TXPOWER_0dBm;

    m_active = false;
    scanner_init();
}

bool tx_test_active(void) { return m_active; }

void tx_test_update(uint32_t now_ms)
{
    if (!m_active)
        return;

    if (m_started_ms == 0)
        m_started_ms = now_ms;

    if (now_ms - m_started_ms >= TX_TEST_MAX_MS)
    {
        tx_test_stop();
        m_started_ms = 0;
    }
}

uint32_t tx_test_remaining_ms(uint32_t now_ms)
{
    if (!m_active || m_started_ms == 0)
        return 0;

    uint32_t elapsed = now_ms - m_started_ms;
    return elapsed >= TX_TEST_MAX_MS ? 0 : TX_TEST_MAX_MS - elapsed;
}

const char *tx_power_name(uint8_t power)
{
    switch (power)
    {
    case TX_POWER_MIN:
        return "-40dBm";
    case TX_POWER_LOW:
        return "-20dBm";
    case TX_POWER_MID:
        return "-8dBm";
    case TX_POWER_MAX:
        return "0dBm";
    default:
        return "?";
    }
}
