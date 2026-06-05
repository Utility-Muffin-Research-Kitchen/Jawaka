#ifndef JW_IPC_CLIENT_H
#define JW_IPC_CLIENT_H

#include "internal/ipc/ipc.h"
#include "internal/db/db.h"

#include <stdbool.h>

typedef struct {
    bool active;
    bool command_ok;
    char command_result[32];
    char system[64];
    char rom_path[512];
    char core_path[512];
    int disk_count;
    int disk_slot;
    bool savestate_supported;
    int state_slot;
} jw_ipc_retroarch_session_info;

typedef struct {
    bool present;
    bool mounted;
    bool busy;
    bool can_unmount;
    char source[32];
    char label[64];
    char mount_path[512];
    char message[256];
} jw_ipc_storage_status_info;

/* Send a "hello" handshake to jawakad.
 * role: "launcher" or "menu".
 * Returns 0 on success, -1 on failure. */
int jw_ipc_hello(const char *socket_path, const char *role);

/* Request a library rescan. Populates status[status_len] with a human-readable
 * result message. Returns 0 on success, -1 on failure. */
int jw_ipc_scan_library(const char *socket_path, char *status, int status_len);
int jw_ipc_library_status(const char *socket_path, int *out_generation);
int jw_ipc_get_storage_status(const char *socket_path, const char *source,
                              jw_ipc_storage_status_info *out,
                              char *status, int status_len);
int jw_ipc_safe_unmount_storage(const char *socket_path, const char *source,
                                char *status, int status_len);

/* Ask jawakad to show the menu overlay. Returns 0 on success, -1 on failure. */
int jw_ipc_open_menu(const char *socket_path);

/* Ask jawakad to launch a game through the daemon-owned RetroArch process.
 * rom_path may be absolute or relative to the SD-card root.
 * Populates status[status_len] with a human-readable result when provided. */
int jw_ipc_launch_game(const char *socket_path, const char *system,
                       const char *rom_path, char *status, int status_len);

/* Same launch request, but asks jawakad to resume the switcher-preferred state
 * after RetroArch starts. Normal game browser launches should not use this. */
int jw_ipc_launch_game_switcher(const char *socket_path, const char *system,
                                const char *rom_path, char *status,
                                int status_len);

/* Ask jawakad to launch an app pak as a foreground child.
 * pak_dir may be absolute or relative to the SD-card root.
 * Populates status[status_len] with a human-readable result when provided. */
int jw_ipc_launch_app(const char *socket_path, const char *pak_dir,
                      char *status, int status_len);

/* Ask jawakad to open the in-game game switcher overlay for the active RetroArch
 * session. Reversible: the daemon pauses + overlays only; it does not save or
 * quit. Replies error when there is no active session. */
int jw_ipc_open_switcher(const char *socket_path, char *status, int status_len);

/* Commit a switch from the in-game switcher to a different game: jawakad saves
 * the current game when supported, quits it, and spawns the selected game
 * directly (no launcher in between). Requires an active RetroArch session. */
int jw_ipc_switch_game(const char *socket_path, const char *system,
                       const char *rom_path, char *status, int status_len);

/* Fetch the daemon-owned RetroArch session and command-interface state. */
int jw_ipc_get_retroarch_session(const char *socket_path,
                                 jw_ipc_retroarch_session_info *out,
                                 char *status, int status_len);

/* Ask jawakad to perform a RetroArch action for the active session.
 * value is action-specific; pass 0 when unused. */
int jw_ipc_retroarch_action(const char *socket_path, const char *action,
                            int value, char *status, int status_len);

/* Reset the shared RetroArch config back to packaged platform defaults. */
int jw_ipc_reset_retroarch_config(const char *socket_path,
                                  char *status, int status_len);

/* Ask jawakad to shut down. Returns 0 on success. */
int jw_ipc_shutdown(const char *socket_path);

/* Ask jawakad to exit Leaf mode and pass this boot to the stock launcher. */
int jw_ipc_exit_stock(const char *socket_path);

/* Notify jawakad that a frontend process has rendered enough to be considered
 * ready. role is usually "launcher" or "menu". Returns 0 on success. */
int jw_ipc_frontend_ready(const char *socket_path, const char *role);

/* Send a platform-action request to jawakad (e.g. "poweroff", "reboot").
 * Returns 0 on success. */
int jw_ipc_platform_action(const char *socket_path, const char *action, int value);

int jw_ipc_platform_brightness(const char *socket_path, int *out_percent);
int jw_ipc_set_brightness(const char *socket_path, int percent,
                          int *out_percent, char *status, int status_len);

int jw_ipc_platform_volume(const char *socket_path, int *out_percent);
int jw_ipc_set_volume(const char *socket_path, int percent,
                      int *out_percent, char *status, int status_len);

/* LED ring. set-led applies + persists in jawakad; get-led reads the cached
   state back from platform-status. mode is "FOREVER"/"BREATH"/"RAINBOW". */
int jw_ipc_set_led(const char *socket_path, int enabled, const char *mode,
                   int r, int g, int b, int brightness, int speed,
                   char *status, int status_len);
int jw_ipc_get_led(const char *socket_path, int *enabled, char *mode, int mode_len,
                   int *r, int *g, int *b, int *brightness, int *speed);

#endif /* JW_IPC_CLIENT_H */
