#include <string.h>

#include "nrf.h"
#include "nrfx_nfct.h"
#include "nfc_t2t_lib.h"
#include "nfc_platform.h"

#include "nfc_ndef.h"
#include "nfc_tag.h"
#include "systime.h"

// The NDEF payload must live in RAM for the whole emulation
static uint8_t m_payload[NDEF_MAX];
static uint8_t m_payload_len;

static nfc_mode_t m_mode = NFC_MODE_OFF;
static nfc_status_t m_stat;
static uint32_t m_field_on_ms;

// ---------------------------------------------------------------------------
// SDK platform glue (the SDK's nfc_platform.c pulls in the clock driver,
// which this firmware replaces)
// ---------------------------------------------------------------------------

void app_error_fault_handler(uint32_t id, uint32_t pc, uint32_t info)
{
    // The T2T library links against this. A hard fault would restart the
    // watchdog anyway; nothing useful can be done here.
    (void)id;
    (void)pc;
    (void)info;
}

static void hfclk_on(void)
{
    // NFCT needs the high frequency clock while it is in the activated
    // state. The radio's own helper does exactly this.
    NRF_CLOCK->TASKS_HFCLKSTART = 1;
    while (NRF_CLOCK->EVENTS_HFCLKSTARTED == 0)
    {
    }
    NRF_CLOCK->EVENTS_HFCLKSTARTED = 0;
}

static void hfclk_off(void)
{
    // Safe even when the radio uses it: the radio only runs while the NFC
    // screen is closed, and NFC and the sweep are never live together
    NRF_CLOCK->TASKS_HFCLKSTOP = 1;
}

// The library calls these; normally nfc_platform.c implements them with the
// clock driver, which this firmware does not use
nrfx_err_t nfc_platform_setup(void)
{
    // Nothing to prepare: the clocks are handled per field event
    return NRFX_SUCCESS;
}

void nfc_platform_event_handler(nrfx_nfct_evt_t const *p_event)
{
    switch (p_event->evt_id)
    {
    case NRFX_NFCT_EVT_FIELD_DETECTED:
        hfclk_on();
        nrfx_nfct_state_force(NRFX_NFCT_STATE_ACTIVATED);
        break;

    case NRFX_NFCT_EVT_FIELD_LOST:
        // Back to sensing: the field is gone, the chip drops to its low
        // power sense state on its own after the release
        nrfx_nfct_state_force(NRFX_NFCT_STATE_SENSING);
        hfclk_off();
        break;

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// T2T events
// ---------------------------------------------------------------------------

static void t2t_callback(void *context, nfc_t2t_event_t event, const uint8_t *data, size_t len)
{
    (void)context;
    (void)data;
    (void)len;

    uint32_t now = systime_ms();

    switch (event)
    {
    case NFC_T2T_EVENT_FIELD_ON:
        m_stat.in_field = true;
        m_stat.field_ons++;
        m_stat.last_field_ms = now;
        m_field_on_ms = now;
        break;

    case NFC_T2T_EVENT_FIELD_OFF:
        m_stat.in_field = false;
        m_stat.last_field_len = (uint16_t)(now - m_field_on_ms);
        break;

    case NFC_T2T_EVENT_DATA_READ:
        m_stat.reads++;
        break;

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

static void uid_random(uint8_t *uid)
{
    // The device id is a per-chip random from the factory, a fine seed
    uint32_t seed = NRF_FICR->DEVICEID[0] ^ NRF_FICR->DEVICEID[1] ^ m_stat.field_ons;
    for (uint8_t i = 0; i < 7; i++)
    {
        // xorshift on the seed, one byte at a time
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        uid[i] = (uint8_t)(seed >> 24);
    }
    // NFCID1 byte 0 constraints: not 0x08 (long UID marker family) is fine,
    // any random value works for a tag that only needs to be read
}

bool nfc_tag_start(nfc_mode_t mode, nfc_msg_type_t msg_type, const char *text, bool random_uid)
{
    if (mode == NFC_MODE_OFF || mode >= NFC_MODE_COUNT)
    {
        nfc_tag_stop();
        return mode == NFC_MODE_OFF;
    }

    nfc_tag_stop();

    if (mode == NFC_MODE_TAG)
    {
        const char *s = text ? text : "";
        m_payload_len = (msg_type == NFC_MSG_TEXT)
                            ? ndef_text_message(m_payload, sizeof(m_payload), s)
                            : ndef_uri_message(m_payload, sizeof(m_payload), s);
        if (m_payload_len == 0)
        {
            // clipped: retry with the raw first bytes
            m_payload_len = (msg_type == NFC_MSG_TEXT)
                                ? ndef_text_message(m_payload, sizeof(m_payload), "")
                                : ndef_uri_message(m_payload, sizeof(m_payload), "pixl");
        }
    }

    if (random_uid)
        uid_random(m_stat.uid);
    else
    {
        // Stable UID from the device id: the same tag identity every time
        m_stat.uid[0] = (uint8_t)(NRF_FICR->DEVICEID[0] >> 24);
        m_stat.uid[1] = (uint8_t)(NRF_FICR->DEVICEID[0] >> 16);
        m_stat.uid[2] = (uint8_t)(NRF_FICR->DEVICEID[0] >> 8);
        m_stat.uid[3] = (uint8_t)(NRF_FICR->DEVICEID[0]);
        m_stat.uid[4] = (uint8_t)(NRF_FICR->DEVICEID[1] >> 24);
        m_stat.uid[5] = (uint8_t)(NRF_FICR->DEVICEID[1] >> 16);
        m_stat.uid[6] = (uint8_t)(NRF_FICR->DEVICEID[1] >> 8);
    }

    if (nfc_t2t_setup(t2t_callback, 0) != 0)
        return false;

    nfc_t2t_parameter_set(NFC_T2T_PARAM_NFCID1, m_stat.uid, 7);

    if (mode == NFC_MODE_TAG && m_payload_len)
        nfc_t2t_payload_set(m_payload, m_payload_len);
    else
        nfc_t2t_payload_set(0, 0); // monitor mode: an empty tag, reads count nothing

    if (nfc_t2t_emulation_start() != 0)
    {
        nfc_t2t_done();
        return false;
    }

    m_mode = mode;
    return true;
}

void nfc_tag_stop(void)
{
    if (m_mode == NFC_MODE_OFF)
        return;

    nfc_t2t_emulation_stop();
    nfc_t2t_done();
    hfclk_off();
    m_mode = NFC_MODE_OFF;
    m_stat.in_field = false;
}

void nfc_tag_status(nfc_status_t *out)
{
    if (out)
        *out = m_stat;
}

void nfc_tag_reset_counters(void)
{
    m_stat.field_ons = 0;
    m_stat.reads = 0;
    m_stat.last_field_len = 0;
    m_stat.last_field_ms = 0;
}

bool nfc_tag_active(void)
{
    return m_mode != NFC_MODE_OFF;
}
