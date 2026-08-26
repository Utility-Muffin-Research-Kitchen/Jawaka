#include "cmd/jawaka-osd/game_launch.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

const char *jw_osd_game_stage_name(jw_osd_game_stage stage) {
    switch (stage) {
        case JW_OSD_GAME_CHECKING: return "checking";
        case JW_OSD_GAME_SYNCING:  return "syncing";
        case JW_OSD_GAME_STOPPING: return "stopping";
        case JW_OSD_GAME_SETTINGS_NOT_SAVED: return "settings-not-saved";
    }
    return "unknown";
}

bool jw_osd_game_launch_parse(const cJSON *root,
                              jw_osd_game_stage *stage,
                              int *pending_items) {
    if (!cJSON_IsObject(root) || !stage || !pending_items) {
        return false;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *stage_item = cJSON_GetObjectItemCaseSensitive(root, "stage");
    const cJSON *pending = cJSON_GetObjectItemCaseSensitive(root, "pending_items");
    if (!cJSON_IsString(type) || !type->valuestring ||
        strcmp(type->valuestring, "show-game-launch") != 0 ||
        !cJSON_IsString(stage_item) || !stage_item->valuestring) {
        return false;
    }

    if (strcmp(stage_item->valuestring, "checking") == 0) {
        *stage = JW_OSD_GAME_CHECKING;
    } else if (strcmp(stage_item->valuestring, "syncing") == 0) {
        *stage = JW_OSD_GAME_SYNCING;
    } else if (strcmp(stage_item->valuestring, "stopping") == 0) {
        *stage = JW_OSD_GAME_STOPPING;
    } else if (strcmp(stage_item->valuestring, "settings-not-saved") == 0) {
        *stage = JW_OSD_GAME_SETTINGS_NOT_SAVED;
    } else {
        return false;
    }

    if (*stage == JW_OSD_GAME_SYNCING) {
        if (!cJSON_IsNumber(pending) || pending->valuedouble < 0.0 ||
            pending->valuedouble > (double)INT_MAX ||
            pending->valuedouble != (double)pending->valueint ||
            cJSON_GetArraySize(root) != 3) {
            return false;
        }
        *pending_items = pending->valueint;
        return true;
    }
    if (pending || cJSON_GetArraySize(root) != 2) {
        return false;
    }
    *pending_items = 0;
    return true;
}

void jw_osd_game_launch_text(jw_osd_game_stage stage, int pending_items,
                             char *title, size_t title_size,
                             char *action, size_t action_size) {
    if (title && title_size > 0) title[0] = '\0';
    if (action && action_size > 0) action[0] = '\0';
    if (!title || title_size == 0 || !action || action_size == 0) {
        return;
    }
    switch (stage) {
        case JW_OSD_GAME_CHECKING:
            snprintf(title, title_size, "SYNCTHING: CHECKING SAVES");
            break;
        case JW_OSD_GAME_SYNCING:
            if (pending_items < 0) pending_items = 0;
            snprintf(title, title_size, pending_items == 1
                         ? "SYNCTHING: SYNCING %d ITEM"
                         : "SYNCTHING: SYNCING %d ITEMS",
                     pending_items);
            snprintf(action, action_size, "MENU: START NOW");
            break;
        case JW_OSD_GAME_STOPPING:
            snprintf(title, title_size, "SYNCTHING: STOPPING");
            break;
        case JW_OSD_GAME_SETTINGS_NOT_SAVED:
            /* Only the glyphs the OSD's bitmap font actually has. */
            snprintf(title, title_size, "RETROARCH SETTINGS");
            snprintf(action, action_size, "NOT SAVED");
            break;
    }
}
