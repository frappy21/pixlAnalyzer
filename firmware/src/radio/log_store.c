#include <string.h>

#include "flash_ext.h"
#include "log_store.h"
#include "systime.h"

// One record on flash: [type][level][freq lo][freq hi][time 4 bytes]
#define LOG_RECORD_SIZE 8

// The whole upper half is log space
#define LOG_BASE FLASH_EXT_SAFE_BASE
#define LOG_LIMIT (1024u * 1024u) // 1MB from the safe base
#define LOG_MAX_RECORDS (LOG_LIMIT / LOG_RECORD_SIZE)

static uint32_t m_count;
static bool m_ok; // chip present and writable

void log_store_init(void)
{
    m_count = 0;
    m_ok = flash_ext_present();

    if (!m_ok)
        return;

    // Walk the records to find the tail: an erased type byte (0xFF) ends
    // the store, an invalid one stops the walk (torn write)
    uint8_t buf[LOG_RECORD_SIZE];
    uint32_t addr = LOG_BASE;
    while (m_count < LOG_MAX_RECORDS)
    {
        if (!flash_ext_read(addr, buf, sizeof(buf)))
        {
            m_ok = false;
            return;
        }
        if (buf[0] == 0xFF)
            break; // erased tail
        if (buf[0] == 0 || buf[0] >= LOG_TYPE_COUNT)
            break; // torn or foreign
        m_count++;
        addr += LOG_RECORD_SIZE;
    }
}

bool log_store_enabled(void)
{
    return m_ok;
}

void log_store_event(uint8_t type, uint16_t freq, uint8_t level)
{
    if (!m_ok || type == 0 || type >= LOG_TYPE_COUNT || m_count >= LOG_MAX_RECORDS)
        return;

    uint8_t buf[LOG_RECORD_SIZE];
    uint32_t t = systime_ms() / 1000u;

    buf[0] = type;
    buf[1] = level;
    buf[2] = (uint8_t)(freq & 0xFF);
    buf[3] = (uint8_t)(freq >> 8);
    buf[4] = (uint8_t)(t & 0xFF);
    buf[5] = (uint8_t)((t >> 8) & 0xFF);
    buf[6] = (uint8_t)((t >> 16) & 0xFF);
    buf[7] = (uint8_t)((t >> 24) & 0xFF);

    if (flash_ext_write(LOG_BASE + m_count * LOG_RECORD_SIZE, buf, sizeof(buf)))
        m_count++;
    else
        m_ok = false; // writes failing: stop trying
}

uint32_t log_store_count(void)
{
    return m_count;
}

bool log_store_get(uint32_t index, log_record_t *out)
{
    if (!m_ok || !out || index >= m_count)
        return false;

    uint8_t buf[LOG_RECORD_SIZE];
    if (!flash_ext_read(LOG_BASE + index * LOG_RECORD_SIZE, buf, sizeof(buf)))
        return false;
    if (buf[0] == 0 || buf[0] >= LOG_TYPE_COUNT)
        return false;

    out->type = buf[0];
    out->level = buf[1];
    out->freq = (uint16_t)(buf[2] | ((uint16_t)buf[3] << 8));
    out->time_s = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) | ((uint32_t)buf[6] << 16) |
                  ((uint32_t)buf[7] << 24);
    return true;
}

bool log_store_clear(void)
{
    if (!m_ok)
        return false;

    // Erase the first sectors until the tail is reached; a full store
    // takes a while, so erase only what is used plus one
    uint32_t sectors = (m_count * LOG_RECORD_SIZE + FLASH_EXT_SECTOR_SIZE - 1) /
                       FLASH_EXT_SECTOR_SIZE;
    if (sectors == 0)
        sectors = 1;

    for (uint32_t i = 0; i < sectors; i++)
    {
        if (!flash_ext_erase_sector(LOG_BASE + i * FLASH_EXT_SECTOR_SIZE))
            return false;
    }
    m_count = 0;
    return true;
}

const char *log_type_name(uint8_t type)
{
    switch (type)
    {
    case LOG_TYPE_RADAR:
        return "RADAR";
    case LOG_TYPE_HUNT:
        return "HUNT";
    case LOG_TYPE_SENTRY:
        return "SENTRY";
    case LOG_TYPE_NFC:
        return "NFC";
    default:
        return "?";
    }
}
