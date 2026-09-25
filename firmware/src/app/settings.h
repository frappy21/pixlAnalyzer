/**
 * Persisted user settings.
 *
 * Records are appended to one internal flash page, so a save costs a single
 * word write until the page fills up and only then a page erase. The most
 * recent valid record wins, which also means a half written record simply
 * loses to the previous one instead of bricking the settings.
 *
 * Record: magic, version, size (bytes of data), data, CRC32 of the data. The
 * data is padded to whole words, so a record walks as 8 + size + 4 bytes and
 * records of different versions can share the page.
 *
 * Versioning: settings_data_t only ever grows at the end. Every older layout
 * is a prefix of the current one, so an older record is migrated by taking its
 * bytes and filling the new fields with their defaults.
 *   v1: contrast .. reserved (16 bytes)
 *   v2: + invert, burnin, saver_min, sentry_db, sentry_period (24 bytes)
 */
#ifndef PIXLA_SETTINGS_H
#define PIXLA_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#define SETTINGS_MAGIC 0x414C5850u // "PXLA"
#define SETTINGS_VERSION 2

// Data size of every version, for the migration
#define SETTINGS_V1_SIZE 16

typedef enum
{
    BAND_ISM = 0,   // 2400..2483, the classic view
    BAND_FULL,      // 2400..2500
    BAND_EXTENDED,  // 2360..2500, uses FREQUENCY.MAP=Low for the low part
    BAND_BLE_ADV,   // only 2402/2426/2480, wide bars
    BAND_COUNT
} band_preset_t;

typedef enum
{
    WF_THRESHOLD = 0, // 1 bit per cell, like the original firmware
    WF_DITHER,        // 4 levels through an ordered dither
    WF_MODE_COUNT
} waterfall_mode_t;

typedef struct
{
    uint8_t contrast;    // 0..63
    uint8_t backlight;   // 0..255, LCD only
    uint8_t band;        // band_preset_t
    uint8_t dwell;       // RSSI samples per channel visit
    uint8_t wf_decim;    // sweeps accumulated into one waterfall row
    uint8_t wf_mode;     // waterfall_mode_t
    uint8_t auto_floor;  // track the noise floor instead of a fixed level
    uint8_t led_hunt;    // LED clicks with signal strength in meter mode
    uint8_t sleep_min;   // inactivity sleep in minutes, 0 disables
    uint8_t dim_s;       // inactivity dim in seconds, 0 disables
    uint8_t log_consent; // user allowed writing to the external flash
    uint8_t shuffle;     // randomise channel visit order
    uint16_t bat_cal;    // battery divider trim, per mille
    uint16_t reserved;
    // v2
    uint8_t invert;        // display inverted (light background)
    uint8_t burnin;        // OLED: shift the frame by a pixel now and then
    uint8_t saver_min;     // OLED: blank the panel after this many idle minutes, 0 disables
    uint8_t sentry_db;     // sentry mode: dB above the noise floor that counts as activity
    uint8_t sentry_period; // sentry mode: one sweep every this many 100ms
    uint8_t reserved2[3];
} settings_data_t;

extern settings_data_t g_settings;

// Loads the newest valid record, or installs defaults if there is none. A
// record of an older version is migrated and leaves the settings dirty, so the
// next save rewrites it in the current format; nothing is written here.
void settings_load(void);

// Version of the record settings_load() used, 0 when none (defaults)
uint16_t settings_loaded_version(void);

// Appends the current settings. Returns false if the flash refused the write.
bool settings_save(void);

void settings_defaults(void);
bool settings_dirty(void);
void settings_mark_dirty(void);

// CRC used for the stored records, exposed for the host tests
uint32_t settings_crc32(const void *data, uint32_t len);

#ifdef SETTINGS_HOST_TEST
// The emulated flash page, so the host tests can plant old records
uint8_t *settings_host_page(void);
#endif

#endif // PIXLA_SETTINGS_H
