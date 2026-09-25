#include <string.h>

#include "ble_adv.h"
#include "ble_sensor.h"

#define UUID_ENV_SENSING 0x181A // ATC1441 / pvvx custom firmware
#define UUID_BTHOME 0xFCD2
#define UUID_MIBEACON 0xFE95
#define UUID_SWITCHBOT 0xFD3D
#define UUID_SWITCHBOT_OLD 0x0D00
#define COMPANY_GOVEE 0xEC88 // not a registered id, Govee just uses it
#define COMPANY_RUUVI 0x0499
#define COMPANY_SWITCHBOT 0x0969

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static void set_temp(ble_sensor_t *s, int32_t centi)
{
    s->temp = (int16_t)centi;
    s->valid |= BLE_SENSOR_TEMP;
}

static void set_hum(ble_sensor_t *s, int32_t centi)
{
    s->hum = (uint16_t)centi;
    s->valid |= BLE_SENSOR_HUM;
}

static void set_batt(ble_sensor_t *s, uint8_t pct)
{
    s->batt = pct > 100 ? 100 : pct;
    s->valid |= BLE_SENSOR_BATT;
}

static void set_mv(ble_sensor_t *s, uint16_t mv)
{
    s->mv = mv;
    s->valid |= BLE_SENSOR_MV;
}

// ---------------------------------------------------------------------------
// BTHome v2
// ---------------------------------------------------------------------------

// Object sizes by object id, 0 for ids we do not know (decoding stops there,
// the spec requires ids in ascending order so the common ones come first),
// 0xFF for the two variable length ones
#define BTH_VAR 0xFF
static const uint8_t bthome_size[0x61] = {
    1, 1, 2, 2, 3, 3, 2, 2, 2, 1, 3, 3, 2, 2, 2, 1,       // 0x00
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,       // 0x10
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,       // 0x20
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 2, 2, 4, 2,       // 0x30
    2, 2, 3, 2, 2, 2, 1, 2, 2, 2, 2, 3, 4, 4, 4, 4,       // 0x40
    4, 2, 2, BTH_VAR, BTH_VAR, 4, 2, 1, 1, 1, 2, 4, 4, 2, 2, 2, // 0x50
    1,                                                    // 0x60
};

static bool decode_bthome(const uint8_t *d, uint8_t n, ble_sensor_t *s)
{
    if (n < 1)
        return false;

    uint8_t info = d[0];
    if ((info >> 5) != 2)
        return false; // v1 or unknown
    s->fmt = BLE_SENSOR_BTHOME;
    if (info & 0x01)
    {
        s->valid = BLE_SENSOR_ENCRYPTED;
        return true;
    }

    uint8_t pos = 1;
    while (pos < n)
    {
        uint8_t id = d[pos++];
        if (id >= sizeof(bthome_size) || bthome_size[id] == 0)
            break;
        uint8_t size = bthome_size[id];
        if (size == BTH_VAR)
        {
            if (pos >= n)
                break;
            size = (uint8_t)(d[pos] + 1);
        }
        if (pos + size > n)
            break;

        const uint8_t *v = &d[pos];
        switch (id)
        {
        case 0x01:
            set_batt(s, v[0]);
            break;
        case 0x02: // sint16, 0.01
            set_temp(s, (int16_t)le16(v));
            break;
        case 0x45: // sint16, 0.1
            set_temp(s, (int16_t)le16(v) * 10);
            break;
        case 0x57: // sint8, 1
            set_temp(s, (int8_t)v[0] * 100);
            break;
        case 0x03: // uint16, 0.01
            set_hum(s, le16(v));
            break;
        case 0x2E: // uint8, 1
            set_hum(s, v[0] * 100);
            break;
        case 0x0C: // uint16, 0.001 V
            set_mv(s, le16(v));
            break;
        default:
            break;
        }
        pos = (uint8_t)(pos + size);
    }
    return true;
}

// ---------------------------------------------------------------------------
// ATC1441 and pvvx custom firmware
// ---------------------------------------------------------------------------

static bool decode_env_sensing(const uint8_t *d, uint8_t n, ble_sensor_t *s)
{
    if (n == 13)
    {
        // ATC1441: MAC (big endian), temp BE 0.1, hum %, batt %, mV BE, count
        s->fmt = BLE_SENSOR_ATC;
        set_temp(s, (int16_t)be16(&d[6]) * 10);
        set_hum(s, d[8] * 100);
        set_batt(s, d[9]);
        set_mv(s, be16(&d[10]));
        return true;
    }
    if (n == 15)
    {
        // pvvx: MAC (little endian), temp LE 0.01, hum LE 0.01, mV LE, batt %, count, flags
        s->fmt = BLE_SENSOR_PVVX;
        set_temp(s, (int16_t)le16(&d[6]));
        set_hum(s, le16(&d[8]));
        set_mv(s, le16(&d[10]));
        set_batt(s, d[12]);
        return true;
    }
    if (n == 8 || n == 11)
    {
        // The encrypted variants of the two formats above
        s->fmt = BLE_SENSOR_PVVX;
        s->valid = BLE_SENSOR_ENCRYPTED;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Xiaomi MiBeacon
// ---------------------------------------------------------------------------

#define MI_ENCRYPTED 0x0008
#define MI_MAC 0x0010
#define MI_CAPABILITY 0x0020
#define MI_OBJECT 0x0040

static bool decode_mibeacon(const uint8_t *d, uint8_t n, ble_sensor_t *s)
{
    if (n < 5)
        return false;

    uint16_t ctrl = le16(d);
    uint8_t pos = 5; // frame control, product id, frame counter
    if (ctrl & MI_MAC)
        pos += 6;
    if (ctrl & MI_CAPABILITY)
    {
        if (pos >= n)
            return false;
        // Capability 0x20 announces two more I/O capability bytes
        pos = (uint8_t)(pos + ((d[pos] & 0x20) ? 3 : 1));
    }
    if (!(ctrl & MI_OBJECT))
        return false; // pairing or presence advert, no reading in it

    s->fmt = BLE_SENSOR_MIBEACON;
    if (ctrl & MI_ENCRYPTED)
    {
        s->valid = BLE_SENSOR_ENCRYPTED;
        return true;
    }

    while (pos + 3 <= n)
    {
        uint16_t id = le16(&d[pos]);
        uint8_t len = d[pos + 2];
        const uint8_t *v = &d[pos + 3];
        if (pos + 3 + len > n)
            break;

        if (id == 0x1004 && len >= 2)
            set_temp(s, (int16_t)le16(v) * 10);
        else if (id == 0x1006 && len >= 2)
            set_hum(s, le16(v) * 10);
        else if (id == 0x100A && len >= 1)
            set_batt(s, v[0]);
        else if (id == 0x100D && len >= 4)
        {
            set_temp(s, (int16_t)le16(v) * 10);
            set_hum(s, le16(&v[2]) * 10);
        }
        pos = (uint8_t)(pos + 3 + len);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Govee
// ---------------------------------------------------------------------------

static bool decode_govee(const uint8_t *d, uint8_t n, ble_sensor_t *s)
{
    if (n == 6)
    {
        // H5075 / H5072 / H5101: 00, then temperature and humidity packed into
        // one 24 bit big endian number (temp * 10000 + hum * 10, bit 23 is the
        // sign), then battery %
        uint32_t v = ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];
        bool neg = (v & 0x800000u) != 0;
        v &= 0x7FFFFFu;
        int32_t temp = (int32_t)(v / 1000u) * 10;
        set_temp(s, neg ? -temp : temp);
        set_hum(s, (int32_t)(v % 1000u) * 10);
        set_batt(s, d[4]);
        s->fmt = BLE_SENSOR_GOVEE;
        return true;
    }
    if (n == 7)
    {
        // H5074: 00, temp LE 0.01, hum LE 0.01, battery %
        set_temp(s, (int16_t)le16(&d[1]));
        set_hum(s, le16(&d[3]));
        set_batt(s, d[5]);
        s->fmt = BLE_SENSOR_GOVEE;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Ruuvi RAWv2
// ---------------------------------------------------------------------------

static bool decode_ruuvi(const uint8_t *d, uint8_t n, ble_sensor_t *s)
{
    if (n < 15 || d[0] != 5)
        return false;

    s->fmt = BLE_SENSOR_RUUVI;
    int16_t t = (int16_t)be16(&d[1]);
    if (t != (int16_t)0x8000)
        set_temp(s, t / 2); // 0.005 C steps
    uint16_t h = be16(&d[3]);
    if (h != 0xFFFF)
        set_hum(s, h / 4); // 0.0025 % steps
    uint16_t power = be16(&d[13]);
    if ((power >> 5) != 0x7FF)
        set_mv(s, (uint16_t)((power >> 5) + 1600));
    return true;
}

// ---------------------------------------------------------------------------
// Inkbird IBS-TH1 / TH2
// ---------------------------------------------------------------------------

static bool decode_inkbird(const uint8_t *ad, uint8_t len, ble_sensor_t *s)
{
    const uint8_t *name;
    uint8_t name_len;
    if (!ble_ad_find(ad, len, BLE_AD_NAME_FULL, &name, &name_len) &&
        !ble_ad_find(ad, len, BLE_AD_NAME_SHORT, &name, &name_len))
        return false;
    if (name_len != 3 || (memcmp(name, "sps", 3) != 0 && memcmp(name, "tps", 3) != 0))
        return false;

    // The whole manufacturer field is the reading: what would be the company
    // id is the temperature
    const uint8_t *v;
    uint8_t n;
    if (!ble_ad_find(ad, len, BLE_AD_MANUFACTURER, &v, &n) || n != 9)
        return false;

    s->fmt = BLE_SENSOR_INKBIRD;
    set_temp(s, (int16_t)le16(v));
    if (name[0] == 's') // the TH2 ("tps") has no humidity sensor
        set_hum(s, le16(&v[2]));
    set_batt(s, v[7]);
    return true;
}

// ---------------------------------------------------------------------------
// SwitchBot meters
// ---------------------------------------------------------------------------

static void switchbot_temp(ble_sensor_t *s, const uint8_t *t)
{
    // t[0] low nibble: tenths, t[1]: whole degrees with bit 7 set when
    // positive, t[2]: humidity %
    int32_t centi = ((t[1] & 0x7F) * 10 + (t[0] & 0x0F)) * 10;
    set_temp(s, (t[1] & 0x80) ? centi : -centi);
    set_hum(s, (t[2] & 0x7F) * 100);
}

static bool decode_switchbot(const uint8_t *ad, uint8_t len, ble_sensor_t *s)
{
    const uint8_t *d;
    uint8_t n;
    if (!ble_ad_service_data(ad, len, UUID_SWITCHBOT, &d, &n) &&
        !ble_ad_service_data(ad, len, UUID_SWITCHBOT_OLD, &d, &n))
        return false;
    if (n < 3)
        return false;

    // Meter 'T', Meter Plus 'i', Outdoor Meter 'w'
    uint8_t type = d[0] & 0x7F;
    if (type != 'T' && type != 'i' && type != 'w')
        return false;

    s->fmt = BLE_SENSOR_SWITCHBOT;
    set_batt(s, d[2] & 0x7F);

    // Newer firmware moved the reading to the manufacturer data
    uint16_t company;
    const uint8_t *m;
    uint8_t mn;
    if (n >= 6)
        switchbot_temp(s, &d[3]);
    else if (ble_ad_manufacturer(ad, len, &company, &m, &mn) && company == COMPANY_SWITCHBOT &&
             mn >= 11)
        switchbot_temp(s, &m[8]);
    return true;
}

// ---------------------------------------------------------------------------

bool ble_sensor_decode(const uint8_t *ad, uint8_t len, ble_sensor_t *out)
{
    const uint8_t *d;
    uint8_t n;
    uint16_t company;

    memset(out, 0, sizeof(*out));

    if (ble_ad_service_data(ad, len, UUID_BTHOME, &d, &n) && decode_bthome(d, n, out))
        return true;
    if (ble_ad_service_data(ad, len, UUID_ENV_SENSING, &d, &n) && decode_env_sensing(d, n, out))
        return true;
    if (ble_ad_service_data(ad, len, UUID_MIBEACON, &d, &n) && decode_mibeacon(d, n, out))
        return true;
    if (decode_switchbot(ad, len, out))
        return true;
    if (decode_inkbird(ad, len, out))
        return true;
    if (ble_ad_manufacturer(ad, len, &company, &d, &n))
    {
        if (company == COMPANY_GOVEE && decode_govee(d, n, out))
            return true;
        if (company == COMPANY_RUUVI && decode_ruuvi(d, n, out))
            return true;
    }

    memset(out, 0, sizeof(*out));
    return false;
}

const char *ble_sensor_name(uint8_t fmt)
{
    static const char *const names[BLE_SENSOR_COUNT] = {
        [BLE_SENSOR_NONE] = "",
        [BLE_SENSOR_BTHOME] = "BTHOME",
        [BLE_SENSOR_ATC] = "ATC",
        [BLE_SENSOR_PVVX] = "PVVX",
        [BLE_SENSOR_MIBEACON] = "XIAOMI",
        [BLE_SENSOR_GOVEE] = "GOVEE",
        [BLE_SENSOR_RUUVI] = "RUUVI",
        [BLE_SENSOR_INKBIRD] = "INKBIRD",
        [BLE_SENSOR_SWITCHBOT] = "SWBOT",
    };
    return fmt < BLE_SENSOR_COUNT ? names[fmt] : "";
}
