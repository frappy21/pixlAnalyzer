#include "ble_scan.h"
#include "display.h"
#include "gfx.h"
#include "ui.h"
#include "ui_ble.h"

// ---------------------------------------------------------------------------
// BLE
// ---------------------------------------------------------------------------

void ui_ble_list(uint8_t selected, uint32_t packets)
{
    char buf[16];
    uint8_t idx[BLE_MAX_DEVICES];
    uint8_t n = ble_scan_sorted(idx, BLE_MAX_DEVICES);

    display_clear();
    gfx_text(2, 1, "BLE");
    gfx_fmt_int(buf, ble_scan_count());
    gfx_text_micro(28, 2, buf);
    gfx_text_micro(28 + gfx_text_micro_width(buf) + 2, 2, "DEV");
    gfx_fmt_int(buf, (int)packets);
    gfx_text_micro(66, 2, buf);
    gfx_text_micro(66 + gfx_text_micro_width(buf) + 2, 2, "PKT");
    ui_battery();
    gfx_hline(0, DISP_W - 1, 9);

    if (n == 0)
    {
        gfx_text(14, 28, "LISTENING...");
        display_flush();
        return;
    }

    uint8_t first = 0;
    if (selected >= 6)
        first = selected - 5;
    if (n > 6 && first > n - 6)
        first = n - 6;

    for (uint8_t row = 0; row < 6 && first + row < n; row++)
    {
        const ble_dev_t *d = ble_scan_device(idx[first + row]);
        int y = 11 + row * 9;

        // Address tail is enough to tell devices apart on a 128px screen
        static const char hex[] = "0123456789ABCDEF";
        char mac[6];
        mac[0] = hex[d->addr[1] >> 4];
        mac[1] = hex[d->addr[1] & 15];
        mac[2] = hex[d->addr[0] >> 4];
        mac[3] = hex[d->addr[0] & 15];
        mac[4] = '\0';
        gfx_text_micro(2, y + 1, mac);

        const char *kind = ble_kind_name(d->kind);
        if (kind[0])
            gfx_text_micro(22, y + 1, kind);
        else if (d->name[0])
            gfx_text_micro(22, y + 1, d->name);

        gfx_fmt_int(buf, -d->rssi);
        gfx_text_micro(DISP_W - 22, y + 1, buf);

        if (first + row == selected)
            gfx_invert(0, y, DISP_W, 8);
    }

    display_flush();
}

void ui_ble_detail(const ble_dev_t *dev)
{
    char buf[20];
    static const char hex[] = "0123456789ABCDEF";

    display_clear();
    ui_title("DEVICE");

    char mac[18];
    int p = 0;
    for (int i = 5; i >= 0; i--)
    {
        mac[p++] = hex[dev->addr[i] >> 4];
        mac[p++] = hex[dev->addr[i] & 15];
        if (i)
            mac[p++] = ':';
    }
    mac[p] = '\0';
    gfx_text_micro(2, 12, mac);

    gfx_text_micro(2, 21, dev->addr_type ? "RANDOM ADDR" : "PUBLIC ADDR");

    if (dev->name[0])
        gfx_text_micro(2, 29, dev->name);

    const char *kind = ble_kind_name(dev->kind);
    if (kind[0])
        gfx_text_micro(70, 29, kind);

    gfx_text_micro(2, 38, "RSSI");
    gfx_fmt_int(buf, -dev->rssi);
    gfx_text_micro(34, 38, buf);
    gfx_text_micro(34 + gfx_text_micro_width(buf) + 2, 38, "DBM");

    gfx_text_micro(70, 38, "PKT");
    gfx_fmt_int(buf, dev->packets);
    gfx_text_micro(96, 38, buf);

    // How long it has been around: the number that matters when you are asking
    // whether something is following you
    uint32_t seen_s = (dev->last_ms - dev->first_ms) / 1000u;
    gfx_text_micro(2, 46, "SEEN FOR");
    gfx_fmt_int(buf, (int)seen_s);
    gfx_text_micro(50, 46, buf);
    gfx_text_micro(50 + gfx_text_micro_width(buf) + 2, 46, "S");

    if (dev->company)
    {
        gfx_text_micro(2, 54, "COMPANY");
        buf[0] = hex[(dev->company >> 12) & 15];
        buf[1] = hex[(dev->company >> 8) & 15];
        buf[2] = hex[(dev->company >> 4) & 15];
        buf[3] = hex[dev->company & 15];
        buf[4] = '\0';
        gfx_text_micro(50, 54, buf);
    }

    display_flush();
}
