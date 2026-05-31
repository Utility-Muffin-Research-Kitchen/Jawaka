#ifndef JW_IPC_CLIENT_H
#define JW_IPC_CLIENT_H

#include "internal/ipc/ipc.h"
#include "internal/db/db.h"

/* Send a "hello" handshake to jawakad.
 * role: "launcher" or "menu".
 * Returns 0 on success, -1 on failure. */
int jw_ipc_hello(const char *socket_path, const char *role);

/* Request a library rescan. Populates status[status_len] with a human-readable
 * result message. Returns 0 on success, -1 on failure. */
int jw_ipc_scan_library(const char *socket_path, char *status, int status_len);

/* Ask jawakad to show the menu overlay. Returns 0 on success, -1 on failure. */
int jw_ipc_open_menu(const char *socket_path);

/* Ask jawakad to launch a game through the daemon-owned RetroArch process.
 * rom_path may be absolute or relative to the SD-card root.
 * Populates status[status_len] with a human-readable result when provided. */
int jw_ipc_launch_game(const char *socket_path, const char *system,
                       const char *rom_path, char *status, int status_len);

/* Ask jawakad to launch an app pak as a foreground child.
 * pak_dir may be absolute or relative to the SD-card root.
 * Populates status[status_len] with a human-readable result when provided. */
int jw_ipc_launch_app(const char *socket_path, const char *pak_dir,
                      char *status, int status_len);

/* Reset the shared RetroArch config back to packaged platform defaults. */
int jw_ipc_reset_retroarch_config(const char *socket_path,
                                  char *status, int status_len);

/* Ask jawakad to shut down. Returns 0 on success. */
int jw_ipc_shutdown(const char *socket_path);

/* Notify jawakad that a frontend process has rendered enough to be considered
 * ready. role is usually "launcher" or "menu". Returns 0 on success. */
int jw_ipc_frontend_ready(const char *socket_path, const char *role);

/* Send a platform-action request to jawakad (e.g. "poweroff", "reboot").
 * Returns 0 on success. */
int jw_ipc_platform_action(const char *socket_path, const char *action, int value);

int jw_ipc_platform_brightness(const char *socket_path, int *out_percent);
int jw_ipc_set_brightness(const char *socket_path, int percent,
                          int *out_percent, char *status, int status_len);

#endif /* JW_IPC_CLIENT_H */
