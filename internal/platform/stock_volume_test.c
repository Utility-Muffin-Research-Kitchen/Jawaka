#include "internal/platform/stock_volume.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static const char *const KEYS[] = {"SPEAKER", "HEADSET", "HDMI", "BLUETOOTH", "USB"};
#define KEY_COUNT ((int)(sizeof(KEYS) / sizeof(KEYS[0])))

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

int main(void) {
    /* Every stock level survives a trip through a percent and back. */
    for (int level = 0; level <= JW_STOCK_VOLUME_LEVELS; level++) {
        assert(jw_stock_volume_level(jw_stock_volume_percent(level)) == level);
    }
    assert(jw_stock_volume_level(100) == 14);
    assert(jw_stock_volume_level(50) == 7);
    assert(jw_stock_volume_level(0) == 0);
    assert(jw_stock_volume_level(150) == 14);
    assert(jw_stock_volume_level(-5) == 0);
    assert(jw_stock_volume_percent(7) == 50);
    assert(jw_stock_volume_percent(20) == 100);

    /* Fresh from stock: no USB output, so the levels are stock's own. */
    cJSON *root = parse("{\"devices\":{\"SPEAKER\":7,\"HEADSET\":14}}");
    assert(jw_stock_volume_get(root, "SPEAKER", false) == 50);
    assert(jw_stock_volume_get(root, "HEADSET", false) == 100);
    assert(jw_stock_volume_get(root, "USB", false) == 100);

    /* Writing one output puts the rest on 0-14 without misreading them, even
       though the write adds the USB output that marks levels Leaf saved. */
    assert(jw_stock_volume_put(root, KEYS, KEY_COUNT, "HEADSET", 60, false) == 0);
    assert(level_of(root, "SPEAKER") == 7);
    assert(level_of(root, "HEADSET") == 8);
    assert(level_of(root, "USB") == 14);
    assert(jw_stock_volume_get(root, "SPEAKER", true) == 50);
    assert(jw_stock_volume_get(root, "HEADSET", true) == 60);
    cJSON_Delete(root);

    /* Saved by Leaf before the tag (this MLP1's row): 0-10 levels with USB. */
    root = parse("{\"bgm\":0,\"devices\":{\"BLUETOOTH\":6,\"HDMI\":10,\"HEADSET\":8,"
                 "\"SPEAKER\":10,\"USB\":10},\"sysVolume\":3}");
    assert(jw_stock_volume_get(root, "SPEAKER", false) == 100);
    assert(jw_stock_volume_get(root, "HEADSET", false) == 80);
    assert(jw_stock_volume_get(root, "BLUETOOTH", false) == 60);

    /* The first write keeps every output where the user had it. */
    assert(jw_stock_volume_put(root, KEYS, KEY_COUNT, "SPEAKER", 95, false) == 0);
    assert(level_of(root, "SPEAKER") == 13);
    assert(level_of(root, "HEADSET") == 11);
    assert(level_of(root, "BLUETOOTH") == 8);
    assert(level_of(root, "HDMI") == 14);
    assert(jw_stock_volume_get(root, "SPEAKER", true) == 95);
    assert(jw_stock_volume_get(root, "HEADSET", true) == 80);
    assert(jw_stock_volume_get(root, "BLUETOOTH", true) == 60);
    assert(jw_stock_volume_get(root, "HDMI", true) == 100);
    /* Even untagged (the tag write failed), the exact percents read first. */
    assert(jw_stock_volume_get(root, "HEADSET", false) == 80);

    /* A level 0-14 between the exact percents' own levels is still exact: the
       5% steps that fall between stock levels survive a reload. */
    assert(jw_stock_volume_put(root, KEYS, KEY_COUNT, "SPEAKER", 55, true) == 0);
    assert(level_of(root, "SPEAKER") == 8);
    assert(jw_stock_volume_get(root, "SPEAKER", true) == 55);

    /* Older Leaf (another card, or a downgrade) keeps our percents but writes
       its own 0-10 level: the mismatch reads it as 0-10. */
    set_level(root, "SPEAKER", 3);
    assert(jw_stock_volume_get(root, "SPEAKER", true) == 30);
    assert(jw_stock_volume_get(root, "HEADSET", true) == 80);

    /* A stock session drops the percents: tagged levels are stock's own, even
       though stock kept the USB output. */
    cJSON_DeleteItemFromObjectCaseSensitive(root, JW_STOCK_VOLUME_LEAF_KEY);
    set_level(root, "SPEAKER", 12);
    assert(jw_stock_volume_get(root, "SPEAKER", true) == 86);
    assert(jw_stock_volume_get(root, "HEADSET", true) == 79);
    cJSON_Delete(root);

    /* A broken percents object is replaced, not trusted. */
    root = parse("{\"devices\":{\"SPEAKER\":7},\"umrkVolumes\":5}");
    assert(jw_stock_volume_get(root, "SPEAKER", true) == 50);
    assert(jw_stock_volume_put(root, KEYS, KEY_COUNT, "SPEAKER", 70, true) == 0);
    assert(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(root, JW_STOCK_VOLUME_LEAF_KEY)));
    assert(jw_stock_volume_get(root, "SPEAKER", true) == 70);

    /* Bad calls change nothing. */
    assert(jw_stock_volume_put(root, KEYS, KEY_COUNT, "SPEAKERS", 70, true) == -1);
    assert(jw_stock_volume_put(root, KEYS, 0, "SPEAKER", 10, true) == -1);
    assert(level_of(root, "SPEAKER") == 10);
    assert(jw_stock_volume_get(root, "SPEAKER", true) == 70);
    cJSON_Delete(root);
    root = parse("{\"devices\":3}");
    assert(jw_stock_volume_put(root, KEYS, KEY_COUNT, "SPEAKER", 70, true) == -1);
    assert(jw_stock_volume_get(root, "SPEAKER", true) == 100);
    cJSON_Delete(root);

    puts("stock volume tests passed");
    return 0;
}
