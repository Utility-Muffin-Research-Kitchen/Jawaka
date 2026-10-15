#include "internal/platform/stock_volume.h"

#include <string.h>

/* One per audio output; the MLP1 has five. */
#define JW__STOCK_VOLUME_MAX_KEYS 8

static int jw__stock_volume_clamp(int value, int lo, int hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

int jw_stock_volume_level(int percent) {
    percent = jw__stock_volume_clamp(percent, 0, 100);
    return (percent * JW_STOCK_VOLUME_LEVELS + 50) / 100;
}

int jw_stock_volume_percent(int level) {
    level = jw__stock_volume_clamp(level, 0, JW_STOCK_VOLUME_LEVELS);
    return (level * 100 + JW_STOCK_VOLUME_LEVELS / 2) / JW_STOCK_VOLUME_LEVELS;
}

static int jw__stock_volume_legacy_percent(int level) {
    level = jw__stock_volume_clamp(level, 0, JW_STOCK_VOLUME_LEGACY_LEVELS);
    return level * 100 / JW_STOCK_VOLUME_LEGACY_LEVELS;
}

int jw_stock_volume_get(const cJSON *root, const char *key, bool tagged) {
    const cJSON *devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
    const cJSON *level = cJSON_GetObjectItemCaseSensitive(devices, key);
    if (!cJSON_IsNumber(level)) {
        return 100;
    }

    const cJSON *leaf = cJSON_GetObjectItemCaseSensitive(root, JW_STOCK_VOLUME_LEAF_KEY);
    const cJSON *exact = cJSON_GetObjectItemCaseSensitive(leaf, key);
    if (cJSON_IsNumber(exact)) {
        int percent = jw__stock_volume_clamp(exact->valueint, 0, 100);
        if (jw_stock_volume_level(percent) == level->valueint) {
            return percent;
        }
        /* Our percent survived but the level under it moved. Stock would have
           dropped the percent, so older Leaf rewrote the level, on 0-10. */
        return jw__stock_volume_legacy_percent(level->valueint);
    }

    /* Untagged levels with a "USB" output were saved by Leaf before the tag:
       stock 1.3.0.32 has no USB output, and Jawaka has saved one since 2026-08.
       Stock keeps unknown outputs when it saves, so the key outlives a later
       stock session too; reading those as 0-10 is what Leaf always did. */
    if (!tagged && cJSON_GetObjectItemCaseSensitive(devices, "USB")) {
        return jw__stock_volume_legacy_percent(level->valueint);
    }
    return jw_stock_volume_percent(level->valueint);
}

static int jw__stock_volume_put_number(cJSON *object, const char *key, int value) {
    cJSON *item = cJSON_CreateNumber(value);
    if (!item) {
        return -1;
    }
    bool ok = cJSON_GetObjectItemCaseSensitive(object, key)
                  ? cJSON_ReplaceItemInObjectCaseSensitive(object, key, item)
                  : cJSON_AddItemToObject(object, key, item);
    if (!ok) {
        cJSON_Delete(item);
        return -1;
    }
    return 0;
}

int jw_stock_volume_put(cJSON *root, const char *const *keys, int key_count,
                        const char *key, int percent, bool tagged) {
    cJSON *devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (!cJSON_IsObject(devices) || !keys || !key ||
        key_count <= 0 || key_count > JW__STOCK_VOLUME_MAX_KEYS) {
        return -1;
    }

    /* Read every output before writing any: writing a "USB" level would make
       the rest look like levels Leaf saved before the tag. */
    int percents[JW__STOCK_VOLUME_MAX_KEYS];
    bool found = false;
    for (int i = 0; i < key_count; i++) {
        if (!keys[i]) {
            return -1;
        }
        if (strcmp(keys[i], key) == 0) {
            percents[i] = jw__stock_volume_clamp(percent, 0, 100);
            found = true;
        } else {
            percents[i] = jw_stock_volume_get(root, keys[i], tagged);
        }
    }
    if (!found) {
        return -1;
    }

    cJSON *leaf = cJSON_GetObjectItemCaseSensitive(root, JW_STOCK_VOLUME_LEAF_KEY);
    if (!cJSON_IsObject(leaf)) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, JW_STOCK_VOLUME_LEAF_KEY);
        leaf = cJSON_AddObjectToObject(root, JW_STOCK_VOLUME_LEAF_KEY);
        if (!leaf) {
            return -1;
        }
    }
    for (int i = 0; i < key_count; i++) {
        if (jw__stock_volume_put_number(devices, keys[i],
                                        jw_stock_volume_level(percents[i])) != 0 ||
            jw__stock_volume_put_number(leaf, keys[i], percents[i]) != 0) {
            return -1;
        }
    }
    return 0;
}
