#ifndef JW_PLATFORM_PATHS_H
#define JW_PLATFORM_PATHS_H

#include <stdbool.h>
#include <stddef.h>

char *jw_runtime_dir(void);
char *jw_sdcard_root(void);
char *jw_state_dir(void);
char *jw_socket_path(void);
char *jw_osd_socket_path(void);
char *jw_db_path(void);
char *jw_retroarch_bin_path(void);
char *jw_retroarch_core_path_for_system(const char *system);
bool jw_sdcard_exec_available_for_path(const char *path, char *error, size_t error_size);
char *jw_write_retroarch_append_config(const char *runtime_dir, const char *sdcard_root,
                                       const char *core_path, int player1_joypad_index);
char *jw_prepare_retroarch_config(const char *runtime_dir, const char *sdcard_root,
                                  const char *core_path, int player1_joypad_index,
                                  bool persist_changes,
                                  char *error, size_t error_size);
int jw_backup_retroarch_config(const char *runtime_config_path, const char *sdcard_root,
                               char *error, size_t error_size);
int jw_reset_retroarch_shared_config(const char *sdcard_root,
                                     char *status, size_t status_size);
char *jw_retroarch_state_dir(const char *sdcard_root);

#endif
