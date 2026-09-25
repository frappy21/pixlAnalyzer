// Host side test of the BLE advert decoders, the sensor formats and the
// device table logic (interval, following, spam flood). Everything here is
// the real code; the adverts are made up byte by byte.
#include <stdio.h>
#include <string.h>

#include "ble_adv.h"
#include "ble_devtab.h"
#include "ble_sensor.h"

static int failures = 0;

static void check(const char *what, long got, long want)
{
    if (got != want)
    {
        printf("  FAIL %-44s got %ld want %ld\n", what, got, want);
        failures++;
    }
    else
    {
        printf("  ok   %-44s\n", what);
    }
}

static char g_lines[48][BLE_LINE_LEN];
static uint8_t g_count;

static void describe(const uint8_t *ad, uint8_t len)
{
    ble_lines_t out = {.lines = g_lines, .max = 48, .count = 0};
    ble_describe_ad(ad, len, &out);
    g_count = out.count;
}

// The card has a line containing text
static void check_line(const char *what, const char *text)
{
    for (uint8_t i = 0; i < g_count; i++)
    {
        if (strstr(g_lines[i], text))
        {
            printf("  ok   %-44s\n", what);
            return;
        }
    }
    printf("  FAIL %-44s no line with \"%s\", have:\n", what, text);
    for (uint8_t i = 0; i < g_count; i++)
        printf("         | %s\n", g_lines[i]);
    failures++;
}

static uint8_t kind_of(const uint8_t *ad, uint8_t len)
{
    ble_ad_summary_t s;
    ble_ad_summarize(ad, len, &s);
    return s.kind;
}

static uint8_t spam_of(const uint8_t *ad, uint8_t len)
{
    ble_ad_summary_t s;
    ble_ad_summarize(ad, len, &s);
    return s.spam;
}

#define AD(...) (const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__})

// ---------------------------------------------------------------------------

static void test_trackers(void)
{
    printf("trackers\n");

    // AirTag away from its owner: Find My type 0x12, 25 byte payload
    static const uint8_t airtag[] = {
        0x1E, 0xFF, 0x4C, 0x00, 0x12, 0x19, 0x10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
        12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 0x01, 0x00,
    };
    check("airtag is find my", kind_of(airtag, sizeof(airtag)), BLE_KIND_FINDMY);
    check("find my is a tracker", ble_kind_is_tracker(BLE_KIND_FINDMY), 1);
    describe(airtag, sizeof(airtag));
    check_line("airtag card company", "MFG 004C APPLE");
    check_line("airtag card separated", "FIND MY SEPARATED");

    // Near its owner: the short form
    describe(AD(0x07, 0xFF, 0x4C, 0x00, 0x12, 0x02, 0x24, 0x01));
    check_line("find my near owner", "FIND MY NEAR OWNER");

    check("smarttag", kind_of(AD(0x05, 0x16, 0x5A, 0xFD, 0x10, 0x20)), BLE_KIND_SMARTTAG);
    check("tile feed list", kind_of(AD(0x03, 0x03, 0xED, 0xFE)), BLE_KIND_TILE);
    check("tile feec data", kind_of(AD(0x04, 0x16, 0xEC, 0xFE, 0x01)), BLE_KIND_TILE);

    // Google FMDN: Eddystone UUID, frame 0x40, 20 byte EID, flags
    static const uint8_t fmdn[] = {
        0x03, 0x03, 0xAA, 0xFE, 0x19, 0x16, 0xAA, 0xFE, 0x41, 1, 2, 3, 4, 5, 6, 7, 8,
        9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 0x80,
    };
    check("fmdn", kind_of(fmdn, sizeof(fmdn)), BLE_KIND_GOOGLE_FMDN);
    describe(fmdn, sizeof(fmdn));
    check_line("fmdn card", "FIND MY DEVICE NETWORK");
    check_line("fmdn utp mode", "UNWANTED TRACKING");

    // A tracker kind is not downgraded by a later plain Apple advert
    check("merge keeps tracker", ble_kind_merge(BLE_KIND_FINDMY, BLE_KIND_APPLE), BLE_KIND_FINDMY);
}

static void test_detail_card(void)
{
    printf("detail card\n");

    describe(AD(0x02, 0x01, 0x06, 0x02, 0x0A, 0xFC, 0x03, 0x19, 0xC2, 0x03, 0x05, 0x03, 0x0F,
                0x18, 0x0A, 0x18, 0x05, 0x09, 'M', 'x', ' ', '3'));
    check_line("flags", "FLAGS 06 GEN LE-ONLY");
    check_line("tx power", "TX POWER -4 DBM");
    check_line("appearance", "LOOKS 03C2 MOUSE");
    check_line("uuid battery", "UUID 180F BATTERY");
    check_line("uuid device info", "UUID 180A DEVICE INFO");
    check_line("name", "NAME Mx 3");

    // iBeacon: UUID, major 1, minor 2, measured power -59
    static const uint8_t ibeacon[] = {
        0x02, 0x01, 0x06, 0x1A, 0xFF, 0x4C, 0x00, 0x02, 0x15, 0xE2, 0xC5, 0x6D, 0xB5,
        0xDF, 0xFB, 0x48, 0xD2, 0xB0, 0x60, 0xD0, 0xF5, 0xA7, 0x10, 0x96, 0xE0,
        0x00, 0x01, 0x00, 0x02, 0xC5,
    };
    check("ibeacon kind", kind_of(ibeacon, sizeof(ibeacon)), BLE_KIND_IBEACON);
    describe(ibeacon, sizeof(ibeacon));
    check_line("ibeacon uuid", "UUID E2C56DB5DFFB48D2");
    check_line("ibeacon major minor", "MAJ 1 MIN 2 PWR -59");

    // Apple continuity names, and proximity pairing as a spam prone pattern
    describe(AD(0x0A, 0xFF, 0x4C, 0x00, 0x10, 0x05, 0x01, 0x18, 0x44, 0x00, 0x00));
    check_line("continuity nearby info", "NEARBY INFO");
    check("nearby info is no spam", spam_of(AD(0x06, 0xFF, 0x4C, 0x00, 0x10, 0x01, 0x01)),
          BLE_SPAM_NONE);
    static const uint8_t prox[] = {0x0A, 0xFF, 0x4C, 0x00, 0x07, 0x05, 0x01, 0x02, 0x20, 0x75, 0xAA};
    check("prox pairing spam", spam_of(prox, sizeof(prox)), BLE_SPAM_APPLE);
    describe(prox, sizeof(prox));
    check_line("prox pairing model", "PROX PAIR MODEL 0220");

    // Microsoft CDP from a Windows desktop, and a Swift Pair mouse
    describe(AD(0x08, 0xFF, 0x06, 0x00, 0x01, 0x09, 0x20, 0x02, 0x00));
    check_line("ms cdp", "CDP WINDOWS DESKTOP");
    static const uint8_t swift[] = {0x0A, 0xFF, 0x06, 0x00, 0x03, 0x00, 0x80, 'M', 'o', 'u', 's'};
    check("swift pair spam", spam_of(swift, sizeof(swift)), BLE_SPAM_SWIFTPAIR);
    check("swift pair kind", kind_of(swift, sizeof(swift)), BLE_KIND_MICROSOFT);
    describe(swift, sizeof(swift));
    check_line("swift pair name", "Mous");

    // Samsung EasySetup buds popup and Fast Pair model id
    check("samsung spam", spam_of(AD(0x08, 0xFF, 0x75, 0x00, 0x42, 0x09, 0x81, 0x02, 0x14)),
          BLE_SPAM_SAMSUNG);
    check("fast pair spam", spam_of(AD(0x06, 0x16, 0x2C, 0xFE, 0x00, 0x20, 0x0B)),
          BLE_SPAM_FASTPAIR);
    describe(AD(0x06, 0x16, 0x2C, 0xFE, 0x00, 0x20, 0x0B));
    check_line("fast pair model", "MODEL 00200B");

    // Eddystone URL https://www.example.com/
    static const uint8_t url[] = {0x03, 0x03, 0xAA, 0xFE, 0x0E, 0x16, 0xAA, 0xFE, 0x10, 0xEB,
                                  0x01, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 0x00};
    check("eddystone kind", kind_of(url, sizeof(url)), BLE_KIND_EDDYSTONE);
    describe(url, sizeof(url));
    check_line("eddystone url", "HTTPS://WWW.example.COM/");

    // Eddystone TLM: 3000 mV, 22.5 C
    describe(AD(0x11, 0x16, 0xAA, 0xFE, 0x20, 0x00, 0x0B, 0xB8, 0x16, 0x80, 0, 0, 0, 1, 0, 0, 0, 2));
    check_line("eddystone tlm", "TLM 3000MV 22.5C");

    // Unknown company is shown by its number
    describe(AD(0x04, 0xFF, 0x34, 0x12, 0x00));
    check_line("unknown company", "MFG 1234");
    check("company name", ble_company_name(0x0499) != 0, 1);
}

static void test_matter_mesh_auracast(void)
{
    printf("matter, mesh, auracast\n");

    // Matter commissionable: discriminator 3840, VID FFF1, PID 8000
    static const uint8_t matter[] = {0x02, 0x01, 0x06, 0x0B, 0x16, 0xF6, 0xFF, 0x00,
                                     0x00, 0x0F, 0xF1, 0xFF, 0x00, 0x80, 0x00};
    check("matter kind", kind_of(matter, sizeof(matter)), BLE_KIND_MATTER);
    describe(matter, sizeof(matter));
    check_line("matter discriminator", "DISCRIM 3840 VER 0");
    check_line("matter vid pid", "VID FFF1 PID 8000");

    // Unprovisioned device beacon
    static const uint8_t unprov[] = {0x14, 0x2B, 0x00, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB,
                                     0xCD, 0xEF, 0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC,
                                     0xFE, 0x00, 0x00};
    check("mesh kind", kind_of(unprov, sizeof(unprov)), BLE_KIND_MESH);
    describe(unprov, sizeof(unprov));
    check_line("mesh unprovisioned", "MESH UNPROVISIONED");
    check_line("mesh uuid", "UUID 0123456789ABCDEF");
    check_line("mesh uuid tail", "1032547698BADCFE");

    // Secure network beacon: IV update in progress, IV index 5
    static const uint8_t snb[] = {0x17, 0x2B, 0x01, 0x02, 1, 2, 3, 4, 5, 6, 7, 8, 0x00,
                                  0x00, 0x00, 0x05, 1, 2, 3, 4, 5, 6, 7, 8};
    describe(snb, sizeof(snb));
    check_line("mesh snb flags", "KEY REFRESH 0 IV UPD 1");
    check_line("mesh snb iv", "IV INDEX 5");
    check_line("mesh snb net id", "NET 0102030405060708");

    // Auracast: broadcast audio announcement + broadcast name
    static const uint8_t aura[] = {0x06, 0x16, 0x52, 0x18, 0x12, 0x34, 0x56,
                                   0x05, 0x30, 'H', 'a', 'l', 'l'};
    ble_ad_summary_t s;
    ble_ad_summarize(aura, sizeof(aura), &s);
    check("auracast kind", s.kind, BLE_KIND_AURACAST);
    check("auracast name", s.name_len == 4 && memcmp(s.name, "Hall", 4) == 0, 1);
    describe(aura, sizeof(aura));
    check_line("auracast id", "AURACAST ID 563412");
    check_line("auracast name line", "BCAST Hall");
}

static void test_extended(void)
{
    printf("extended advertising\n");
    ble_ext_t e;

    // ADV_EXT_IND: ADI (SID 5, DID 0x123) and AuxPtr (ch 12, 30us units,
    // offset 100 = 3000us, 1M)
    static const uint8_t ind[] = {0x06, 0x18, 0x23, 0x51, 0x0C, 0x64, 0x00};
    check("ext parse", ble_ext_parse(ind, sizeof(ind), &e), 1);
    check("ext no adva", e.adva == 0, 1);
    check("ext sid", e.sid, 5);
    check("ext did", e.did, 0x123);
    check("ext aux chan", e.aux_chan, 12);
    check("ext aux offset", (long)e.aux_offset_us, 3000);
    check("ext aux phy", e.aux_phy, BLE_PHY_1M);
    check("ext no data", e.ad_len, 0);

    // 300us units, 2M PHY
    static const uint8_t ind2[] = {0x04, 0x10, 0x80 | 0x05, 0x0A, 0x20};
    check("ext parse 2", ble_ext_parse(ind2, sizeof(ind2), &e), 1);
    check("ext 300us units", (long)e.aux_offset_us, 3000);
    check("ext 2m", e.aux_phy, BLE_PHY_2M);

    // AUX_ADV_IND: AdvA, ADI, TxPower, then AdvData with a name
    static const uint8_t aux[] = {0x0A, 0x49, 1, 2, 3, 4, 5, 6, 0x23, 0x51, 0xF8,
                                  0x04, 0x09, 'E', 'x', 't'};
    check("aux parse", ble_ext_parse(aux, sizeof(aux), &e), 1);
    check("aux adva", e.adva != 0 && e.adva[0] == 1, 1);
    check("aux tx power", e.tx_power, -8);
    check("aux data len", e.ad_len, 5);

    // Header longer than the payload
    static const uint8_t bad[] = {0x0A, 0x01, 1, 2};
    check("ext truncated", ble_ext_parse(bad, sizeof(bad), &e), 0);

    check("data ch 0", ble_data_channel_freq(0), 4);
    check("data ch 10", ble_data_channel_freq(10), 24);
    check("data ch 11", ble_data_channel_freq(11), 28);
    check("data ch 36", ble_data_channel_freq(36), 78);

    // Whole structures only when a long advert is squeezed into 31 bytes
    uint8_t big[40], out[BLE_AD_MAX];
    memset(big, 0, sizeof(big));
    big[0] = 12;
    big[1] = 0xFF;
    big[13] = 12;
    big[14] = 0xFF;
    big[26] = 4;
    big[27] = 0x09;
    big[31] = 7;
    big[32] = 0x09;
    check("copy whole", ble_ad_copy_whole(out, BLE_AD_MAX, big, 39), 26 + 5);
}

static void test_sensors(void)
{
    printf("sensors\n");
    ble_sensor_t s;

    // BTHome v2: packet id, battery 93, 25.06 C, 50.55 %
    check("bthome", ble_sensor_decode(AD(0x0E, 0x16, 0xD2, 0xFC, 0x40, 0x00, 0x01, 0x01, 0x5D,
                                         0x02, 0xCA, 0x09, 0x03, 0xBF, 0x13), &s), 1);
    check("bthome temp", s.temp, 2506);
    check("bthome hum", s.hum, 5055);
    check("bthome batt", s.batt, 93);
    check("bthome encrypted", ble_sensor_decode(AD(0x08, 0x16, 0xD2, 0xFC, 0x41, 1, 2, 3, 4), &s)
                                  && s.valid == BLE_SENSOR_ENCRYPTED, 1);

    // ATC1441: 23.5 C, 45 %, 90 %, 3000 mV
    check("atc", ble_sensor_decode(AD(0x10, 0x16, 0x1A, 0x18, 0xA4, 0xC1, 0x38, 1, 2, 3, 0x00,
                                      0xEB, 0x2D, 0x5A, 0x0B, 0xB8, 0x07), &s), 1);
    check("atc fmt", s.fmt, BLE_SENSOR_ATC);
    check("atc temp", s.temp, 2350);
    check("atc hum", s.hum, 4500);
    check("atc mv", s.mv, 3000);

    // pvvx: 23.50 C, 45.00 %, 3000 mV, 90 %
    check("pvvx", ble_sensor_decode(AD(0x12, 0x16, 0x1A, 0x18, 3, 2, 1, 0x38, 0xC1, 0xA4, 0x2E,
                                       0x09, 0x94, 0x11, 0xB8, 0x0B, 0x5A, 0x07, 0x00), &s), 1);
    check("pvvx fmt", s.fmt, BLE_SENSOR_PVVX);
    check("pvvx temp", s.temp, 2350);
    check("pvvx hum", s.hum, 4500);
    check("pvvx batt", s.batt, 90);

    // MiBeacon v5 with MAC and the temperature + humidity object
    check("mibeacon", ble_sensor_decode(AD(0x15, 0x16, 0x95, 0xFE, 0x50, 0x50, 0x5B, 0x05, 0x01,
                                           1, 2, 3, 4, 5, 6, 0x0D, 0x10, 0x04, 0xEB, 0x00,
                                           0xC2, 0x01), &s), 1);
    check("mibeacon temp", s.temp, 2350);
    check("mibeacon hum", s.hum, 4500);
    check("mibeacon encrypted", ble_sensor_decode(AD(0x0C, 0x16, 0x95, 0xFE, 0x58, 0x58, 0x5B,
                                                     0x05, 0x01, 1, 2, 3, 4), &s) &&
                                    s.valid == BLE_SENSOR_ENCRYPTED, 1);

    // Govee H5075: 21.7 C, 50.2 %, 84 %; and a negative temperature
    check("govee", ble_sensor_decode(AD(0x09, 0xFF, 0x88, 0xEC, 0x00, 0x03, 0x51, 0x9E, 0x54,
                                        0x00), &s), 1);
    check("govee temp", s.temp, 2170);
    check("govee hum", s.hum, 5020);
    check("govee batt", s.batt, 84);
    ble_sensor_decode(AD(0x09, 0xFF, 0x88, 0xEC, 0x00, 0x80, 0x13, 0x88, 0x54, 0x00), &s);
    check("govee negative", s.temp, -50);

    // Ruuvi RAWv2, the reference vector: 24.3 C, 53.49 %, 2977 mV
    check("ruuvi", ble_sensor_decode(AD(0x1B, 0xFF, 0x99, 0x04, 0x05, 0x12, 0xFC, 0x53, 0x94,
                                        0xC3, 0x7C, 0x00, 0x04, 0xFF, 0xFC, 0x04, 0x0C, 0xAC,
                                        0x36, 0x42, 0x00, 0xCD, 0xCB, 0xB8, 0x33, 0x4C, 0x88,
                                        0x4F), &s), 1);
    check("ruuvi temp", s.temp, 2430);
    check("ruuvi hum", s.hum, 5349);
    check("ruuvi mv", s.mv, 2977);

    // Inkbird IBS-TH1: 23.45 C, 45.00 %, 85 %
    check("inkbird", ble_sensor_decode(AD(0x04, 0x09, 's', 'p', 's', 0x0A, 0xFF, 0x29, 0x09,
                                          0x94, 0x11, 0x00, 0x00, 0x00, 0x55, 0x08), &s), 1);
    check("inkbird temp", s.temp, 2345);
    check("inkbird hum", s.hum, 4500);
    check("inkbird batt", s.batt, 85);
    check("no inkbird without name",
          ble_sensor_decode(AD(0x0A, 0xFF, 0x29, 0x09, 0x94, 0x11, 0x00, 0x00, 0x00, 0x55, 0x08), &s),
          0);

    // SwitchBot meter: 22.5 C, 45 %, 90 %; and -5.5 C
    check("switchbot", ble_sensor_decode(AD(0x09, 0x16, 0x3D, 0xFD, 0x54, 0x00, 0x5A, 0x05, 0x96,
                                            0x2D), &s), 1);
    check("switchbot temp", s.temp, 2250);
    check("switchbot hum", s.hum, 4500);
    check("switchbot batt", s.batt, 90);
    ble_sensor_decode(AD(0x09, 0x16, 0x3D, 0xFD, 0x54, 0x00, 0x5A, 0x05, 0x05, 0x2D), &s);
    check("switchbot negative", s.temp, -550);

    check("not a sensor", ble_sensor_decode(AD(0x02, 0x01, 0x06), &s), 0);

    describe(AD(0x0E, 0x16, 0xD2, 0xFC, 0x40, 0x00, 0x01, 0x01, 0x5D, 0x02, 0xCA, 0x09, 0x03,
                0xBF, 0x13));
    check_line("sensor card", "SENSOR BTHOME");
    check_line("sensor card values", "25.06C 50.55 RH");
    check_line("sensor card battery", "BATT 93 PCT");
}

// ---------------------------------------------------------------------------
// Device table
// ---------------------------------------------------------------------------

static ble_dev_t g_table[8];

static void feed(const uint8_t *addr, uint8_t type, const uint8_t *ad, uint8_t len, int8_t rssi,
                 uint32_t now_ms, uint32_t listen_ms)
{
    ble_rx_t rx;
    memset(&rx, 0, sizeof(rx));
    rx.addr = addr;
    rx.addr_type = type;
    rx.pdu_type = 0x02; // ADV_NONCONN_IND
    rx.rssi = rssi;
    rx.now_ms = now_ms;
    rx.listen_ms = listen_ms;
    rx.ad = ad;
    rx.ad_len = len;
    ble_devtab_packet(&rx);
}

static void test_devtab(void)
{
    printf("device table\n");
    static const uint8_t plain[] = {0x02, 0x01, 0x06};
    static const uint8_t airtag[] = {0x07, 0xFF, 0x4C, 0x00, 0x12, 0x02, 0x24, 0x01};
    uint8_t a[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    uint8_t b[6] = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xE6};
    uint8_t c[6] = {0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6};

    ble_devtab_attach(g_table, 8);

    // Interval: one advert per 100 ms of listening, whatever the wall clock
    for (int i = 0; i < 10; i++)
        feed(a, BLE_ADDR_PUBLIC, plain, sizeof(plain), (int8_t)(-60 - i), 1000u + i * 370u,
             (uint32_t)i * 100u);
    const ble_dev_t *d = ble_devtab_find(a, BLE_ADDR_PUBLIC);
    check("dev found", d != 0, 1);
    check("dev packets", d->packets, 10);
    check("dev interval", ble_dev_interval_ms(d), 100);
    check("dev rssi max", d->rssi, -60);
    check("dev rssi min", d->rssi_min, -69);
    check("dev rssi last", d->rssi_last, -69);
    check("dev first", (long)d->first_ms, 1000);
    check("dev last", (long)d->last_ms, 1000 + 9 * 370);
    check("dev not following", (d->flags & BLE_DEV_FOLLOW) != 0, 0);

    // Following: a tag heard every 90 s for 12 minutes, and a plain device
    // doing the same; only the tag counts in tracker mode
    for (uint32_t t = 0; t <= 12u * 60000u; t += 90000u)
    {
        feed(b, BLE_ADDR_RANDOM, airtag, sizeof(airtag), -70, 5000u + t, 0);
        feed(c, BLE_ADDR_RANDOM, plain, sizeof(plain), -70, 5000u + t, 0);
    }
    d = ble_devtab_find(b, BLE_ADDR_RANDOM);
    check("tag following", (d->flags & BLE_DEV_FOLLOW) != 0, 1);
    check("tag kind", d->kind, BLE_KIND_FINDMY);
    check("following trackers", ble_devtab_following(BLE_FOLLOW_TRACKERS), 1);
    check("following all", ble_devtab_following(BLE_FOLLOW_ALL), 2);
    check("following off", ble_devtab_following(BLE_FOLLOW_OFF), 0);

    // Heard for 12 minutes but only in two of them: not following
    uint8_t e[6] = {0xE1, 0, 0, 0, 0, 0xC0};
    feed(e, BLE_ADDR_RANDOM, plain, sizeof(plain), -70, 1000, 0);
    feed(e, BLE_ADDR_RANDOM, plain, sizeof(plain), -70, 1000 + 12u * 60000u, 0);
    check("two sightings not following",
          (ble_devtab_find(e, BLE_ADDR_RANDOM)->flags & BLE_DEV_FOLLOW) != 0, 0);

    uint8_t idx[8];
    check("filter trackers", ble_devtab_sorted(idx, 8, BLE_FILTER_TRACKERS, BLE_SORT_RSSI), 1);
    check("filter follow", ble_devtab_sorted(idx, 8, BLE_FILTER_FOLLOW, BLE_SORT_RSSI), 2);
    uint8_t n = ble_devtab_sorted(idx, 8, BLE_FILTER_ALL, BLE_SORT_RSSI);
    check("sort rssi first", ble_devtab_device(idx[0])->rssi, -60);
    n = ble_devtab_sorted(idx, 8, BLE_FILTER_ALL, BLE_SORT_FIRST);
    check("sort first seen", (long)ble_devtab_device(idx[0])->first_ms, 1000);
    check("sort count", n, 4);

    // Anonymous extended advert, keyed by its set id
    ble_rx_t rx;
    memset(&rx, 0, sizeof(rx));
    rx.addr_type = BLE_ADDR_EXT_ANON;
    rx.pdu_type = 0x07;
    rx.ext = true;
    rx.sid = 3;
    rx.has_auxptr = true;
    rx.aux_chan = 17;
    rx.aux_phy = BLE_PHY_2M;
    rx.rssi = -80;
    rx.now_ms = 20000;
    ble_devtab_packet(&rx);
    uint8_t anon[6] = {3, 0, 0, 0, 0, 0};
    d = ble_devtab_find(anon, BLE_ADDR_EXT_ANON);
    check("ext anon entry", d != 0, 1);
    check("ext anon flags", (d->flags & BLE_DEV_EXT) != 0, 1);
    check("ext anon aux", d->aux_chan * 10 + d->aux_phy, 171);
    check("filter ext", ble_devtab_sorted(idx, 8, BLE_FILTER_EXT, BLE_SORT_RSSI), 1);

    // A full table recycles the stalest non-tracker, never the tag
    for (uint8_t i = 0; i < 6; i++)
    {
        uint8_t x[6] = {i, 0x77, 0, 0, 0, 0x40};
        feed(x, BLE_ADDR_RANDOM, plain, sizeof(plain), -90, 900000u + i, 0);
    }
    check("table full", ble_devtab_count(), 8);
    check("tag kept", ble_devtab_find(b, BLE_ADDR_RANDOM) != 0, 1);
    check("stalest recycled", ble_devtab_find(e, BLE_ADDR_RANDOM) == 0, 1);
}

static void test_spam(void)
{
    printf("spam flood\n");
    static const uint8_t prox[] = {0x0A, 0xFF, 0x4C, 0x00, 0x07, 0x05, 0x01, 0x02, 0x20, 0x75, 0xAA};
    static const uint8_t plain[] = {0x02, 0x01, 0x06};

    ble_devtab_attach(g_table, 8);

    // A phone with an AirPods case: one address, no flood
    uint8_t one[6] = {1, 2, 3, 4, 5, 0x40};
    for (int i = 0; i < 20; i++)
        feed(one, BLE_ADDR_RANDOM, prox, sizeof(prox), -50, 1000u + i * 50u, 0);
    check("one case no alert", ble_devtab_spam(2000)->active, 0);

    // Many new random addresses without the pattern: no alert either
    for (uint8_t i = 0; i < 20; i++)
    {
        uint8_t x[6] = {i, 0x55, 0, 0, 0, 0x40};
        feed(x, BLE_ADDR_RANDOM, plain, sizeof(plain), -50, 3000u + i * 20u, 0);
    }
    check("crowd no alert", ble_devtab_spam(3500)->active, 0);

    // A flood: 20 new addresses with the pattern within a second
    for (uint8_t i = 0; i < 20; i++)
    {
        uint8_t x[6] = {i, 0x99, 0, 0, 0, 0x40};
        feed(x, BLE_ADDR_RANDOM, prox, sizeof(prox), -40, 10000u + i * 50u, 0);
    }
    const ble_spam_state_t *s = ble_devtab_spam(11000);
    check("flood alert", s->active, 1);
    check("flood family", s->family, BLE_SPAM_APPLE);
    check("flood rate", s->rate >= BLE_SPAM_THRESHOLD, 1);
    check("flood kept out", s->dropped > 0, 1);
    check("flood alert holds", ble_devtab_spam(11000 + 5000)->active, 1);
    check("flood alert ends", ble_devtab_spam(11000 + BLE_SPAM_HOLD_MS + 1000)->active, 0);
    check("flood rate decays", ble_devtab_spam(11000 + BLE_SPAM_HOLD_MS + 1000)->rate, 0);

    ble_devtab_attach(0, 0);
    feed(one, BLE_ADDR_RANDOM, prox, sizeof(prox), -50, 1000, 0);
    check("detached ignores", ble_devtab_count(), 0);
}

int main(void)
{
    test_trackers();
    test_detail_card();
    test_matter_mesh_auracast();
    test_extended();
    test_sensors();
    test_devtab();
    test_spam();

    if (failures)
    {
        printf("%d BLE test(s) FAILED\n", failures);
        return 1;
    }
    printf("all BLE tests passed\n");
    return 0;
}
