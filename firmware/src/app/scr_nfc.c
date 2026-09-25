/**
 * NFC screen: the board's unused NFC antenna as a field detector and a
 * readable NDEF tag.
 *
 * Mode row: OFF / MONITOR / TAG. In MONITOR the tag content is empty and
 * only reader fields are counted - the hidden-reader hunter (each field
    that appears gets counted, the LED blinks). In TAG mode the message is
 * the URI or text from the settings, the "digital business card": tap a
 * phone and it opens. The UID row picks stable or random per start.
 *
 * Short MID on the mode row restarts the emulation with the new settings,
 * so changes take effect immediately. Long MID resets the counters.
 */
#include <string.h>

#include "app.h"
#include "buttons.h"
#include "gfx.h"
#include "led.h"
#include "nfc_ndef.h"
#include "nfc_tag.h"
#include "screens.h"
#include "settings.h"
#include "systime.h"
#include "ui.h"

// Local helpers: strncpy with termination, a hex digit, the character set
// of the text editor
static void strncpy0(char *dst, const char *src, uint8_t max)
{
    uint8_t i = 0;
    while (i + 1 < max && src[i])
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static char hex_digit(uint8_t v)
{
    return v < 10 ? (char)('0' + v) : (char)('A' + v - 10);
}

// The editable alphabet: upper case, digits, and the URL punctuation
static const char c_charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:/_ ";
#define CHARSET_LEN (sizeof(c_charset) - 1)

static char next_charset(char c)
{
    for (uint8_t i = 0; i < CHARSET_LEN; i++)
    {
        if (c_charset[i] == c)
            return c_charset[(i + 1) % CHARSET_LEN];
    }
    return c_charset[0];
}

enum
{
    ROW_MODE = 0,
    ROW_MSG,
    ROW_UID,
    ROW_TEXT,
    ROW_COUNT
};

// The NFC settings are persisted (settings v4), the screen is their editor
static uint8_t m_row;
static uint8_t m_mode;      // nfc_mode_t
static uint8_t m_msg_type;  // nfc_msg_type_t
static bool m_random_uid;
static bool m_text_edit;

// The text lives in the settings; a local edit buffer with a cursor
static char m_edit[NFC_TEXT_MAX];
static uint8_t m_cursor;

static void apply(void)
{
    // Start with what the rows say. The text is the settings string
    nfc_tag_start((nfc_mode_t)m_mode, (nfc_msg_type_t)m_msg_type, m_edit, m_random_uid);
}

static void nfc_enter(void)
{
    m_row = ROW_MODE;
    m_text_edit = false;
    m_cursor = 0;
    strncpy0(m_edit, g_settings.nfc_text, sizeof(m_edit));
    m_mode = g_settings.nfc_mode >= NFC_MODE_COUNT ? NFC_MODE_OFF : g_settings.nfc_mode;
    m_msg_type = g_settings.nfc_msg_type >= NFC_MSG_COUNT ? NFC_MSG_URL : g_settings.nfc_msg_type;
    m_random_uid = g_settings.nfc_uid_random != 0;

    // The mode the settings left it in is live again on entry
    if (m_mode != NFC_MODE_OFF)
        apply();
}

static void nfc_leave(void)
{
    nfc_tag_stop();
    led_off();

    // Persist the edited state
    g_settings.nfc_mode = m_mode;
    g_settings.nfc_msg_type = m_msg_type;
    g_settings.nfc_uid_random = m_random_uid ? 1 : 0;
    if (strcmp(g_settings.nfc_text, m_edit) != 0)
    {
        strncpy0(g_settings.nfc_text, m_edit, sizeof(g_settings.nfc_text));
        settings_mark_dirty();
    }
    settings_save();
}

static void status_rows(const nfc_status_t *st)
{
    char buf[10];

    // Field state and counters
    gfx_text_micro(2, 34, st->in_field ? "FIELD ON " : "FIELD OFF");

    gfx_fmt_int(buf, (int)st->field_ons);
    gfx_text_micro(64, 34, buf);
    gfx_text_micro(64 + gfx_text_micro_width(buf) + 1, 34, "FLDS");

    gfx_fmt_int(buf, (int)st->reads);
    gfx_text_micro(2, 42, buf);
    gfx_text_micro(2 + gfx_text_micro_width(buf) + 1, 42, "READS");

    gfx_fmt_int(buf, (int)st->last_field_len);
    gfx_text_micro(64, 42, buf);
    gfx_text_micro(64 + gfx_text_micro_width(buf) + 1, 42, "MS LONG");
}

static void nfc_draw(void)
{
    char buf[10];
    static const char *const rows[ROW_COUNT] = {"Mode", "Message", "UID", "Text"};
    const char *vals[ROW_COUNT];

    nfc_status_t st;
    nfc_tag_status(&st);

    display_clear();
    ui_title("NFC");
    ui_battery();

    vals[ROW_MODE] = m_mode == NFC_MODE_MONITOR ? "MONITOR" : (m_mode == NFC_MODE_TAG ? "TAG" : "OFF");
    vals[ROW_MSG] = m_msg_type == NFC_MSG_TEXT ? "TEXT" : "URL";
    vals[ROW_UID] = m_random_uid ? "RANDOM" : "STABLE";
    vals[ROW_TEXT] = m_text_edit ? "EDIT >" : "EDIT >";

    // The rows
    for (uint8_t i = 0; i < ROW_COUNT; i++)
    {
        int y = 10 + i * 8;
        gfx_text_micro(2, y, rows[i]);
        if (i == ROW_TEXT)
        {
            // The text itself, clipped
            gfx_text_micro(40, y, m_edit);
        }
        else
        {
            gfx_text_micro(DISP_W - 2 - gfx_text_micro_width(vals[i]), y, vals[i]);
        }
        if (i == m_row)
            gfx_invert(0, y - 1, DISP_W, 8);
    }

    // The UID under the rows
    gfx_hline(0, DISP_W - 1, 43);
    gfx_text_micro(2, 46, "ID");

    char hex[3];
    for (uint8_t i = 0; i < 7; i++)
    {
        uint8_t b = st.uid[i];
        hex[0] = hex_digit(b >> 4);
        hex[1] = hex_digit(b & 0x0F);
        hex[2] = 0;
        gfx_text_micro(14 + i * 11, 46, hex);
    }

    status_rows(&st);

    // Hint
    gfx_text_micro(2, 58, "MID: NEXT, HOLD: RECOUNT");
    display_flush();
}

static void nfc_tick(uint32_t now)
{
    (void)now;

    if (m_text_edit)
    {
        // The text editor: LEFT/RIGHT move, MID cycles the character, a
        // long MID ends the edit
        if (app_left())
        {
            m_cursor = m_cursor ? m_cursor - 1 : 0;
            app_redraw();
        }
        if (app_right())
        {
            if (m_cursor + 1 < sizeof(m_edit) - 1)
                m_cursor++;
            app_redraw();
        }
        if (app_ok())
        {
            m_edit[m_cursor] = next_charset(m_edit[m_cursor]);
            app_redraw();
        }
        if (app_ok_long())
        {
            m_text_edit = false;
            apply(); // the new text goes live immediately
            app_redraw();
        }
    }
    else
    {
        if (app_left() || app_right())
        {
            int dir = app_left() ? -1 : 1;
            switch (m_row)
            {
            case ROW_MODE:
                m_mode = (uint8_t)((m_mode + NFC_MODE_COUNT + dir) % NFC_MODE_COUNT);
                apply();
                break;
            case ROW_MSG:
                m_msg_type = (uint8_t)((m_msg_type + NFC_MSG_COUNT + dir) % NFC_MSG_COUNT);
                if (m_mode != NFC_MODE_OFF)
                    apply();
                break;
            case ROW_UID:
                m_random_uid = !m_random_uid;
                if (m_mode != NFC_MODE_OFF)
                    apply();
                break;
            default:
                break;
            }
            app_redraw();
        }

        if (app_ok())
        {
            if (m_row == ROW_TEXT)
            {
                m_text_edit = true;
                m_cursor = 0;
                app_redraw();
            }
            else
            {
                m_row = (uint8_t)((m_row + 1) % ROW_COUNT);
                app_redraw();
            }
        }

        if (app_ok_long())
        {
            nfc_tag_reset_counters();
            ui_message("COUNTERS RESET", 0, 600);
        }
    }

    // Blink while a field is on
    nfc_status_t st;
    nfc_tag_status(&st);
    led_set(st.in_field && (systime_ms() / 250) & 1);

    if (app_take_redraw())
        nfc_draw();
}

const app_screen_t scr_nfc = {
    .name = "NFC",
    .group = APP_GROUP_NFC,
    .enter = nfc_enter,
    .tick = nfc_tick,
    .leave = nfc_leave,
    .busy = true,
};
