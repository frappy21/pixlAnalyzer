// Host side test of the NDEF builders: URI prefix detection, text
// records, clipping and truncation.
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "nfc_ndef.h"

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

static void test_uri(void)
{
    printf("ndef uri\n");

    uint8_t buf[NDEF_MAX];

    // A https://www URL: prefix 0x02 and the host without the scheme
    uint8_t n = ndef_uri_message(buf, sizeof(buf), "https://www.example.com/page");
    check("https www length", n, 4 + 1 + (uint8_t)strlen("example.com/page"));
    check("header SR well-known", buf[0], 0xD1);
    check("type U", buf[3], 'U');
    check("prefix code https www", buf[4], 0x02);
    check_true("the host is kept",
               memcmp(&buf[5], "example.com/page", 16) == 0);

    // A bare https URL: prefix 0x04
    n = ndef_uri_message(buf, sizeof(buf), "https://pixl.example");
    check("prefix code https", buf[4], 0x04);
    check_true("host without scheme", memcmp(&buf[5], "pixl.example", 12) == 0);

    // http://www.
    n = ndef_uri_message(buf, sizeof(buf), "http://www.example.org");
    check("prefix code http www", buf[4], 0x01);

    // No scheme at all: as-is
    n = ndef_uri_message(buf, sizeof(buf), "example.com");
    check("prefix code none", buf[4], 0x00);
    check_true("uri kept verbatim", memcmp(&buf[5], "example.com", 11) == 0);
}

static void test_text(void)
{
    printf("ndef text\n");

    uint8_t buf[NDEF_MAX];

    uint8_t n = ndef_text_message(buf, sizeof(buf), "HELLO LAB");
    check("text length", n, 4 + 3 + 9);
    check("type T", buf[3], 'T');
    check("status: lang len 2", buf[4], 0x02);
    check("language en", buf[5], 'e');
    check("language n", buf[6], 'n');
    check_true("the text follows", memcmp(&buf[7], "HELLO LAB", 9) == 0);
}

static void test_limits(void)
{
    printf("ndef limits\n");

    uint8_t buf[NDEF_MAX];

    // Too long: refuses rather than truncating (the caller clips the text)
    char long_uri[80];
    memset(long_uri, 'a', sizeof(long_uri) - 1);
    long_uri[sizeof(long_uri) - 1] = 0;
    check("overlong uri refused", ndef_uri_message(buf, sizeof(buf), long_uri), 0);
    check("overlong text refused", ndef_text_message(buf, sizeof(buf), long_uri), 0);

    // A tiny buffer
    check("tiny buffer refused", ndef_uri_message(buf, 3, "a"), 0);
}

int main(void)
{
    test_uri();
    test_text();
    test_limits();

    if (failures)
    {
        printf("ndef: %d failures\n", failures);
        return 1;
    }
    printf("ndef: all checks passed\n");
    return 0;
}
