#include <string.h>

#include "nrf.h"

#include "esb_tx.h"
#include "power.h"
#include "scanner.h"
#include "systime.h"

// Packets per pump() call: with a 2 ms gap that is about 8 ms of radio time,
// comfortably inside one main loop pass
#define TX_BURST 4

static bool m_active;
static esb_tx_cfg_t m_cfg;
static uint32_t m_sent;
static uint32_t m_started_ms;

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

static void radio_setup(void)
{
    radio_disable();

    NRF_RADIO->MODE = (m_cfg.rate == 2 ? RADIO_MODE_MODE_Nrf_2Mbit : RADIO_MODE_MODE_Nrf_1Mbit)
                      << RADIO_MODE_MODE_Pos;
    NRF_RADIO->MODECNF0 = (RADIO_MODECNF0_RU_Fast << RADIO_MODECNF0_RU_Pos) |
                          (RADIO_MODECNF0_DTX_Center << RADIO_MODECNF0_DTX_Pos);

    // Raw bytes after the address: fixed length, no LENGTH field, no CRC
    // (a replayed capture carries its own CRC, a crafted packet gets one
    // from esb_frame_build())
    NRF_RADIO->PCNF0 = (0 << RADIO_PCNF0_S0LEN_Pos) |
                       (0 << RADIO_PCNF0_LFLEN_Pos) |
                       (0 << RADIO_PCNF0_S1LEN_Pos) |
                       (RADIO_PCNF0_PLEN_8bit << RADIO_PCNF0_PLEN_Pos);
    NRF_RADIO->PCNF1 = ((uint32_t)m_cfg.raw_len << RADIO_PCNF1_MAXLEN_Pos) |
                       ((uint32_t)m_cfg.raw_len << RADIO_PCNF1_STATLEN_Pos) |
                       ((uint32_t)m_cfg.addr_len << RADIO_PCNF1_BALEN_Pos) |
                       (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos) |
                       (RADIO_PCNF1_WHITEEN_Disabled << RADIO_PCNF1_WHITEEN_Pos);

    // The address as the receiver recovered it: prefix byte first, then the
    // base. The radio transmits the preamble itself, alternating to match
    // the first address bit.
    NRF_RADIO->BASE0 = ((uint32_t)m_cfg.addr[1] << 24) | ((uint32_t)m_cfg.addr[2] << 16) |
                       ((uint32_t)m_cfg.addr[3] << 8) | m_cfg.addr[4];
    NRF_RADIO->PREFIX0 = m_cfg.addr[0];
    NRF_RADIO->TXADDRESS = 0;

    NRF_RADIO->CRCCNF = RADIO_CRCCNF_LEN_Disabled << RADIO_CRCCNF_LEN_Pos;
    NRF_RADIO->FREQUENCY = m_cfg.mhz - 2400;
    NRF_RADIO->PACKETPTR = (uint32_t)m_cfg.raw;
    NRF_RADIO->SHORTS = 0;
}

bool esb_tx_start(const esb_tx_cfg_t *cfg)
{
    if (!cfg || cfg->addr_len < 2 || cfg->addr_len > 5 || cfg->raw_len == 0 ||
        cfg->raw_len > ESB_CAPTURE_MAX || cfg->mhz < 2400 || cfg->mhz > 2500)
        return false;

    esb_tx_stop();

    m_cfg = *cfg;
    m_sent = 0;
    m_started_ms = 0; // stamped on the first update call

    radio_hfxo_start();
    radio_setup();

    NRF_RADIO->TXPOWER = tx_power_reg(m_cfg.power);
    m_active = true;
    return true;
}

// One packet: ramp up, start, wait for the end of it
static bool send_one(void)
{
    NRF_RADIO->EVENTS_READY = 0;
    NRF_RADIO->TASKS_TXEN = 1;
    if (!wait_event(&NRF_RADIO->EVENTS_READY))
        return false;

    NRF_RADIO->EVENTS_END = 0;
    NRF_RADIO->TASKS_START = 1;
    if (!wait_event(&NRF_RADIO->EVENTS_END))
        return false;

    m_sent++;
    return true;
}

void esb_tx_pump(void)
{
    if (!m_active)
        return;

    for (uint8_t i = 0; i < TX_BURST; i++)
    {
        if (m_cfg.count && m_sent >= m_cfg.count)
            return;
        if (!send_one())
            return;

        // The gap between retransmissions, like a real ESB sender
        uint32_t until = systime_us() + m_cfg.gap_us;
        while ((int32_t)(systime_us() - until) < 0)
        {
        }
    }
}

void esb_tx_stop(void)
{
    if (!m_active)
        return;

    radio_disable();
    NRF_RADIO->TXPOWER = RADIO_TXPOWER_TXPOWER_0dBm;
    m_active = false;

    NRF_RADIO->SHORTS = 0;
    NRF_RADIO->PACKETPTR = 0;
    scanner_init();
}

bool esb_tx_active(void) { return m_active; }

void esb_tx_update(uint32_t now_ms)
{
    if (!m_active)
        return;

    if (m_started_ms == 0)
        m_started_ms = now_ms;

    if (m_cfg.count && m_sent >= m_cfg.count)
    {
        esb_tx_stop();
        return;
    }

    if (now_ms - m_started_ms >= ESB_TX_MAX_MS)
        esb_tx_stop();
}

uint32_t esb_tx_remaining_ms(uint32_t now_ms)
{
    if (!m_active || m_started_ms == 0)
        return 0;

    uint32_t elapsed = now_ms - m_started_ms;
    return elapsed >= ESB_TX_MAX_MS ? 0 : ESB_TX_MAX_MS - elapsed;
}

uint32_t esb_tx_sent(void) { return m_sent; }
