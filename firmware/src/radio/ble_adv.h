/**
 * BLE advertising data decoders. Pure functions over byte arrays, no radio and
 * no display, so the host tests (test/test_ble.c) run them on hand made
 * adverts.
 *
 * Three layers:
 *  - AD structure walking (ble_ad_next and the finders built on it),
 *  - the per packet summary the scanner keeps (kind, company, spam pattern),
 *  - the text description of the detail card (ble_describe_ad), one short
 *    upper case line per fact, sized for the 3x5 font.
 *
 * Receive only: nothing here builds an advert, and encrypted payloads are
 * reported as encrypted, never decrypted.
 */
#ifndef PIXLA_BLE_ADV_H
#define PIXLA_BLE_ADV_H

#include <stdbool.h>
#include <stdint.h>

// Legacy advertising data (and a scan response) is at most 31 bytes
#define BLE_AD_MAX 31

// AD types
#define BLE_AD_FLAGS 0x01
#define BLE_AD_UUID16_SOME 0x02
#define BLE_AD_UUID16_ALL 0x03
#define BLE_AD_UUID32_SOME 0x04
#define BLE_AD_UUID32_ALL 0x05
#define BLE_AD_UUID128_SOME 0x06
#define BLE_AD_UUID128_ALL 0x07
#define BLE_AD_NAME_SHORT 0x08
#define BLE_AD_NAME_FULL 0x09
#define BLE_AD_TX_POWER 0x0A
#define BLE_AD_SERVICE_DATA16 0x16
#define BLE_AD_APPEARANCE 0x19
#define BLE_AD_MESH_PB_ADV 0x29
#define BLE_AD_MESH_MESSAGE 0x2A
#define BLE_AD_MESH_BEACON 0x2B
#define BLE_AD_BROADCAST_NAME 0x30
#define BLE_AD_MANUFACTURER 0xFF

// Company identifiers and 16 bit UUIDs the classifier keys on
#define BLE_COMPANY_MICROSOFT 0x0006
#define BLE_COMPANY_APPLE 0x004C
#define BLE_COMPANY_SAMSUNG 0x0075
#define BLE_UUID_BROADCAST_AUDIO 0x1852
#define BLE_UUID_PUBLIC_BROADCAST 0x1856
#define BLE_UUID_MESH_PROV 0x1827
#define BLE_UUID_MESH_PROXY 0x1828
#define BLE_UUID_SMARTTAG 0xFD5A
#define BLE_UUID_FASTPAIR 0xFE2C
#define BLE_UUID_EDDYSTONE 0xFEAA
#define BLE_UUID_TILE 0xFEED
#define BLE_UUID_TILE2 0xFEEC
#define BLE_UUID_MATTER 0xFFF6

// What a device most likely is. The order is the priority: a device that
// sends several kinds of adverts keeps the highest one (see ble_kind_merge).
typedef enum
{
    BLE_KIND_PLAIN = 0,
    BLE_KIND_EXT,       // extended advertising, nothing else known
    BLE_KIND_APPLE,
    BLE_KIND_MICROSOFT,
    BLE_KIND_FASTPAIR,  // Google Fast Pair
    BLE_KIND_EDDYSTONE,
    BLE_KIND_IBEACON,
    BLE_KIND_SENSOR,    // one of the formats in ble_sensor.h
    BLE_KIND_MESH,
    BLE_KIND_MATTER,
    BLE_KIND_AURACAST,
    BLE_KIND_FINDMY,    // Apple offline finding, i.e. AirTag class
    BLE_KIND_TILE,
    BLE_KIND_SMARTTAG,  // Samsung
    BLE_KIND_GOOGLE_FMDN,
    BLE_KIND_COUNT
} ble_kind_t;

// Advert patterns that spam tools repeat from a new random address every few
// dozen milliseconds. On their own they are ordinary (AirPods cases, Windows
// Swift Pair mice); only a flood of new addresses makes them suspicious.
typedef enum
{
    BLE_SPAM_NONE = 0,
    BLE_SPAM_APPLE,     // continuity proximity pairing / nearby action
    BLE_SPAM_SWIFTPAIR, // Microsoft Swift Pair beacon
    BLE_SPAM_SAMSUNG,   // Samsung EasySetup (buds / watch popups)
    BLE_SPAM_FASTPAIR,  // Google Fast Pair model id
    BLE_SPAM_COUNT
} ble_spam_t;

// Per packet summary, all the scanner keeps from one advert besides the raw
// bytes
typedef struct
{
    uint8_t kind;          // ble_kind_t
    uint8_t spam;          // ble_spam_t
    uint16_t company;      // manufacturer company id, 0 if none
    const uint8_t *name;   // local or broadcast name inside the advert, not terminated
    uint8_t name_len;
} ble_ad_summary_t;

// ---------------------------------------------------------------------------
// AD structure walking
// ---------------------------------------------------------------------------

typedef struct
{
    const uint8_t *ad;
    uint8_t len;
    uint8_t pos;
} ble_ad_iter_t;

void ble_ad_iter_init(ble_ad_iter_t *it, const uint8_t *ad, uint8_t len);

// Next AD structure. Stops at the end, at a zero length (padding) and at a
// structure that would run past the data.
bool ble_ad_next(ble_ad_iter_t *it, uint8_t *type, const uint8_t **val, uint8_t *val_len);

bool ble_ad_find(const uint8_t *ad, uint8_t len, uint8_t type, const uint8_t **val,
                 uint8_t *val_len);

// Service data for one 16 bit UUID; data points after the UUID
bool ble_ad_service_data(const uint8_t *ad, uint8_t len, uint16_t uuid, const uint8_t **data,
                         uint8_t *data_len);

// Manufacturer specific data; data points after the company id
bool ble_ad_manufacturer(const uint8_t *ad, uint8_t len, uint16_t *company,
                         const uint8_t **data, uint8_t *data_len);

// True when the UUID is in a 16 bit service UUID list or has service data
bool ble_ad_has_uuid16(const uint8_t *ad, uint8_t len, uint16_t uuid);

// Copies whole AD structures while they fit into dst (max bytes), so a long
// extended advert can be kept in a legacy sized buffer without a torn last
// structure. Returns the bytes copied.
uint8_t ble_ad_copy_whole(uint8_t *dst, uint8_t max, const uint8_t *ad, uint8_t len);

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------

void ble_ad_summarize(const uint8_t *ad, uint8_t len, ble_ad_summary_t *out);

// The kind a device keeps when it sends adverts of two kinds
uint8_t ble_kind_merge(uint8_t old_kind, uint8_t new_kind);

bool ble_kind_is_tracker(uint8_t kind);
const char *ble_kind_name(uint8_t kind); // "" for BLE_KIND_PLAIN
const char *ble_spam_name(uint8_t spam);

// ---------------------------------------------------------------------------
// Extended advertising (ADV_EXT_IND / AUX_ADV_IND common payload format)
// ---------------------------------------------------------------------------

#define BLE_PHY_1M 0
#define BLE_PHY_2M 1
#define BLE_PHY_CODED 2

typedef struct
{
    uint8_t adv_mode;       // 0 non-connectable non-scannable, 1 connectable, 2 scannable
    const uint8_t *adva;    // NULL when the header has no AdvA
    bool has_adi;
    uint8_t sid;            // advertising set id, from the ADI
    uint16_t did;           // data id, from the ADI
    bool has_aux;
    uint8_t aux_chan;       // data channel index 0..36
    uint8_t aux_phy;        // BLE_PHY_*
    uint16_t aux_unit_us;   // 30 or 300: the aux packet starts within one unit after the offset
    uint32_t aux_offset_us; // from the start of this packet
    bool has_tx_power;
    int8_t tx_power;
    const uint8_t *ad;      // AdvData after the extended header
    uint8_t ad_len;
} ble_ext_t;

// Parses the payload of a PDU of type 0x7 (after the 2 byte PDU header).
// Returns false when the extended header does not fit the payload.
bool ble_ext_parse(const uint8_t *payload, uint8_t len, ble_ext_t *out);

// RF channel of a data channel index 0..36, in MHz above 2400
uint8_t ble_data_channel_freq(uint8_t chan);

// ---------------------------------------------------------------------------
// Names and the detail card text
// ---------------------------------------------------------------------------

const char *ble_company_name(uint16_t company); // NULL when not in the short table
const char *ble_uuid16_name(uint16_t uuid);     // NULL when not in the short table
const char *ble_appearance_name(uint16_t appearance);
const char *ble_apple_type_name(uint8_t type);  // continuity message type

// Line builder for the detail card: fixed size lines, silently truncated
#define BLE_LINE_LEN 32

typedef struct
{
    char s[BLE_LINE_LEN];
    uint8_t n;
} ble_line_t;

void ble_ln_init(ble_line_t *l, const char *text);
void ble_ln_str(ble_line_t *l, const char *text);
void ble_ln_char(ble_line_t *l, char c);
void ble_ln_int(ble_line_t *l, int32_t v);
void ble_ln_hex(ble_line_t *l, uint32_t v, uint8_t digits);
void ble_ln_fixed(ble_line_t *l, int32_t v, uint8_t decimals); // v scaled by 10^decimals
void ble_ln_bytes(ble_line_t *l, const uint8_t *b, uint8_t n); // printable ASCII, '.' otherwise

typedef struct
{
    char (*lines)[BLE_LINE_LEN];
    uint8_t max;
    uint8_t count;
} ble_lines_t;

void ble_lines_add(ble_lines_t *out, const ble_line_t *l);

// Appends a description of every AD structure: flags, TX power, appearance,
// names, UUIDs with names, manufacturer data (Apple continuity, Microsoft CDP
// and Swift Pair, iBeacon), service data (Eddystone, Google FMDN, Matter,
// Fast Pair, Auracast), mesh beacons and any sensor reading (ble_sensor.h).
void ble_describe_ad(const uint8_t *ad, uint8_t len, ble_lines_t *out);

#endif // PIXLA_BLE_ADV_H
