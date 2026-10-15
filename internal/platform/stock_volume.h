#ifndef JW_PLATFORM_STOCK_VOLUME_H
#define JW_PLATFORM_STOCK_VOLUME_H

#include "cJSON.h"

#include <stdbool.h>

/* Stock loong_service keeps one 0-14 volume level per output in SOUND_PARAM's
   "devices" object and writes it straight to /sys/kernel/rk817/volume. Leaf
   wrote the same levels on a 0-10 scale until the device was tagged (see
   JW_STOCK_VOLUME_SCALE_PARAM). */
#define JW_STOCK_VOLUME_LEVELS 14
#define JW_STOCK_VOLUME_LEGACY_LEVELS 10

/* Leaf's exact per-output percents, kept beside "devices". Stock rebuilds
   SOUND_PARAM from fixed top-level fields when it saves, so this is gone once
   stock has written; older Leaf rewrites the whole object and keeps it. */
#define JW_STOCK_VOLUME_LEAF_KEY "umrkVolumes"

/* system_config row Leaf sets once it has written 0-14 levels on this device.
   It lives in loong.db with SOUND_PARAM rather than on the SD card, so a second
   Leaf card in the same device does not rescale the levels again. */
#define JW_STOCK_VOLUME_SCALE_PARAM "UMRK_SOUND_LEVELS"

int jw_stock_volume_level(int percent);
int jw_stock_volume_percent(int level);

/* The percent for one output of a parsed SOUND_PARAM. tagged: the device has
   JW_STOCK_VOLUME_SCALE_PARAM. An output with no level plays at 100%. */
int jw_stock_volume_get(const cJSON *root, const char *key, bool tagged);

/* Set key to percent, and rewrite every key in keys[] as a 0-14 level with its
   exact percent beside it, so stock reads every output on its own scale. */
int jw_stock_volume_put(cJSON *root, const char *const *keys, int key_count,
                        const char *key, int percent, bool tagged);

#endif /* JW_PLATFORM_STOCK_VOLUME_H */
