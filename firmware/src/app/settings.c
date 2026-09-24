#include <string.h>

#include "app_config.h"
#include "settings.h"

#ifndef SETTINGS_HOST_TEST
#include "flash_int.h"
#endif

settings_data_t g_settings;
static bool m_dirty;

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    settings_data_t data;
    uint32_t crc;
} settings_record_t;

#define RECORD_STRIDE ((sizeof(settings_record_t) + 3) & ~3u)

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
}

static bool record_valid(const settings_record_t *rec)
{
    if (rec->magic != SETTINGS_MAGIC || rec->version != SETTINGS_VERSION)
        return false;
    if (rec->size != sizeof(settings_data_t))
        return false;

    return rec->crc == settings_crc32(&rec->data, sizeof(rec->data));
}

bool settings_dirty(void) { return m_dirty; }
void settings_mark_dirty(void) { m_dirty = true; }

#ifndef SETTINGS_HOST_TEST

void settings_load(void)
{
    settings_defaults();

    const uint8_t *page = (const uint8_t *)SETTINGS_PAGE_ADDR;
    const settings_record_t *newest = 0;

    for (uint32_t off = 0; off + RECORD_STRIDE <= FLASH_INT_PAGE_SIZE; off += RECORD_STRIDE)
    {
        const settings_record_t *rec = (const settings_record_t *)(page + off);
        if (rec->magic == 0xFFFFFFFF)
            break; // erased tail, nothing beyond this point
        if (record_valid(rec))
            newest = rec;
    }

    if (newest)
        memcpy(&g_settings, &newest->data, sizeof(g_settings));

    m_dirty = false;
}

bool settings_save(void)
{
    settings_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic = SETTINGS_MAGIC;
    rec.version = SETTINGS_VERSION;
    rec.size = sizeof(settings_data_t);
    memcpy(&rec.data, &g_settings, sizeof(rec.data));
    rec.crc = settings_crc32(&rec.data, sizeof(rec.data));

    const uint8_t *page = (const uint8_t *)SETTINGS_PAGE_ADDR;
    uint32_t off = 0;
    while (off + RECORD_STRIDE <= FLASH_INT_PAGE_SIZE)
    {
        const settings_record_t *slot = (const settings_record_t *)(page + off);
        if (slot->magic == 0xFFFFFFFF)
            break;
        off += RECORD_STRIDE;
    }

    if (off + RECORD_STRIDE > FLASH_INT_PAGE_SIZE)
    {
        if (!flash_int_erase_page(SETTINGS_PAGE_ADDR))
            return false;
        off = 0;
    }

    if (!flash_int_write(SETTINGS_PAGE_ADDR + off, &rec, sizeof(rec)))
        return false;

    m_dirty = false;
    return true;
}

#else // SETTINGS_HOST_TEST: in-memory backing for the host test harness

static uint8_t m_fake_page[FLASH_INT_PAGE_SIZE_HOST];

void settings_load(void)
{
    settings_defaults();
    const settings_record_t *rec = (const settings_record_t *)m_fake_page;
    if (record_valid(rec))
        memcpy(&g_settings, &rec->data, sizeof(g_settings));
    m_dirty = false;
}

bool settings_save(void)
{
    settings_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic = SETTINGS_MAGIC;
    rec.version = SETTINGS_VERSION;
    rec.size = sizeof(settings_data_t);
    memcpy(&rec.data, &g_settings, sizeof(rec.data));
    rec.crc = settings_crc32(&rec.data, sizeof(rec.data));
    memcpy(m_fake_page, &rec, sizeof(rec));
    m_dirty = false;
    return true;
}

#endif
