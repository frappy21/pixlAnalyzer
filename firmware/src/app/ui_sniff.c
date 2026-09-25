#include <string.h>

#include "display.h"
#include "gfx.h"
#include "ui.h"
#include "ui_sniff.h"
#include "unifying.h"

static const char hex_digits[] = "0123456789ABCDEF";

// "5511 2233" - the address tail, enough to tell devices apart on 128px
static void addr_str(char *out, const uint8_t *addr, uint8_t addr_len)
{
    // Show the last four bytes: the first byte is the one every match
    // forces (0x55/0xAA), it carries no information
    uint8_t first = addr_len > 4 ? addr_len - 4 : 1;
    uint8_t n = 0;
    for (uint8_t i = first; i < addr_len; i++)
    {
        out[n++] = hex_digits[addr[i] >> 4];
        out[n++] = hex_digits[addr[i] & 15];
        if (i + 1 < addr_len && ((i - first) & 1) == 1 && n < 12)
            out[n++] = ' ';
    }
    out[n] = '\0';
}

void ui_sniff_list(uint16_t lock_mhz, uint16_t hop_mhz, uint32_t locks, uint32_t decoded)
{
    char buf[16];

    display_clear();
    gfx_text(2, 1, "SNIFF");
    if (lock_mhz)
    {
        gfx_fmt_int(buf, lock_mhz);
        gfx_text_micro(34, 2, buf);
    }
    else
    {
        gfx_fmt_int(buf, hop_mhz);
        gfx_text_micro(30, 2, "HOP ");
        gfx_text_micro(30 + gfx_text_micro_width("HOP ") + 1, 2, buf);
    }
    gfx_fmt_int(buf, (int)decoded);
    gfx_text_micro(DISP_W - 30 - gfx_text_micro_width(buf), 2, buf);
    gfx_text_micro(DISP_W - 24, 2, "OK/");
    gfx_fmt_int(buf, (int)locks);
    gfx_text_micro(DISP_W - 1 - gfx_text_micro_width(buf), 2, buf);
    ui_battery();
    gfx_hline(0, DISP_W - 1, 9);

    uint8_t idx[ESB_SNIFF_MAX_DEV];
    uint8_t n = esb_sniff_dev_sorted(idx, ESB_SNIFF_MAX_DEV);

    if (n == 0)
    {
        gfx_text_micro(2, 24, "NO SHOCKBURST PACKETS YET");
        gfx_text_micro(2, 34, "LOOKING FOR 1M/2M ESB TRAFFIC");
        gfx_text_micro(2, 44, "MOVE A MOUSE OR TYPE");
        display_flush();
        return;
    }

    for (uint8_t row = 0; row < 5 && row < n; row++)
    {
        const esb_dev_t *d = esb_sniff_dev(idx[row]);
        int y = 11 + row * 9;

        gfx_fmt_int(buf, d->rate);
        gfx_text_micro(2, y + 1, buf);
        gfx_text_micro(2 + gfx_text_micro_width(buf) + 1, y + 1, "M");

        addr_str(buf, d->addr, d->addr_len);
        gfx_text_micro(16, y + 1, buf);

        gfx_fmt_int(buf, d->mhz);
        gfx_text_micro(64, y + 1, buf);

        gfx_fmt_int(buf, d->packets);
        gfx_text_micro(80, y + 1, buf);
        gfx_text_micro(80 + gfx_text_micro_width(buf) + 1, y + 1, "PKT");

        char *q = buf;
        *q++ = '-';
        gfx_fmt_int(q, d->rssi);
        gfx_text_micro(DISP_W - 1 - gfx_text_micro_width(buf), y + 1, buf);
    }

    gfx_hline(0, DISP_W - 1, 55);
    gfx_text_micro(2, 58, "MID: PACKETS  L/R: CHANNEL");
    display_flush();
}

// One payload row: "00 11 22 33 44 55 66 77"
static void hex_row(char *out, const uint8_t *b, uint8_t n)
{
    uint8_t k = 0;
    for (uint8_t i = 0; i < n; i++)
    {
        out[k++] = hex_digits[b[i] >> 4];
        out[k++] = hex_digits[b[i] & 15];
        out[k++] = ' ';
    }
    out[k ? k - 1 : 0] = '\0';
}

void ui_sniff_pkt(const esb_pkt_t *p, uint8_t pos, uint8_t count, bool captured)
{
    char buf[28];

    display_clear();
    gfx_text(2, 1, "PACKET");
    gfx_fmt_int(buf, pos);
    uint8_t n = (uint8_t)strlen(buf);
    buf[n++] = '/';
    gfx_fmt_int(&buf[n], count);
    gfx_text_micro(38, 2, buf);
    if (captured)
        gfx_text_micro(66, 2, "CAPTURED");
    ui_battery();
    gfx_hline(0, DISP_W - 1, 9);

    // Radio side
    gfx_fmt_int(buf, p->mhz);
    uint8_t k = (uint8_t)strlen(buf);
    buf[k++] = 'M';
    buf[k++] = 'H';
    buf[k++] = 'Z';
    buf[k++] = ' ';
    buf[k++] = (char)('0' + p->f.addr_len);
    buf[k++] = 'B';
    buf[k++] = ' ';
    gfx_fmt_int(&buf[k], -(int)p->rssi);
    gfx_text_micro(2, 12, buf);

    // PCF: length, PID, NO_ACK
    gfx_text_micro(2, 19, "LEN ");
    gfx_fmt_int(buf, p->f.plen);
    gfx_text_micro(14, 19, buf);
    gfx_text_micro(14 + gfx_text_micro_width(buf) + 2, 19, "PID ");
    gfx_fmt_int(buf, p->f.pid);
    gfx_text_micro(32, 19, buf);
    gfx_text_micro(32 + gfx_text_micro_width(buf) + 2, 19, p->f.noack ? "NOACK" : "ACK");

    // The address, byte per byte; a Unifying frame names itself and its
    // key on the same row, on the right of the address
    hex_row(buf, p->f.addr, p->f.addr_len);
    gfx_text_micro(40, 26, buf);

    unify_view_t uv;
    if (unify_decode(p->f.payload, p->f.plen, &uv))
    {
        char line[22];
        uint8_t k = 0;
        const char *kn = unify_kind_name(uv.kind);
        while (*kn && k + 1 < sizeof(line))
            line[k++] = *kn++;

        if (uv.kind == UNIFY_KEY)
        {
            for (uint8_t i = 0; i < 6 && k + 1 < sizeof(line); i++)
            {
                if (!uv.keys[i])
                    break;
                const char *name = unify_key_name(uv.keys[i]);
                if (name)
                    while (*name && k + 1 < sizeof(line))
                        line[k++] = *name++;
                else
                    line[k++] = '?';
            }
        }
        else if (uv.kind == UNIFY_MOUSE)
        {
            line[k++] = ' ';
            if (uv.dx >= 0)
                line[k++] = '+';
            gfx_fmt_int(&line[k], uv.dx);
            k = (uint8_t)strlen(line);
            if (uv.dy >= 0)
                line[k++] = '+';
            gfx_fmt_int(&line[k], uv.dy);
        }
        line[k] = 0;
        gfx_text_micro(2, 26, line);
    }

    // Payload hex, eight bytes per row
    if (p->f.plen == 0)
    {
        gfx_text_micro(2, 36, "EMPTY PAYLOAD");
    }
    else
    {
        for (uint8_t row = 0; row < 4 && row * 8 < p->f.plen; row++)
        {
            uint8_t n2 = (uint8_t)(p->f.plen - row * 8);
            if (n2 > 8)
                n2 = 8;
            hex_row(buf, &p->f.payload[row * 8], n2);
            gfx_text_micro(2, 34 + row * 7, buf);
        }
    }

    gfx_text_micro(2, 62, "MID: CAPTURE FOR TX  L/R: BROWSE");
    display_flush();
}
