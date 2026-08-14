#ifndef JW_OSD_GAME_LAUNCH_H
#define JW_OSD_GAME_LAUNCH_H

#include "cJSON.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    JW_OSD_GAME_CHECKING = 0,
    JW_OSD_GAME_SYNCING,
    JW_OSD_GAME_STOPPING,
} jw_osd_game_stage;

bool jw_osd_game_launch_parse(const cJSON *root,
                              jw_osd_game_stage *stage,
                              int *pending_items);
void jw_osd_game_launch_text(jw_osd_game_stage stage, int pending_items,
                             char *title, size_t title_size,
                             char *action, size_t action_size);
const char *jw_osd_game_stage_name(jw_osd_game_stage stage);

#endif
