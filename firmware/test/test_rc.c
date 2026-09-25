// Host side test of the RC protocol table: checksums, stick round trips
// (decode of a built frame and build of a decoded one), the address
// matching and the generic stick tracker. The layouts are the nRF24
// Multiprotocol ones, the byte values here are built to match them.
#include <stdio.h>
#include <string.h>

#include "rc_proto.h"

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

static void check_true(const char *what, bool got)
{
    check(what, got ? 1 : 0, 1);
}

// ---------------------------------------------------------------------------
// Table
// ---------------------------------------------------------------------------

static void test_table(void)
{
    printf("rc table\n");

    check("eight protocols", rc_proto_count(), 8);
    check("bayang bind address A0", rc_proto_a0(0), 0x00);
    check("symax a0", rc_proto_a0(1), 0xAB);
    check("h8 a0", rc_proto_a0(3), 0xC4);

    // Address matching: a full capture of the H8 address
    const uint8_t h8[5] = {0xC4, 0x57, 0x09, 0x65, 0x21};
    check("h8 by address", rc_proto_by_addr(h8, 5), 3);
    check("h8 by two bytes", rc_proto_by_addr(h8, 2), 3);

    // X5C and MJX share the first two bytes and differ in the third
    const uint8_t x5c[5] = {0x6D, 0x6A, 0x73, 0x73, 0x73};
    const uint8_t mjx[5] = {0x6D, 0x6A, 0x77, 0x77, 0x77};
    check("x5c by address", rc_proto_by_addr(x5c, 5), 2);
    check("mjx by address", rc_proto_by_addr(mjx, 5), 4);

    // Two bytes only: ambiguous, the 6D6A family must not claim it
    check_true("6D6A prefix alone is unknown", rc_proto_by_addr(x5c, 2) < 0);

    // The Bayang bind address is all zeros
    const uint8_t bayang[5] = {0, 0, 0, 0, 0};
    check("bayang bind by address", rc_proto_by_addr(bayang, 5), 0);

    // FlySky AFHDS2A: byte 1 = 0x05 distinguishes it from Bayang
    const uint8_t flysky[5] = {0x00, 0x05, 0x05, 0x05, 0x05};
    check("flysky by address", rc_proto_by_addr(flysky, 5), 5);
    check("flysky by two bytes", rc_proto_by_addr(flysky, 2), 5);

    // Bayang all-zero address still resolves to Bayang (not JJRC) from addr alone
    check("bayang still wins on addr alone", rc_proto_by_addr(bayang, 5), 0);

    // JJRC is detected by refine, not address
    uint8_t jjrc_payload[15];
    memset(jjrc_payload, 0, sizeof(jjrc_payload));
    jjrc_payload[0] = 0x03; // hop channel index, < 0x10
    check("jjrc refine from bayang", rc_proto_refine(0, jjrc_payload, 15), 6);

    // Bayang payload (byte 0 = 0xA5) is not refined to JJRC
    uint8_t bay_payload[15];
    memset(bay_payload, 0, sizeof(bay_payload));
    bay_payload[0] = 0xA5;
    check("bayang not refined", rc_proto_refine(0, bay_payload, 15), 0);

    // WLToys V911S detected from unknown proto + 8-byte payload with 0xDD marker
    uint8_t wl_payload[8];
    memset(wl_payload, 0, sizeof(wl_payload));
    wl_payload[0] = 0xDD;
    check("wltoys refine from unknown", rc_proto_refine(-1, wl_payload, 8), 7);

    // An unknown family (wrong byte 1, not 0x05 for FlySky)
    const uint8_t unknown[5] = {0x55, 0x12, 0x34, 0x56, 0x78};
    check_true("unknown address", rc_proto_by_addr(unknown, 5) < 0);
}

// ---------------------------------------------------------------------------
// Checksums (values recomputed by hand from the layouts)
// ---------------------------------------------------------------------------

static void test_checksums(void)
{
    printf("rc checksums\n");

    // BAYANG: additive over bytes 0..13
    uint8_t p[15];
    memset(p, 0, sizeof(p));
    p[0] = 0xA5;
    p[8] = 0x30; // a mid throttle, lsb first
    p[14] = rc_proto_checksum(0, p, 15);
    uint8_t sum = 0xA5 + 0x30;
    check("bayang checksum", p[14], sum);

    // SYMAX: XOR over 0..8 plus 0x55
    uint8_t s[10];
    memset(s, 0, sizeof(s));
    s[0] = 0x80; // mid throttle
    s[3] = 0x55; // a stick
    s[9] = rc_proto_checksum(1, s, 10);
    check("symax checksum", s[9], (uint8_t)((0x80 ^ 0x55) + 0x55));

    // H8: additive over 9..18
    uint8_t h[20];
    memset(h, 0, sizeof(h));
    h[9] = 0x40;
    h[10] = 0x7F;
    h[19] = rc_proto_checksum(3, h, 20);
    check("h8 checksum", h[19], (uint8_t)(0x40 + 0x7F));

    // FlySky: XOR over bytes 0..14 at byte 15
    uint8_t f[16];
    memset(f, 0, sizeof(f));
    f[0] = 0xAA;
    f[1] = 0xE8; f[2] = 0x03; // channel 1 = 1000
    f[15] = rc_proto_checksum(5, f, 16);
    uint8_t fxor = 0xAA ^ 0xE8 ^ 0x03;
    check("flysky checksum", f[15], fxor);

    // JJRC: additive over bytes 1..13 at byte 14 (byte 0 = hop idx, excluded)
    uint8_t j[15];
    memset(j, 0, sizeof(j));
    j[0] = 0x02; // hop index, not summed
    j[3] = 0x64; // partial throttle
    j[14] = rc_proto_checksum(6, j, 15);
    check("jjrc checksum", j[14], 0x64); // only byte 3 is non-zero in 1..13

    // WLToys: additive over bytes 0..6
    uint8_t w[8];
    memset(w, 0, sizeof(w));
    w[0] = 0xDD;
    w[1] = 0x80; // mid throttle
    w[7] = rc_proto_checksum(7, w, 8);
    check("wltoys checksum", w[7], (uint8_t)(0xDD + 0x80));
}

// ---------------------------------------------------------------------------
// Stick round trips
// ---------------------------------------------------------------------------

// Build a frame with known sticks, decode it, check the sticks survived
static void round_trip(rc_proto_t proto, uint8_t throttle, int8_t yaw, int8_t pitch, int8_t roll)
{
    uint8_t p[32];
    memset(p, 0, sizeof(p));

    // Set header-ish bytes so the frames look like real ones
    if (proto == 0) // BAYANG
    {
        p[0] = 0xA5;
        p[1] = 0xFA;
        p[13] = 0x0A;
    }
    else if (proto == 5) // FLYSKY: byte 0 must be 0xAA for data frames
    {
        p[0] = 0xAA;
    }
    else if (proto == 7) // WLTOYS: byte 0 must be 0xDD for data frames
    {
        p[0] = 0xDD;
    }

    rc_sticks_t in = {.throttle = throttle, .yaw = yaw, .pitch = pitch, .roll = roll};
    check_true("build", rc_build(proto, p, rc_proto_payload_len(proto), &in));

    rc_sticks_t out;
    check_true("decode", rc_decode(proto, p, rc_proto_payload_len(proto), &out));

    // The tolerance: one stick unit of the byte scalers
    int dt = (int)out.throttle - (int)throttle;
    if (dt < 0)
        dt = -dt;
    check("throttle within 1", dt <= 4, 1);
    int dy = (int)out.yaw - (int)yaw;
    if (dy < 0)
        dy = -dy;
    check("yaw within 2", dy <= 6, 1);
    int dp = (int)out.pitch - (int)pitch;
    if (dp < 0)
        dp = -dp;
    check("pitch within 2", dp <= 6, 1);
    int dr = (int)out.roll - (int)roll;
    if (dr < 0)
        dr = -dr;
    check("roll within 2", dr <= 6, 1);
}

static void test_sticks(void)
{
    printf("rc stick round trips\n");

    // Each protocol, at centre, at the corners and at a mixed position
    for (rc_proto_t p = 0; p < rc_proto_count(); p++)
    {
        round_trip(p, 128, 0, 0, 0);
        round_trip(p, 255, 100, -100, 100);
        round_trip(p, 0, -100, 100, -100);
        round_trip(p, 64, 33, -47, 80);
    }
}

// A wrong checksum must reject the decode
static void test_reject(void)
{
    printf("rc rejection\n");

    uint8_t p[16];
    memset(p, 0, sizeof(p));
    rc_sticks_t st = {.throttle = 100};
    rc_build(2, p, 16, &st);
    p[3] ^= 0x40; // corrupt a stick byte without fixing the checksum
    rc_sticks_t out;
    check_true("corrupt frame rejected", !rc_decode(2, p, 16, &out));

    // A wrong length too
    check_true("wrong length rejected", !rc_decode(2, p, 15, &out));
}

// ---------------------------------------------------------------------------
// Bind packets
// ---------------------------------------------------------------------------

static void test_bind(void)
{
    printf("rc bind\n");

    // Bayang bind: 0xA4, the data address, the hop list, the id
    uint8_t p[15];
    memset(p, 0, sizeof(p));
    rc_bind_t bind;
    bind.data_addr[0] = 0x12;
    bind.data_addr[1] = 0x34;
    bind.data_addr[2] = 0x56;
    bind.data_addr[3] = 0x78;
    bind.data_addr[4] = 0xED;
    bind.chan_mhz[0] = 0x00;
    bind.chan_mhz[1] = 0x25;
    bind.chan_mhz[2] = 0x45;
    bind.chan_mhz[3] = 0x65;
    bind.txid[0] = 0xAA;
    bind.txid[1] = 0xBB;
    bind.txid[2] = 0xCC;

    check_true("bind build", rc_bind_build(0, p, 15, &bind));
    check("bind header", p[0], 0xA4);
    check("bind data addr byte", p[1], 0x12);
    check("bind hop channel", p[7], 0x25);
    check("bind txid", p[10], 0xAA);

    // The built packet must decode back
    rc_bind_t back;
    check_true("bind decode", rc_bind_decode(0, p, 15, &back));
    check("bind round trip addr", back.data_addr[0], 0x12);
    check("bind round trip hop", back.chan_mhz[2], 0x45);
    check("bind round trip txid", back.txid[1], 0xBB);
}

// ---------------------------------------------------------------------------
// Generic tracker
// ---------------------------------------------------------------------------

static void test_track(void)
{
    printf("rc generic tracker\n");

    rc_track_t t;
    rc_track_start(&t, 10);

    // Header bytes stay fixed, stick bytes move
    for (uint8_t i = 0; i < 20; i++)
    {
        uint8_t p[10] = {0xA5, 0xFA, 0, 0, 0, 0, 0, 0, 0, 0};
        p[4] = (uint8_t)(i * 12);      // moving: throttle-ish
        p[6] = (uint8_t)(128 + i * 6); // moving: yaw-ish, no wrap
        rc_track_feed(&t, p, 10);
    }

    uint32_t live = rc_track_live(&t);
    check_true("header byte still", !(live & 0x000F));
    check_true("byte 4 is live", live & (1u << 4));
    check_true("byte 6 is live", live & (1u << 6));
    check("byte 5 not live", (live >> 5) & 1, 0);

    check("centre of byte 6 is between min and max", t.max[6] >= rc_track_centre(&t, 6) &&
                                                          rc_track_centre(&t, 6) >= t.min[6], 1);
    check("packets counted", t.seen, 20);
}

int main(void)
{
    test_table();
    test_checksums();
    test_sticks();
    test_reject();
    test_bind();
    test_track();

    if (failures)
    {
        printf("rc_proto: %d failures\n", failures);
        return 1;
    }
    printf("rc_proto: all checks passed\n");
    return 0;
}
