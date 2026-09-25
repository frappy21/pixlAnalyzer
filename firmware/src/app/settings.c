#include <string.h>

#include "app_config.h"
#include "settings.h"

#ifndef SETTINGS_HOST_TEST
#include "flash_int.h"
#define PAGE_SIZE FLASH_INT_PAGE_SIZE
#else
#define PAGE_SIZE FLASH_INT_PAGE_SIZE_HOST
#endif

settings_data_t g_settings;
static bool m_dirty;
static uint16_t m_loaded_version;

// Record header; the data follows it and the CRC follows the data. For the
// current version this is exactly settings_record_t below.
typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t size;
} record_header_t;

typedef struct
{
    record_header_t hdr;
    settings_data_t data;
    uint32_t crc;
} settings_record_t;

#define HEADER_SIZE ((uint32_t)sizeof(record_header_t))
#define RECORD_LEN(size) (HEADER_SIZE + (((uint32_t)(size) + 3u) & ~3u) + 4u)

// Larger than any layout this firmware will ever have: a size beyond it is a
// torn or foreign record, not a newer version
#define DATA_SIZE_MAX 256

_Static_assert(sizeof(settings_data_t) % 4 == 0, "settings data must be whole words");
_Static_assert(sizeof(settings_record_t) == RECORD_LEN(sizeof(settings_data_t)),
               "record layout must match the page walk");
_Static_assert(SETTINGS_V1_SIZE <= sizeof(settings_data_t), "layouts only grow");

uint32_t settings_crc32(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;

    while (len--)
    {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

void settings_defaults(void)
{
    memset(&g_settings, 0, sizeof(g_settings));
    g_settings.contrast = 32;
    g_settings.backlight = 200;
    g_settings.band = BAND_ISM;
    g_settings.dwell = SCAN_DWELL_SAMPLES_DEFAULT;
    g_settings.wf_decim = 2;
    g_settings.wf_mode = WF_DITHER;
    g_settings.auto_floor = 1;
    g_settings.led_hunt = 1;
    g_settings.sleep_min = 0; // no inactivity sleep unless the user asks for it
    g_settings.dim_s = 30;
    g_settings.log_consent = 0;
    g_settings.shuffle = 1;
    g_settings.bat_cal = 1000;

    // v2
    g_settings.invert = 0;
    g_settings.burnin = 1;
    g_settings.saver_min = 0;
    g_settings.sentry_db = 20;
    g_settings.sentry_period = 5;

    // v3
    g_settings.sniff_rate = 0;      // auto: 2Mbit first, then 1Mbit
    g_settings.sniff_bits = 0;      // payload bytes most significant bit first
    g_settings.beacon_type = 0;     // the plain name
    g_settings.beacon_int = 1;      // 250 ms
}

// Values outside their range (a record from a buggy build, a bit flip that
// kept the CRC valid is not a concern) fall back to the default of that field
static void sanitize(void)
{
    settings_data_t *s = &g_settings;
    if (s->contrast > 63)
        s->contrast = 32;
    if (s->band >= BAND_COUNT)
        s->band = BAND_ISM;
    if (s->dwell < 4 || s->dwell > SCAN_DWELL_SAMPLES_MAX)
        s->dwell = SCAN_DWELL_SAMPLES_DEFAULT;
    if (s->wf_decim < 1 || s->wf_decim > 32)
        s->wf_decim = 2;
    if (s->wf_mode >= WF_MODE_COUNT)
        s->wf_mode = WF_DITHER;
    if (s->bat_cal < 800 || s->bat_cal > 1200)
        s->bat_cal = 1000;
    if (s->saver_min > 60)
        s->saver_min = 0;
    if (s->sentry_db < 6 || s->sentry_db > 40)
        s->sentry_db = 20;
    if (s->sentry_period < 1 || s->sentry_period > 50)
        s->sentry_period = 5;
    if (s->sniff_rate > 2)
        s->sniff_rate = 0;
    if (s->sniff_bits > 1)
        s->sniff_bits = 0;
    if (s->beacon_type >= 7) // BLE_BEACON_TYPE_COUNT
        s->beacon_type = 0;
    if (s->beacon_int >= 5) // BLE_BEACON_INTV_COUNT
        s->beacon_int = 1;
}

bool settings_dirty(void) { return m_dirty; }
void settings_mark_dirty(void) { m_dirty = true; }
uint16_t settings_loaded_version(void) { return m_loaded_version; }

// ---------------------------------------------------------------------------
// Page access: the internal flash page on target, a RAM copy on the host
// ---------------------------------------------------------------------------

#ifndef SETTINGS_HOST_TEST

static const uint8_t *page_base(void)
{
    return (const uint8_t *)SETTINGS_PAGE_ADDR;
}

static bool page_erase(void)
{
    return flash_int_erase_page(SETTINGS_PAGE_ADDR);
}

static bool page_write(uint32_t off, const void *data, uint32_t len)
{
    return flash_int_write(SETTINGS_PAGE_ADDR + off, data, len);
}

#else // SETTINGS_HOST_TEST: in-memory backing for the host test harness

static uint8_t m_fake_page[PAGE_SIZE];

uint8_t *settings_host_page(void)
{
    return m_fake_page;
}

static const uint8_t *page_base(void)
{
    return m_fake_page;
}

static bool page_erase(void)
{
    memset(m_fake_page, 0xFF, sizeof(m_fake_page));
    return true;
}

// NOR flash only clears bits
static bool page_write(uint32_t off, const void *data, uint32_t len)
{
    const uint8_t *src = (const uint8_t *)data;
    for (uint32_t i = 0; i < len; i++)
        m_fake_page[off + i] &= src[i];
    return true;
}

#endif

// ---------------------------------------------------------------------------
// Page walk
// ---------------------------------------------------------------------------

typedef enum
{
    WALK_RECORD, // a well formed record at off, next one at off + len
    WALK_ERASED, // erased tail: nothing beyond this point, free space
    WALK_BROKEN, // torn or foreign data: the rest of the page is unusable
} walk_t;

static walk_t walk_at(const uint8_t *page, uint32_t off, uint32_t *len)
{
    if (off + HEADER_SIZE + 4 > PAGE_SIZE)
        return WALK_BROKEN; // no room for even an empty record: page full

    record_header_t hdr;
    memcpy(&hdr, page + off, sizeof(hdr));

    if (hdr.magic == 0xFFFFFFFF)
        return WALK_ERASED;
    if (hdr.magic != SETTINGS_MAGIC || hdr.size == 0 || hdr.size > DATA_SIZE_MAX)
        return WALK_BROKEN;

    *len = RECORD_LEN(hdr.size);
    if (off + *len > PAGE_SIZE)
        return WALK_BROKEN;
    return WALK_RECORD;
}

// A record this firmware can use: CRC intact and a layout that is a prefix
// of settings_data_t, or the current one extended by a newer firmware (the
// layout only grows, so its first bytes are still ours)
static bool record_usable(const uint8_t *rec, uint16_t *version)
{
    record_header_t hdr;
    memcpy(&hdr, rec, sizeof(hdr));

    uint32_t crc;
    memcpy(&crc, rec + HEADER_SIZE + ((hdr.size + 3u) & ~3u), sizeof(crc));
    if (crc != settings_crc32(rec + HEADER_SIZE, hdr.size))
        return false;

    bool ok;
    if (hdr.version == 1)
        ok = (hdr.size == SETTINGS_V1_SIZE);
    else if (hdr.version == SETTINGS_VERSION)
        ok = (hdr.size == sizeof(settings_data_t));
    else
        ok = (hdr.version > SETTINGS_VERSION && hdr.size >= sizeof(settings_data_t));

    *version = hdr.version;
    return ok;
}

void settings_load(void)
{
    settings_defaults();
    m_loaded_version = 0;

    const uint8_t *page = page_base();
    const uint8_t *newest = 0;
    uint16_t newest_version = 0;
    uint32_t off = 0;
    uint32_t len;

    // The most recent usable record wins, whatever its version
    while (walk_at(page, off, &len) == WALK_RECORD)
    {
        uint16_t version;
        if (record_usable(page + off, &version))
        {
            newest = page + off;
            newest_version = version;
        }
        off += len;
    }

    m_dirty = false;
    if (!newest)
        return;

    record_header_t hdr;
    memcpy(&hdr, newest, sizeof(hdr));

    // Migration: an older layout is a prefix of the current one, the fields
    // it does not have keep their defaults
    uint32_t n = hdr.size < sizeof(g_settings) ? hdr.size : sizeof(g_settings);
    memcpy(&g_settings, newest + HEADER_SIZE, n);
    sanitize();

    m_loaded_version = newest_version;
    if (newest_version != SETTINGS_VERSION)
        m_dirty = true; // rewritten in the current format by the next save
}

bool settings_save(void)
{
    settings_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.hdr.magic = SETTINGS_MAGIC;
    rec.hdr.version = SETTINGS_VERSION;
    rec.hdr.size = sizeof(settings_data_t);
    memcpy(&rec.data, &g_settings, sizeof(rec.data));
    rec.crc = settings_crc32(&rec.data, sizeof(rec.data));

    // Append after the last record. A full page, or one whose tail cannot be
    // walked (a torn write), is erased first: the record written next holds
    // everything, so nothing is lost but the history.
    const uint8_t *page = page_base();
    uint32_t off = 0;
    uint32_t len;
    walk_t w;
    while ((w = walk_at(page, off, &len)) == WALK_RECORD)
        off += len;

    if (w != WALK_ERASED || off + sizeof(rec) > PAGE_SIZE)
    {
        if (!page_erase())
            return false;
        off = 0;
    }

    if (!page_write(off, &rec, sizeof(rec)))
        return false;

    m_dirty = false;
    return true;
}
