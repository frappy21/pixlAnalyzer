/**
 * Minimal NDEF message builders for the NFC tag emulator: a URI record
 * ("digital business card" - tap a phone and it opens the link) and a text
 * record. Pure byte assembly, host tested.
 *
 * Only what a Type 2 tag read needs: one short, well-known record. No
 * chunking, no ID fields, no multiple records.
 */
#ifndef PIXLA_NFC_NDEF_H
#define PIXLA_NFC_NDEF_H

#include <stdint.h>

// Longest NDEF message the tag carries (the URI/text plus the record
// header): the settings field behind it is the real limit
#define NDEF_MAX 64

// URI prefix codes of the NFC Forum URI record type, the useful ones
#define NDEF_URI_NONE 0x00    // the URI as-is
#define NDEF_URI_HTTP_WWW 0x01 // http://www.
#define NDEF_URI_HTTPS_WWW 0x02 // https://www.
#define NDEF_URI_HTTP 0x03   // http://
#define NDEF_URI_HTTPS 0x04  // https://:

// Builds one URI record message into out. Detects the best prefix code so
// the message stays short. Returns the message length, 0 when it does not
// fit. A URI without a scheme is sent as-is (prefix NONE) - phones then
// treat it as a search, which is still useful.
uint8_t ndef_uri_message(uint8_t *out, uint8_t max, const char *uri);

// Builds one text record message (UTF-8, "en") into out. Returns the
// length, 0 when it does not fit.
uint8_t ndef_text_message(uint8_t *out, uint8_t max, const char *text);

#endif // PIXLA_NFC_NDEF_H
