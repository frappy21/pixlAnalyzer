#include <string.h>

#include "ble_beacon.h"

#include "ble_adv.h"

#ifndef BLE_BEACON_HOST_TEST
#include "nrf.h"

#include "power.h"
#include "scanner.h"
#include "systime.h"
#include "tx_test.h"
#endif

// ---------------------------------------------------------------------------
// Payload builders, pure
// ---------------------------------------------------------------------------

// Advertising interval choices, ms
const uint16_t ble_beacon_intervals[BLE_BEACON_INTV_COUNT] = {100, 250, 500, 1000, 2000};

const char *ble_beacon_name(uint8_t type)
{
    static const char *const names[BLE_BEACON_TYPE_COUNT] = {
        "NAME",  "IBEACON", "EDDY UID", "EDDY URL", "ALTBEACON", "SWIFTPAIR", "FINDMY",
    };
    return type < BLE_BEACON_TYPE_COUNT ? names[type] : "?";
}

const char *ble_beacon_desc(uint8_t type)
{
    static const char *const descs[BLE_BEACON_TYPE_COUNT] = {
        "PLAIN NAME",
        "APPLE IBEACON",
        "EDDYSTONE UID",
        "EDDYSTONE URL",
        "ALTBEACON",
        "MSFT SWIFT PAIR",
        "APPLE 0x12 TEST",
    };
    return type < BLE_BEACON_TYPE_COUNT ? descs[type] : "?";
}

// Static random address from the device id: two top bits set, the rest of
// the id spread over the six bytes. On air byte order.
void ble_beacon_addr(const uint8_t dev_id[8], uint8_t addr[6])
{
    for (int i = 0; i < 6; i++)
        addr[i] = dev_id[(i + 2) & 7];
    addr[5] = (uint8_t)(0xC0 | (addr[5] & 0x3F)); // static random
}

// Appends one AD structure, returns the new length (unchanged when it does
// not fit)
static uint8_t ad_append(uint8_t *ad, uint8_t len, uint8_t type, const uint8_t *val, uint8_t n)
{
    if ((uint16_t)len + 2 + n > BLE_BEACON_AD_MAX)
        return len;
    ad[len++] = (uint8_t)(n + 1);
    ad[len++] = type;
    memcpy(ad + len, val, n);
    return (uint8_t)(len + n);
}

// LE 16 bit word
static void le16_put(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

uint8_t ble_beacon_build(uint8_t type, const uint8_t dev_id[8], uint8_t ad[BLE_BEACON_AD_MAX])
{
    static const uint8_t flags[] = {0x06};
    uint8_t len = 0;

    switch (type)
    {
    case BLE_BEACON_NAME:
    {
        // Flags plus the full name: shows up as an ordinary device
        len = ad_append(ad, len, BLE_AD_FLAGS, flags, 1);
        static const uint8_t name[] = "PIXLANALYZER";
        len = ad_append(ad, len, BLE_AD_NAME_FULL, name, (uint8_t)(sizeof(name) - 1));
        break;
    }
    case BLE_BEACON_IBEACON:
    {
        // Manufacturer data: Apple company id, sub type 0x02 (iBeacon),
        // 0x15 bytes of UUID, major, minor, measured power
        uint8_t v[25];
        v[0] = 0x4C; // company 0x004C, little endian
        v[1] = 0x00;
        v[2] = 0x02;
        v[3] = 0x15;
        for (int i = 0; i < 16; i++)
            v[4 + i] = dev_id[i & 7];
        v[20] = dev_id[0]; // major
        v[21] = dev_id[1];
        v[22] = dev_id[2]; // minor
        v[23] = dev_id[3];
        v[24] = 0xC5; // -59 dBm, the iBeacon convention
        len = ad_append(ad, len, BLE_AD_FLAGS, flags, 1);
        len = ad_append(ad, len, BLE_AD_MANUFACTURER, v, 25);
        break;
    }
    case BLE_BEACON_EDDY_UID:
    {
        // Service data on the Eddystone UUID 0xFEAA, frame 0x00 (UID): TX
        // power, 10 byte namespace, 6 byte instance, 2 RFU bytes
        uint8_t v[22];
        v[0] = 0xAA;
        v[1] = 0xFE;
        v[2] = 0x00;
        v[3] = 0xEE; // -18 dBm
        for (int i = 0; i < 10; i++)
            v[4 + i] = dev_id[i & 7];
        for (int i = 0; i < 6; i++)
            v[14 + i] = dev_id[(i + 4) & 7];
        v[20] = 0;
        v[21] = 0;
        len = ad_append(ad, len, BLE_AD_FLAGS, flags, 1);
        len = ad_append(ad, len, BLE_AD_SERVICE_DATA16, v, 22);
        break;
    }
    case BLE_BEACON_EDDY_URL:
    {
        // Service data, frame 0x10 (URL): TX power, scheme prefix, URL
        static const uint8_t url[] = {'p', 'i', 'x', 'l', '.', 'j', 's'};
        uint8_t v[2 + 1 + 1 + sizeof(url)];
        le16_put(v, 0xFEAA);
        v[2] = 0x10;
        v[3] = 0xEE; // -18 dBm
        memcpy(&v[4], url, sizeof(url));
        len = ad_append(ad, len, BLE_AD_FLAGS, flags, 1);
        len = ad_append(ad, len, BLE_AD_SERVICE_DATA16, v, (uint8_t)sizeof(v));
        break;
    }
    case BLE_BEACON_ALT:
    {
        // AltBeacon: manufacturer id 0xBEAC, 20 byte id, 2 byte org, RSSI,
        // 1 reserved byte
        uint8_t v[26];
        le16_put(v, 0xBEAC);
        v[2] = 0xAC; // the AltBeacon marker, LE of the id above
        for (int i = 0; i < 19; i++)
            v[3 + i] = dev_id[(i + 1) & 7];
        v[22] = 0xBE;
        v[23] = 0xAC;
        v[24] = 0xEE;  // -18 dBm reference
        v[25] = 0x00;  // manufacturer reserved
        len = ad_append(ad, len, BLE_AD_FLAGS, flags, 1);
        len = ad_append(ad, len, BLE_AD_MANUFACTURER, v, 26);
        break;
    }
    case BLE_BEACON_SWIFTPAIR:
    {
        // Microsoft: company 0x0006, beacon sub id 0x03, then the beacon
        uint8_t v[7];
        v[0] = 0x06;
        v[1] = 0x00;
        v[2] = 0x03; // Swift Pair beacon id, what this firmware keys on
        v[3] = 0x00;
        v[4] = dev_id[0];
        v[5] = dev_id[1];
        v[6] = dev_id[2];
        len = ad_append(ad, len, BLE_AD_FLAGS, flags, 1);
        len = ad_append(ad, len, BLE_AD_MANUFACTURER, v, 7);
        break;
    }
    case BLE_BEACON_FINDMY:
    {
        // Apple continuity style: type 0x12 (offline finding) with a 22
        // byte payload, for checking this firmware's own FindMy detector
        uint8_t v[26];
        v[0] = 0x4C;
        v[1] = 0x00;
        v[2] = 0x12;
        v[3] = 22;
        for (int i = 0; i < 20; i++)
            v[4 + i] = dev_id[i & 7];
        v[24] = 0x00; // counter
        v[25] = 0x01;
        len = ad_append(ad, len, BLE_AD_FLAGS, flags, 1);
        len = ad_append(ad, len, BLE_AD_MANUFACTURER, v, 26);
        break;
    }
    default:
        break;
    }

    return len;
}

// ---------------------------------------------------------------------------
// Transmitter, target only
// ---------------------------------------------------------------------------

#ifndef BLE_BEACON_HOST_TEST

// Advertising channel plan: index 37/38/39 at 2402/2426/2480 MHz
static const uint8_t adv_freq[3] = {2, 26, 80};
static const uint8_t adv_index[3] = {37, 38, 39};

#define BLE_ACCESS_ADDRESS 0x8E89BED6u
#define BLE_CRC_POLY 0x0000065Bu
#define BLE_CRC_INIT 0x00555555u

// PDU buffer: S0 (header), LENGTH, AdvA, AD data
static uint8_t m_pdu[2 + 6 + BLE_BEACON_AD_MAX] __attribute__((aligned(4)));

static bool m_active;
static uint8_t m_type;
static uint8_t m_power;
static uint16_t m_interval_ms;
static uint32_t m_sent;
static uint32_t m_started_ms;
static uint32_t m_next_ms;
static uint8_t m_ch; // 0..2, rotates through the advertising channels

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

// Same advertising receiver setup as ble_scan.c, but for transmit
static void radio_setup(uint8_t chan_index, uint8_t freq, uint8_t payload_len)
{
    radio_disable();

    NRF_RADIO->MODE = RADIO_MODE_MODE_Ble_1Mbit << RADIO_MODE_MODE_Pos;
    NRF_RADIO->MODECNF0 = (RADIO_MODECNF0_RU_Fast << RADIO_MODECNF0_RU_Pos) |
                          (RADIO_MODECNF0_DTX_Center << RADIO_MODECNF0_DTX_Pos);

    // Advertising PDU header: 1 byte S0 (type/flags), 8 bit length field
    NRF_RADIO->PCNF0 = (1 << RADIO_PCNF0_S0LEN_Pos) |
                       (8 << RADIO_PCNF0_LFLEN_Pos) |
                       (0 << RADIO_PCNF0_S1LEN_Pos) |
                       (RADIO_PCNF0_PLEN_8bit << RADIO_PCNF0_PLEN_Pos);
    NRF_RADIO->PCNF1 = ((uint32_t)payload_len << RADIO_PCNF1_MAXLEN_Pos) |
                       (0 << RADIO_PCNF1_STATLEN_Pos) |
                       (3 << RADIO_PCNF1_BALEN_Pos) |
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos) |
                       (RADIO_PCNF1_WHITEEN_Enabled << RADIO_PCNF1_WHITEEN_Pos);

    NRF_RADIO->BASE0 = (BLE_ACCESS_ADDRESS << 8) & 0xFFFFFF00u;
    NRF_RADIO->PREFIX0 = (BLE_ACCESS_ADDRESS >> 24) & 0xFF;
    NRF_RADIO->TXADDRESS = 0;

    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Three << RADIO_CRCCNF_LEN_Pos) |
                        (RADIO_CRCCNF_SKIPADDR_Skip << RADIO_CRCCNF_SKIPADDR_Pos);
    NRF_RADIO->CRCPOLY = BLE_CRC_POLY;
    NRF_RADIO->CRCINIT = BLE_CRC_INIT;

    // Whitening seeded with the channel index, bit 6 always set
    NRF_RADIO->DATAWHITEIV = 0x40 | chan_index;
    NRF_RADIO->FREQUENCY = freq;
    NRF_RADIO->PACKETPTR = (uint32_t)m_pdu;
    NRF_RADIO->SHORTS = 0;
    NRF_RADIO->EVENTS_END = 0;
}

static void send_event(void)
{
    uint8_t ad[BLE_BEACON_AD_MAX];
    uint8_t dev_id[8];
    uint32_t d0 = NRF_FICR->DEVICEID[0];
    uint32_t d1 = NRF_FICR->DEVICEID[1];
    for (int i = 0; i < 4; i++)
    {
        dev_id[i] = (uint8_t)(d0 >> (8 * i));
        dev_id[4 + i] = (uint8_t)(d1 >> (8 * i));
    }

    uint8_t ad_len = ble_beacon_build(m_type, dev_id, ad);
    if (ad_len == 0)
        return;

    uint8_t addr[6];
    ble_beacon_addr(dev_id, addr);

    // ADV_NONCONN_IND with a random Tx address: a beacon nobody can connect to
    m_pdu[0] = 0x02 | 0x40;
    m_pdu[1] = (uint8_t)(6 + ad_len);
    memcpy(&m_pdu[2], addr, 6);
    memcpy(&m_pdu[8], ad, ad_len);

    radio_hfxo_start();
    radio_setup(adv_index[m_ch], adv_freq[m_ch], 6 + ad_len);
    NRF_RADIO->TXPOWER = tx_power_reg(m_power);

    NRF_RADIO->EVENTS_READY = 0;
    NRF_RADIO->TASKS_TXEN = 1;
    if (!wait_event(&NRF_RADIO->EVENTS_READY))
        return;
    NRF_RADIO->EVENTS_END = 0;
    NRF_RADIO->TASKS_START = 1;
    if (wait_event(&NRF_RADIO->EVENTS_END))
        m_sent++;

    radio_disable();

    m_ch = (uint8_t)((m_ch + 1) % 3);
}

bool ble_beacon_start(uint8_t type, uint8_t power, uint16_t interval_ms, const uint8_t dev_id[8])
{
    if (type >= BLE_BEACON_TYPE_COUNT)
        return false;

    ble_beacon_stop();

    (void)dev_id; // the address is derived per event from the FICR

    m_type = type;
    m_power = power;
    m_interval_ms = interval_ms < 20 ? 20 : interval_ms;
    m_sent = 0;
    m_started_ms = 0;
    m_next_ms = 0; // first event on the first update
    m_ch = 0;
    m_active = true;
    return true;
}

void ble_beacon_stop(void)
{
    if (!m_active)
        return;

    radio_disable();
    NRF_RADIO->TXPOWER = RADIO_TXPOWER_TXPOWER_0dBm;
    m_active = false;

    NRF_RADIO->PACKETPTR = 0;
    scanner_init();
}

bool ble_beacon_active(void) { return m_active; }

void ble_beacon_update(uint32_t now_ms)
{
    if (!m_active)
        return;

    if (m_started_ms == 0)
    {
        m_started_ms = now_ms;
        m_next_ms = now_ms;
    }

    if (now_ms - m_started_ms >= BLE_BEACON_MAX_MS)
    {
        ble_beacon_stop();
        return;
    }

    if ((int32_t)(now_ms - m_next_ms) >= 0)
    {
        m_next_ms = now_ms + m_interval_ms;
        send_event();
    }
}

uint32_t ble_beacon_sent(void) { return m_sent; }

uint32_t ble_beacon_remaining_ms(uint32_t now_ms)
{
    if (!m_active || m_started_ms == 0)
        return 0;

    uint32_t elapsed = now_ms - m_started_ms;
    return elapsed >= BLE_BEACON_MAX_MS ? 0 : BLE_BEACON_MAX_MS - elapsed;
}

#endif // BLE_BEACON_HOST_TEST
