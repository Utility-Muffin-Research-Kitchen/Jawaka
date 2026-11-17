#include "internal/platform/stock_volume.h"

#include <stdio.h>
#include <string.h>

/* One per audio output; the MLP1 has five. */
#define JW__STOCK_VOLUME_MAX_KEYS 8

static int jw__stock_volume_clamp(int value, int lo, int hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

int jw_stock_volume_level(int percent, int levels) {
    percent = jw__stock_volume_clamp(percent, 0, 100);
    return (percent * levels + 50) / 100;
}

int jw_stock_volume_percent(int level, int levels) {
    level = jw__stock_volume_clamp(level, 0, levels);
    return (level * 100 + levels / 2) / levels;
}

static int jw__stock_volume_legacy_percent(int level) {
    level = jw__stock_volume_clamp(level, 0, JW_STOCK_VOLUME_LEGACY_LEVELS);
    return level * 100 / JW_STOCK_VOLUME_LEGACY_LEVELS;
}

static const char *const JW__STOCK_VOLUME_AUDIO_KEYS[][2] = {
    {"SPEAKER", "AUDIO_SPEAKER"},
    {"HEADSET", "AUDIO_HEADSET"},
    {"HDMI", "AUDIO_HDMI"},
    {"BLUETOOTH", "AUDIO_BLUETOOTH"},
};

const char *jw_stock_volume_audio_key(const char *key) {
    if (!key) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(JW__STOCK_VOLUME_AUDIO_KEYS) /
                               sizeof(JW__STOCK_VOLUME_AUDIO_KEYS[0]); i++) {
        if (strcmp(key, JW__STOCK_VOLUME_AUDIO_KEYS[i][0]) == 0) {
            return JW__STOCK_VOLUME_AUDIO_KEYS[i][1];
        }
    }
    return NULL;
}

int jw_stock_volume_service_has_audio_keys(const char *path) {
    FILE *fp = path ? fopen(path, "rb") : NULL;
    if (!fp) {
        return -1;
    }
    /* A whole string: 1.3 has LOONG_AUDIO_SPEAKER too. */
    static const char needle[] = "\0" JW_STOCK_VOLUME_AUDIO_MARKER;
    const size_t needle_len = sizeof(needle); /* both NULs */
    char buf[65536];
    size_t keep = 0;
    int found = 0;
    for (;;) {
        size_t got = fread(buf + keep, 1, sizeof(buf) - keep, fp);
        if (got == 0) {
            break;
        }
        size_t len = keep + got;
        for (size_t i = 0; i + needle_len <= len; i++) {
            if (buf[i] == '\0' && memcmp(buf + i, needle, needle_len) == 0) {
                found = 1;
                break;
            }
        }
        if (found) {
            break;
        }
        keep = len < needle_len - 1 ? len : needle_len - 1;
        memmove(buf, buf + len - keep, keep);
    }
    bool failed = ferror(fp) != 0;
    fclose(fp);
    return failed && !found ? -1 : found;
}

bool jw_stock_volume_has_audio_keys(const cJSON *root) {
    const cJSON *devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
    for (size_t i = 0; i < sizeof(JW__STOCK_VOLUME_AUDIO_KEYS) /
                               sizeof(JW__STOCK_VOLUME_AUDIO_KEYS[0]); i++) {
        if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(
                devices, JW__STOCK_VOLUME_AUDIO_KEYS[i][1]))) {
            return true;
        }
    }
    return false;
}

/* The old name's level: stock 1.3's own, or Leaf's on any firmware. */
static int jw__stock_volume_get_plain(const cJSON *root, const char *key, bool tagged) {
    const cJSON *devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
    const cJSON *level = cJSON_GetObjectItemCaseSensitive(devices, key);
    if (!cJSON_IsNumber(level)) {
        return 100;
    }

    const cJSON *leaf = cJSON_GetObjectItemCaseSensitive(root, JW_STOCK_VOLUME_LEAF_KEY);
    const cJSON *exact = cJSON_GetObjectItemCaseSensitive(leaf, key);
    if (cJSON_IsNumber(exact)) {
        int percent = jw__stock_volume_clamp(exact->valueint, 0, 100);
        if (jw_stock_volume_level(percent, JW_STOCK_VOLUME_LEVELS) == level->valueint) {
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
    return jw_stock_volume_percent(level->valueint, JW_STOCK_VOLUME_LEVELS);
}

int jw_stock_volume_get(const cJSON *root, const char *key,
                        const jw_stock_volume_scale *scale) {
    bool tagged = scale && scale->tagged;
    const char *audio = scale && scale->audio_keys ? jw_stock_volume_audio_key(key) : NULL;
    if (audio) {
        const cJSON *devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
        const cJSON *leaf = cJSON_GetObjectItemCaseSensitive(root, JW_STOCK_VOLUME_LEAF_KEY);
        const cJSON *level = cJSON_GetObjectItemCaseSensitive(devices, audio);
        /* Exact percents mean Leaf (this one or an older one, which writes only
           the old names) saved last, so the old name is current. Without them
           stock 1.4 saved last, and its AUDIO_ level is current once Leaf has
           written those levels; before that the old name still holds the level
           from before the OTA, and the AUDIO_ one may be stock's 0 for an
           output it had no level for. */
        if (cJSON_IsNumber(level) &&
            !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(leaf, key)) &&
            (scale->audio_tagged ||
             !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(devices, key)))) {
            return jw_stock_volume_percent(level->valueint, JW_STOCK_VOLUME_AUDIO_LEVELS);
        }
    }
    return jw__stock_volume_get_plain(root, key, tagged);
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
                        const char *key, int percent,
                        const jw_stock_volume_scale *scale) {
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
            percents[i] = jw_stock_volume_get(root, keys[i], scale);
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
    bool audio_keys = scale && scale->audio_keys;
    for (int i = 0; i < key_count; i++) {
        const char *audio = audio_keys ? jw_stock_volume_audio_key(keys[i]) : NULL;
        if (jw__stock_volume_put_number(
                devices, keys[i],
                jw_stock_volume_level(percents[i], JW_STOCK_VOLUME_LEVELS)) != 0 ||
            (audio && jw__stock_volume_put_number(
                          devices, audio,
                          jw_stock_volume_level(percents[i],
                                                JW_STOCK_VOLUME_AUDIO_LEVELS)) != 0) ||
            jw__stock_volume_put_number(leaf, keys[i], percents[i]) != 0) {
            return -1;
        }
    }
    return 0;
}
