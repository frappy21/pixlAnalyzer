#include "display.h"
#include "gfx.h"
#include "ui.h"
#include "ui_zigbee.h"
#include "zb_rx.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const char k_hex[] = "0123456789ABCDEF";

static char *fmt_hex16(char *buf, uint16_t v)
{
    buf[0] = k_hex[(v >> 12) & 15];
    buf[1] = k_hex[(v >> 8) & 15];
    buf[2] = k_hex[(v >> 4) & 15];
    buf[3] = k_hex[v & 15];
    buf[4] = '\0';
    return buf;
}

// Extended address / PAN ID, stored little endian as on air, printed most
// significant byte first the way tools and labels show them
static char *fmt_hex64(char *buf, const uint8_t *le)
{
    char *p = buf;
    for (int i = 7; i >= 0; i--)
    {
        *p++ = k_hex[le[i] >> 4];
        *p++ = k_hex[le[i] & 15];
    }
    *p = '\0';
    return buf;
}

// "LABEL value" in the micro font, returns the x after it
static int micro_pair(int x, int y, const char *label, int value)
{
    char buf[12];
    gfx_text_micro(x, y, label);
    x += gfx_text_micro_width(label) + 2;
    gfx_fmt_int(buf, value);
    gfx_text_micro(x, y, buf);
    return x + gfx_text_micro_width(buf) + 4;
}

static uint16_t sat16(uint32_t v)
{
    return v > 0xFFFFu ? 0xFFFFu : (uint16_t)v;
}

// Bar height for a decayed frame count: logarithmic, so one frame shows and
// a busy channel does not flatten everything else
static int activity_height(const zb_chan_t *c, int max_h)
{
    uint8_t r = c->recent;
    int h = 0;
    while (r)
    {
        h += 2;
        r >>= 1;
    }
    if (h > max_h)
        h = max_h;
    if (h == 0 && (c->good || c->trunc))
        h = 1; // heard at some point
    return h;
}

static const char *proto_tag(const zb_pan_t *p)
{
    if (p->flags & ZB_PAN_THREAD)
        return "TH";
    if (!(p->flags & ZB_PAN_BEACON))
        return "";
    if (p->flags & ZB_PAN_ZIGBEE)
        return "ZB";
    return "BC";
}

// True when any known PAN on this channel has a Thread hint
static bool chan_is_thread(uint8_t ch)
{
    uint8_t idx[ZB_MAX_PANS];
    uint8_t n = zb_rx_sorted(idx, ZB_MAX_PANS);
    for (uint8_t i = 0; i < n; i++)
    {
        const zb_pan_t *p = zb_rx_pan(idx[i]);
        if (p->ch == ch && (p->flags & ZB_PAN_THREAD))
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Overview
// ---------------------------------------------------------------------------

#define BAR_TOP 11
#define BAR_H 9
#define LABEL_Y 21
#define LIST_Y 29
#define LIST_ROWS 4

void ui_zigbee_main(uint8_t lock)
{
    char buf[12];
    const zb_totals_t *t = zb_rx_totals();

    display_clear();
    gfx_text(2, 1, "ZB");
    if (lock)
    {
        buf[0] = 'C';
        buf[1] = 'H';
        gfx_fmt_int(buf + 2, lock);
        gfx_text_micro(17, 2, buf);
    }
    else
    {
        gfx_text_micro(17, 2, "HOP");
    }
    if (t)
    {
        int x = micro_pair(38, 2, "OK", (int)sat16(t->good + t->trunc));
        micro_pair(x, 2, "BAD", (int)sat16(t->bad));
    }
    ui_battery();
    gfx_hline(0, DISP_W - 1, 9);

    // One 8 pixel column per channel: activity bar over its number
    for (uint8_t ch = ZB_CH_FIRST; ch <= ZB_CH_LAST; ch++)
    {
        int x = (ch - ZB_CH_FIRST) * 8;
        const zb_chan_t *c = zb_rx_chan(ch);
        int h = c ? activity_height(c, BAR_H) : 0;
        if (h)
            gfx_box(x + 1, BAR_TOP + BAR_H - h, 6, h, true, true);

        gfx_fmt_int(buf, ch);
        gfx_text_micro(x + 1, LABEL_Y, buf);
        if (ch == lock)
            gfx_invert(x, LABEL_Y - 1, 8, 7);
        // Small 2-pixel dot in the gap above the bar for Thread channels
        if (chan_is_thread(ch))
            gfx_box(x + 3, BAR_TOP - 1, 2, 1, true, false);
    }
    gfx_hline(0, DISP_W - 1, LIST_Y - 2);

    uint8_t idx[ZB_MAX_PANS];
    uint8_t n = zb_rx_sorted(idx, ZB_MAX_PANS);
    if (n == 0)
    {
        gfx_text_micro(2, LIST_Y + 6, "NO PAN SEEN YET");
        gfx_text_micro(2, LIST_Y + 15, lock ? "L/R: CHANNEL OR HOP" : "L/R: LOCK A CHANNEL");
        display_flush();
        return;
    }

    for (uint8_t row = 0; row < LIST_ROWS && row < n; row++)
    {
        const zb_pan_t *p = zb_rx_pan(idx[row]);
        int y = LIST_Y + row * 9 + 1;

        gfx_text_micro(2, y, fmt_hex16(buf, p->pan));
        gfx_fmt_int(buf, p->ch);
        gfx_text_micro(22, y, buf);
        gfx_fmt_int(buf, p->frames);
        gfx_text_micro(34, y, buf);
        gfx_fmt_int(buf, -(int)p->rssi);
        gfx_text_micro(58, y, buf);

        gfx_text_micro(76, y, proto_tag(p));
        if (p->flags & ZB_PAN_PJ)
            gfx_text_micro(88, y, "PJ");
        if (p->flags & ZB_PAN_SECURED)
            gfx_text_micro(100, y, "SEC");
    }
    if (n > LIST_ROWS)
    {
        gfx_fmt_int(buf, n - LIST_ROWS);
        gfx_text_micro(DISP_W - 1 - gfx_text_micro_width("+") - gfx_text_micro_width(buf),
                       LIST_Y + (LIST_ROWS - 1) * 9 + 1, "+");
        gfx_text_micro(DISP_W - 1 - gfx_text_micro_width(buf), LIST_Y + (LIST_ROWS - 1) * 9 + 1,
                       buf);
    }

    display_flush();
}

// ---------------------------------------------------------------------------
// One PAN
// ---------------------------------------------------------------------------

#define ADDR_Y 40
#define ADDR_LINES 3
#define SHORTS_PER_LINE 6

static const char *frame_type_name(uint8_t type)
{
    switch (type)
    {
    case ZB_FT_BEACON:
        return "BEACON";
    case ZB_FT_DATA:
        return "DATA";
    case ZB_FT_ACK:
        return "ACK";
    case ZB_FT_CMD:
        return "CMD";
    default:
        return "?";
    }
}

uint8_t ui_zigbee_pan(const zb_pan_t *p, uint8_t pos, uint8_t n, uint8_t page)
{
    char buf[20];

    display_clear();
    gfx_text(2, 1, "PAN");
    gfx_text(22, 1, fmt_hex16(buf, p->pan));
    buf[0] = 'C';
    buf[1] = 'H';
    gfx_fmt_int(buf + 2, p->ch);
    gfx_text_micro(50, 2, buf);

    // Position in the list instead of the battery: L/R pages through PANs
    char *q = gfx_fmt_int(buf, pos + 1);
    while (*q)
        q++;
    *q++ = '/';
    gfx_fmt_int(q, n);
    gfx_text_micro(DISP_W - 1 - gfx_text_micro_width(buf), 2, buf);
    gfx_hline(0, DISP_W - 1, 9);

    int x = micro_pair(2, 11, "FRM", p->frames);
    x = micro_pair(x, 11, "BCN", p->beacons);
    micro_pair(x, 11, "SEC", p->secured);

    x = micro_pair(2, 18, "RSSI", -(int)p->rssi);
    gfx_text_micro(x, 18, "LAST");
    x += gfx_text_micro_width("LAST") + 2;
    gfx_text_micro(x, 18, frame_type_name(p->last_type));
    x += gfx_text_micro_width(frame_type_name(p->last_type)) + 2;
    gfx_text_micro(x, 18, "SEQ");
    x += gfx_text_micro_width("SEQ") + 2;
    gfx_text_micro(x, 18, gfx_fmt_int(buf, p->last_seq));

    // What the beacons said: the Zigbee payload, never anything encrypted
    if (p->flags & ZB_PAN_THREAD)
    {
        x = gfx_text_micro_width("THREAD") + 2;
        gfx_text_micro(2, 25, "THREAD");
        if (p->flags & ZB_PAN_BEACON)
            gfx_text_micro(2 + x, 25, (p->flags & ZB_PAN_PJ) ? "JOIN OPEN" : "JOIN CLOSED");
        else
            gfx_text_micro(2 + x, 25, "PAN:FACE");
    }
    else if (p->flags & ZB_PAN_ZIGBEE)
    {
        const char *stack = p->stack_profile == 2   ? "ZIGBEE PRO"
                            : p->stack_profile == 1 ? "ZIGBEE 2006"
                                                    : "ZIGBEE";
        gfx_text_micro(2, 25, stack);
        x = 2 + gfx_text_micro_width(stack) + 4;
        gfx_text_micro(x, 25, (p->flags & ZB_PAN_PJ) ? "JOIN OPEN" : "JOIN CLOSED");
    }
    else if (p->flags & ZB_PAN_BEACON)
    {
        x = micro_pair(2, 25, "BEACON PROTO", p->proto_id);
        if (p->flags & ZB_PAN_PJ)
            gfx_text_micro(x, 25, "ASSOC OK");
    }
    else
    {
        gfx_text_micro(2, 25, "NO BEACON HEARD");
    }
    if (p->flags & ZB_PAN_COORD)
        gfx_text_micro(DISP_W - 1 - gfx_text_micro_width("COORD"), 25, "COORD");

    if (p->flags & ZB_PAN_EXT_PAN)
    {
        gfx_text_micro(2, 32, "XPAN");
        gfx_text_micro(22, 32, fmt_hex64(buf, p->ext_pan));
    }

    // Address list: short addresses six to a line, then one extended per line
    uint8_t short_lines = (uint8_t)((p->n_short + SHORTS_PER_LINE - 1) / SHORTS_PER_LINE);
    uint8_t lines = (uint8_t)(short_lines + p->n_ext + ((p->flags & ZB_PAN_MORE) ? 1 : 0));
    uint8_t pages = lines ? (uint8_t)((lines + ADDR_LINES - 1) / ADDR_LINES) : 1;
    page %= pages;

    if (pages > 1)
    {
        q = gfx_fmt_int(buf, page + 1);
        while (*q)
            q++;
        *q++ = '/';
        gfx_fmt_int(q, pages);
        gfx_text_micro(DISP_W - 1 - gfx_text_micro_width(buf), 32, buf);
    }
    gfx_hline(0, DISP_W - 1, ADDR_Y - 2);

    if (!lines)
    {
        gfx_text_micro(2, ADDR_Y + 1, "NO ADDRESSES");
        display_flush();
        return pages;
    }

    uint8_t first = (uint8_t)(page * ADDR_LINES);
    for (uint8_t row = 0; row < ADDR_LINES && first + row < lines; row++)
    {
        uint8_t line = first + row;
        int y = ADDR_Y + row * 8 + 1;

        if (line < short_lines)
        {
            for (uint8_t i = 0; i < SHORTS_PER_LINE; i++)
            {
                uint8_t k = (uint8_t)(line * SHORTS_PER_LINE + i);
                if (k >= p->n_short)
                    break;
                gfx_text_micro(2 + i * 21, y, fmt_hex16(buf, p->shorts[k]));
            }
        }
        else if (line < short_lines + p->n_ext)
        {
            gfx_text_micro(2, y, fmt_hex64(buf, p->exts[line - short_lines]));
        }
        else
        {
            gfx_text_micro(2, y, "MORE NOT KEPT");
        }
    }

    display_flush();
    return pages;
}
