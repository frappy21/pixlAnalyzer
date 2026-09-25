// Host side test of the Unifying payload model: the verified MouseJack
// example frames, the checksum law, and the builders.
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "unifying.h"

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

static void test_verified(void)
{
    printf("unifying: verified captures\n");

    // The keep-alive from the MouseJack captures: 00 40 00 55 6B
    const uint8_t keep[5] = {0x00, 0x40, 0x00, 0x55, 0x6B};
    unify_view_t v;
    check_true("keep-alive decodes", unify_decode(keep, 5, &v));
    check("keep-alive kind", v.kind, UNIFY_KEEPALIVE);
    check("keep-alive timeout", v.timeout, 0x55);

    // The 'a' key down from the DEF CON slides: 00 C1 00 04 00 00 00 00 00 3B
    const uint8_t adown[10] = {0x00, 0xC1, 0x00, 0x04, 0, 0, 0, 0, 0, 0x3B};
    check_true("key down decodes", unify_decode(adown, 10, &v));
    check("key kind", v.kind, UNIFY_KEY);
    check("key code", v.keys[0], 0x04);

    // The 'a' key up: 00 C1 00 00 00 00 00 00 00 3F
    const uint8_t aup[10] = {0x00, 0xC1, 0x00, 0, 0, 0, 0, 0, 0, 0x3F};
    check_true("key up decodes", unify_decode(aup, 10, &v));

    // The mouse click frame: 00 C2 00 00 00 00 00 00 00 3E
    const uint8_t click[10] = {0x00, 0xC2, 0x00, 0, 0, 0, 0, 0, 0, 0x3E};
    check_true("mouse click decodes", unify_decode(click, 10, &v));
    check("mouse kind", v.kind, UNIFY_MOUSE);
    check("mouse dx", v.dx, 0);

    // The encrypted 22 byte frame
    const uint8_t enc[22] = {0x00, 0xD3, 0xEA, 0x98, 0xB7, 0x30, 0xEE, 0x49, 0x59, 0x97,
                             0x9C, 0xC2, 0xAC, 0xDA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                             0x00, 0xB9};
    check_true("encrypted decodes", unify_decode(enc, 22, &v));
    check("encrypted kind", v.kind, UNIFY_ENCRYPTED);
}

static void test_checksum(void)
{
    printf("unifying: checksum\n");

    // The law: last byte = 0 - sum(everything before it)
    const uint8_t frame[10] = {0x07, 0xC1, 0x00, 0x05, 0, 0, 0, 0, 0, 0x33};
    check("checksum of the frame", unify_checksum(frame, 10), 0x33);

    // A flipped bit must fail the decode
    uint8_t bad[10];
    memcpy(bad, frame, 10);
    bad[3] ^= 1;
    unify_view_t v;
    check_true("flipped bit rejected", !unify_decode(bad, 10, &v));

    // A non-Unifying payload
    const uint8_t junk[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    check_true("junk rejected", !unify_decode(junk, 10, &v));
}

static void test_build(void)
{
    printf("unifying: builders\n");

    uint8_t out[22];

    // A keystroke that decodes back with the right key
    uint8_t n = unify_build_keystroke(out, sizeof(out), 0x07, 0x00, 0x04);
    check("keystroke length", n, 10);
    unify_view_t v;
    check_true("built keystroke decodes", unify_decode(out, n, &v));
    check("built key code", v.keys[0], 0x04);
    check("built device", v.device, 0x07);

    // A release
    n = unify_build_release(out, sizeof(out), 0x07);
    check("release length", n, 10);
    check_true("release decodes", unify_decode(out, n, &v));
    check("release has no keys", v.keys[0], 0);

    // A keep-alive
    n = unify_build_keepalive(out, sizeof(out), 0x08);
    check("keep-alive length", n, 5);
    check_true("keep-alive decodes", unify_decode(out, n, &v));
    check("keep-alive kind", v.kind, UNIFY_KEEPALIVE);

    // Too small a buffer
    check("small buffer refused", unify_build_keystroke(out, 9, 0, 0, 0), 0);
}

static void test_names(void)
{
    printf("unifying: names\n");

    check_true("A is named", strcmp(unify_key_name(0x04), "A") == 0);
    check_true("RET is named", strcmp(unify_key_name(0x28), "RET") == 0);
    check_true("unknown code has no name", unify_key_name(0xE0) == 0);
}

int main(void)
{
    test_verified();
    test_checksum();
    test_build();
    test_names();

    if (failures)
    {
        printf("unifying: %d failures\n", failures);
        return 1;
    }
    printf("unifying: all checks passed\n");
    return 0;
}
