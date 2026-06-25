#ifndef JW_LAUNCHER_SYSTEM_NAMES_H
#define JW_LAUNCHER_SYSTEM_NAMES_H

#include <stddef.h>

#define JW_CONTENT_SETTING_DISPLAY_NAME "display_name"

void jw_system_display_name(const char *db_path,
                            const char *system_id,
                            char *out,
                            size_t out_size);

#endif /* JW_LAUNCHER_SYSTEM_NAMES_H */
