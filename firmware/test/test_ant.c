#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "ant_rx.h"

static int failures = 0;

static void check(const char *what, long got, long want)
{
    if (got != want)
    {
        printf("  FAIL %-46s got %ld want %ld\n", what, got, want);
        failures++;
    }
    else
    {
        printf("  ok   %-46s\n", what);
    }
}

// Build a valid ANT broadcast buffer: [type][ch][d0..d7][checksum].
// The correlator already consumed [0xA4][0x08], so those two are folded
// into the XOR but not stored in the buffer.
static void make_frame(uint8_t *buf, uint8_t ch, const uint8_t *data)
{
    buf[0] = 0x4E; // type: broadcast
    buf[1] = ch;
    memcpy(buf + 2, data, 8);
    uint8_t xor = 0xA4 ^ 0x08; // sync + len bytes consumed by correlator
    for (int i = 0; i < 10; i++)
        xor ^= buf[i];
    buf[10] = xor; // checksum makes the whole thing XOR to zero
}

static void test_checksum(void)
{
    printf("checksum:\n");

    uint8_t buf[11];
    uint8_t data[8] = {0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 75, 0x00};
    make_frame(buf, 1, data);
    check("valid frame passes", ant_frame_valid(buf), 1);

    buf[10] ^= 0xFF; // corrupt checksum
    check("corrupted frame fails", ant_frame_valid(buf), 0);

    buf[10] ^= 0xFF; // restore
    buf[5] ^= 0x01;  // corrupt a data byte
    check("corrupted data fails", ant_frame_valid(buf), 0);
}

static void test_hr_decode(void)
{
    printf("HR decode:\n");

    static uint8_t arena[512];
    ant_rx_init(arena, sizeof(arena));

    // HR page 0x04, data[6] = bpm
    uint8_t data[8] = {0x04, 0x01, 0x02, 0x03, 0x04, 0x05, 142, 0x00};
    uint8_t buf[11];
    make_frame(buf, 3, data);

    // Inject via the internal decode path by calling ant_frame_valid then
    // decode_frame. Since ANT_RX_HOST_TEST disables the radio, we call
    // through the exported API by calling ant_rx_init + ant_rx_count +
    // ant_rx_device. We need to expose decode for testing, so we replicate
    // its logic here using only the public API.

    // Instead: call the internal decode via a thin shim.
    // ant_rx.c compiles with ANT_RX_HOST_TEST so radio functions are absent.
    // We call the test-only helper defined at the bottom of this file.
    extern void ant_test_inject(const uint8_t *buf, uint8_t rssi, uint32_t ms);

    ant_test_inject(buf, 80, 1000);

    check("count == 1", ant_rx_count(), 1);
    const ant_dev_t *d = ant_rx_device(0);
    check("device != NULL", d != NULL, 1);
    if (d)
    {
        check("channel == 3", d->channel, 3);
        check("page == 0x04", d->page, 0x04);
        check("profile == HR", d->profile, ANT_PROF_HR);
        check("hr_bpm == 142", d->hr_bpm, 142);
        check("rssi == 80", d->rssi, 80);
        check("frames == 1", (long)d->frames, 1L);
    }

    // Second frame: same channel, different bpm
    data[6] = 155;
    make_frame(buf, 3, data);
    ant_test_inject(buf, 78, 2000);

    check("count still 1", ant_rx_count(), 1);
    d = ant_rx_device(0);
    if (d)
    {
        check("hr_bpm updated", d->hr_bpm, 155);
        check("frames == 2", (long)d->frames, 2L);
        check("rssi is minimum (78)", d->rssi, 78);
    }
}

static void test_power_decode(void)
{
    printf("power decode:\n");

    static uint8_t arena[512];
    ant_rx_init(arena, sizeof(arena));

    extern void ant_test_inject(const uint8_t *buf, uint8_t rssi, uint32_t ms);

    // Power main page 0x10, data[6..7] = watts little-endian
    uint16_t watts = 285;
    uint8_t data[8] = {0x10, 0, 0, 0, 0, 0, (uint8_t)(watts & 0xFF), (uint8_t)(watts >> 8)};
    uint8_t buf[11];
    make_frame(buf, 7, data);
    ant_test_inject(buf, 85, 1000);

    const ant_dev_t *d = ant_rx_device(0);
    check("power profile detected", d ? d->profile : -1, ANT_PROF_POWER);
    check("power watts == 285", d ? d->power_w : -1, 285);
}

static void test_multi_channel(void)
{
    printf("multi channel:\n");

    static uint8_t arena[512];
    ant_rx_init(arena, sizeof(arena));

    extern void ant_test_inject(const uint8_t *buf, uint8_t rssi, uint32_t ms);

    uint8_t buf[11];
    for (uint8_t ch = 0; ch < 8; ch++)
    {
        uint8_t data[8] = {0x04, 0, 0, 0, 0, 0, (uint8_t)(60 + ch), 0};
        make_frame(buf, ch, data);
        ant_test_inject(buf, 70, (uint32_t)(ch * 100));
    }

    check("count == 8", ant_rx_count(), 8);
    check("device 0 channel 0", ant_rx_device(0) ? ant_rx_device(0)->channel : 255, 0);
    check("device 7 channel 7", ant_rx_device(7) ? ant_rx_device(7)->channel : 255, 7);
}

int main(void)
{
    test_checksum();
    test_hr_decode();
    test_power_decode();
    test_multi_channel();

    if (failures)
        printf("FAILED: %d\n", failures);
    else
        printf("ALL PASSED\n");
    return failures ? 1 : 0;
}
