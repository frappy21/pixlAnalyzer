// Host side test of the 802.15.4 receiver: frames are built byte by byte,
// spread to chips with the chip table as printed in the standard, turned into
// the MSK bit stream a BLE 2M receiver would see, cut after the sync address
// like the radio does, and handed to the real decoder and MAC parser.
#include <stdio.h>
#include <string.h>

#include "zb_rx.h"

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

// IEEE 802.15.4-2006 table 73: all 16 symbol-to-chip mappings, c0 first.
// The firmware only carries row 0 and derives the rest.
static const char *const k_table73[16] = {
    "11011001110000110101001000101110", "11101101100111000011010100100010",
    "00101110110110011100001101010010", "00100010111011011001110000110101",
    "01010010001011101101100111000011", "00110101001000101110110110011100",
    "11000011010100100010111011011001", "10011100001101010010001011101101",
    "10001100100101100000011101111011", "10111000110010010110000001110111",
    "01111011100011001001011000000111", "01110111101110001100100101100000",
    "00000111011110111000110010010110", "01100000011101111011100011001001",
    "10010110000001110111101110001100", "11001001011000000111011110111000",
};

// ---------------------------------------------------------------------------
// Transmitter model: symbols -> chips -> MSK bits, one bit per byte
// ---------------------------------------------------------------------------

static uint8_t g_bits[16384];
static int g_nbits;
static int g_prev_chip;

static void tx_reset(void)
{
    g_nbits = 0;
    g_prev_chip = 0;
}

static void tx_symbol(uint8_t sym)
{
    const char *c = k_table73[sym & 15];
    for (int n = 0; n < 32; n++)
    {
        int chip = c[n] - '0';
        g_bits[g_nbits++] = (uint8_t)(chip ^ g_prev_chip ^ (n & 1));
        g_prev_chip = chip;
    }
}

static void tx_byte(uint8_t b)
{
    tx_symbol(b & 15);
    tx_symbol(b >> 4);
}

static uint32_t g_lcg = 12345;
static uint8_t rnd_bit(void)
{
    g_lcg = g_lcg * 1103515245u + 12345u;
    return (g_lcg >> 16) & 1u;
}

// Preamble, SFD, PHR, PSDU (FCS appended here), then noise
static void tx_frame(const uint8_t *mpdu, uint8_t n, bool add_fcs)
{
    uint8_t psdu[128];
    memcpy(psdu, mpdu, n);
    if (add_fcs)
    {
        uint16_t fcs = zb_crc16(psdu, n);
        psdu[n++] = (uint8_t)fcs;
        psdu[n++] = (uint8_t)(fcs >> 8);
    }

    tx_reset();
    for (int i = 0; i < 4; i++)
        tx_byte(0x00);
    tx_byte(0xA7);
    tx_byte(n);
    for (int i = 0; i < n; i++)
        tx_byte(psdu[i]);
    for (int i = 0; i < 2048; i++)
        g_bits[g_nbits++] = rnd_bit();
}

// Flip MSK bit `bit` of the symbol `sym` counted from the first PSDU symbol
static void flip(int psdu_sym, int bit)
{
    int first = (8 + 2 + 2) * 32; // preamble, SFD, PHR
    g_bits[first + psdu_sym * 32 + bit] ^= 1;
}

// Receiver model: find the sync address like the correlator does, then pack
// the next 255 bytes LSB first (PCNF1.ENDIAN little)
static uint8_t g_air[ZB_AIR_LEN];

static int rx_capture(uint32_t address, int min_start)
{
    for (int p = min_start; p + 32 + ZB_AIR_LEN * 8 <= g_nbits; p++)
    {
        uint32_t w = 0;
        for (int i = 0; i < 32; i++)
            w |= (uint32_t)g_bits[p + i] << i;
        if (w != address)
            continue;

        memset(g_air, 0, sizeof(g_air));
        for (int i = 0; i < ZB_AIR_LEN * 8; i++)
            g_air[i / 8] |= (uint8_t)(g_bits[p + 32 + i] << (i % 8));
        return p;
    }
    return -1;
}

static zb_work_t g_work;
static uint32_t g_table[16];

static void run(const char *name, zb_rx_result_t *res)
{
    int p = rx_capture(zb_sync_address(g_table), 0);
    char what[64];
    snprintf(what, sizeof(what), "%s: sync found at the SFD", name);
    check(what, p, 8 * 32 + 16); // second half of symbol 7, after 8 preamble symbols
    zb_rx_feed(g_air, ZB_AIR_LEN, 15, 58, 1000, res);
}

int main(void)
{
    printf("test_zb_rx\n");

    // --- Derivation of the MSK table against the whole of table 73 ---------
    zb_msk_table(g_table);
    int table_ok = 1;
    for (int s = 0; s < 16; s++)
    {
        uint32_t want = 0;
        int prev = 0;
        for (int n = 0; n < 32; n++)
        {
            int chip = k_table73[s][n] - '0';
            want |= (uint32_t)(chip ^ prev ^ (n & 1)) << n;
            prev = chip;
        }
        if (want != g_table[s])
            table_ok = 0;
    }
    check("MSK table derived from row 0 matches table 73", table_ok, 1);

    // Published WazaBee value for symbol 0, bits 1..31
    const char *wazabee0 = "1100000011101111010111001101100";
    int wb_ok = 1;
    for (int n = 1; n < 32; n++)
        wb_ok &= (int)((g_table[0] >> n) & 1u) == wazabee0[n - 1] - '0';
    check("symbol 0 matches the WazaBee MSK sequence", wb_ok, 1);

    // Minimum distance between patterns over the 31 compared bits
    int dmin = 99;
    for (int a = 0; a < 16; a++)
        for (int b = a + 1; b < 16; b++)
        {
            uint32_t x = (g_table[a] ^ g_table[b]) & 0xFFFFFFFEu;
            int d = 0;
            while (x)
            {
                d += x & 1u;
                x >>= 1;
            }
            if (d < dmin)
                dmin = d;
        }
    check("minimum pattern distance", dmin, 13);

    // Every symbol survives any 6 flipped bits (random sample)
    int robust = 1;
    for (int trial = 0; trial < 4000; trial++)
    {
        uint8_t s = (uint8_t)(trial & 15);
        uint32_t w = g_table[s];
        uint32_t mask = 0;
        while (__builtin_popcount(mask) < 6)
        {
            int b = 0;
            for (int i = 0; i < 5; i++)
                b |= rnd_bit() << i;
            if (b)
                mask |= 1u << b;
        }
        uint8_t d;
        if (zb_symbol_decode(g_table, w ^ mask, &d) != s || d != 6)
            robust = 0;
    }
    check("6 flipped chips per symbol always decode", robust, 1);

    // --- CRC ------------------------------------------------------------------
    check("CRC-16/KERMIT check value", zb_crc16((const uint8_t *)"123456789", 9), 0x2189);

    check("init in the work buffer", zb_rx_init(&g_work, sizeof(g_work)), 1);
    check("init refuses a short buffer", zb_rx_init(&g_work, sizeof(g_work) - 1), 0);
    zb_rx_init(&g_work, sizeof(g_work));

    zb_rx_result_t res;

    // --- Zigbee PRO beacon, permit join on ----------------------------------
    static const uint8_t beacon[] = {
        0x00, 0x80,             // FC: beacon, src short, v2003
        0x42,                   // seq
        0x62, 0x1A,             // src PAN
        0x00, 0x00,             // src addr: coordinator
        0xFF, 0xCF,             // superframe: BO/SO 15, PAN coord, assoc permit
        0x00,                   // GTS
        0x00,                   // pending addresses
        0x00, 0x22, 0x84,       // Zigbee, profile 2 v2, router + end device capacity
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, // extended PAN ID
        0xFF, 0xFF, 0xFF, 0x00, // tx offset, update id
    };
    tx_frame(beacon, sizeof(beacon), true);
    run("beacon", &res);
    check("beacon: FCS good", res.status, ZB_RX_GOOD);
    check("beacon: length", res.len, 28);
    check("beacon: parsed", res.parsed, 1);
    check("beacon: type", res.mac.type, ZB_FT_BEACON);
    check("beacon: seq", res.mac.seq, 0x42);
    check("beacon: src PAN", res.mac.src_pan, 0x1A62);
    check("beacon: src mode short", res.mac.src_mode, ZB_AM_SHORT);
    check("beacon: no dst", res.mac.dst_mode, ZB_AM_NONE);
    check("beacon: permit join", res.mac.assoc_permit, 1);
    check("beacon: PAN coordinator", res.mac.pan_coord, 1);
    check("beacon: Zigbee protocol id", res.mac.proto_id, 0);
    check("beacon: stack profile PRO", res.mac.stack_profile, 2);
    check("beacon: ext PAN", res.mac.ext_pan[7], 0x88);
    check("beacon: no symbol errors", res.max_dist, 0);

    // --- Secured data frame, PAN ID compression, 3 flipped chips in 8 symbols
    static const uint8_t data_sec[] = {
        0x69, 0xD8,                                     // FC: data, sec, ack req, comp, dst short, v2006, src ext
        0x07,                                           // seq
        0x62, 0x1A,                                     // dst PAN
        0x00, 0x00,                                     // dst: coordinator
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, // src ext
        0x05, 0x01, 0x00, 0x00, 0x00,                   // aux security header
        0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11,             // encrypted, never parsed
    };
    tx_frame(data_sec, sizeof(data_sec), true);
    for (int s = 0; s < 8; s++)
        for (int b = 0; b < 3; b++)
            flip(s * 5 + 1, (s * 7 + b * 11) % 32);
    run("secured data", &res);
    check("secured data: FCS good despite chip errors", res.status, ZB_RX_GOOD);
    check("secured data: worst distance", res.max_dist, 3);
    check("secured data: type", res.mac.type, ZB_FT_DATA);
    check("secured data: security bit", res.mac.security, 1);
    check("secured data: PAN compression", res.mac.pan_comp, 1);
    check("secured data: no src PAN field", res.mac.has_src_pan, 0);
    check("secured data: PAN", res.mac.pan, 0x1A62);
    check("secured data: src ext", res.mac.src_addr[7], 0x08);
    check("secured data: header length", res.mac.hdr_len, 15);

    // --- Same frame with one symbol destroyed: FCS catches it ----------------
    tx_frame(data_sec, sizeof(data_sec), true);
    for (int b = 1; b < 17; b++)
        flip(20, b);
    run("damaged", &res);
    check("damaged: FCS bad", res.status, ZB_RX_BAD);

    // --- ACK -----------------------------------------------------------------
    static const uint8_t ack[] = {0x02, 0x00, 0x07};
    tx_frame(ack, sizeof(ack), true);
    run("ack", &res);
    check("ack: FCS good", res.status, ZB_RX_GOOD);
    check("ack: type", res.mac.type, ZB_FT_ACK);
    check("ack: no PAN", res.mac.has_pan, 0);

    // --- Long frame: truncated, header still trusted --------------------------
    uint8_t longf[98];
    memset(longf, 0x5A, sizeof(longf));
    longf[0] = 0x41; // data, PAN comp
    longf[1] = 0x88; // dst short, v2003, src short
    longf[2] = 0x99;
    longf[3] = 0xEF; // PAN 0xBEEF
    longf[4] = 0xBE;
    longf[5] = 0x34; // dst 0x1234
    longf[6] = 0x12;
    longf[7] = 0x78; // src 0x5678
    longf[8] = 0x56;
    tx_frame(longf, sizeof(longf), true);
    run("long", &res);
    check("long: truncated", res.status, ZB_RX_TRUNC);
    check("long: length", res.len, 100);
    check("long: bytes captured", res.got, 30);
    check("long: airtime left", res.rest_us, 32 * 70);
    check("long: PAN", res.mac.pan, 0xBEEF);

    // --- The sync address against the preamble ----------------------------------
    // Every window that starts inside the preamble must be clearly different
    // from the address, or a bit error would lock the receiver off by some bits
    tx_frame(ack, sizeof(ack), true);
    uint32_t address = zb_sync_address(g_table);
    int closest = 32;
    for (int p = 0; p < 8 * 32; p++)
    {
        int d = 0;
        for (int i = 0; i < 32; i++)
            d += g_bits[p + i] != ((address >> i) & 1u);
        if (d < closest)
            closest = d;
    }
    check("address vs preamble windows: min distance", closest, 7);

    // Symbol 7 alone would have been a poor address: 1 bit from the preamble
    closest = 32;
    for (int p = 0; p < 7 * 32; p++)
    {
        int d = 0;
        for (int i = 0; i < 32; i++)
            d += g_bits[p + i] != ((g_bits[8 * 32 + i]) & 1u);
        if (d < closest)
            closest = d;
    }
    check("symbol 7 vs preamble windows: min distance", closest, 1);

    // --- Noise -----------------------------------------------------------------
    for (int i = 0; i < ZB_AIR_LEN; i++)
    {
        uint8_t b = 0;
        for (int j = 0; j < 8; j++)
            b |= (uint8_t)(rnd_bit() << j);
        g_air[i] = b;
    }
    zb_rx_feed(g_air, ZB_AIR_LEN, 15, 58, 1000, &res);
    check("noise: no sync", res.status, ZB_RX_NOSYNC);

    // --- 802.15.4-2015 PAN ID presence ------------------------------------------
    static const uint8_t v2[] = {
        0x01, 0xEC, 0x10,                               // data, dst ext, v2015, src ext, comp 0
        0xCD, 0xAB,                                     // dst PAN
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
    };
    zb_mac_t m;
    check("2015 frame parses", zb_mac_parse(v2, sizeof(v2), false, &m), 1);
    check("2015 ext/ext: dst PAN present", m.has_dst_pan, 1);
    check("2015 ext/ext: src PAN absent", m.has_src_pan, 0);
    check("2015 ext/ext: PAN", m.pan, 0xABCD);

    // --- Bookkeeping -------------------------------------------------------------
    const zb_totals_t *t = zb_rx_totals();
    check("totals: good", (long)t->good, 3);
    check("totals: bad", (long)t->bad, 1);
    check("totals: truncated", (long)t->trunc, 1);
    check("totals: nosync", (long)t->nosync, 1);
    check("totals: secured", (long)t->secured, 1);
    check("channel 15 good", zb_rx_chan(15)->good, 3);
    check("PANs seen", zb_rx_pan_count(), 2);

    uint8_t idx[ZB_MAX_PANS];
    check("sorted count", zb_rx_sorted(idx, ZB_MAX_PANS), 2);
    const zb_pan_t *p = zb_rx_pan(idx[0]);
    check("PAN 1A62 first", p->pan, 0x1A62);
    check("PAN 1A62 channel", p->ch, 15);
    check("PAN 1A62 frames", p->frames, 2);
    check("PAN 1A62 beacons", p->beacons, 1);
    check("PAN 1A62 secured", p->secured, 1);
    check("PAN 1A62 flags", p->flags,
          ZB_PAN_BEACON | ZB_PAN_PJ | ZB_PAN_ZIGBEE | ZB_PAN_SECURED | ZB_PAN_EXT_PAN |
              ZB_PAN_COORD);
    check("PAN 1A62 short addresses", p->n_short, 1);
    check("PAN 1A62 ext addresses", p->n_ext, 1);
    p = zb_rx_pan(idx[1]);
    check("PAN BEEF addresses", p->n_short, 2);

    if (failures)
    {
        printf("test_zb_rx: %d FAILED\n", failures);
        return 1;
    }
    printf("test_zb_rx: all passed\n");
    return 0;
}
