#ifndef JW_OSD_BACKEND_H
#define JW_OSD_BACKEND_H

#include <stdint.h>

int  jw_osd_backend_init(void);
void jw_osd_backend_show_brightness(int percent, uint64_t now_ms);
void jw_osd_backend_show_volume(int percent, uint64_t now_ms);
void jw_osd_backend_tick(uint64_t now_ms);
void jw_osd_backend_shutdown(void);

#endif /* JW_OSD_BACKEND_H */
