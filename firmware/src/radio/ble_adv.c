#include <string.h>

#include "ble_adv.h"
#include "ble_sensor.h"

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

// ---------------------------------------------------------------------------
// AD structure walking
// ---------------------------------------------------------------------------

void ble_ad_iter_init(ble_ad_iter_t *it, const uint8_t *ad, uint8_t len)
{
    it->ad = ad;
    it->len = len;
    it->pos = 0;
}

bool ble_ad_next(ble_ad_iter_t *it, uint8_t *type, const uint8_t **val, uint8_t *val_len)
{
    if (it->pos + 1 >= it->len)
        return false;

    uint8_t field_len = it->ad[it->pos];
    // The structure occupies ad[pos .. pos + field_len], all inside the data
    if (field_len == 0 || it->pos + field_len >= it->len)
        return false;

    *type = it->ad[it->pos + 1];
    *val = &it->ad[it->pos + 2];
    *val_len = (uint8_t)(field_len - 1);
    it->pos = (uint8_t)(it->pos + field_len + 1);
    return true;
}

bool ble_ad_find(const uint8_t *ad, uint8_t len, uint8_t type, const uint8_t **val,
                 uint8_t *val_len)
{
    ble_ad_iter_t it;
    uint8_t t;
    ble_ad_iter_init(&it, ad, len);
    while (ble_ad_next(&it, &t, val, val_len))
    {
        if (t == type)
            return true;
    }
    return false;
}

bool ble_ad_service_data(const uint8_t *ad, uint8_t len, uint16_t uuid, const uint8_t **data,
                         uint8_t *data_len)
{
    ble_ad_iter_t it;
    uint8_t t, n;
    const uint8_t *v;
    ble_ad_iter_init(&it, ad, len);
    while (ble_ad_next(&it, &t, &v, &n))
    {
        if (t == BLE_AD_SERVICE_DATA16 && n >= 2 && le16(v) == uuid)
        {
            *data = v + 2;
            *data_len = (uint8_t)(n - 2);
            return true;
        }
    }
    return false;
}

bool ble_ad_manufacturer(const uint8_t *ad, uint8_t len, uint16_t *company,
                         const uint8_t **data, uint8_t *data_len)
{
    const uint8_t *v;
    uint8_t n;
    if (!ble_ad_find(ad, len, BLE_AD_MANUFACTURER, &v, &n) || n < 2)
        return false;
    *company = le16(v);
    *data = v + 2;
    *data_len = (uint8_t)(n - 2);
    return true;
}

bool ble_ad_has_uuid16(const uint8_t *ad, uint8_t len, uint16_t uuid)
{
    ble_ad_iter_t it;
    uint8_t t, n;
    const uint8_t *v;
    ble_ad_iter_init(&it, ad, len);
    while (ble_ad_next(&it, &t, &v, &n))
    {
        if (t == BLE_AD_SERVICE_DATA16 && n >= 2 && le16(v) == uuid)
            return true;
        if (t == BLE_AD_UUID16_SOME || t == BLE_AD_UUID16_ALL)
        {
            for (uint8_t i = 0; i + 1 < n; i += 2)
            {
                if (le16(&v[i]) == uuid)
                    return true;
            }
        }
    }
    return false;
}

uint8_t ble_ad_copy_whole(uint8_t *dst, uint8_t max, const uint8_t *ad, uint8_t len)
{
    ble_ad_iter_t it;
    uint8_t t, n;
    const uint8_t *v;
    uint8_t out = 0;
    ble_ad_iter_init(&it, ad, len);
    while (ble_ad_next(&it, &t, &v, &n))
    {
        uint8_t whole = (uint8_t)(n + 2);
        if (out + whole > max)
            continue; // a later, shorter one may still fit
        memcpy(&dst[out], v - 2, whole);
        out = (uint8_t)(out + whole);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------

uint8_t ble_kind_merge(uint8_t old_kind, uint8_t new_kind)
{
    return new_kind > old_kind ? new_kind : old_kind;
}

bool ble_kind_is_tracker(uint8_t kind)
{
    return kind == BLE_KIND_FINDMY || kind == BLE_KIND_TILE || kind == BLE_KIND_SMARTTAG ||
           kind == BLE_KIND_GOOGLE_FMDN;
}

const char *ble_kind_name(uint8_t kind)
{
    static const char *const names[BLE_KIND_COUNT] = {
        [BLE_KIND_PLAIN] = "",
        [BLE_KIND_EXT] = "EXT ADV",
        [BLE_KIND_APPLE] = "APPLE",
        [BLE_KIND_MICROSOFT] = "MSFT",
        [BLE_KIND_FASTPAIR] = "FASTPAIR",
        [BLE_KIND_EDDYSTONE] = "EDDYST",
        [BLE_KIND_IBEACON] = "IBEACON",
        [BLE_KIND_SENSOR] = "SENSOR",
        [BLE_KIND_MESH] = "MESH",
        [BLE_KIND_MATTER] = "MATTER",
        [BLE_KIND_AURACAST] = "AURACAST",
        [BLE_KIND_FINDMY] = "FINDMY",
        [BLE_KIND_TILE] = "TILE",
        [BLE_KIND_SMARTTAG] = "SMARTTAG",
        [BLE_KIND_GOOGLE_FMDN] = "GOOGLE FMDN",
    };
    return kind < BLE_KIND_COUNT ? names[kind] : "";
}

const char *ble_spam_name(uint8_t spam)
{
    static const char *const names[BLE_SPAM_COUNT] = {
        [BLE_SPAM_NONE] = "",
        [BLE_SPAM_APPLE] = "APPLE",
        [BLE_SPAM_SWIFTPAIR] = "SWIFTPAIR",
        [BLE_SPAM_SAMSUNG] = "SAMSUNG",
        [BLE_SPAM_FASTPAIR] = "FASTPAIR",
    };
    return spam < BLE_SPAM_COUNT ? names[spam] : "";
}

// Apple continuity: a list of type, length, value messages after the company id
static void summarize_apple(const uint8_t *d, uint8_t n, ble_ad_summary_t *out)
{
    uint8_t kind = BLE_KIND_APPLE;
    for (uint8_t pos = 0; pos + 2 <= n;)
    {
        uint8_t type = d[pos];
        uint8_t len = d[pos + 1];
        if (type == 0x12)
            kind = ble_kind_merge(kind, BLE_KIND_FINDMY);
        else if (type == 0x02 && len == 0x15)
            kind = ble_kind_merge(kind, BLE_KIND_IBEACON);
        else if (type == 0x07 || type == 0x0F)
            out->spam = BLE_SPAM_APPLE;
        pos = (uint8_t)(pos + 2 + len);
    }
    out->kind = ble_kind_merge(out->kind, kind);
}

static void summarize_uuid(uint16_t uuid, const uint8_t *data, uint8_t data_len,
                           ble_ad_summary_t *out)
{
    uint8_t kind = BLE_KIND_PLAIN;
    switch (uuid)
    {
    case BLE_UUID_TILE:
    case BLE_UUID_TILE2:
        kind = BLE_KIND_TILE;
        break;
    case BLE_UUID_SMARTTAG:
        kind = BLE_KIND_SMARTTAG;
        break;
    case BLE_UUID_EDDYSTONE:
        // Google's Find My Device network rides on the Eddystone UUID with
        // frame type 0x40, or 0x41 in unwanted tracking protection mode
        if (data && data_len >= 1 && (data[0] == 0x40 || data[0] == 0x41))
            kind = BLE_KIND_GOOGLE_FMDN;
        else
            kind = BLE_KIND_EDDYSTONE;
        break;
    case BLE_UUID_FASTPAIR:
        kind = BLE_KIND_FASTPAIR;
        // A bare 3 byte model id is the "pair me" popup that spam tools repeat
        if (data && data_len == 3)
            out->spam = BLE_SPAM_FASTPAIR;
        break;
    case BLE_UUID_MATTER:
        kind = BLE_KIND_MATTER;
        break;
    case BLE_UUID_BROADCAST_AUDIO:
    case BLE_UUID_PUBLIC_BROADCAST:
        kind = BLE_KIND_AURACAST;
        break;
    case BLE_UUID_MESH_PROV:
    case BLE_UUID_MESH_PROXY:
        kind = BLE_KIND_MESH;
        break;
    default:
        break;
    }
    out->kind = ble_kind_merge(out->kind, kind);
}

void ble_ad_summarize(const uint8_t *ad, uint8_t len, ble_ad_summary_t *out)
{
    ble_ad_iter_t it;
    uint8_t type, n;
    const uint8_t *v;

    memset(out, 0, sizeof(*out));
    ble_ad_iter_init(&it, ad, len);
    while (ble_ad_next(&it, &type, &v, &n))
    {
        switch (type)
        {
        case BLE_AD_NAME_SHORT:
        case BLE_AD_NAME_FULL:
        case BLE_AD_BROADCAST_NAME:
            if (!out->name || type != BLE_AD_NAME_SHORT)
            {
                out->name = v;
                out->name_len = n;
            }
            break;

        case BLE_AD_MANUFACTURER:
            if (n < 2)
                break;
            out->company = le16(v);
            if (out->company == BLE_COMPANY_APPLE)
            {
                summarize_apple(v + 2, (uint8_t)(n - 2), out);
            }
            else if (out->company == BLE_COMPANY_MICROSOFT)
            {
                out->kind = ble_kind_merge(out->kind, BLE_KIND_MICROSOFT);
                // Swift Pair: beacon id 0x03
                if (n >= 3 && v[2] == 0x03)
                    out->spam = BLE_SPAM_SWIFTPAIR;
            }
            else if (out->company == BLE_COMPANY_SAMSUNG && n >= 6)
            {
                // EasySetup popups for buds (42 09 81) and watches (01 00 02 00)
                if ((v[2] == 0x42 && v[3] == 0x09 && v[4] == 0x81) ||
                    (v[2] == 0x01 && v[3] == 0x00 && v[4] == 0x02 && v[5] == 0x00))
                    out->spam = BLE_SPAM_SAMSUNG;
            }
            break;

        case BLE_AD_SERVICE_DATA16:
            if (n >= 2)
                summarize_uuid(le16(v), v + 2, (uint8_t)(n - 2), out);
            break;

        case BLE_AD_UUID16_SOME:
        case BLE_AD_UUID16_ALL:
            for (uint8_t i = 0; i + 1 < n; i += 2)
                summarize_uuid(le16(&v[i]), 0, 0, out);
            break;

        case BLE_AD_MESH_PB_ADV:
        case BLE_AD_MESH_MESSAGE:
        case BLE_AD_MESH_BEACON:
            out->kind = ble_kind_merge(out->kind, BLE_KIND_MESH);
            break;

        default:
            break;
        }
    }

    ble_sensor_t sensor;
    if (ble_sensor_decode(ad, len, &sensor))
        out->kind = ble_kind_merge(out->kind, BLE_KIND_SENSOR);
}

// ---------------------------------------------------------------------------
// Extended advertising
// ---------------------------------------------------------------------------

#define EXT_ADVA 0x01
#define EXT_TARGETA 0x02
#define EXT_CTEINFO 0x04
#define EXT_ADI 0x08
#define EXT_AUXPTR 0x10
#define EXT_SYNCINFO 0x20
#define EXT_TXPOWER 0x40

bool ble_ext_parse(const uint8_t *p, uint8_t len, ble_ext_t *out)
{
    memset(out, 0, sizeof(*out));
    if (len < 1)
        return false;

    uint8_t hdr_len = p[0] & 0x3F;
    out->adv_mode = p[0] >> 6;
    if (1 + hdr_len > len)
        return false;

    out->ad = &p[1 + hdr_len];
    out->ad_len = (uint8_t)(len - 1 - hdr_len);
    if (hdr_len == 0)
        return true;

    uint8_t flags = p[1];
    uint8_t pos = 2;
    uint8_t end = (uint8_t)(1 + hdr_len);

    // Fields follow in flag bit order; each one must fit the header
    if (flags & EXT_ADVA)
    {
        if (pos + 6 > end)
            return false;
        out->adva = &p[pos];
        pos += 6;
    }
    if (flags & EXT_TARGETA)
        pos += 6;
    if (flags & EXT_CTEINFO)
        pos += 1;
    if (flags & EXT_ADI)
    {
        if (pos + 2 > end)
            return false;
        uint16_t adi = le16(&p[pos]);
        out->has_adi = true;
        out->did = adi & 0x0FFF;
        out->sid = (uint8_t)(adi >> 12);
        pos += 2;
    }
    if (flags & EXT_AUXPTR)
    {
        if (pos + 3 > end)
            return false;
        uint16_t off = le16(&p[pos + 1]);
        out->has_aux = true;
        out->aux_chan = p[pos] & 0x3F;
        out->aux_unit_us = (p[pos] & 0x80) ? 300 : 30;
        out->aux_offset_us = (uint32_t)(off & 0x1FFF) * out->aux_unit_us;
        out->aux_phy = (uint8_t)(off >> 13);
        pos += 3;
    }
    if (flags & EXT_SYNCINFO)
        pos += 18;
    if (flags & EXT_TXPOWER)
    {
        if (pos + 1 > end)
            return false;
        out->has_tx_power = true;
        out->tx_power = (int8_t)p[pos];
        pos += 1;
    }
    return pos <= end;
}

uint8_t ble_data_channel_freq(uint8_t chan)
{
    // Data channels 0..10 sit at 2404..2424 MHz, 11..36 at 2428..2478 MHz,
    // around the advertising channels at 2402, 2426 and 2480
    if (chan <= 10)
        return (uint8_t)(4 + 2 * chan);
    return (uint8_t)(28 + 2 * (chan - 11));
}

// ---------------------------------------------------------------------------
// Name tables. Short on purpose: the common ones only.
// ---------------------------------------------------------------------------

typedef struct
{
    uint16_t id;
    const char *name;
} id_name_t;

static const char *lookup(const id_name_t *table, uint8_t count, uint16_t id)
{
    for (uint8_t i = 0; i < count; i++)
    {
        if (table[i].id == id)
            return table[i].name;
    }
    return 0;
}

#define COUNT_OF(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

static const id_name_t companies[] = {
    {0x0000, "ERICSSON"},  {0x0002, "INTEL"},     {0x0006, "MICROSOFT"}, {0x000A, "QUALCOMM"},
    {0x000D, "TI"},        {0x000F, "BROADCOM"},  {0x001D, "QUALCOMM"},  {0x0030, "ST"},
    {0x0046, "MEDIATEK"},  {0x004C, "APPLE"},     {0x0059, "NORDIC"},    {0x005D, "REALTEK"},
    {0x0075, "SAMSUNG"},   {0x0078, "NIKE"},      {0x0087, "GARMIN"},    {0x00D2, "DIALOG"},
    {0x00E0, "GOOGLE"},    {0x0131, "CYPRESS"},   {0x0157, "HUAMI"},     {0x0171, "AMAZON"},
    {0x01DA, "LOGITECH"},  {0x02E5, "ESPRESSIF"}, {0x02FF, "SILABS"},    {0x038F, "XIAOMI"},
    {0x0499, "RUUVI"},     {0x05A7, "SONOS"},     {0x0822, "ADAFRUIT"},  {0x0969, "SWITCHBOT"},
    {0xEC88, "GOVEE"},
};

static const id_name_t uuids[] = {
    {0x1800, "GAP"},          {0x1801, "GATT"},        {0x1802, "IMMED ALERT"},
    {0x1803, "LINK LOSS"},    {0x1804, "TX POWER"},    {0x1805, "TIME"},
    {0x1809, "THERMOMETER"},  {0x180A, "DEVICE INFO"}, {0x180D, "HEART RATE"},
    {0x180F, "BATTERY"},      {0x1810, "BLOOD PRESS"}, {0x1812, "HID"},
    {0x1816, "CYCLING SPEED"}, {0x1818, "CYCLING POWER"}, {0x181A, "ENV SENSING"},
    {0x181C, "USER DATA"},    {0x181D, "WEIGHT"},      {0x1822, "PULSE OXIM"},
    {0x1826, "FITNESS"},      {0x1827, "MESH PROV"},   {0x1828, "MESH PROXY"},
    {0x184E, "AUDIO STREAM"}, {0x1851, "BASIC AUDIO"}, {0x1852, "BCAST AUDIO"},
    {0x1853, "COMMON AUDIO"}, {0x1854, "HEARING AID"}, {0x1856, "PUBLIC BCAST"},
    {0xFCD2, "BTHOME"},       {0xFCF1, "GOOGLE"},      {0xFD3D, "SWITCHBOT"},
    {0xFD5A, "SMARTTAG"},     {0xFD69, "SAMSUNG FIND"}, {0xFD6F, "EXPOSURE NOTIF"},
    {0xFE03, "AMAZON"},       {0xFE0F, "PHILIPS HUE"}, {0xFE2C, "FAST PAIR"},
    {0xFE95, "XIAOMI"},       {0xFE9F, "GOOGLE"},      {0xFEAA, "EDDYSTONE"},
    {0xFEEC, "TILE"},         {0xFEED, "TILE"},        {0xFFF6, "MATTER"},
};

// Appearance: the upper 10 bits are the category
static const char *const appearance_categories[] = {
    "UNKNOWN",     "PHONE",        "COMPUTER",    "WATCH",        "CLOCK",
    "DISPLAY",     "REMOTE",       "GLASSES",     "TAG",          "KEYRING",
    "MEDIA PLAYER", "BARCODE",     "THERMOMETER", "HEART RATE",   "BLOOD PRESS",
    "HID",         "GLUCOSE",      "RUNNING",     "CYCLING",      "CONTROL",
    "NETWORK",     "SENSOR",       "LIGHT",       "FAN",          "HVAC",
    "AIRCON",      "HUMIDIFIER",   "HEATING",     "ACCESS CTRL",  "MOTORIZED",
    "POWER",       "LIGHT SOURCE", "WINDOW COVER", "AUDIO SINK",  "AUDIO SOURCE",
    "VEHICLE",     "APPLIANCE",    "EARPHONES",   "AIRCRAFT",     "AV EQUIPMENT",
    "DISPLAY EQ",  "HEARING AID",  "GAMING",      "SIGNAGE",
};

static const id_name_t appearances[] = {
    {0x03C1, "KEYBOARD"}, {0x03C2, "MOUSE"},   {0x03C3, "JOYSTICK"},
    {0x03C4, "GAMEPAD"},  {0x0941, "EARBUD"},  {0x0942, "HEADSET"},
    {0x0943, "HEADPHONES"},
};

static const char *const apple_types[] = {
    [0x03] = "AIRPRINT",   [0x05] = "AIRDROP",     [0x06] = "HOMEKIT",
    [0x07] = "PROX PAIR",  [0x08] = "HEY SIRI",    [0x09] = "AIRPLAY TGT",
    [0x0A] = "AIRPLAY SRC", [0x0B] = "MAGIC SWITCH", [0x0C] = "HANDOFF",
    [0x0D] = "TETHER TGT", [0x0E] = "HOTSPOT",     [0x0F] = "NEARBY ACTION",
    [0x10] = "NEARBY INFO", [0x12] = "FIND MY",
};

const char *ble_company_name(uint16_t company)
{
    return lookup(companies, COUNT_OF(companies), company);
}

const char *ble_uuid16_name(uint16_t uuid)
{
    return lookup(uuids, COUNT_OF(uuids), uuid);
}

const char *ble_appearance_name(uint16_t appearance)
{
    const char *name = lookup(appearances, COUNT_OF(appearances), appearance);
    if (name)
        return name;
    uint16_t category = appearance >> 6;
    if (category < COUNT_OF(appearance_categories))
        return appearance_categories[category];
    return "OTHER";
}

const char *ble_apple_type_name(uint8_t type)
{
    if (type == 0x02)
        return "IBEACON";
    if (type < COUNT_OF(apple_types) && apple_types[type])
        return apple_types[type];
    return 0;
}

// ---------------------------------------------------------------------------
// Line builder
// ---------------------------------------------------------------------------

void ble_ln_init(ble_line_t *l, const char *text)
{
    l->n = 0;
    l->s[0] = '\0';
    ble_ln_str(l, text);
}

void ble_ln_char(ble_line_t *l, char c)
{
    if (l->n + 1 < BLE_LINE_LEN)
    {
        l->s[l->n++] = c;
        l->s[l->n] = '\0';
    }
}

void ble_ln_str(ble_line_t *l, const char *text)
{
    while (text && *text)
        ble_ln_char(l, *text++);
}

void ble_ln_int(ble_line_t *l, int32_t v)
{
    char tmp[12];
    uint8_t n = 0;
    uint32_t u = v < 0 ? (uint32_t)(-(v + 1)) + 1u : (uint32_t)v;
    if (v < 0)
        ble_ln_char(l, '-');
    do
    {
        tmp[n++] = (char)('0' + u % 10u);
        u /= 10u;
    } while (u);
    while (n)
        ble_ln_char(l, tmp[--n]);
}

void ble_ln_hex(ble_line_t *l, uint32_t v, uint8_t digits)
{
    static const char hex[] = "0123456789ABCDEF";
    while (digits--)
        ble_ln_char(l, hex[(v >> (digits * 4)) & 15]);
}

void ble_ln_fixed(ble_line_t *l, int32_t v, uint8_t decimals)
{
    int32_t scale = 1;
    for (uint8_t i = 0; i < decimals; i++)
        scale *= 10;
    if (v < 0)
    {
        ble_ln_char(l, '-');
        v = -v;
    }
    ble_ln_int(l, v / scale);
    if (decimals)
    {
        ble_ln_char(l, '.');
        int32_t frac = v % scale;
        for (int32_t s = scale / 10; s > 0; s /= 10)
            ble_ln_char(l, (char)('0' + (frac / s) % 10));
    }
}

void ble_ln_bytes(ble_line_t *l, const uint8_t *b, uint8_t n)
{
    for (uint8_t i = 0; i < n; i++)
        ble_ln_char(l, (b[i] >= 32 && b[i] < 127) ? (char)b[i] : '.');
}

void ble_lines_add(ble_lines_t *out, const ble_line_t *l)
{
    if (out->count < out->max)
    {
        memcpy(out->lines[out->count], l->s, BLE_LINE_LEN);
        out->count++;
    }
}

// ---------------------------------------------------------------------------
// Detail card text
// ---------------------------------------------------------------------------

static void emit(ble_lines_t *out, ble_line_t *l)
{
    ble_lines_add(out, l);
}

static void emit_hex_bytes(ble_lines_t *out, const char *label, const uint8_t *b, uint8_t n)
{
    ble_line_t l;
    ble_ln_init(&l, label);
    for (uint8_t i = 0; i < n; i++)
        ble_ln_hex(&l, b[i], 2);
    emit(out, &l);
}

static void describe_apple(const uint8_t *d, uint8_t n, ble_lines_t *out)
{
    ble_line_t l;
    for (uint8_t pos = 0; pos + 2 <= n;)
    {
        uint8_t type = d[pos];
        uint8_t len = d[pos + 1];
        const uint8_t *v = &d[pos + 2];
        bool whole = pos + 2 + len <= n;
        const char *name = ble_apple_type_name(type);

        ble_ln_init(&l, " ");
        if (name)
            ble_ln_str(&l, name);
        else
        {
            ble_ln_str(&l, "TYPE ");
            ble_ln_hex(&l, type, 2);
        }

        if (type == 0x12)
        {
            // Offline finding: the full 22 byte key share means the tag is
            // away from its owner, the 2 byte form means it is near them
            ble_ln_str(&l, len >= 0x19 ? " SEPARATED" : " NEAR OWNER");
            emit(out, &l);
        }
        else if (type == 0x02 && len == 0x15 && whole)
        {
            emit(out, &l);
            emit_hex_bytes(out, " UUID ", v, 8);
            ble_ln_init(&l, " MAJ ");
            ble_ln_int(&l, be16(&v[16]));
            ble_ln_str(&l, " MIN ");
            ble_ln_int(&l, be16(&v[18]));
            ble_ln_str(&l, " PWR ");
            ble_ln_int(&l, (int8_t)v[20]);
            emit(out, &l);
        }
        else if (type == 0x07 && len >= 3 && whole)
        {
            // Proximity pairing (AirPods and Beats cases): model id
            ble_ln_str(&l, " MODEL ");
            ble_ln_hex(&l, be16(&v[1]), 4);
            emit(out, &l);
        }
        else
        {
            emit(out, &l);
        }
        pos = (uint8_t)(pos + 2 + len);
    }
}

static void describe_microsoft(const uint8_t *d, uint8_t n, ble_lines_t *out)
{
    static const char *const cdp_types[] = {
        [1] = "XBOX",     [6] = "IPHONE",   [7] = "IPAD",       [8] = "ANDROID",
        [9] = "WINDOWS DESKTOP", [11] = "WINDOWS PHONE", [12] = "LINUX",
        [13] = "WINDOWS IOT", [14] = "SURFACE HUB", [15] = "WINDOWS LAPTOP",
        [16] = "WINDOWS TABLET",
    };
    ble_line_t l;
    if (n >= 2 && d[0] == 0x01)
    {
        // Connected Devices Platform beacon: version and device type
        uint8_t type = d[1] & 0x1F;
        ble_ln_init(&l, " CDP ");
        if (type < COUNT_OF(cdp_types) && cdp_types[type])
            ble_ln_str(&l, cdp_types[type]);
        else
            ble_ln_int(&l, type);
        emit(out, &l);
    }
    else if (n >= 1 && d[0] == 0x03)
    {
        ble_ln_init(&l, " SWIFT PAIR");
        emit(out, &l);
        // Sub scenario 0 carries a reserved RSSI byte, then the display name
        if (n > 3 && d[1] == 0x00)
        {
            ble_ln_init(&l, " ");
            ble_ln_bytes(&l, &d[3], (uint8_t)(n - 3));
            emit(out, &l);
        }
    }
}

static void describe_eddystone(const uint8_t *d, uint8_t n, ble_lines_t *out)
{
    static const char *const schemes[] = {"HTTP://WWW.", "HTTPS://WWW.", "HTTP://", "HTTPS://"};
    static const char *const expansions[] = {".COM/", ".ORG/", ".EDU/", ".NET/", ".INFO/",
                                             ".BIZ/", ".GOV/", ".COM",  ".ORG",  ".EDU",
                                             ".NET",  ".INFO", ".BIZ",  ".GOV"};
    ble_line_t l;
    if (n < 1)
        return;

    switch (d[0])
    {
    case 0x00:
        if (n >= 18)
        {
            emit_hex_bytes(out, " UID NS ", &d[2], 10);
            emit_hex_bytes(out, " UID ID ", &d[12], 6);
        }
        break;
    case 0x10:
        ble_ln_init(&l, " ");
        if (n >= 3 && d[2] < 4)
            ble_ln_str(&l, schemes[d[2]]);
        for (uint8_t i = 3; i < n; i++)
        {
            if (d[i] < COUNT_OF(expansions))
                ble_ln_str(&l, expansions[d[i]]);
            else
                ble_ln_bytes(&l, &d[i], 1);
        }
        emit(out, &l);
        break;
    case 0x20:
        if (n >= 6)
        {
            ble_ln_init(&l, " TLM ");
            ble_ln_int(&l, be16(&d[2]));
            ble_ln_str(&l, "MV ");
            // Temperature is signed 8.8 fixed point, 0x8000 when not supported
            if (be16(&d[4]) != 0x8000)
            {
                ble_ln_fixed(&l, ((int16_t)be16(&d[4]) * 10) / 256, 1);
                ble_ln_char(&l, 'C');
            }
            emit(out, &l);
        }
        break;
    case 0x30:
        ble_ln_init(&l, " EID");
        emit(out, &l);
        break;
    case 0x40:
    case 0x41:
        ble_ln_init(&l, " FIND MY DEVICE NETWORK");
        emit(out, &l);
        if (d[0] == 0x41)
        {
            ble_ln_init(&l, " UNWANTED TRACKING MODE");
            emit(out, &l);
        }
        break;
    default:
        break;
    }
}

static void describe_service_data(uint16_t uuid, const uint8_t *d, uint8_t n, ble_lines_t *out)
{
    ble_line_t l;
    switch (uuid)
    {
    case BLE_UUID_EDDYSTONE:
        describe_eddystone(d, n, out);
        break;

    case BLE_UUID_MATTER:
        // Commissionable node advert: opcode, 12 bit discriminator with a 4
        // bit version, vendor id, product id
        if (n >= 7 && d[0] == 0x00)
        {
            ble_ln_init(&l, " DISCRIM ");
            ble_ln_int(&l, le16(&d[1]) & 0x0FFF);
            ble_ln_str(&l, " VER ");
            ble_ln_int(&l, le16(&d[1]) >> 12);
            emit(out, &l);
            ble_ln_init(&l, " VID ");
            ble_ln_hex(&l, le16(&d[3]), 4);
            ble_ln_str(&l, " PID ");
            ble_ln_hex(&l, le16(&d[5]), 4);
            emit(out, &l);
        }
        break;

    case BLE_UUID_BROADCAST_AUDIO:
        if (n >= 3)
        {
            ble_ln_init(&l, " AURACAST ID ");
            ble_ln_hex(&l, (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16), 6);
            emit(out, &l);
        }
        break;

    case BLE_UUID_PUBLIC_BROADCAST:
        if (n >= 1)
        {
            ble_ln_init(&l, " PBP");
            if (d[0] & 0x01)
                ble_ln_str(&l, " ENCRYPTED");
            if (d[0] & 0x02)
                ble_ln_str(&l, " SQ");
            if (d[0] & 0x04)
                ble_ln_str(&l, " HQ");
            emit(out, &l);
        }
        break;

    case BLE_UUID_FASTPAIR:
        if (n == 3)
        {
            ble_ln_init(&l, " MODEL ");
            ble_ln_hex(&l, ((uint32_t)d[0] << 16) | ((uint32_t)d[1] << 8) | d[2], 6);
        }
        else
        {
            ble_ln_init(&l, " ACCOUNT DATA");
        }
        emit(out, &l);
        break;

    case BLE_UUID_MESH_PROXY:
        if (n >= 1)
        {
            ble_ln_init(&l, d[0] == 0x00 ? " NETWORK ID" : " NODE IDENTITY");
            emit(out, &l);
        }
        break;

    default:
        break;
    }
}

static void describe_mesh_beacon(const uint8_t *d, uint8_t n, ble_lines_t *out)
{
    ble_line_t l;
    if (n >= 17 && d[0] == 0x00)
    {
        ble_ln_init(&l, "MESH UNPROVISIONED");
        emit(out, &l);
        emit_hex_bytes(out, " UUID ", &d[1], 8);
        emit_hex_bytes(out, "      ", &d[9], 8);
        if (n >= 19)
        {
            ble_ln_init(&l, " OOB ");
            ble_ln_hex(&l, be16(&d[17]), 4);
            emit(out, &l);
        }
    }
    else if (n >= 14 && d[0] == 0x01)
    {
        ble_ln_init(&l, "MESH NET BEACON");
        emit(out, &l);
        ble_ln_init(&l, " KEY REFRESH ");
        ble_ln_int(&l, d[1] & 1);
        ble_ln_str(&l, " IV UPD ");
        ble_ln_int(&l, (d[1] >> 1) & 1);
        emit(out, &l);
        emit_hex_bytes(out, " NET ", &d[2], 8);
        ble_ln_init(&l, " IV INDEX ");
        ble_ln_int(&l, (int32_t)(((uint32_t)d[10] << 24) | ((uint32_t)d[11] << 16) |
                                 ((uint32_t)d[12] << 8) | d[13]));
        emit(out, &l);
    }
    else if (n >= 1 && d[0] == 0x02)
    {
        ble_ln_init(&l, "MESH PRIVATE BEACON");
        emit(out, &l);
    }
    else
    {
        ble_ln_init(&l, "MESH BEACON ?");
        emit(out, &l);
    }
}

static void describe_sensor(const uint8_t *ad, uint8_t len, ble_lines_t *out)
{
    ble_sensor_t s;
    ble_line_t l;
    if (!ble_sensor_decode(ad, len, &s))
        return;

    ble_ln_init(&l, "SENSOR ");
    ble_ln_str(&l, ble_sensor_name(s.fmt));
    if (s.valid & BLE_SENSOR_ENCRYPTED)
        ble_ln_str(&l, " ENCRYPTED");
    emit(out, &l);

    if (s.valid & (BLE_SENSOR_TEMP | BLE_SENSOR_HUM))
    {
        ble_ln_init(&l, " ");
        if (s.valid & BLE_SENSOR_TEMP)
        {
            ble_ln_fixed(&l, s.temp, 2);
            ble_ln_str(&l, "C ");
        }
        if (s.valid & BLE_SENSOR_HUM)
        {
            ble_ln_fixed(&l, s.hum, 2);
            ble_ln_str(&l, " RH");
        }
        emit(out, &l);
    }
    if (s.valid & (BLE_SENSOR_BATT | BLE_SENSOR_MV))
    {
        ble_ln_init(&l, " BATT");
        if (s.valid & BLE_SENSOR_BATT)
        {
            ble_ln_char(&l, ' ');
            ble_ln_int(&l, s.batt);
            ble_ln_str(&l, " PCT");
        }
        if (s.valid & BLE_SENSOR_MV)
        {
            ble_ln_char(&l, ' ');
            ble_ln_int(&l, s.mv);
            ble_ln_str(&l, "MV");
        }
        emit(out, &l);
    }
}

void ble_describe_ad(const uint8_t *ad, uint8_t len, ble_lines_t *out)
{
    ble_ad_iter_t it;
    uint8_t type, n;
    const uint8_t *v;
    ble_line_t l;

    ble_ad_iter_init(&it, ad, len);
    while (ble_ad_next(&it, &type, &v, &n))
    {
        switch (type)
        {
        case BLE_AD_FLAGS:
            ble_ln_init(&l, "FLAGS ");
            if (n >= 1)
            {
                ble_ln_hex(&l, v[0], 2);
                if (v[0] & 0x01)
                    ble_ln_str(&l, " LIM");
                if (v[0] & 0x02)
                    ble_ln_str(&l, " GEN");
                if (v[0] & 0x04)
                    ble_ln_str(&l, " LE-ONLY");
                if (v[0] & 0x18)
                    ble_ln_str(&l, " BR/EDR");
            }
            emit(out, &l);
            break;

        case BLE_AD_NAME_SHORT:
        case BLE_AD_NAME_FULL:
        case BLE_AD_BROADCAST_NAME:
            ble_ln_init(&l, type == BLE_AD_BROADCAST_NAME ? "BCAST " : "NAME ");
            ble_ln_bytes(&l, v, n);
            emit(out, &l);
            break;

        case BLE_AD_TX_POWER:
            if (n >= 1)
            {
                ble_ln_init(&l, "TX POWER ");
                ble_ln_int(&l, (int8_t)v[0]);
                ble_ln_str(&l, " DBM");
                emit(out, &l);
            }
            break;

        case BLE_AD_APPEARANCE:
            if (n >= 2)
            {
                ble_ln_init(&l, "LOOKS ");
                ble_ln_hex(&l, le16(v), 4);
                ble_ln_char(&l, ' ');
                ble_ln_str(&l, ble_appearance_name(le16(v)));
                emit(out, &l);
            }
            break;

        case BLE_AD_UUID16_SOME:
        case BLE_AD_UUID16_ALL:
            for (uint8_t i = 0; i + 1 < n; i += 2)
            {
                ble_ln_init(&l, "UUID ");
                ble_ln_hex(&l, le16(&v[i]), 4);
                ble_ln_char(&l, ' ');
                ble_ln_str(&l, ble_uuid16_name(le16(&v[i])));
                emit(out, &l);
            }
            break;

        case BLE_AD_UUID32_SOME:
        case BLE_AD_UUID32_ALL:
            for (uint8_t i = 0; i + 3 < n; i += 4)
            {
                ble_ln_init(&l, "UUID ");
                ble_ln_hex(&l, (uint32_t)le16(&v[i]) | ((uint32_t)le16(&v[i + 2]) << 16), 8);
                emit(out, &l);
            }
            break;

        case BLE_AD_UUID128_SOME:
        case BLE_AD_UUID128_ALL:
            // Most significant bytes first, the way UUIDs are written
            for (uint8_t i = 0; i + 15 < n; i += 16)
            {
                ble_ln_init(&l, "UUID ");
                for (uint8_t b = 0; b < 12; b++)
                    ble_ln_hex(&l, v[i + 15 - b], 2);
                ble_ln_str(&l, "..");
                emit(out, &l);
            }
            break;

        case BLE_AD_SERVICE_DATA16:
            if (n >= 2)
            {
                uint16_t uuid = le16(v);
                const char *name = ble_uuid16_name(uuid);
                ble_ln_init(&l, "DATA ");
                ble_ln_hex(&l, uuid, 4);
                ble_ln_char(&l, ' ');
                ble_ln_str(&l, name);
                emit(out, &l);
                describe_service_data(uuid, v + 2, (uint8_t)(n - 2), out);
            }
            break;

        case BLE_AD_MANUFACTURER:
            if (n >= 2)
            {
                uint16_t company = le16(v);
                ble_ln_init(&l, "MFG ");
                ble_ln_hex(&l, company, 4);
                ble_ln_char(&l, ' ');
                ble_ln_str(&l, ble_company_name(company));
                emit(out, &l);
                if (company == BLE_COMPANY_APPLE)
                    describe_apple(v + 2, (uint8_t)(n - 2), out);
                else if (company == BLE_COMPANY_MICROSOFT)
                    describe_microsoft(v + 2, (uint8_t)(n - 2), out);
            }
            break;

        case BLE_AD_MESH_PB_ADV:
            ble_ln_init(&l, "MESH PROVISIONING");
            emit(out, &l);
            break;

        case BLE_AD_MESH_MESSAGE:
            ble_ln_init(&l, "MESH MESSAGE ENCRYPTED");
            emit(out, &l);
            break;

        case BLE_AD_MESH_BEACON:
            describe_mesh_beacon(v, n, out);
            break;

        default:
            ble_ln_init(&l, "AD ");
            ble_ln_hex(&l, type, 2);
            ble_ln_str(&l, " LEN ");
            ble_ln_int(&l, n);
            emit(out, &l);
            break;
        }
    }

    describe_sensor(ad, len, out);
}
