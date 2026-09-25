/**
 * ESB transmitter screen and its payload editor.
 *
 * Configuration rows: mode (replay the captured packet / inject a crafted
 * one), PHY, channel, power, packet count, and for injection the payload
 * length and the payload itself (a hex editor: LEFT/RIGHT move the cursor,
 * MID increments the nibble under it).
 *
 * Starting is deliberate, like the TX test screen: a one second hold of MID
 * from the row view. While it runs, any button stops it, it also stops
 * itself after ESB_TX_MAX_MS, and leaving the screen by any route goes
 * through tx_leave() first.
 *
 * Lab use on self owned links only: replaying someone else's packets or
 * injecting into someone else's receiver is not this tool's purpose.
 */
#include <string.h>

#include "app.h"
#include "buttons.h"
#include "esb_sniff.h"
#include "esb_tx.h"
#include "gfx.h"
#include "screens.h"
#include "tx_test.h"
#include "ui.h"
#include "ui_esb_tx.h"
#include "unifying.h"

enum
{
    ROW_BACK = 0,
    ROW_MODE, // replay the capture / inject a crafted packet
    ROW_RATE,
    ROW_CH,
    ROW_PWR,
    ROW_COUNT,
    ROW_SRC,  // inject only: the payload source
    ROW_LEN,  // inject only
    ROW_PAYLOAD,
    ROW_COUNT_ALL
};

// Packet count choices, index 0 is "until stopped"
static const uint16_t count_values[] = {0, 1, 8, 32, 128};
#define COUNT_CHOICES 5

// Injection payload source: a raw pattern
static const char *const src_names[] = {
    "PATTERN",
};
#define SRC_CHOICES 1

#define REPLAY_GAP_US 2000

static bool m_active;
static bool m_edit;

static uint8_t m_row_sel;

// The configuration
static uint8_t m_mode;     // 0 replay, 1 inject
static uint8_t m_rate = 2; // 1M/2M
static uint16_t m_mhz = 2440;
static uint8_t m_power = TX_POWER_MIN;
static uint8_t m_count_sel = 2; // 8 packets
static uint8_t m_src;           // injection payload source
static uint8_t m_plen = 8;
static uint8_t m_payload[ESB_MAX_PAYLOAD];

// Loads the selected payload source into m_payload (pattern only)
static void src_load(void)
{
    memset(m_payload, 0, sizeof(m_payload));
    for (uint8_t i = 0; i < ESB_MAX_PAYLOAD; i++)
        m_payload[i] = i;
    m_plen = 8;
}

// The editor cursor: nibble index into m_payload, 0..2*plen-1
static uint8_t m_cursor;

extern const app_screen_t scr_esb_payload;

// Formats the value column next to each row
static void values_format(char storage[ROW_COUNT_ALL][10])
{
    storage[ROW_RATE][0] = (char)('0' + m_rate);
    storage[ROW_RATE][1] = 'M';
    storage[ROW_RATE][2] = '\0';
    gfx_fmt_int(storage[ROW_CH], m_mhz);
    if (m_count_sel)
        gfx_fmt_int(storage[ROW_COUNT], (int)count_values[m_count_sel]);
    else
        memcpy(storage[ROW_COUNT], "LOOP", 5);
    gfx_fmt_int(storage[ROW_LEN], m_plen);
}

// Draws the configuration list, hiding the injection only rows in replay
// mode
static void rows_draw(void)
{
    static const char *const items[ROW_COUNT_ALL] = {
        "Back", "Mode", "Rate", "Channel", "Power", "Count", "Source", "Length", "Payload",
    };
    static char storage[ROW_COUNT_ALL][10];
    const char *vals[ROW_COUNT_ALL];

    values_format(storage);
    vals[ROW_MODE] = m_mode ? "INJECT" : "REPLAY";
    vals[ROW_RATE] = storage[ROW_RATE];
    vals[ROW_CH] = storage[ROW_CH];
    vals[ROW_PWR] = tx_power_name(m_power);
    vals[ROW_COUNT] = storage[ROW_COUNT];
    vals[ROW_SRC] = src_names[m_src];
    vals[ROW_LEN] = storage[ROW_LEN];
    vals[ROW_PAYLOAD] = m_mode ? "EDIT >" : "-";
    vals[ROW_BACK] = "";

    // The Source, Length and Payload rows only exist for injection
    uint8_t count = m_mode ? ROW_COUNT_ALL : ROW_COUNT;

    ui_list(m_edit ? "TX: EDIT" : "ESB TX", items, count, m_row_sel, vals);

    // The list fills the screen, so the hold hint goes into the title row
    gfx_text_micro(52, 2, "HOLD MID:START");
}

static void tx_enter(void)
{
    m_active = false;
    m_edit = false;
    m_row_sel = 0;

    // Prefill from a capture when there is one: replaying it is the common
    // case after a sniffing session
    esb_pkt_t cap;
    if (esb_sniff_capture_get(&cap))
    {
        m_mode = 0;
        m_rate = cap.rate ? cap.rate : 2;
        m_mhz = cap.mhz;
    }
    else
    {
        m_mode = 1;
        src_load();
    }
}

static void tx_leave(void)
{
    if (esb_tx_active())
    {
        esb_tx_stop();
        ui_message("TX STOPPED", 0, 600);
    }
    m_active = false;
}

// Builds the frame for injection and starts the transmitter
static bool tx_arm(void)
{
    esb_tx_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    if (!m_mode)
    {
        esb_pkt_t cap;
        if (!esb_sniff_capture_get(&cap))
        {
            ui_message("NO CAPTURE", "SNIFF A PACKET FIRST", 1200);
            return false;
        }
        memcpy(cfg.addr, cap.f.addr, 5);
        cfg.addr_len = cap.f.addr_len;
        cfg.raw_len = cap.raw_len;
        memcpy(cfg.raw, cap.raw, cap.raw_len);
        cfg.rate = m_rate;
    }
    else
    {
        esb_frame_t f;
        memset(&f, 0, sizeof(f));

        // A captured address when there is one, else the nRF24 default
        esb_pkt_t cap;
        if (esb_sniff_capture_get(&cap))
        {
            memcpy(f.addr, cap.f.addr, 5);
            f.addr_len = cap.f.addr_len;
            f.payload_lsb = cap.f.payload_lsb;
            f.crc_model = cap.f.crc_model;
        }
        else
        {
            f.addr[0] = 0xE7;
            f.addr[1] = 0xE7;
            f.addr[2] = 0xE7;
            f.addr[3] = 0xE7;
            f.addr[4] = 0xE7;
            f.addr_len = 5;
        }
        f.plen = m_plen;
        f.pid = 0;
        f.noack = false;
        memcpy(f.payload, m_payload, m_plen);

        cfg.raw_len = esb_frame_build(cfg.raw, ESB_CAPTURE_MAX, &f);
        if (cfg.raw_len == 0)
        {
            ui_message("TX FAILED", 0, 800);
            return false;
        }
        memcpy(cfg.addr, f.addr, 5);
        cfg.addr_len = f.addr_len;
        cfg.rate = m_rate;
    }

    cfg.mhz = m_mhz;
    cfg.power = m_power;
    cfg.count = count_values[m_count_sel];
    cfg.gap_us = REPLAY_GAP_US;

    if (!esb_tx_start(&cfg))
    {
        ui_message("TX FAILED", 0, 800);
        return false;
    }
    return true;
}

static void config_tick(void)
{
    uint8_t rows = m_mode ? ROW_COUNT_ALL : ROW_COUNT;

    if (m_edit)
    {
        if (app_left() || app_right())
        {
            int dir = app_left() ? -1 : 1;
            switch (m_row_sel)
            {
            case ROW_MODE:
                m_mode = (uint8_t)((m_mode + 2 + dir) % 2);
                m_row_sel = 0;
                m_edit = false; // the row set changed, leave edit
                break;
            case ROW_RATE:
                m_rate = (uint8_t)(m_rate == 1 ? 2 : 1);
                break;
            case ROW_CH:
            {
                int v = m_mhz + dir;
                m_mhz = (uint16_t)(v < 2400 ? 2400 : (v > 2500 ? 2500 : v));
                break;
            }
            case ROW_PWR:
                m_power = (uint8_t)((m_power + TX_POWER_COUNT + dir) % TX_POWER_COUNT);
                break;
            case ROW_COUNT:
                m_count_sel = (uint8_t)((m_count_sel + COUNT_CHOICES + dir) % COUNT_CHOICES);
                break;
            case ROW_SRC:
                m_src = (uint8_t)((m_src + SRC_CHOICES + dir) % SRC_CHOICES);
                src_load();
                m_cursor = 0;
                break;
            case ROW_LEN:
            {
                int v = m_plen + dir;
                m_plen = (uint8_t)(v < 1 ? 1 : (v > ESB_MAX_PAYLOAD ? ESB_MAX_PAYLOAD : v));
                m_cursor = 0;
                break;
            }
            default:
                break;
            }
            app_redraw();
        }
        if (app_ok())
        {
            m_edit = false;
            app_redraw();
        }
        return;
    }

    if (app_left())
    {
        m_row_sel = (uint8_t)((m_row_sel + rows - 1) % rows);
        app_redraw();
    }
    if (app_right())
    {
        m_row_sel = (uint8_t)((m_row_sel + 1) % rows);
        app_redraw();
    }

    if (app_ok())
    {
        if (m_row_sel == ROW_BACK)
        {
            app_back();
            return;
        }
        if (m_row_sel == ROW_PAYLOAD && m_mode)
        {
            app_open(&scr_esb_payload);
            return;
        }
        m_edit = true;
        app_redraw();
        return;
    }

    // Deliberate hold, so a stray press can never put a packet on the air
    if (buttons_down(BTN_MID) && buttons_held_ms(BTN_MID) > 1000)
    {
        app_note_input();
        if (tx_arm())
        {
            m_active = true;
            buttons_flush();
            app_redraw();
        }
        return;
    }

    if (app_take_redraw())
        rows_draw();
}

static void active_tick(uint32_t now)
{
    esb_tx_pump();
    esb_tx_update(now);

    if (!esb_tx_active())
    {
        ui_message("TX DONE", 0, 600);
        m_active = false;
        app_redraw();
        return;
    }

    // Stops on the press edge of any button: nothing may delay stopping
    if (app_any())
    {
        app_back();
        return;
    }

    (void)app_take_redraw();
    ui_esb_tx_active(m_mode, m_mhz, m_rate, esb_tx_sent(), esb_tx_remaining_ms(now));
}

static void tx_tick(uint32_t now)
{
    if (m_active)
        active_tick(now);
    else
        config_tick();
}

const app_screen_t scr_esb_tx = {
    .name = "ESB TX",
    .group = APP_GROUP_TRANSMIT,
    .enter = tx_enter,
    .tick = tx_tick,
    .leave = tx_leave,
};

// ---------------------------------------------------------------------------
// Payload editor (injection only)
// ---------------------------------------------------------------------------

static void payload_enter(void)
{
    if (!m_cursor && !m_payload[0])
    {
        // First visit: an incrementing pattern, so a change is visible
        for (uint8_t i = 0; i < ESB_MAX_PAYLOAD; i++)
            m_payload[i] = i;
    }
    m_cursor = 0;
}

static void payload_tick(uint32_t now)
{
    (void)now;

    if (app_left())
    {
        m_cursor = m_cursor ? (uint8_t)(m_cursor - 1) : (uint8_t)(2 * m_plen - 1);
        app_redraw();
    }
    if (app_right())
    {
        m_cursor = (uint8_t)((m_cursor + 1) % (2 * m_plen));
        app_redraw();
    }
    if (app_ok())
    {
        uint8_t byte = m_cursor >> 1;
        uint8_t nibble = m_cursor & 1 ? m_payload[byte] & 0x0F : m_payload[byte] >> 4;
        nibble = (uint8_t)((nibble + 1) & 0x0F);
        if (m_cursor & 1)
            m_payload[byte] = (uint8_t)((m_payload[byte] & 0xF0) | nibble);
        else
            m_payload[byte] = (uint8_t)((m_payload[byte] & 0x0F) | (nibble << 4));
        app_redraw();
    }

    if (app_take_redraw())
    {
        static const char hex_digits[] = "0123456789ABCDEF";
        char buf[28];

        display_clear();
        ui_title("PAYLOAD");

        gfx_text_micro(2, 12, "MID RAISES THE NIBBLE");
        gfx_text_micro(2, 19, "L/R MOVE, LONG L: BACK");

        for (uint8_t row = 0; row < 4 && row * 8 < m_plen; row++)
        {
            uint8_t n = (uint8_t)(m_plen - row * 8);
            if (n > 8)
                n = 8;
            uint8_t k = 0;
            for (uint8_t i = 0; i < n; i++)
            {
                buf[k++] = hex_digits[m_payload[row * 8 + i] >> 4];
                buf[k++] = hex_digits[m_payload[row * 8 + i] & 15];
                buf[k++] = ' ';
            }
            buf[k ? k - 1 : 0] = '\0';
            int y = 30 + row * 8;
            gfx_text_micro(2, y, buf);

            // Cursor: an inverted box over the nibble under it
            if (m_cursor / 2 >= row * 8 && m_cursor / 2 < row * 8 + n)
            {
                int x = 2 + (m_cursor - row * 16) * 4;
                gfx_invert(x, y - 1, 3, 7);
            }
        }

        display_flush();
    }
}

const app_screen_t scr_esb_payload = {
    .name = "Payload",
    .group = APP_GROUP_HIDDEN,
    .enter = payload_enter,
    .tick = payload_tick,
};
