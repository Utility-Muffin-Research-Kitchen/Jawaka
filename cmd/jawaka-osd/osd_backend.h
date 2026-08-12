#ifndef JW_OSD_BACKEND_H
#define JW_OSD_BACKEND_H

#include "cmd/jawaka-osd/game_launch.h"

#include <stdint.h>

int  jw_osd_backend_init(void);
void jw_osd_backend_show_brightness(int percent, uint64_t now_ms);
void jw_osd_backend_show_volume(int percent, uint64_t now_ms);
void jw_osd_backend_show_game_launch(jw_osd_game_stage stage,
                                     int pending_items, uint64_t now_ms);
void jw_osd_backend_hide_game_launch(void);
void jw_osd_backend_tick(uint64_t now_ms);
void jw_osd_backend_shutdown(void);

#endif /* JW_OSD_BACKEND_H */
