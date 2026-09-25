#include <string.h>

#include "nrf.h"

#include "ble_scan.h"
#include "power.h"
#include "scanner.h"
#include "systime.h"

// Advertising channel plan: index 37/38/39 at 2402/2426/2480 MHz
static const uint8_t adv_freq[3] = {2, 26, 80};
static const uint8_t adv_index[3] = {37, 38, 39};

#define BLE_ACCESS_ADDRESS 0x8E89BED6u
#define BLE_CRC_POLY 0x0000065Bu
#define BLE_CRC_INIT 0x00555555u

// Advertising channel PDU types that are AdvA + AD data
#define PDU_ADV_IND 0x00
#define PDU_ADV_NONCONN_IND 0x02
#define PDU_SCAN_RSP 0x04
#define PDU_ADV_SCAN_IND 0x06
// ADV_EXT_IND on a primary channel, AUX_ADV_IND on a data channel
#define PDU_ADV_EXT 0x07

// Longest PDU payload taken on a primary channel: legacy adverts are at most
// 37 bytes, an ADV_EXT_IND header a little more. A longer limit would only
// keep the receiver busy with noise.
#define PRIMARY_MAXLEN 64
#define AUX_MAXLEN 255

// Retune early enough for the fast ramp up (40us) and some slack
#define AUX_EARLY_US 100u
// The AUX_ADV_IND must be at least this far away to be worth chasing
#define AUX_MIN_LEAD_US 150u

static uint32_t m_packets;
static ble_scan_listener_t m_listener;
static ble_ext_stats_t m_ext;

// Receiver time: how long the receiver has listened on the primary channels,
// in ms plus a microsecond remainder. Feeds the interval estimate.
static uint32_t m_listen_ms;
static uint32_t m_listen_rem_us;

// PDU buffer: S0 (header), LENGTH, then up to 255 payload bytes for AUX
static uint8_t m_pdu[2 + AUX_MAXLEN + 3] __attribute__((aligned(4)));

// An ADV_EXT_IND whose AUX_ADV_IND is worth chasing. Copied out of m_pdu,
// which the AUX reception overwrites.
typedef struct
{
    bool has_adva;
    uint8_t adva[6];
    uint8_t tx_add;
    uint8_t sid;
    uint8_t chan;
    uint8_t phy;
    uint16_t unit_us;
    int8_t rssi;
    uint32_t due_us; // start of the AUX_ADV_IND, at the earliest
    uint32_t now_ms;
    uint32_t listen_ms;
} aux_job_t;

static bool wait_event(volatile uint32_t *event, uint32_t spins)
{
    for (uint32_t i = 0; i < spins; i++)
    {
        if (*event)
        {
            *event = 0;
            return true;
        }
    }
    return false;
}

void ble_scan_reset(void)
{
    m_packets = 0;
    memset(&m_ext, 0, sizeof(m_ext));
}

void ble_scan_init(void)
{
    ble_scan_reset();
    m_listener = 0;
}

void ble_scan_set_listener(ble_scan_listener_t listener)
{
    m_listener = listener;
}

static void deliver(const ble_rx_t *rx)
{
    ble_devtab_packet(rx);
    if (m_listener)
        m_listener(rx);
}

static void radio_configure(uint8_t chan_index, uint8_t freq, uint8_t phy, uint8_t maxlen)
{
    radio_disable();

    bool two_m = (phy == BLE_PHY_2M);
    NRF_RADIO->MODE = (two_m ? RADIO_MODE_MODE_Ble_2Mbit : RADIO_MODE_MODE_Ble_1Mbit)
                      << RADIO_MODE_MODE_Pos;
    NRF_RADIO->MODECNF0 = (RADIO_MODECNF0_RU_Fast << RADIO_MODECNF0_RU_Pos) |
                          (RADIO_MODECNF0_DTX_Center << RADIO_MODECNF0_DTX_Pos);

    // Advertising PDU header: 1 byte S0 (type/flags), 8 bit length field.
    // The 2M PHY has a two byte preamble.
    NRF_RADIO->PCNF0 = (1 << RADIO_PCNF0_S0LEN_Pos) |
                       (8 << RADIO_PCNF0_LFLEN_Pos) |
                       (0 << RADIO_PCNF0_S1LEN_Pos) |
                       ((two_m ? RADIO_PCNF0_PLEN_16bit : RADIO_PCNF0_PLEN_8bit)
                        << RADIO_PCNF0_PLEN_Pos);
    NRF_RADIO->PCNF1 = ((uint32_t)maxlen << RADIO_PCNF1_MAXLEN_Pos) |
                       (0 << RADIO_PCNF1_STATLEN_Pos) |
                       (3 << RADIO_PCNF1_BALEN_Pos) |
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos) |
                       (RADIO_PCNF1_WHITEEN_Enabled << RADIO_PCNF1_WHITEEN_Pos);

    // Access address 0x8E89BED6 as prefix + 3 byte base. AUX_ADV_IND uses it
    // too, on a data channel.
    NRF_RADIO->BASE0 = (BLE_ACCESS_ADDRESS << 8) & 0xFFFFFF00u;
    NRF_RADIO->PREFIX0 = (BLE_ACCESS_ADDRESS >> 24) & 0xFF;
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1;

    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Three << RADIO_CRCCNF_LEN_Pos) |
                        (RADIO_CRCCNF_SKIPADDR_Skip << RADIO_CRCCNF_SKIPADDR_Pos);
    NRF_RADIO->CRCPOLY = BLE_CRC_POLY;
    NRF_RADIO->CRCINIT = BLE_CRC_INIT;

    // Whitening is seeded with the channel index, bit 6 always set
    NRF_RADIO->DATAWHITEIV = 0x40 | chan_index;
    NRF_RADIO->FREQUENCY = freq;
    NRF_RADIO->PACKETPTR = (uint32_t)m_pdu;

    // Sampling RSSI at address match gives the level of the packet itself
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_ADDRESS_RSSISTART_Msk;
    NRF_RADIO->EVENTS_END = 0;
    NRF_RADIO->EVENTS_READY = 0;
    NRF_RADIO->EVENTS_ADDRESS = 0;
}

static int8_t packet_rssi(void)
{
    return -(int8_t)(NRF_RADIO->RSSISAMPLE & RADIO_RSSISAMPLE_RSSISAMPLE_Msk);
}

// A packet on a primary channel. Delivers it, or returns true with job
// filled in when its AUX_ADV_IND is to be chased first.
static bool handle_packet(int8_t rssi, uint32_t now_ms, uint32_t listen_ms, uint32_t end_us,
                          aux_job_t *job)
{
    uint8_t header = m_pdu[0];
    uint8_t length = m_pdu[1];
    uint8_t pdu_type = header & 0x0F;
    uint8_t tx_add = (header >> 6) & 1;

    ble_rx_t rx;
    memset(&rx, 0, sizeof(rx));
    rx.pdu_type = pdu_type;
    rx.rssi = rssi;
    rx.now_ms = now_ms;
    rx.listen_ms = listen_ms;

    if (pdu_type == PDU_ADV_EXT)
    {
        ble_ext_t ext;
        if (!ble_ext_parse(&m_pdu[2], length, &ext))
            return false;

        m_ext.ext++;
        rx.ext = true;
        rx.addr = ext.adva;
        rx.addr_type = ext.adva ? tx_add : BLE_ADDR_EXT_ANON;
        rx.sid = ext.sid;
        rx.ad = ext.ad;
        rx.ad_len = ext.ad_len;
        if (ext.has_aux)
        {
            rx.has_auxptr = true;
            rx.aux_chan = ext.aux_chan;
            rx.aux_phy = ext.aux_phy;
            m_ext.last_chan = ext.aux_chan;
            m_ext.last_phy = ext.aux_phy;
            m_ext.last_offset_us = ext.aux_offset_us;

            // Start of this packet: 1M air time is 8us per byte of preamble,
            // access address, header, payload and CRC
            uint32_t start_us = end_us - (uint32_t)(1 + 4 + 2 + length + 3) * 8u;
            uint32_t due_us = start_us + ext.aux_offset_us;
            int32_t lead = (int32_t)(due_us - systime_us());

            if (ext.aux_phy > BLE_PHY_2M)
            {
                m_ext.aux_coded++;
            }
            else if (ext.aux_chan > 36 || ext.aux_offset_us > BLE_AUX_MAX_US ||
                     lead < (int32_t)AUX_MIN_LEAD_US)
            {
                m_ext.aux_far++;
            }
            else
            {
                memset(job, 0, sizeof(*job));
                job->has_adva = ext.adva != 0;
                if (ext.adva)
                    memcpy(job->adva, ext.adva, 6);
                job->tx_add = tx_add;
                job->sid = ext.sid;
                job->chan = ext.aux_chan;
                job->phy = ext.aux_phy;
                job->unit_us = ext.aux_unit_us;
                job->rssi = rssi;
                job->due_us = due_us;
                job->now_ms = now_ms;
                job->listen_ms = listen_ms;
                return true;
            }
        }
        deliver(&rx);
        return false;
    }

    // Only advertising PDUs that carry AdvA followed by AD data. ADV_DIRECT_IND,
    // SCAN_REQ and CONNECT_IND have a second address or LL data after AdvA, so
    // they still count as packets but never enter the device table.
    if (length < 6 || length > 37)
        return false;
    if (pdu_type != PDU_ADV_IND && pdu_type != PDU_ADV_NONCONN_IND &&
        pdu_type != PDU_SCAN_RSP && pdu_type != PDU_ADV_SCAN_IND)
        return false;

    rx.addr = &m_pdu[2];
    rx.addr_type = tx_add;
    rx.ad = &m_pdu[8];
    rx.ad_len = (uint8_t)(length - 6);
    deliver(&rx);
    return false;
}

// Retunes to the data channel of an AuxPtr, receives the one AUX_ADV_IND and
// delivers it, then the ADV_EXT_IND that pointed to it (under the AdvA of the
// AUX packet when the primary one had none). Busy waits until the packet is
// due, at most BLE_AUX_MAX_US.
static void follow_aux(const aux_job_t *job)
{
    ble_rx_t rx;
    uint8_t adva[6];
    bool got_adva = false;
    uint8_t adva_type = BLE_ADDR_EXT_ANON;

    m_ext.aux_tried++;
    radio_configure(job->chan, ble_data_channel_freq(job->chan), job->phy, AUX_MAXLEN);

    while ((int32_t)(systime_us() - (job->due_us - AUX_EARLY_US)) < 0)
    {
    }
    NRF_RADIO->TASKS_RXEN = 1;

    // The packet starts within one offset unit after the due time; its access
    // address is on air 40us (1M) later
    uint32_t addr_deadline = job->due_us + job->unit_us + 60u;
    bool got = false;
    while ((int32_t)(systime_us() - addr_deadline) < 0)
    {
        if (NRF_RADIO->EVENTS_ADDRESS)
        {
            got = true;
            break;
        }
    }
    if (got)
    {
        // Longest AUX_ADV_IND: 255 byte payload plus header and CRC at 8us a byte
        uint32_t end_deadline = systime_us() + 2200u;
        got = false;
        while ((int32_t)(systime_us() - end_deadline) < 0)
        {
            if (NRF_RADIO->EVENTS_END)
            {
                got = true;
                break;
            }
        }
    }

    if (got && NRF_RADIO->CRCSTATUS == RADIO_CRCSTATUS_CRCSTATUS_CRCOk &&
        (m_pdu[0] & 0x0F) == PDU_ADV_EXT)
    {
        ble_ext_t ext;
        if (ble_ext_parse(&m_pdu[2], m_pdu[1], &ext))
        {
            m_ext.aux_ok++;
            m_packets++;

            memset(&rx, 0, sizeof(rx));
            rx.pdu_type = PDU_ADV_EXT;
            rx.aux = true;
            rx.rssi = packet_rssi();
            rx.now_ms = job->now_ms;
            rx.listen_ms = job->listen_ms;
            rx.has_auxptr = true;
            rx.aux_chan = job->chan;
            rx.aux_phy = job->phy;
            rx.sid = job->sid;
            rx.ad = ext.ad;
            rx.ad_len = ext.ad_len;
            if (ext.adva)
            {
                memcpy(adva, ext.adva, 6);
                got_adva = true;
                adva_type = (m_pdu[0] >> 6) & 1;
                rx.addr = adva;
                rx.addr_type = adva_type;
            }
            else if (job->has_adva)
            {
                rx.addr = job->adva;
                rx.addr_type = job->tx_add;
            }
            else
            {
                rx.addr_type = BLE_ADDR_EXT_ANON;
            }
            deliver(&rx);
        }
    }

    radio_disable();

    // The primary packet itself, counted for the interval estimate
    memset(&rx, 0, sizeof(rx));
    rx.pdu_type = PDU_ADV_EXT;
    rx.ext = true;
    rx.rssi = job->rssi;
    rx.now_ms = job->now_ms;
    rx.listen_ms = job->listen_ms;
    rx.has_auxptr = true;
    rx.aux_chan = job->chan;
    rx.aux_phy = job->phy;
    rx.sid = job->sid;
    if (job->has_adva)
    {
        rx.addr = job->adva;
        rx.addr_type = job->tx_add;
    }
    else if (got_adva)
    {
        rx.addr = adva;
        rx.addr_type = adva_type;
    }
    else
    {
        rx.addr_type = BLE_ADDR_EXT_ANON;
    }
    deliver(&rx);
}

uint16_t ble_scan_run(uint32_t window_ms)
{
    uint16_t received = 0;
    uint32_t per_channel = window_ms / 3;
    if (per_channel == 0)
        per_channel = 1;

    radio_hfxo_start();

    for (int c = 0; c < 3; c++)
    {
        radio_configure(adv_index[c], adv_freq[c], BLE_PHY_1M, PRIMARY_MAXLEN);

        NRF_RADIO->EVENTS_READY = 0;
        NRF_RADIO->TASKS_RXEN = 1;
        if (!wait_event(&NRF_RADIO->EVENTS_READY, 200000))
            continue;

        uint32_t start_us = systime_us();
        uint32_t window_us = per_channel * 1000u;
        uint32_t away_us = 0; // spent on data channels chasing AUX packets
        bool armed = true;

        while ((systime_us() - start_us) < window_us)
        {
            // The READY_START shortcut already armed the first reception, only
            // the following ones need an explicit restart
            if (!armed)
            {
                NRF_RADIO->EVENTS_END = 0;
                NRF_RADIO->TASKS_START = 1;
            }
            armed = false;

            bool got = false;
            uint32_t end_us = 0;
            while ((systime_us() - start_us) < window_us)
            {
                if (NRF_RADIO->EVENTS_END)
                {
                    end_us = systime_us();
                    NRF_RADIO->EVENTS_END = 0;
                    got = true;
                    break;
                }
            }
            if (!got)
                break;

            if (NRF_RADIO->CRCSTATUS == RADIO_CRCSTATUS_CRCSTATUS_CRCOk)
            {
                uint32_t heard_us = m_listen_rem_us + (end_us - start_us) - away_us;
                uint32_t listen_ms = m_listen_ms + heard_us / 1000u;
                aux_job_t job;

                received++;
                m_packets++;
                if (handle_packet(packet_rssi(), systime_ms(), listen_ms, end_us, &job))
                {
                    uint32_t t0 = systime_us();
                    follow_aux(&job);

                    // Back to the primary channel, READY_START arms the receiver
                    radio_configure(adv_index[c], adv_freq[c], BLE_PHY_1M, PRIMARY_MAXLEN);
                    NRF_RADIO->TASKS_RXEN = 1;
                    wait_event(&NRF_RADIO->EVENTS_READY, 200000);
                    armed = true;
                    away_us += systime_us() - t0;
                }
            }

            power_watchdog_feed();
        }

        uint32_t spent_us = systime_us() - start_us;
        if (spent_us > window_us)
            spent_us = window_us;
        spent_us = spent_us > away_us ? spent_us - away_us : 0;
        m_listen_rem_us += spent_us;
        m_listen_ms += m_listen_rem_us / 1000u;
        m_listen_rem_us %= 1000u;

        NRF_RADIO->EVENTS_DISABLED = 0;
        NRF_RADIO->TASKS_DISABLE = 1;
        wait_event(&NRF_RADIO->EVENTS_DISABLED, 200000);
    }

    // Leave the radio the way the sweep engine expects to find it
    NRF_RADIO->SHORTS = 0;
    NRF_RADIO->PACKETPTR = 0;
    scanner_init();

    return received;
}

uint32_t ble_scan_packets(void) { return m_packets; }

const ble_ext_stats_t *ble_scan_ext_stats(void) { return &m_ext; }
