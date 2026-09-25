/**
 * Event log in the upper half of the external SPI NOR flash, written only
 * with the user's consent (Settings: NOR log). Records what the radar and
 * the sentry see, so a session of hunting can be reviewed afterwards
 * without a PC.
 *
 * The store is an append only sequence of 8 byte records starting at
 * FLASH_EXT_SAFE_BASE, walked at init to find the write position, the
 * same way the settings page is: an erased byte in the type field is the
 * end, anything invalid stops the walk (a torn write loses the tail, the
 * records before it are fine).
 *
 * Every call is main loop context (the SPI bus is shared with the display
 * and not safe from interrupts), and the flash is put back to deep power
 * down after each burst.
 */
#ifndef PIXLA_LOG_STORE_H
#define PIXLA_LOG_STORE_H

#include <stdbool.h>
#include <stdint.h>

// Record types
#define LOG_TYPE_RADAR 1   // a new radar signal: freq, level dB above floor
#define LOG_TYPE_HUNT 2    // something appeared after the baseline
#define LOG_TYPE_SENTRY 3  // the sentry alarm fired
#define LOG_TYPE_NFC 4     // an NFC field event
#define LOG_TYPE_COUNT 5

typedef struct
{
    uint8_t type;    // LOG_TYPE_*
    uint8_t level;   // dB above the floor, or 0
    uint16_t freq;   // MHz, or 0
    uint32_t time_s; // seconds since boot
} log_record_t;

// Reads the store position. Call once after flash_ext_init().
void log_store_init(void);

// True when logging is on (the consent setting said yes and the chip is
// there)
bool log_store_enabled(void);

// Appends one record. Safe to call from the screens; a full store, no
// consent or no chip is a silent no-op.
void log_store_event(uint8_t type, uint16_t freq, uint8_t level);

// How many records are stored
uint32_t log_store_count(void);

// Reads one record, oldest first (index 0). False when out of range.
bool log_store_get(uint32_t index, log_record_t *out);

// Erases everything (the LOG screen's hold-to-confirm action)
bool log_store_clear(void);

// Type name for the log screen
const char *log_type_name(uint8_t type);

#endif // PIXLA_LOG_STORE_H
