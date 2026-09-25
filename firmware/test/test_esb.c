// Host side test of the ESB frame model and the sniffer bookkeeping.
// Everything here is the real code; the frames are built and decoded byte
// by bit, and the captures are fed to the same bookkeeping the radio part
// uses on target.
#include <stdio.h>
#include <string.h>

#include "esb_frame.h"
#include "esb_sniff.h"

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
// CRC
// ---------------------------------------------------------------------------

static void test_crc(void)
{
    printf("esb crc16\n");

    // CRC-16/CCITT-FALSE over "123456789": the serial form over the 72 bits
    // (each byte most significant bit first) must give the check value
    const uint8_t digits[] = "123456789";
    check("crc16 over 123456789", esb_crc16_serial(digits, 72), 0x29B1);

    // An empty bit stream is just the init value
    check("crc16 of nothing", esb_crc16_serial(digits, 0), 0xFFFF);
}

// ---------------------------------------------------------------------------
// Frame round trips
// ---------------------------------------------------------------------------

static void fill_frame(esb_frame_t *f, uint8_t addr_len, uint8_t plen, bool payload_lsb,
                       uint8_t crc_model, uint8_t seed)
{
    memset(f, 0, sizeof(*f));
    f->addr[0] = 0x55; // the byte an 0xAA55 match consumes
    for (uint8_t i = 1; i < addr_len; i++)
        f->addr[i] = (uint8_t)(0x11 * i + seed);
    f->addr_len = addr_len;
    f->plen = plen;
    f->pid = seed & 3;
    f->noack = (seed & 4) != 0;
    for (uint8_t i = 0; i < plen; i++)
        f->payload[i] = (uint8_t)(seed + i * 7);
    f->payload_lsb = payload_lsb;
    f->crc_model = crc_model;
}

static void roundtrip(uint8_t addr_len, uint8_t plen, bool payload_lsb, uint8_t crc_model,
                      uint8_t seed)
{
    char what[80];

    esb_frame_t f;
    fill_frame(&f, addr_len, plen, payload_lsb, crc_model, seed);

    uint8_t raw[ESB_CAPTURE_MAX];
    uint8_t raw_len = esb_frame_build(raw, sizeof(raw), &f);
    snprintf(what, sizeof(what), "build L=%u plen=%u fits", addr_len, plen);
    check_true(what, raw_len == esb_frame_raw_len(&f));
    check_true(what, raw_len != 0);

    esb_frame_t g;
    bool ok = esb_frame_decode(raw, raw_len, f.addr[0], payload_lsb, &g);
    snprintf(what, sizeof(what), "decode L=%u plen=%u lsb=%u crc=%u", addr_len, plen, payload_lsb,
             crc_model);
    check_true(what, ok);

    if (!ok)
        return;

    snprintf(what, sizeof(what), "address length L=%u", addr_len);
    check(what, g.addr_len, addr_len);
    snprintf(what, sizeof(what), "plen L=%u", addr_len);
    check(what, g.plen, plen);
    snprintf(what, sizeof(what), "pid L=%u", addr_len);
    check(what, g.pid, f.pid);
    snprintf(what, sizeof(what), "noack L=%u", addr_len);
    check(what, g.noack, f.noack);
    for (uint8_t i = 0; i < addr_len; i++)
    {
        snprintf(what, sizeof(what), "addr[%u] L=%u", i, addr_len);
        check(what, g.addr[i], f.addr[i]);
    }
    for (uint8_t i = 0; i < plen; i++)
    {
        snprintf(what, sizeof(what), "payload[%u] L=%u plen=%u", i, addr_len, plen);
        check(what, g.payload[i], f.payload[i]);
    }
    check("crc model survives", g.crc_model, crc_model);
}

static void test_roundtrips(void)
{
    printf("\nesb frame round trips\n");

    for (uint8_t l = 2; l <= 5; l++)
    {
        roundtrip(l, 0, false, 0, 1);
        roundtrip(l, 1, false, 0, 2);
        roundtrip(l, 8, false, 0, 3);
        roundtrip(l, 32, false, 0, 4);
    }

    // The other bit order and CRC engine candidates
    roundtrip(5, 16, true, 1, 5);
    roundtrip(5, 16, false, 2, 6);
    roundtrip(4, 7, true, 3, 7);
    roundtrip(5, 20, true, 2, 8);
}

static void test_rejects(void)
{
    printf("\nesb frame rejects\n");

    esb_frame_t f;
    fill_frame(&f, 5, 16, false, 0, 9);

    uint8_t raw[ESB_CAPTURE_MAX];
    uint8_t raw_len = esb_frame_build(raw, sizeof(raw), &f);
    check_true("built", raw_len != 0);

    esb_frame_t g;
    check_true("clean capture decodes", esb_frame_decode(raw, raw_len, 0x55, false, &g));

    // Flip one payload bit: the CRC must fail everywhere
    raw[5] ^= 0x40;
    check_true("flipped bit rejected", !esb_frame_decode(raw, raw_len, 0x55, false, &g));

    // Back, flip the last CRC bit (the last on-air bit of the frame)
    raw[5] ^= 0x40;
    raw[raw_len - 1] ^= 0x80;
    check_true("flipped crc rejected", !esb_frame_decode(raw, raw_len, 0x55, false, &g));

    // A length beyond 32 cannot be a frame
    raw[4] = 0xFF;
    check_true("insane length rejected", !esb_frame_decode(raw, raw_len, 0x55, false, &g));

    // Too short to hold PCF and CRC
    check_true("too short rejected", !esb_frame_decode(raw, 2, 0x55, false, &g));
}

// ---------------------------------------------------------------------------
// Sniffer bookkeeping
// ---------------------------------------------------------------------------

static uint8_t work[sizeof(esb_sniff_work_t) + 4] __attribute__((aligned(4)));

// Builds a frame and feeds its capture, as the radio part would
static void feed_frame(uint8_t addr_len, uint8_t plen, uint8_t seed, uint8_t rate, uint16_t mhz,
                       uint8_t rssi, uint32_t now)
{
    esb_frame_t f;
    fill_frame(&f, addr_len, plen, false, 0, seed);
    f.addr[1] = seed; // devices differ in the second byte

    uint8_t raw[ESB_CAPTURE_MAX];
    uint8_t raw_len = esb_frame_build(raw, sizeof(raw), &f);
    esb_sniff_feed(raw, raw_len, f.addr[0], rate, mhz, rssi, now, false);
}

static void test_bookkeeping(void)
{
    printf("\nesb sniffer bookkeeping\n");

    check_true("init fits", esb_sniff_init(work, sizeof(work)));
    esb_sniff_reset();

    check("nothing decoded yet", esb_sniff_decoded(), 0);
    check("no devices yet", esb_sniff_dev_count(), 0);

    feed_frame(5, 8, 0x20, 2, 2447, 60, 1000);
    check("one packet decoded", esb_sniff_decoded(), 1);
    check("one device", esb_sniff_dev_count(), 1);
    check("device packets", esb_sniff_dev(0)->packets, 1);
    check("device rate", esb_sniff_dev(0)->rate, 2);
    check("device mhz", esb_sniff_dev(0)->mhz, 2447);
    check("device rssi", esb_sniff_dev(0)->rssi, 60);
    check("device addr len", esb_sniff_dev(0)->addr_len, 5);
    check("device first byte", esb_sniff_dev(0)->addr[0], 0x55);
    check("device second byte", esb_sniff_dev(0)->addr[1], 0x20);

    // Same device again: aggregates, does not add a row
    feed_frame(5, 8, 0x20, 2, 2447, 55, 2000);
    check("same device aggregates", esb_sniff_dev_count(), 1);
    check("packets counted", esb_sniff_dev(0)->packets, 2);
    check("strongest rssi kept", esb_sniff_dev(0)->rssi, 55);

    // A different device
    feed_frame(5, 4, 0x21, 1, 2450, 70, 3000);
    check("second device", esb_sniff_dev_count(), 2);

    // A corrupt capture does not count
    uint8_t junk[ESB_CAPTURE_MAX];
    memset(junk, 0xA5, sizeof(junk));
    junk[0] = 0x12; // not constant, but not a frame either
    esb_sniff_feed(junk, sizeof(junk), 0x55, 2, 2440, 80, 4000, false);
    check("junk not decoded", esb_sniff_decoded(), 3);

    // A constant pattern is dropped before the CRC
    memset(junk, 0x77, sizeof(junk));
    esb_sniff_feed(junk, sizeof(junk), 0x55, 2, 2440, 80, 5000, false);
    check("constant pattern not a lock", esb_sniff_locks(), 4);

    // Ring: 3 decoded packets so far, fill it past the limit
    for (uint32_t i = 0; i < 20; i++)
        feed_frame(5, 8, (uint8_t)(0x20 + (i & 1)), 2, 2447, 60, 6000 + i * 100);
    check("ring capped", esb_sniff_pkt_count(), ESB_SNIFF_RING);
    check("device count capped", esb_sniff_dev_count(), 2);

    // Newest first, and it is the last fed
    const esb_pkt_t *newest = esb_sniff_pkt(0);
    check_true("newest exists", newest != 0);
    check("newest timestamp", newest->ms, 6000 + 19 * 100);

    // Sorted: the device with more packets first
    uint8_t idx[ESB_SNIFF_MAX_DEV];
    uint8_t n = esb_sniff_dev_sorted(idx, ESB_SNIFF_MAX_DEV);
    check("sorted count", n, 2);
    check("busiest first", esb_sniff_dev(idx[0])->packets >= esb_sniff_dev(idx[1])->packets, 1);

    // The mailbox
    esb_pkt_t box;
    check_true("no capture yet", !esb_sniff_capture_get(&box));
    esb_sniff_capture_set(newest);
    check_true("capture stored", esb_sniff_capture_get(&box));
    check("capture payload survives", box.f.plen, newest->f.plen);
    check("capture raw survives", box.raw_len, newest->raw_len);

    // The mailbox survives a reset (it is outside the arena)
    esb_sniff_reset();
    check("arena reset clears the ring", esb_sniff_pkt_count(), 0);
    check_true("capture survives the reset", esb_sniff_capture_get(&box));
    esb_sniff_capture_clear();
    check_true("capture cleared", !esb_sniff_capture_get(&box));
}

// The raw of a decoded capture must be exactly what a replay sends: build
// and decode must agree on the bit layout
static void test_replay_bytes(void)
{
    printf("\nesb replay raw\n");

    esb_frame_t f;
    fill_frame(&f, 5, 12, false, 0, 0x31);
    uint8_t raw[ESB_CAPTURE_MAX];
    uint8_t raw_len = esb_frame_build(raw, sizeof(raw), &f);
    check_true("built", raw_len != 0);

    // Feed it, then read the packet back: the raw the mailbox keeps must be
    // byte identical, so the transmitter resends the exact capture
    esb_sniff_init(work, sizeof(work));
    esb_sniff_feed(raw, raw_len, f.addr[0], 2, 2440, 60, 1000, false);
    const esb_pkt_t *p = esb_sniff_pkt(0);
    check("raw kept byte identical", p->raw_len, raw_len);
    check("raw bytes identical", memcmp(p->raw, raw, raw_len), 0);
}

int main(void)
{
    test_crc();
    test_roundtrips();
    test_rejects();
    test_bookkeeping();
    test_replay_bytes();

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
