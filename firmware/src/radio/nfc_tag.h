/**
 * NFC tag emulation on the board's unused NFC antenna, through the SDK's
 * NFC T2T library (a closed binary) and the nrfx NFCT driver.
 *
 * The glue this firmware provides itself:
 *  - nfc_platform_setup / nfc_platform_event_handler: normally the SDK's
 *    platform layer, which pulls in the clock driver. This firmware owns
 *    its clocks (radio_hfxo_start), so the platform functions live here:
 *    a field detected starts the HFXO the NFCT hardware needs, a field
 *    lost stops it again.
 *  - app_error_fault_handler: the library links against it; this firmware
 *    never uses app_error, so a quiet stub is provided.
 *
 * What the screen sees:
 *  - field events: count and last time a reader's field was on
 *  - NDEF reads: how often the tag content was actually read
 *  - the UID, which can be randomised per session (anti-tracking demo)
 *
 * The NFCT peripheral works without any SoftDevice; the radio is never
 * used while the tag is live (the NFC screen is the only user).
 */
#ifndef PIXLA_NFC_TAG_H
#define PIXLA_NFC_TAG_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    NFC_MODE_OFF = 0,
    NFC_MODE_MONITOR, // field detector: count readers, no tag content
    NFC_MODE_TAG,     // full tag: NDEF message a phone can read
    NFC_MODE_COUNT
} nfc_mode_t;

// Tag content type
typedef enum
{
    NFC_MSG_URL = 0,
    NFC_MSG_TEXT,
    NFC_MSG_COUNT
} nfc_msg_type_t;

typedef struct
{
    uint32_t field_ons;      // reader fields seen since the last reset
    uint32_t reads;          // NDEF reads (the reader actually took the message)
    uint32_t last_field_ms;  // when the last field went on, 0 = never
    uint16_t last_field_len; // how long the last field stayed on, ms
    bool in_field;           // a field is on right now
    uint8_t uid[7];          // the current NFCID1, 7 bytes (double size)
} nfc_status_t;

// Starts the emulation / monitoring. mode OFF stops everything. msg_type
// and text select the tag content for NFC_MODE_TAG (text is the URI or the
// text, up to NDEF_MAX-5 bytes, longer is clipped). random_uid picks a new
// NFCID1 each start. False when the driver refused to start.
bool nfc_tag_start(nfc_mode_t mode, nfc_msg_type_t msg_type, const char *text, bool random_uid);

// Stops the emulation and releases the hardware. Idempotent; the screen's
// leave() calls it on every exit path.
void nfc_tag_stop(void);

// Snapshot of the counters, for the screen
void nfc_tag_status(nfc_status_t *out);

// Reset the counters without stopping the tag
void nfc_tag_reset_counters(void);

// True while the emulation or monitoring is running
bool nfc_tag_active(void);

#endif // PIXLA_NFC_TAG_H
