#include <string.h>

#include "nfc_ndef.h"

// One short record: [flags+TNF][type length][payload length][type][payload]
// flags: MB=1 ME=1 CF=0 SR=1 IL=0, TNF=0x05 (well known) -> 0xD1
#define NDEF_HEADER_SR 0xD1

static uint8_t str_prefix(const char *s, const char *pfx)
{
    uint8_t n = 0;
    while (pfx[n] && s[n] == pfx[n])
        n++;
    return pfx[n] ? 0 : n;
}

uint8_t ndef_uri_message(uint8_t *out, uint8_t max, const char *uri)
{
    if (!out || !uri || max < 5)
        return 0;

    uint8_t code = NDEF_URI_NONE;
    uint8_t skip = 0;
    uint8_t n;

    if ((n = str_prefix(uri, "https://www.")) != 0)
    {
        code = NDEF_URI_HTTPS_WWW;
        skip = n;
    }
    else if ((n = str_prefix(uri, "http://www.")) != 0)
    {
        code = NDEF_URI_HTTP_WWW;
        skip = n;
    }
    else if ((n = str_prefix(uri, "https://")) != 0)
    {
        code = NDEF_URI_HTTPS;
        skip = n;
    }
    else if ((n = str_prefix(uri, "http://")) != 0)
    {
        code = NDEF_URI_HTTP;
        skip = n;
    }

    const char *rest = uri + skip;
    uint8_t rest_len = (uint8_t)strlen(rest);
    if (rest_len > NDEF_MAX)
        rest_len = NDEF_MAX;

    uint8_t payload_len = (uint8_t)(rest_len + 1);
    uint8_t total = (uint8_t)(4 + payload_len);
    if (total > max)
        return 0;

    out[0] = NDEF_HEADER_SR;
    out[1] = 1; // type length: 'U'
    out[2] = payload_len;
    out[3] = 'U';
    out[4] = code;
    memcpy(&out[5], rest, rest_len);
    return total;
}

uint8_t ndef_text_message(uint8_t *out, uint8_t max, const char *text)
{
    if (!out || !text || max < 7)
        return 0;

    uint8_t text_len = (uint8_t)strlen(text);
    if (text_len > NDEF_MAX)
        text_len = NDEF_MAX;

    // payload: [status: lang len 2, UTF-8][lang "en"][text]
    uint8_t payload_len = (uint8_t)(3 + text_len);
    uint8_t total = (uint8_t)(4 + payload_len);
    if (total > max)
        return 0;

    out[0] = NDEF_HEADER_SR;
    out[1] = 1; // type length: 'T'
    out[2] = payload_len;
    out[3] = 'T';
    out[4] = 0x02; // UTF-8, language length 2
    out[5] = 'e';
    out[6] = 'n';
    memcpy(&out[7], text, text_len);
    return total;
}
