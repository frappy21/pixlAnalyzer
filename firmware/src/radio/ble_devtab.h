/**
 * BLE device table: the session log of every advertiser heard, plus what is
 * derived from it over time (advertising interval, following alert, spam
 * flood alert).
 *
 * The table storage belongs to whoever attaches it (the BLE screen puts it in
 * the shared arena), so it lives exactly as long as that screen: nothing is
 * kept across a screen switch and nothing is ever written to flash. Without
 * a table attached every packet is simply dropped here.
 *
 * Pure logic, no radio: the scanner hands over one ble_rx_t per packet, and
 * the host tests (test/test_ble.c) do the same with made up packets.
 */
#ifndef PIXLA_BLE_DEVTAB_H
#define PIXLA_BLE_DEVTAB_H

#include <stdbool.h>
#include <stdint.h>

#include "ble_adv.h"

#define BLE_NAME_LEN 12

// Address types
#define BLE_ADDR_PUBLIC 0
#define BLE_ADDR_RANDOM 1
#define BLE_ADDR_EXT_ANON 2 // extended advert without AdvA, keyed by its SID

// Device flags
#define BLE_DEV_FOLLOW 0x01  // heard persistently over a long window
#define BLE_DEV_EXT 0x02     // sends extended adverts (ADV_EXT_IND)
#define BLE_DEV_AUX 0x04     // its AUX_ADV_IND was received, adv[] holds that data
#define BLE_DEV_SPAMMY 0x08  // matched a spam prone pattern
#define BLE_DEV_CONN 0x10    // connectable
#define BLE_DEV_RSP 0x20     // a scan response was heard (answering someone else's scan)

// Following: seen for at least this long, in at least this many different
// minutes. Gaps are allowed, a tag in a bag is not heard all the time.
#define BLE_FOLLOW_SPAN_MS (10u * 60u * 1000u)
#define BLE_FOLLOW_MINUTES 6

// Spam flood: this many new random addresses with a spam prone pattern
// within the window
#define BLE_SPAM_BUCKET_MS 500u
#define BLE_SPAM_BUCKETS 4 // 2 s window
#define BLE_SPAM_THRESHOLD 8
#define BLE_SPAM_HOLD_MS 10000u // alert stays up this long after the last burst

// Interval estimate needs a few adverts
#define BLE_INTERVAL_MIN_ADV 4

typedef struct
{
    uint8_t addr[6];
    uint8_t addr_type;     // BLE_ADDR_*
    uint8_t pdu_type;      // last PDU type
    int8_t rssi;           // strongest
    int8_t rssi_min;       // weakest
    int8_t rssi_last;
    uint8_t kind;          // ble_kind_t
    uint8_t flags;         // BLE_DEV_*
    uint8_t minutes;       // distinct minutes heard in, saturating
    uint8_t aux_chan;      // last AuxPtr channel, extended adverts
    uint8_t aux_phy;       // last AuxPtr PHY, BLE_PHY_*
    uint16_t company;      // manufacturer company id, 0 if none
    uint16_t packets;
    uint16_t adv_count;    // primary channel adverts, for the interval
    uint16_t last_minute;  // minute (since boot) it was last heard in
    uint32_t first_ms;
    uint32_t last_ms;
    uint32_t listen_first; // receiver time (ms spent listening) at the first advert
    uint32_t listen_last;  // and at the last one
    uint8_t adv_len;
    uint8_t rsp_len;
    uint8_t adv[BLE_AD_MAX]; // last advertising data (or AUX data, whole structures)
    uint8_t rsp[BLE_AD_MAX]; // last scan response data
    char name[BLE_NAME_LEN];
} ble_dev_t;

// One received packet, as the scanner hands it over
typedef struct
{
    const uint8_t *addr;  // 6 bytes, little endian as on air; NULL for BLE_ADDR_EXT_ANON
    uint8_t addr_type;    // BLE_ADDR_*
    uint8_t pdu_type;     // PDU type from the header (0x7 for extended)
    uint8_t sid;          // BLE_ADDR_EXT_ANON: advertising set id
    bool ext;             // ADV_EXT_IND on a primary channel
    bool aux;             // AUX_ADV_IND followed onto a data channel
    bool has_auxptr;
    uint8_t aux_chan;
    uint8_t aux_phy;
    int8_t rssi;
    uint32_t now_ms;
    uint32_t listen_ms;   // total time the receiver has been listening, see ble_dev_interval_ms
    const uint8_t *ad;
    uint8_t ad_len;
} ble_rx_t;

// Attach storage for up to capacity devices and clear it. NULL detaches.
void ble_devtab_attach(ble_dev_t *table, uint8_t capacity);
bool ble_devtab_attached(void);
void ble_devtab_clear(void);

void ble_devtab_packet(const ble_rx_t *rx);

uint8_t ble_devtab_count(void);
const ble_dev_t *ble_devtab_device(uint8_t index);
const ble_dev_t *ble_devtab_find(const uint8_t *addr, uint8_t addr_type);

// List views
typedef enum
{
    BLE_FILTER_ALL = 0,
    BLE_FILTER_TRACKERS,
    BLE_FILTER_FOLLOW,
    BLE_FILTER_EXT,
    BLE_FILTER_COUNT
} ble_filter_t;

typedef enum
{
    BLE_SORT_RSSI = 0, // strongest first
    BLE_SORT_RECENT,   // last heard first
    BLE_SORT_FIRST,    // first heard first: the log order
    BLE_SORT_COUNT
} ble_sort_t;

// Fills idx with table indices, filtered and sorted. Returns how many.
uint8_t ble_devtab_sorted(uint8_t *idx, uint8_t max, uint8_t filter, uint8_t sort);

// Advertising interval estimate in ms, 0 when there is not enough data.
//
// The receiver listens on one advertising channel at a time, and every
// advertising event sends one packet on each channel, so a device is heard
// about once per interval of listening time, whatever channel is up. Time
// spent drawing or anywhere else off the air does not count. Packets lost to
// CRC errors make the estimate longer than the truth for weak devices.
uint16_t ble_dev_interval_ms(const ble_dev_t *dev);

// Following alert. mode: 0 off, 1 trackers only, 2 any device.
typedef enum
{
    BLE_FOLLOW_OFF = 0,
    BLE_FOLLOW_TRACKERS,
    BLE_FOLLOW_ALL,
    BLE_FOLLOW_MODE_COUNT
} ble_follow_mode_t;

uint8_t ble_devtab_following(uint8_t mode); // devices flagged that the mode alerts on

// Spam flood detector
typedef struct
{
    bool active;
    uint8_t family;     // ble_spam_t seen most in the flood
    uint16_t rate;      // new addresses in the last window
    uint16_t peak;      // highest rate of this session
    uint32_t total;     // new spam pattern addresses this session
    uint32_t dropped;   // flood adverts kept out of the table
} ble_spam_state_t;

const ble_spam_state_t *ble_devtab_spam(uint32_t now_ms);

#endif // PIXLA_BLE_DEVTAB_H
