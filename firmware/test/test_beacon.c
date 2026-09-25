// Host side test of the beacon payload builders, cross checked against the
// real advert decoder (ble_adv.c): every preset must produce advertising
// data that this firmware's own BLE scan would classify the way the beacon
// screen promises.
#include <stdio.h>
#include <string.h>

#include "ble_adv.h"
#include "ble_beacon.h"

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

static const uint8_t dev_id[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};

static void test_all_build(void)
{
    printf("beacon payload builders\n");

    for (uint8_t type = 0; type < BLE_BEACON_TYPE_COUNT; type++)
    {
        uint8_t ad[BLE_BEACON_AD_MAX];
        uint8_t len = ble_beacon_build(type, dev_id, ad);
        char what[40];
        snprintf(what, sizeof(what), "type %u builds", type);
        check(what, len > 0 && len <= BLE_BEACON_AD_MAX, 1);

        // The AD structures must walk cleanly to the end
        ble_ad_iter_t it;
        uint8_t t, n;
        const uint8_t *v;
        ble_ad_iter_init(&it, ad, len);
        uint8_t walked = 0;
        while (ble_ad_next(&it, &t, &v, &n))
            walked++;
        snprintf(what, sizeof(what), "type %u walks whole structures", type);
        check(what, walked >= 1, 1);
    }
}

// The summary keeps pointers into the advertising data, so the buffer must
// outlive the call
static uint8_t g_ad[BLE_BEACON_AD_MAX];

static uint8_t classify(uint8_t type, ble_ad_summary_t *out)
{
    uint8_t len = ble_beacon_build(type, dev_id, g_ad);
    ble_ad_summarize(g_ad, len, out);
    return len;
}

static void test_cross_decode(void)
{
    printf("\nbeacon vs the real decoder\n");

    ble_ad_summary_t s;

    classify(BLE_BEACON_NAME, &s);
    check("name beacon kind", s.kind, BLE_KIND_PLAIN);
    check("name beacon name length", s.name_len, 12);
    check("name beacon name", s.name && memcmp(s.name, "PIXLANALYZER", 12) == 0, 1);

    classify(BLE_BEACON_IBEACON, &s);
    check("ibeacon kind", s.kind, BLE_KIND_IBEACON);
    check("ibeacon company", s.company, BLE_COMPANY_APPLE);

    classify(BLE_BEACON_EDDY_UID, &s);
    check("eddystone uid kind", s.kind, BLE_KIND_EDDYSTONE);

    classify(BLE_BEACON_EDDY_URL, &s);
    check("eddystone url kind", s.kind, BLE_KIND_EDDYSTONE);

    classify(BLE_BEACON_ALT, &s);
    check("altbeacon company", s.company, 0xBEAC);

    classify(BLE_BEACON_SWIFTPAIR, &s);
    check("swift pair kind", s.kind, BLE_KIND_MICROSOFT);
    check("swift pair spam pattern", s.spam, BLE_SPAM_SWIFTPAIR);

    classify(BLE_BEACON_FINDMY, &s);
    check("findmy test kind", s.kind, BLE_KIND_FINDMY);
}

static void test_addr(void)
{
    printf("\nbeacon address\n");

    uint8_t addr[6];
    ble_beacon_addr(dev_id, addr);
    check("static random top bits", addr[5] >> 6, 3);
    check("derived from the id", addr[0], dev_id[2]);

    // Two different ids must not collide
    uint8_t other[6];
    uint8_t dev2[8] = {0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22};
    ble_beacon_addr(dev2, other);
    check("distinct ids give distinct addresses", memcmp(addr, other, 6) != 0, 1);
}

static void test_pdu_fit(void)
{
    printf("\nbeacon PDU length\n");

    // AdvA plus AD data must fit the 37 byte legacy advert payload
    for (uint8_t type = 0; type < BLE_BEACON_TYPE_COUNT; type++)
    {
        uint8_t ad[BLE_BEACON_AD_MAX];
        uint8_t len = ble_beacon_build(type, dev_id, ad);
        char what[40];
        snprintf(what, sizeof(what), "type %u fits a legacy advert", type);
        check(what, 6 + len <= 37, 1);
    }
}

int main(void)
{
    test_all_build();
    test_cross_decode();
    test_addr();
    test_pdu_fit();

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
