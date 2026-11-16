#ifndef JW_PLATFORM_STOCK_VOLUME_H
#define JW_PLATFORM_STOCK_VOLUME_H

#include "cJSON.h"

#include <stdbool.h>

/* Stock loong_service keeps one volume level per output in SOUND_PARAM's
   "devices" object and writes it straight to /sys/kernel/rk817/volume.

   LoongOS 1.3 names the outputs SPEAKER, HEADSET, HDMI and BLUETOOTH and
   counts 0-14. LoongOS 1.4 names them AUDIO_SPEAKER, AUDIO_HEADSET, AUDIO_HDMI
   and AUDIO_BLUETOOTH and counts 0-16 (its kernel table has 17 levels too). It
   never reads the old names: an output it has no AUDIO_ level for starts at 0
   (muted) and is saved that way.

   Leaf writes the old names on 0-14 on every firmware, so UMRK_SOUND_LEVELS
   keeps its meaning and an older Leaf card on a 1.4 device reads what it
   always did. On 1.4 it also writes the AUDIO_ names on 0-16, which is what
   stock reads. Leaf wrote the old names on 0-10 until the device was tagged
   (see JW_STOCK_VOLUME_SCALE_PARAM). */
#define JW_STOCK_VOLUME_LEVELS 14
#define JW_STOCK_VOLUME_AUDIO_LEVELS 16
#define JW_STOCK_VOLUME_LEGACY_LEVELS 10

/* Leaf's exact per-output percents, kept beside "devices" under the old names.
   Stock rebuilds SOUND_PARAM from fixed top-level fields when it saves, so this
   is gone once stock has written; older Leaf rewrites the whole object and
   keeps it. */
#define JW_STOCK_VOLUME_LEAF_KEY "umrkVolumes"

/* system_config row Leaf sets once it has written 0-14 levels on this device.
   It lives in loong.db with SOUND_PARAM rather than on the SD card, so a second
   Leaf card in the same device does not rescale the levels again. */
#define JW_STOCK_VOLUME_SCALE_PARAM "UMRK_SOUND_LEVELS"

/* system_config row Leaf sets once it has written the AUDIO_ levels. Until
   then those levels are stock 1.4's alone, and right after an OTA from 1.3 they
   are the 0 stock filled in for an output it found no AUDIO_ level for, so the
   old names (the level from before the OTA) read first. */
#define JW_STOCK_VOLUME_AUDIO_SCALE_PARAM "UMRK_SOUND_AUDIO_LEVELS"

/* loong_service string that marks a firmware using the AUDIO_ names. */
#define JW_STOCK_VOLUME_AUDIO_MARKER "AUDIO_SPEAKER"

typedef struct {
    bool tagged;       /* JW_STOCK_VOLUME_SCALE_PARAM is set */
    bool audio_keys;   /* stock uses the AUDIO_ names on 0-16 (LoongOS 1.4) */
    bool audio_tagged; /* JW_STOCK_VOLUME_AUDIO_SCALE_PARAM is set */
} jw_stock_volume_scale;

/* Level on a 0-levels scale for a percent, and back. */
int jw_stock_volume_level(int percent, int levels);
int jw_stock_volume_percent(int level, int levels);

/* LoongOS 1.4's name for an old output name, or NULL for an output stock has
   no level for (USB). */
const char *jw_stock_volume_audio_key(const char *key);

/* 1 if the loong_service binary at path uses the AUDIO_ names, 0 if not, -1 if
   it cannot be read. */
int jw_stock_volume_service_has_audio_keys(const char *path);

/* True if a parsed SOUND_PARAM already holds an AUDIO_ level: stock 1.4 has
   saved it. */
bool jw_stock_volume_has_audio_keys(const cJSON *root);

/* The percent for one output (old name) of a parsed SOUND_PARAM. An output with
   no level plays at 100%. */
int jw_stock_volume_get(const cJSON *root, const char *key,
                        const jw_stock_volume_scale *scale);

/* Set key to percent, and rewrite every key in keys[] (old names) as a level on
   each scale stock reads, with its exact percent beside it, so stock reads every
   output on its own scale. */
int jw_stock_volume_put(cJSON *root, const char *const *keys, int key_count,
                        const char *key, int percent,
                        const jw_stock_volume_scale *scale);

#endif /* JW_PLATFORM_STOCK_VOLUME_H */
