// Host side test of the settings page: v1 records written by the previous
// firmware must load into the current layout without losing a field, and
// records of both versions must be able to share the page.
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "settings.h"

static int failures = 0;

static void check(const char *what, long got, long want)
{
    if (got != want)
    {
        printf("  FAIL %-52s got %ld want %ld\n", what, got, want);
        failures++;
    }
    else
    {
        printf("  ok   %-52s\n", what);
    }
}

// The v1 layout exactly as the previous firmware wrote it
typedef struct
{
    uint8_t contrast, backlight, band, dwell, wf_decim, wf_mode, auto_floor, led_hunt;
    uint8_t sleep_min, dim_s, log_consent, shuffle;
    uint16_t bat_cal;
    uint16_t reserved;
} v1_data_t;

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    v1_data_t data;
    uint32_t crc;
} v1_record_t;

static v1_data_t user_v1(void)
{
    v1_data_t d = {
        .contrast = 17,
        .backlight = 90,
        .band = BAND_EXTENDED,
        .dwell = 64,
        .wf_decim = 7,
        .wf_mode = WF_THRESHOLD,
        .auto_floor = 0,
        .led_hunt = 0,
        .sleep_min = 12,
        .dim_s = 60,
        .log_consent = 1,
        .shuffle = 0,
        .bat_cal = 1050,
        .reserved = 0,
    };
    return d;
}

static uint32_t plant_v1(uint32_t off, const v1_data_t *d)
{
    v1_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic = SETTINGS_MAGIC;
    rec.version = 1;
    rec.size = sizeof(v1_data_t);
    rec.data = *d;
    rec.crc = settings_crc32(&rec.data, sizeof(rec.data));
    memcpy(settings_host_page() + off, &rec, sizeof(rec));
    return off + sizeof(rec);
}

static void erase(void)
{
    memset(settings_host_page(), 0xFF, FLASH_INT_PAGE_SIZE_HOST);
}

static void check_user_fields(const char *when)
{
    char what[80];
    v1_data_t want = user_v1();

#define FIELD(f)                                                                                   \
    snprintf(what, sizeof(what), "%s: " #f " kept", when);                                         \
    check(what, g_settings.f, want.f)

    FIELD(contrast);
    FIELD(backlight);
    FIELD(band);
    FIELD(dwell);
    FIELD(wf_decim);
    FIELD(wf_mode);
    FIELD(auto_floor);
    FIELD(led_hunt);
    FIELD(sleep_min);
    FIELD(dim_s);
    FIELD(log_consent);
    FIELD(shuffle);
    FIELD(bat_cal);
#undef FIELD
}

int main(void)
{
    printf("settings layout\n");
    check("v1 layout is 16 bytes", sizeof(v1_data_t), SETTINGS_V1_SIZE);
    check("v1 record walks as 28 bytes", sizeof(v1_record_t), 28);
    check("v1 fields are a prefix of the current layout",
          offsetof(settings_data_t, reserved) + 2, SETTINGS_V1_SIZE);

    printf("\nempty page\n");
    erase();
    settings_load();
    check("erased page loads defaults", g_settings.contrast, 32);
    check("no record used", settings_loaded_version(), 0);
    check("defaults are not dirty", settings_dirty(), 0);

    printf("\nv1 -> v%d migration\n", SETTINGS_VERSION);
    erase();
    v1_data_t old = user_v1();
    uint32_t end = plant_v1(0, &old);
    settings_load();
    check("v1 record used", settings_loaded_version(), 1);
    check_user_fields("migrated");
    check("new field invert takes its default", g_settings.invert, 0);
    check("new field burnin takes its default", g_settings.burnin, 1);
    check("new field saver_min takes its default", g_settings.saver_min, 0);
    check("new field sentry_db takes its default", g_settings.sentry_db, 20);
    check("new field sentry_period takes its default", g_settings.sentry_period, 5);
    check("a migrated record leaves the settings dirty", settings_dirty(), 1);

    printf("\nv2 appended after v1\n");
    g_settings.invert = 1;
    g_settings.sentry_db = 25;
    check("save succeeds", settings_save(), 1);
    uint32_t magic;
    memcpy(&magic, settings_host_page(), 4);
    check("v1 record left in place", magic, SETTINGS_MAGIC);
    memcpy(&magic, settings_host_page() + end, 4);
    check("v2 record appended right after it", magic, SETTINGS_MAGIC);
    g_settings.contrast = 0;
    g_settings.invert = 0;
    settings_load();
    check("newest record is the v2 one", settings_loaded_version(), SETTINGS_VERSION);
    check_user_fields("after resave");
    check("new field survives the round trip", g_settings.invert, 1);
    check("sentry threshold survives the round trip", g_settings.sentry_db, 25);
    check("a current record is not dirty", settings_dirty(), 0);

    printf("\ncorrupted newest record\n");
    // Flip a data byte of the v2 record: the CRC fails and the v1 one wins
    settings_host_page()[end + 8] ^= 0x01;
    settings_load();
    check("falls back to the older v1 record", settings_loaded_version(), 1);
    check_user_fields("fallback");

    printf("\ntorn write at the tail\n");
    erase();
    end = plant_v1(0, &old);
    uint32_t torn = SETTINGS_MAGIC; // only the first word made it to flash
    memcpy(settings_host_page() + end, &torn, 4);
    settings_load();
    check("torn record ignored, v1 still loads", settings_loaded_version(), 1);
    check("contrast kept despite the torn tail", g_settings.contrast, 17);
    check("save after a torn tail succeeds", settings_save(), 1);
    g_settings.contrast = 0;
    settings_load();
    check("page rewritten with the current version", settings_loaded_version(),
          SETTINGS_VERSION);
    check("contrast kept through the rewrite", g_settings.contrast, 17);

    printf("\nfull page\n");
    erase();
    plant_v1(0, &old);
    settings_load();
    int saves = 0;
    for (int i = 0; i < 300; i++)
    {
        g_settings.backlight = (uint8_t)i;
        if (settings_save())
            saves++;
    }
    check("300 saves across page erases succeed", saves, 300);
    g_settings.backlight = 0;
    settings_load();
    check("last save wins after wrapping", g_settings.backlight, (uint8_t)299);
    check("user contrast survives the wrap", g_settings.contrast, 17);

    printf("\nout of range values\n");
    erase();
    old.band = 99;
    old.contrast = 200;
    plant_v1(0, &old);
    settings_load();
    check("bad band falls back to ISM", g_settings.band, BAND_ISM);
    check("bad contrast falls back to default", g_settings.contrast, 32);
    check("good dwell kept", g_settings.dwell, 64);

    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
