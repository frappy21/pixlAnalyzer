#include <string.h>

#include "ble_scan.h"
#include "display.h"
#include "gfx.h"
#include "ui.h"
#include "ui_ble.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const char hex_digits[] = "0123456789ABCDEF";

// Address tail, enough to tell devices apart on a 128px screen. An extended
// advert without an address shows its advertising set instead.
static void addr_tail(char *out, const uint8_t *addr, uint8_t addr_type)
{
    if (addr_type == BLE_ADDR_EXT_ANON)
    {
        out[0] = 'S';
        out[1] = 'I';
        out[2] = 'D';
        out[3] = hex_digits[addr[0] & 15];
    }
    else
    {
        out[0] = hex_digits[addr[1] >> 4];
        out[1] = hex_digits[addr[1] & 15];
        out[2] = hex_digits[addr[0] >> 4];
        out[3] = hex_digits[addr[0] & 15];
    }
    out[4] = '\0';
}

// "42S", "12:05", "3H07"
static void ln_age(ble_line_t *l, uint32_t ms)
{
    uint32_t s = ms / 1000u;
    if (s < 60)
    {
        ble_ln_int(l, (int32_t)s);
        ble_ln_char(l, 'S');
        return;
    }
    uint32_t hi = s < 3600 ? s / 60 : s / 3600;
    uint32_t lo = s < 3600 ? s % 60 : (s / 60) % 60;
    ble_ln_int(l, (int32_t)hi);
    ble_ln_char(l, s < 3600 ? ':' : 'H');
    ble_ln_char(l, (char)('0' + lo / 10));
    ble_ln_char(l, (char)('0' + lo % 10));
}

static const char *phy_name(uint8_t phy)
{
    switch (phy)
    {
    case BLE_PHY_1M:
        return "1M";
    case BLE_PHY_2M:
        return "2M";
    case BLE_PHY_CODED:
        return "CODED";
    default:
        return "?";
    }
}

// Inverted full width banner on the top row, for alerts
static void banner(const char *text)
{
    gfx_text(2, 1, text);
    gfx_invert(0, 0, DISP_W, 9);
}

// ---------------------------------------------------------------------------
// Device list
// ---------------------------------------------------------------------------

static const char *const filter_names[BLE_FILTER_COUNT] = {
    [BLE_FILTER_ALL] = "",
    [BLE_FILTER_TRACKERS] = "TRACKERS",
    [BLE_FILTER_FOLLOW] = "FOLLOWING",
    [BLE_FILTER_EXT] = "EXTENDED",
};

void ui_ble_list(const ble_list_view_t *v)
{
    char buf[16];
    ble_line_t l;

    display_clear();

    // Header, or an alert in its place: a spam flood first (it is happening
    // right now), then devices that seem to follow
    if (v->spam && v->spam->active)
    {
        ble_ln_init(&l, "SPAM ");
        ble_ln_str(&l, ble_spam_name(v->spam->family));
        ble_ln_char(&l, ' ');
        ble_ln_int(&l, v->spam->rate);
        ble_ln_str(&l, "/2s");
        banner(l.s);
    }
    else if (v->following)
    {
        ble_ln_init(&l, "FOLLOWING: ");
        ble_ln_int(&l, v->following);
        banner(l.s);
    }
    else
    {
        gfx_text(2, 1, "BLE");
        gfx_fmt_int(buf, ble_devtab_count());
        gfx_text_micro(24, 2, buf);
        gfx_text_micro(24 + gfx_text_micro_width(buf) + 2, 2, "DEV");
        if (v->filter != BLE_FILTER_ALL)
        {
            gfx_text_micro(56, 2, filter_names[v->filter]);
        }
        else
        {
            gfx_fmt_int(buf, (int)v->packets);
            gfx_text_micro(56, 2, buf);
            gfx_text_micro(56 + gfx_text_micro_width(buf) + 2, 2, "PKT");
        }
        ui_battery();
    }
    gfx_hline(0, DISP_W - 1, 9);

    if (v->n == 0)
    {
        if (v->filter == BLE_FILTER_EXT)
        {
            // No extended advertiser in the table yet, but say what was heard
            ble_ln_init(&l, "EXT ");
            ble_ln_int(&l, (int32_t)v->ext->ext);
            ble_ln_str(&l, " AUX ");
            ble_ln_int(&l, (int32_t)v->ext->aux_ok);
            ble_ln_char(&l, '/');
            ble_ln_int(&l, (int32_t)v->ext->aux_tried);
            gfx_text_micro(2, 30, l.s);
        }
        else
        {
            gfx_text(14, 28, v->filter == BLE_FILTER_ALL ? "LISTENING..." : "NONE YET");
        }
        display_flush();
        return;
    }

    uint8_t first = 0;
    if (v->selected >= 6)
        first = v->selected - 5;
    if (v->n > 6 && first > v->n - 6)
        first = v->n - 6;

    for (uint8_t row = 0; row < 6 && first + row < v->n; row++)
    {
        const ble_dev_t *d = ble_devtab_device(v->idx[first + row]);
        int y = 11 + row * 9;

        addr_tail(buf, d->addr, d->addr_type);
        gfx_text_micro(2, y + 1, buf);

        // Trackers by kind, everything else by name when it has one
        const char *kind = ble_kind_name(d->kind);
        const char *text = (d->name[0] && !ble_kind_is_tracker(d->kind)) ? d->name : kind;
        ble_ln_init(&l, "");
        for (uint8_t i = 0; text[i] && i < 16; i++)
            ble_ln_char(&l, text[i]);
        gfx_text_micro(22, y + 1, l.s);

        // Markers: F following, X extended, S spam pattern
        int mx = 88;
        if (d->flags & BLE_DEV_FOLLOW)
        {
            gfx_text_micro(mx, y + 1, "F");
            mx += 4;
        }
        if (d->flags & BLE_DEV_EXT)
        {
            gfx_text_micro(mx, y + 1, "X");
            mx += 4;
        }
        if (d->flags & BLE_DEV_SPAMMY)
            gfx_text_micro(mx, y + 1, "S");

        gfx_fmt_int(buf, -d->rssi);
        gfx_text_micro(DISP_W - 22, y + 1, buf);

        if (first + row == v->selected)
            gfx_invert(0, y, DISP_W, 8);
    }

    display_flush();
}

// ---------------------------------------------------------------------------
// Device detail
// ---------------------------------------------------------------------------

static const char *pdu_name(uint8_t pdu_type)
{
    switch (pdu_type)
    {
    case 0x00:
        return "ADV IND";
    case 0x02:
        return "ADV NONCONN";
    case 0x04:
        return "SCAN RSP";
    case 0x06:
        return "ADV SCAN";
    case 0x07:
        return "ADV EXT";
    default:
        return "?";
    }
}

static const char *addr_type_name(const ble_dev_t *dev)
{
    if (dev->addr_type == BLE_ADDR_PUBLIC)
        return "PUBLIC";
    if (dev->addr_type == BLE_ADDR_EXT_ANON)
        return "NONE EXT SET";
    // The two top bits of a random address tell its kind
    switch (dev->addr[5] >> 6)
    {
    case 3:
        return "RANDOM STATIC";
    case 1:
        return "RANDOM PRIVATE";
    case 0:
        return "RANDOM NONRESOLV";
    default:
        return "RANDOM ?";
    }
}

uint8_t ui_ble_detail_lines(const ble_dev_t *dev, uint32_t now_ms, char (*lines)[BLE_LINE_LEN],
                            uint8_t max)
{
    ble_lines_t out = {.lines = lines, .max = max, .count = 0};
    ble_line_t l;

    if (dev->addr_type == BLE_ADDR_EXT_ANON)
    {
        ble_ln_init(&l, "EXT ADV SET ");
        ble_ln_int(&l, dev->addr[0]);
    }
    else
    {
        ble_ln_init(&l, "");
        for (int i = 5; i >= 0; i--)
        {
            ble_ln_hex(&l, dev->addr[i], 2);
            if (i)
                ble_ln_char(&l, ':');
        }
    }
    ble_lines_add(&out, &l);

    ble_ln_init(&l, "ADDR ");
    ble_ln_str(&l, addr_type_name(dev));
    ble_lines_add(&out, &l);

    if (dev->kind != BLE_KIND_PLAIN)
    {
        ble_ln_init(&l, "KIND ");
        ble_ln_str(&l, ble_kind_name(dev->kind));
        ble_lines_add(&out, &l);
    }

    ble_ln_init(&l, "RSSI ");
    ble_ln_int(&l, dev->rssi_last);
    ble_ln_str(&l, " MIN ");
    ble_ln_int(&l, dev->rssi_min);
    ble_ln_str(&l, " MAX ");
    ble_ln_int(&l, dev->rssi);
    ble_lines_add(&out, &l);

    ble_ln_init(&l, "PKT ");
    ble_ln_int(&l, dev->packets);
    uint16_t interval = ble_dev_interval_ms(dev);
    ble_ln_str(&l, " EVERY ");
    if (interval)
    {
        ble_ln_int(&l, interval);
        ble_ln_str(&l, "MS");
    }
    else
    {
        ble_ln_char(&l, '?');
    }
    ble_lines_add(&out, &l);

    ble_ln_init(&l, "FIRST ");
    ln_age(&l, now_ms - dev->first_ms);
    ble_ln_str(&l, " LAST ");
    ln_age(&l, now_ms - dev->last_ms);
    ble_lines_add(&out, &l);

    if (dev->flags & BLE_DEV_FOLLOW)
    {
        ble_ln_init(&l, "FOLLOWING: ");
        ble_ln_int(&l, dev->minutes);
        ble_ln_str(&l, " MINUTES");
        ble_lines_add(&out, &l);
    }
    if (dev->flags & BLE_DEV_SPAMMY)
    {
        ble_ln_init(&l, "SPAM PRONE PATTERN");
        ble_lines_add(&out, &l);
    }

    ble_ln_init(&l, "PDU ");
    ble_ln_str(&l, pdu_name(dev->pdu_type));
    if (dev->flags & BLE_DEV_CONN)
        ble_ln_str(&l, " CONN");
    if (dev->flags & BLE_DEV_RSP)
        ble_ln_str(&l, " +RSP");
    ble_lines_add(&out, &l);

    if (dev->flags & BLE_DEV_EXT)
    {
        ble_ln_init(&l, "AUX CH ");
        ble_ln_int(&l, dev->aux_chan);
        ble_ln_char(&l, ' ');
        ble_ln_str(&l, phy_name(dev->aux_phy));
        ble_ln_str(&l, (dev->flags & BLE_DEV_AUX) ? " RECEIVED" : " NOT RECEIVED");
        ble_lines_add(&out, &l);
    }

    if (dev->company && !ble_company_name(dev->company))
    {
        ble_ln_init(&l, "COMPANY ");
        ble_ln_hex(&l, dev->company, 4);
        ble_lines_add(&out, &l);
    }

    if (dev->adv_len)
    {
        ble_ln_init(&l, (dev->flags & BLE_DEV_AUX) ? "- AUX DATA -" : "- ADVERT -");
        ble_lines_add(&out, &l);
        ble_describe_ad(dev->adv, dev->adv_len, &out);
    }
    if (dev->rsp_len)
    {
        ble_ln_init(&l, "- SCAN RESPONSE -");
        ble_lines_add(&out, &l);
        ble_describe_ad(dev->rsp, dev->rsp_len, &out);
    }
    return out.count;
}

static int hunt_scale(int8_t rssi, int span); // defined in the Hunt section below

void ui_ble_detail(char (*lines)[BLE_LINE_LEN], uint8_t count, uint8_t first,
                   const int8_t *trend, uint8_t trend_len)
{
    char buf[12];

    display_clear();
    ui_title("DEVICE");

    // Position in the card: first shown line / total
    gfx_fmt_int(buf, first + 1);
    uint8_t n = (uint8_t)strlen(buf);
    buf[n++] = '/';
    gfx_fmt_int(&buf[n], count);
    gfx_text_micro(DISP_W - 1 - gfx_text_micro_width(buf), 2, buf);

    // With a sparkline, leave 8px at the bottom for it
    uint8_t rows_shown = (trend_len > 0) ? UI_BLE_DETAIL_ROWS - 1 : UI_BLE_DETAIL_ROWS;
    for (uint8_t row = 0; row < rows_shown && first + row < count; row++)
        gfx_text_micro(2, 12 + row * 7, lines[first + row]);

    // RSSI sparkline at the bottom 7 rows (y=57..63)
    if (trend_len > 0)
    {
        int sparky = DISP_H - 7;
        gfx_hline(0, DISP_W - 1, sparky - 1);
        for (uint8_t i = 0; i < trend_len && i < DISP_W - 4; i++)
        {
            int h = hunt_scale(trend[i], 6);
            if (h > 0)
                gfx_vline(2 + i, DISP_H - h, DISP_H - 1);
        }
    }

    display_flush();
}

// ---------------------------------------------------------------------------
// Hunt
// ---------------------------------------------------------------------------

// Bar and trend scale, dBm
#define HUNT_FLOOR -100
#define HUNT_CEIL -30

static int hunt_scale(int8_t rssi, int span)
{
    if (rssi == 0 || rssi <= HUNT_FLOOR)
        return 0;
    int v = ((rssi - HUNT_FLOOR) * span) / (HUNT_CEIL - HUNT_FLOOR);
    return v > span ? span : v;
}

void ui_ble_hunt(const ble_dev_t *dev, int8_t rssi, uint32_t age_ms, const int8_t *trend,
                 uint8_t trend_len)
{
    char buf[16];
    ble_line_t l;

    display_clear();

    // Header: what we are after
    const char *kind = ble_kind_name(dev->kind);
    gfx_text(2, 1, "HUNT");
    ble_ln_init(&l, "");
    addr_tail(buf, dev->addr, dev->addr_type);
    ble_ln_str(&l, buf);
    ble_ln_char(&l, ' ');
    ble_ln_str(&l, kind[0] ? kind : dev->name);
    gfx_text_micro(30, 2, l.s);
    gfx_hline(0, DISP_W - 1, 9);

    if (rssi == 0)
    {
        gfx_text(34, 14, "NOT HEARD");
    }
    else
    {
        gfx_fmt_int(buf, rssi);
        gfx_text(4, 14, buf);
        gfx_text(4 + gfx_text_width(buf) + 4, 14, "dBm");
    }

    ble_ln_init(&l, "LAST ");
    ln_age(&l, age_ms);
    gfx_text_micro(DISP_W - 2 - gfx_text_micro_width(l.s), 15, l.s);

    int w = hunt_scale(rssi, DISP_W - 10);
    gfx_box(4, 26, DISP_W - 8, 8, false, true);
    if (w > 0)
        gfx_box(5, 27, w, 6, true, true);

    // Trend: the last samples, so you can see if you are getting warmer
    for (uint8_t i = 0; i < trend_len && i < DISP_W - 4; i++)
    {
        int h = hunt_scale(trend[i], 22);
        if (h > 0)
            gfx_vline(2 + i, 62 - h, 62);
    }
    gfx_hline(0, DISP_W - 1, 63);

    display_flush();
}

// ---------------------------------------------------------------------------
// Sensor dashboard
// ---------------------------------------------------------------------------

#define SENSOR_ROWS 5

void ui_ble_sensors(const ble_sensor_entry_t *list, uint8_t n, uint8_t selected, uint32_t now_ms)
{
    char buf[12];
    ble_line_t l;

    display_clear();
    gfx_text(2, 1, "SENSORS");
    gfx_fmt_int(buf, n);
    gfx_text_micro(48, 2, buf);
    ui_battery();
    gfx_hline(0, DISP_W - 1, 9);

    if (n == 0)
    {
        gfx_text(14, 28, "LISTENING...");
        display_flush();
        return;
    }

    uint8_t first = 0;
    if (selected >= SENSOR_ROWS)
        first = selected - (SENSOR_ROWS - 1);

    for (uint8_t row = 0; row < SENSOR_ROWS && first + row < n; row++)
    {
        const ble_sensor_entry_t *e = &list[first + row];
        int y = 11 + row * 9;

        // Format, then the name or the address tail
        gfx_text_micro(2, y + 1, ble_sensor_name(e->s.fmt));
        if (e->name[0])
        {
            ble_ln_init(&l, "");
            for (uint8_t i = 0; e->name[i] && i < 6; i++)
                ble_ln_char(&l, e->name[i]);
            gfx_text_micro(34, y + 1, l.s);
        }
        else
        {
            addr_tail(buf, e->addr, e->addr_type);
            gfx_text_micro(34, y + 1, buf);
        }

        if (e->s.valid & BLE_SENSOR_ENCRYPTED)
        {
            gfx_text_micro(62, y + 1, "ENCRYPTED");
        }
        else
        {
            if (e->s.valid & BLE_SENSOR_TEMP)
            {
                ble_ln_init(&l, "");
                ble_ln_fixed(&l, e->s.temp / 10, 1);
                ble_ln_char(&l, 'C');
                gfx_text_micro(84 - gfx_text_micro_width(l.s), y + 1, l.s);
            }
            if (e->s.valid & BLE_SENSOR_HUM)
            {
                ble_ln_init(&l, "");
                ble_ln_int(&l, (e->s.hum + 50) / 100);
                ble_ln_char(&l, 'H');
                gfx_text_micro(106 - gfx_text_micro_width(l.s), y + 1, l.s);
            }
            if (e->s.valid & BLE_SENSOR_BATT)
            {
                gfx_fmt_int(buf, e->s.batt);
                gfx_text_micro(DISP_W - 1 - gfx_text_micro_width(buf), y + 1, buf);
            }
        }

        if (first + row == selected)
            gfx_invert(0, y, DISP_W, 8);
    }

    // Footer: the selected sensor's radio side
    const ble_sensor_entry_t *e = &list[selected < n ? selected : 0];
    ble_ln_init(&l, "RSSI ");
    ble_ln_int(&l, e->rssi);
    ble_ln_str(&l, " AGO ");
    ln_age(&l, now_ms - e->last_ms);
    if (e->s.valid & BLE_SENSOR_MV)
    {
        ble_ln_char(&l, ' ');
        ble_ln_int(&l, e->s.mv);
        ble_ln_str(&l, "MV");
    }
    gfx_hline(0, DISP_W - 1, 56);
    gfx_text_micro(2, 58, l.s);

    display_flush();
}
