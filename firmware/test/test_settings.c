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
    check("v3 sniff_rate takes its default", g_settings.sniff_rate, 0);
    check("v3 sniff_bits takes its default", g_settings.sniff_bits, 0);
    check("v5 sp_rbw takes its default", g_settings.sp_rbw, 1);
    check("v5 ble_spam takes its default", g_settings.ble_spam, 1);
    check("a migrated record leaves the settings dirty", settings_dirty(), 1);

    printf("\ncurrent version appended after v1\n");
    g_settings.invert = 1;
    g_settings.sentry_db = 25;
    g_settings.sniff_rate = 2;
    g_settings.sniff_bits = 1;
    check("save succeeds", settings_save(), 1);
    uint32_t magic;
    memcpy(&magic, settings_host_page(), 4);
    check("v1 record left in place", magic, SETTINGS_MAGIC);
    memcpy(&magic, settings_host_page() + end, 4);
    check("current record appended right after it", magic, SETTINGS_MAGIC);
    g_settings.contrast = 0;
    g_settings.invert = 0;
    settings_load();
    check("newest record is the current one", settings_loaded_version(), SETTINGS_VERSION);
    check_user_fields("after resave");
    check("invert survives the round trip", g_settings.invert, 1);
    check("sniff_rate survives the round trip", g_settings.sniff_rate, 2);
    check("sniff_bits survives the round trip", g_settings.sniff_bits, 1);
    check("sentry threshold survives the round trip", g_settings.sentry_db, 25);
    check("a current record is not dirty", settings_dirty(), 0);

    printf("\ncorrupted newest record\n");
    // Flip a data byte of the current record: the CRC fails and the v1 one wins
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

    printf("\nv4 -> v%d migration\n", SETTINGS_VERSION);
    check("v4 fields are a prefix of the current layout",
          offsetof(settings_data_t, sp_trace), SETTINGS_V4_SIZE);
    erase();
    settings_defaults();
    settings_data_t v4snap = g_settings;
    // Copy the v1 user fields into v4snap and tweak some v4 fields
    memcpy(&v4snap, &old, sizeof(old));
    v4snap.band = BAND_ISM;
    v4snap.contrast = 17;
    v4snap.invert = 1;
    v4snap.sentry_db = 30;
    v4snap.sniff_rate = 1;
    v4snap.intro_done = 1;
    v4snap.home_screen = 2;
    {
        uint8_t rec[8 + SETTINGS_V4_SIZE + 4];
        uint32_t m4 = SETTINGS_MAGIC;
        uint16_t ver4 = 4, sz4 = SETTINGS_V4_SIZE;
        memcpy(rec, &m4, 4);
        memcpy(rec + 4, &ver4, 2);
        memcpy(rec + 6, &sz4, 2);
        memcpy(rec + 8, &v4snap, SETTINGS_V4_SIZE);
        uint32_t crc4 = settings_crc32(rec + 8, SETTINGS_V4_SIZE);
        memcpy(rec + 8 + SETTINGS_V4_SIZE, &crc4, 4);
        memcpy(settings_host_page(), rec, sizeof(rec));
        end = sizeof(rec);
    }
    settings_load();
    check("v4 record used", settings_loaded_version(), 4);
    check("v4 invert kept", g_settings.invert, 1);
    check("v4 sentry_db kept", g_settings.sentry_db, 30);
    check("v4 sniff_rate kept", g_settings.sniff_rate, 1);
    check("v4 intro_done kept", g_settings.intro_done, 1);
    check("v4 home_screen kept", g_settings.home_screen, 2);
    check("v5 sp_adaptive takes its default", g_settings.sp_adaptive, 1);
    check("v5 ble_follow takes its default", g_settings.ble_follow, 1);
    check("v5 zb_lock takes its default", g_settings.zb_lock, 0);
    check("a migrated v4 record leaves the settings dirty", settings_dirty(), 1);
    g_settings.sp_cal = -6;
    g_settings.sp_alarm_db = 20;
    g_settings.zb_lock = 15;
    check("v5 save after v4 succeeds", settings_save(), 1);
    g_settings.sp_cal = 0;
    g_settings.zb_lock = 0;
    settings_load();
    check("newest record is v5", settings_loaded_version(), SETTINGS_VERSION);
    check("negative cal offset survives", g_settings.sp_cal, -6);
    check("alarm threshold survives", g_settings.sp_alarm_db, 20);
    check("zigbee lock survives", g_settings.zb_lock, 15);
    check("v4 invert still there", g_settings.invert, 1);

    printf("\nv5 out of range values\n");
    g_settings.sp_cal = 50;
    g_settings.sp_rbw = 7;
    g_settings.zb_lock = 5;
    settings_save();
    settings_load();
    check("bad cal offset falls back to 0", g_settings.sp_cal, 0);
    check("bad rbw falls back to 1", g_settings.sp_rbw, 1);
    check("bad zigbee channel falls back to hop", g_settings.zb_lock, 0);

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
