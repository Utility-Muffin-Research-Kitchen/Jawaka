#ifndef JW_OSD_GAME_LAUNCH_H
#define JW_OSD_GAME_LAUNCH_H

#include "cJSON.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    JW_OSD_GAME_CHECKING = 0,
    JW_OSD_GAME_SYNCING,
    JW_OSD_GAME_STOPPING,
    /* Not a launch stage: the RetroArch app runner could not write the
       session's settings back to the durable shared config. It reuses this
       banner because it is the one text surface the OSD has, and it must be
       seen -- a silent loss is exactly the failure Leaf#48 reported. Unlike
       the Syncthing stages it hides itself after a few seconds; nothing is
       waiting on it. */
    JW_OSD_GAME_SETTINGS_NOT_SAVED,
} jw_osd_game_stage;

/* Stages that dismiss themselves instead of waiting for hide-game-launch. */
#define JW_OSD_GAME_STAGE_IS_TRANSIENT(stage) \
    ((stage) == JW_OSD_GAME_SETTINGS_NOT_SAVED)
#define JW_OSD_GAME_TRANSIENT_MS 4000u

bool jw_osd_game_launch_parse(const cJSON *root,
                              jw_osd_game_stage *stage,
                              int *pending_items);
void jw_osd_game_launch_text(jw_osd_game_stage stage, int pending_items,
                             char *title, size_t title_size,
                             char *action, size_t action_size);
const char *jw_osd_game_stage_name(jw_osd_game_stage stage);

#endif
