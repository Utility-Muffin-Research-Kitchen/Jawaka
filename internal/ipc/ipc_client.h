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

/* Ask jawakad to shut down. Returns 0 on success. */
int jw_ipc_shutdown(const char *socket_path);

#endif /* JW_IPC_CLIENT_H */
