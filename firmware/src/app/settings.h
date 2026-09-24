/**
 * Persisted user settings.
 *
 * Records are appended to one internal flash page, so a save costs a single
 * word write until the page fills up and only then a page erase. The most
 * recent valid record wins, which also means a half written record simply
 * loses to the previous one instead of bricking the settings.
 */
#ifndef PIXLA_SETTINGS_H
#define PIXLA_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#define SETTINGS_MAGIC 0x414C5850u // "PXLA"
#define SETTINGS_VERSION 1

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
} settings_data_t;

extern settings_data_t g_settings;

// Loads the newest valid record, or installs defaults if there is none
void settings_load(void);

// Appends the current settings. Returns false if the flash refused the write.
bool settings_save(void);

void settings_defaults(void);
bool settings_dirty(void);
void settings_mark_dirty(void);

// CRC used for the stored records, exposed for the host tests
uint32_t settings_crc32(const void *data, uint32_t len);

#endif // PIXLA_SETTINGS_H
