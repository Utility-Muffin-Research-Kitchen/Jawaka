#include "internal/platform/stock_volume.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const KEYS[] = {"SPEAKER", "HEADSET", "HDMI", "BLUETOOTH", "USB"};
#define KEY_COUNT ((int)(sizeof(KEYS) / sizeof(KEYS[0])))

/* LoongOS 1.3 (old names only), before and after Leaf's first 0-14 write. */
static const jw_stock_volume_scale OLD = {false, false, false};
static const jw_stock_volume_scale OLD_TAGGED = {true, false, false};
/* LoongOS 1.4 (AUDIO_ names on 0-16), before and after Leaf writes them. */
static const jw_stock_volume_scale NEW = {true, true, false};
static const jw_stock_volume_scale NEW_TAGGED = {true, true, true};

static cJSON *parse(const char *json) {
    cJSON *root = cJSON_Parse(json);
    assert(root);
    return root;
}

static int level_of(const cJSON *root, const char *key) {
    const cJSON *devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
    const cJSON *level = cJSON_GetObjectItemCaseSensitive(devices, key);
    assert(cJSON_IsNumber(level));
    return level->valueint;
}

static void set_level(cJSON *root, const char *key, int level) {
    cJSON *devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
    assert(cJSON_ReplaceItemInObjectCaseSensitive(devices, key, cJSON_CreateNumber(level)));
}

static bool has_level(const cJSON *root, const char *key) {
    const cJSON *devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
    return cJSON_GetObjectItemCaseSensitive(devices, key) != NULL;
}

/* A file holding bytes at offset, zero-filled before it. */
static void write_service(const char *path, long offset, const char *bytes, size_t len) {
    FILE *fp = fopen(path, "wb");
    assert(fp);
    for (long i = 0; i < offset; i++) {
        assert(fputc(i % 7 == 0 ? 'x' : '\0', fp) != EOF);
    }
    assert(fwrite(bytes, 1, len, fp) == len);
    assert(fclose(fp) == 0);
}

static void test_service_scan(void) {
    const char *tmp = getenv("TMPDIR");
    char path[1024];
    snprintf(path, sizeof(path), "%s/stock-volume-test-%ld", tmp && tmp[0] ? tmp : "/tmp",
             (long)getpid());

    /* LoongOS 1.3 has only the longer string. */
    static const char old[] = "\0LOONG_AUDIO_SPEAKER\0SPEAKER\0";
    write_service(path, 100, old, sizeof(old));
    assert(jw_stock_volume_service_has_audio_keys(path) == 0);

    /* LoongOS 1.4's own string, across the reader's 64 KiB chunks. */
    static const char new14[] = "\0AUDIO_SPEAKER\0AUDIO_HEADSET\0";
    write_service(path, 65536 - 6, new14, sizeof(new14));
    assert(jw_stock_volume_service_has_audio_keys(path) == 1);

    /* A prefix only, at the very end. */
    static const char cut[] = "\0AUDIO_SPEAKE";
    write_service(path, 10, cut, sizeof(cut) - 1);
    assert(jw_stock_volume_service_has_audio_keys(path) == 0);

    assert(unlink(path) == 0);
    assert(jw_stock_volume_service_has_audio_keys(path) == -1);
    assert(jw_stock_volume_service_has_audio_keys(NULL) == -1);
}

static void test_loongos_14(void) {
    for (int level = 0; level <= JW_STOCK_VOLUME_AUDIO_LEVELS; level++) {
        assert(jw_stock_volume_level(jw_stock_volume_percent(level, 16), 16) == level);
    }
    assert(jw_stock_volume_level(100, 16) == 16);
    assert(jw_stock_volume_level(50, 16) == 8);
    assert(jw_stock_volume_percent(8, 16) == 50);
    assert(jw_stock_volume_percent(15, 16) == 94);

    assert(strcmp(jw_stock_volume_audio_key("SPEAKER"), "AUDIO_SPEAKER") == 0);
    assert(strcmp(jw_stock_volume_audio_key("BLUETOOTH"), "AUDIO_BLUETOOTH") == 0);
    assert(jw_stock_volume_audio_key("USB") == NULL);
    assert(jw_stock_volume_audio_key(NULL) == NULL);

    /* The 1.4.0.52 unit's row: stock saved once after the OTA (AUDIO_SPEAKER is
       its 0 for a missing level), then Leaf without AUDIO_ support wrote. */
    cJSON *root = parse(
        "{\"bgm\":0,\"devices\":{\"AUDIO_SPEAKER\":0,\"BLUETOOTH\":8,\"HDMI\":14,"
        "\"HEADSET\":10,\"SPEAKER\":7,\"USB\":14},\"sysVolume\":0,\"toneVolume\":10,"
        "\"umrkVolumes\":{\"SPEAKER\":49,\"HEADSET\":71,\"HDMI\":100,"
        "\"BLUETOOTH\":57,\"USB\":100}}");
    assert(jw_stock_volume_has_audio_keys(root));
    assert(jw_stock_volume_get(root, "SPEAKER", &NEW) == 49);
    assert(jw_stock_volume_get(root, "HEADSET", &NEW) == 71);
    assert(jw_stock_volume_get(root, "BLUETOOTH", &NEW) == 57);
    /* Even if the AUDIO_ tag were set, Leaf's exact percents say it saved last. */
    assert(jw_stock_volume_get(root, "SPEAKER", &NEW_TAGGED) == 49);

    /* Stock saving again drops the percents. The old names still hold the
       levels from before; stock's 0 does not mute Leaf. */
    cJSON_DeleteItemFromObjectCaseSensitive(root, JW_STOCK_VOLUME_LEAF_KEY);
    assert(jw_stock_volume_get(root, "SPEAKER", &NEW) == 50);
    assert(jw_stock_volume_get(root, "HEADSET", &NEW) == 71);

    /* Leaf's first write on 1.4: every stock output gets an AUDIO_ level on
       0-16 beside its old 0-14 one, so stock's 100% is Leaf's. */
    assert(jw_stock_volume_put(root, KEYS, KEY_COUNT, "SPEAKER", 100, &NEW) == 0);
    assert(level_of(root, "AUDIO_SPEAKER") == 16);
    assert(level_of(root, "SPEAKER") == 14);
    assert(level_of(root, "AUDIO_HEADSET") == 11);
    assert(level_of(root, "HEADSET") == 10);
    assert(level_of(root, "AUDIO_HDMI") == 16);
    assert(level_of(root, "AUDIO_BLUETOOTH") == 9);
    assert(level_of(root, "USB") == 14);
    assert(!has_level(root, "AUDIO_USB"));
    assert(jw_stock_volume_get(root, "SPEAKER", &NEW_TAGGED) == 100);
    assert(jw_stock_volume_get(root, "HEADSET", &NEW_TAGGED) == 71);

    /* A stock session after that: the AUDIO_ levels are stock's own, and its
       top two levels read below 100%. */
    cJSON_DeleteItemFromObjectCaseSensitive(root, JW_STOCK_VOLUME_LEAF_KEY);
    set_level(root, "AUDIO_SPEAKER", 15);
    assert(jw_stock_volume_get(root, "SPEAKER", &NEW_TAGGED) == 94);
    assert(jw_stock_volume_get(root, "HEADSET", &NEW_TAGGED) == 69);
    assert(jw_stock_volume_get(root, "USB", &NEW_TAGGED) == 100);

    /* Older Leaf (another card) writes only the old names, with its percents:
       those read first over the stale AUDIO_ level. */
    set_level(root, "SPEAKER", 5);
    cJSON *leaf = cJSON_AddObjectToObject(root, JW_STOCK_VOLUME_LEAF_KEY);
    assert(leaf && cJSON_AddNumberToObject(leaf, "SPEAKER", 36));
    assert(jw_stock_volume_get(root, "SPEAKER", &NEW_TAGGED) == 36);
    cJSON_Delete(root);

    /* A device that never ran 1.3 has only the AUDIO_ names. */
    root = parse("{\"devices\":{\"AUDIO_SPEAKER\":8,\"AUDIO_HEADSET\":16}}");
    assert(jw_stock_volume_get(root, "SPEAKER", &NEW) == 50);
    assert(jw_stock_volume_get(root, "HEADSET", &NEW) == 100);
    assert(jw_stock_volume_get(root, "HDMI", &NEW) == 100);
    assert(jw_stock_volume_put(root, KEYS, KEY_COUNT, "HEADSET", 25, &NEW) == 0);
    assert(level_of(root, "AUDIO_SPEAKER") == 8);
    assert(level_of(root, "SPEAKER") == 7);
    assert(level_of(root, "AUDIO_HEADSET") == 4);
    assert(jw_stock_volume_get(root, "SPEAKER", &NEW_TAGGED) == 50);
    assert(jw_stock_volume_get(root, "HEADSET", &NEW_TAGGED) == 25);
    cJSON_Delete(root);

    /* On 1.3 the AUDIO_ names are neither read nor written. */
    root = parse("{\"devices\":{\"SPEAKER\":7}}");
    assert(!jw_stock_volume_has_audio_keys(root));
    cJSON_Delete(root);
    root = parse("{\"devices\":{\"SPEAKER\":7,\"AUDIO_SPEAKER\":16}}");
    assert(jw_stock_volume_get(root, "SPEAKER", &OLD_TAGGED) == 50);
    assert(jw_stock_volume_put(root, KEYS, KEY_COUNT, "SPEAKER", 30, &OLD_TAGGED) == 0);
    assert(level_of(root, "SPEAKER") == 4);
    assert(level_of(root, "AUDIO_SPEAKER") == 16);
    assert(!has_level(root, "AUDIO_HEADSET"));
    cJSON_Delete(root);
}

int main(void) {
    /* Every stock level survives a trip through a percent and back. */
    for (int level = 0; level <= JW_STOCK_VOLUME_LEVELS; level++) {
        assert(jw_stock_volume_level(jw_stock_volume_percent(level, 14), 14) == level);
    }
    assert(jw_stock_volume_level(100, 14) == 14);
    assert(jw_stock_volume_level(50, 14) == 7);
    assert(jw_stock_volume_level(0, 14) == 0);
    assert(jw_stock_volume_level(150, 14) == 14);
    assert(jw_stock_volume_level(-5, 14) == 0);
    assert(jw_stock_volume_percent(7, 14) == 50);
    assert(jw_stock_volume_percent(20, 14) == 100);

    /* Fresh from stock: no USB output, so the levels are stock's own. */
    cJSON *root = parse("{\"devices\":{\"SPEAKER\":7,\"HEADSET\":14}}");
    assert(jw_stock_volume_get(root, "SPEAKER", &OLD) == 50);
    assert(jw_stock_volume_get(root, "HEADSET", &OLD) == 100);
    assert(jw_stock_volume_get(root, "USB", &OLD) == 100);

    /* Writing one output puts the rest on 0-14 without misreading them, even
       though the write adds the USB output that marks levels Leaf saved. */
    assert(jw_stock_volume_put(root, KEYS, KEY_COUNT, "HEADSET", 60, &OLD) == 0);
    assert(level_of(root, "SPEAKER") == 7);
    assert(level_of(root, "HEADSET") == 8);
    assert(level_of(root, "USB") == 14);
    assert(jw_stock_volume_get(root, "SPEAKER", &OLD_TAGGED) == 50);
    assert(jw_stock_volume_get(root, "HEADSET", &OLD_TAGGED) == 60);
    cJSON_Delete(root);

    /* Saved by Leaf before the tag (this MLP1's row): 0-10 levels with USB. */
    root = parse("{\"bgm\":0,\"devices\":{\"BLUETOOTH\":6,\"HDMI\":10,\"HEADSET\":8,"
                 "\"SPEAKER\":10,\"USB\":10},\"sysVolume\":3}");
    assert(jw_stock_volume_get(root, "SPEAKER", &OLD) == 100);
    assert(jw_stock_volume_get(root, "HEADSET", &OLD) == 80);
    assert(jw_stock_volume_get(root, "BLUETOOTH", &OLD) == 60);

    /* The first write keeps every output where the user had it. */
    assert(jw_stock_volume_put(root, KEYS, KEY_COUNT, "SPEAKER", 95, &OLD) == 0);
    assert(level_of(root, "SPEAKER") == 13);
    assert(level_of(root, "HEADSET") == 11);
    assert(level_of(root, "BLUETOOTH") == 8);
    assert(level_of(root, "HDMI") == 14);
    assert(jw_stock_volume_get(root, "SPEAKER", &OLD_TAGGED) == 95);
    assert(jw_stock_volume_get(root, "HEADSET", &OLD_TAGGED) == 80);
    assert(jw_stock_volume_get(root, "BLUETOOTH", &OLD_TAGGED) == 60);
    assert(jw_stock_volume_get(root, "HDMI", &OLD_TAGGED) == 100);
    /* Even untagged (the tag write failed), the exact percents read first. */
    assert(jw_stock_volume_get(root, "HEADSET", &OLD) == 80);

    /* A level 0-14 between the exact percents' own levels is still exact: the
       5% steps that fall between stock levels survive a reload. */
    assert(jw_stock_volume_put(root, KEYS, KEY_COUNT, "SPEAKER", 55, &OLD_TAGGED) == 0);
    assert(level_of(root, "SPEAKER") == 8);
    assert(jw_stock_volume_get(root, "SPEAKER", &OLD_TAGGED) == 55);

    /* Older Leaf (another card, or a downgrade) keeps our percents but writes
       its own 0-10 level: the mismatch reads it as 0-10. */
    set_level(root, "SPEAKER", 3);
    assert(jw_stock_volume_get(root, "SPEAKER", &OLD_TAGGED) == 30);
    assert(jw_stock_volume_get(root, "HEADSET", &OLD_TAGGED) == 80);

    /* A stock session drops the percents: tagged levels are stock's own, even
       though stock kept the USB output. */
    cJSON_DeleteItemFromObjectCaseSensitive(root, JW_STOCK_VOLUME_LEAF_KEY);
    set_level(root, "SPEAKER", 12);
    assert(jw_stock_volume_get(root, "SPEAKER", &OLD_TAGGED) == 86);
    assert(jw_stock_volume_get(root, "HEADSET", &OLD_TAGGED) == 79);
    cJSON_Delete(root);

    /* A broken percents object is replaced, not trusted. */
    root = parse("{\"devices\":{\"SPEAKER\":7},\"umrkVolumes\":5}");
    assert(jw_stock_volume_get(root, "SPEAKER", &OLD_TAGGED) == 50);
    assert(jw_stock_volume_put(root, KEYS, KEY_COUNT, "SPEAKER", 70, &OLD_TAGGED) == 0);
    assert(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(root, JW_STOCK_VOLUME_LEAF_KEY)));
    assert(jw_stock_volume_get(root, "SPEAKER", &OLD_TAGGED) == 70);

    /* Bad calls change nothing. */
    assert(jw_stock_volume_put(root, KEYS, KEY_COUNT, "SPEAKERS", 70, &OLD_TAGGED) == -1);
    assert(jw_stock_volume_put(root, KEYS, 0, "SPEAKER", 10, &OLD_TAGGED) == -1);
    assert(level_of(root, "SPEAKER") == 10);
    assert(jw_stock_volume_get(root, "SPEAKER", &OLD_TAGGED) == 70);
    cJSON_Delete(root);
    root = parse("{\"devices\":3}");
    assert(jw_stock_volume_put(root, KEYS, KEY_COUNT, "SPEAKER", 70, &OLD_TAGGED) == -1);
    assert(jw_stock_volume_get(root, "SPEAKER", &OLD_TAGGED) == 100);
    cJSON_Delete(root);

    test_loongos_14();
    test_service_scan();

    puts("stock volume tests passed");
    return 0;
}
