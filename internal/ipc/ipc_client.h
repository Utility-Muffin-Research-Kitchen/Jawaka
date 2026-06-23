#ifndef JW_IPC_CLIENT_H
#define JW_IPC_CLIENT_H

#include "internal/ipc/ipc.h"
#include "internal/db/db.h"
#include "internal/platform/device.h"

#include <stdbool.h>

#define JW_IPC_UPDATE_MAX_OPTIONS 20

typedef struct {
    bool active;
    bool command_ok;
    char command_result[32];
    char system[64];
    char rom_path[512];
    char core_path[512];
    char core_id[64];
    int disk_count;
    int disk_slot;
    bool savestate_supported;
    int state_slot;
} jw_ipc_retroarch_session_info;

typedef struct {
    jw_platform_audio_output output;
    unsigned available_outputs;
    int volume_percent[JW_PLATFORM_AUDIO_OUTPUT_COUNT];
    int test_playing;          /* 1 while the Test Sound clip is playing, else 0 */
} jw_ipc_audio_status;

typedef struct {
    bool supported;
    char name[JW_PLATFORM_PERF_VALUE_MAX];
    char governor[JW_PLATFORM_PERF_VALUE_MAX];
    int current_freq;
    int set_freq;
    char available_governors[JW_PLATFORM_PERF_LIST_MAX];
    char available_frequencies[JW_PLATFORM_PERF_LIST_MAX];
} jw_ipc_performance_domain_status;

typedef struct {
    bool supported;
    char active_profile[JW_PLATFORM_PERF_VALUE_MAX];
    char global_profile[JW_PLATFORM_PERF_VALUE_MAX];
    char session_profile[JW_PLATFORM_PERF_VALUE_MAX];
    bool session_override;
    int soc_temp_c;
    char message[256];
    char last_error[256];
    jw_ipc_performance_domain_status domains[JW_PLATFORM_PERF_DOMAIN_COUNT];
} jw_ipc_performance_status_info;

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

typedef struct {
    int index;
    bool selected;
    bool installed;
    char release_id[128];
    char version[128];
    char published_at[64];
    char notes_url[512];
    char artifact_kind[64];
    char artifact_name[256];
    long long artifact_size;
    long long installed_size;
} jw_ipc_update_option_info;

typedef struct {
    char state[32];
    bool compatible;
    bool has_update;
    bool current_unknown;
    int installed_schema;
    char platform_id[32];
    char current_release_id[128];
    char current_version[128];
    char release_id[128];
    char version[128];
    char published_at[64];
    char source_manifest[512];
    char manifest_url[1024];
    char notes_url[512];
    char artifact_kind[64];
    char artifact_name[256];
    char artifact_url[1024];
    char artifact_sha256[65];
    long long artifact_size;
    long long installed_size;
    bool downloaded;
    bool download_active;
    long long download_received;
    long long download_total;
    int download_percent;
    char download_path[512];
    char handoff_type[64];
    char handoff_completion[64];
    char handoff_trigger_file[256];
    char recovery_name[256];
    char recovery_url[1024];
    int managed_apps_count;
    int migrations_count;
    bool install_ready;
    bool install_blocked;
    bool install_needs_confirmation;
    bool install_active;
    bool install_armed;
    bool install_idle;
    int install_battery_percent;
    int install_charging;
    long long install_required_free;
    long long install_available_free;
    char install_result_state[64];
    char install_result_release_id[128];
    char install_result_message[256];
    char install_request_path[512];
    char install_result_path[512];
    char install_reason[64];
    char install_message[256];
    char message[256];
    int selected_option;
    int option_count;
    jw_ipc_update_option_info options[JW_IPC_UPDATE_MAX_OPTIONS];
} jw_ipc_update_status_info;

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
int jw_ipc_platform_power_status(const char *socket_path,
                                 int *out_battery_percent,
                                 int *out_charging);
int jw_ipc_set_brightness(const char *socket_path, int percent,
                          int *out_percent, char *status, int status_len);

int jw_ipc_platform_volume(const char *socket_path, int *out_percent);
/* HDMI 1080p120 auto-revert: seconds left before revert (0 = none pending), and
   the user's "keep this mode" confirmation that cancels the revert. */
int jw_ipc_hdmi_revert_status(const char *socket_path, int *out_seconds);
int jw_ipc_hdmi_revert_keep(const char *socket_path);
int jw_ipc_set_volume(const char *socket_path, int percent,
                      int *out_percent, char *status, int status_len);
int jw_ipc_platform_audio_status(const char *socket_path,
                                 jw_ipc_audio_status *out_status);
int jw_ipc_set_audio_output(const char *socket_path,
                            jw_platform_audio_output output,
                            char *status, int status_len);

int jw_ipc_get_performance_status(const char *socket_path,
                                  jw_ipc_performance_status_info *out,
                                  char *status, int status_len);
int jw_ipc_set_performance_profile(const char *socket_path,
                                   const char *scope,
                                   const char *profile,
                                   char *status, int status_len);
int jw_ipc_set_performance_custom(const char *socket_path,
                                  const jw_platform_perf_request *request,
                                  char *status, int status_len);
int jw_ipc_reset_performance_session(const char *socket_path,
                                     char *status, int status_len);

int jw_ipc_update_status(const char *socket_path,
                         jw_ipc_update_status_info *out,
                         char *status, int status_len);
int jw_ipc_update_check(const char *socket_path,
                        const char *manifest_path,
                        jw_ipc_update_status_info *out,
                        char *status, int status_len);
int jw_ipc_update_select(const char *socket_path,
                         int option_index,
                         jw_ipc_update_status_info *out,
                         char *status, int status_len);
int jw_ipc_update_download(const char *socket_path,
                           jw_ipc_update_status_info *out,
                           char *status, int status_len);
int jw_ipc_update_cancel(const char *socket_path,
                         jw_ipc_update_status_info *out,
                         char *status, int status_len);
int jw_ipc_update_install_preflight(const char *socket_path,
                                    bool confirm_unknown_battery,
                                    jw_ipc_update_status_info *out,
                                    char *status, int status_len);
int jw_ipc_update_install(const char *socket_path,
                          bool confirm_unknown_battery,
                          jw_ipc_update_status_info *out,
                          char *status, int status_len);

int jw_ipc_get_adb(const char *socket_path, int *out_enabled,
                   int *out_intent_enabled, bool *out_supported);
int jw_ipc_set_adb(const char *socket_path, int enabled,
                   char *status, int status_len);
int jw_ipc_get_boot_splash(const char *socket_path, int *out_enabled,
                           bool *out_supported);
int jw_ipc_set_boot_splash(const char *socket_path, int enabled,
                           char *status, int status_len);
int jw_ipc_get_refresh_rate(const char *socket_path, int *out_hz,
                            bool *out_supported);
int jw_ipc_set_refresh_rate(const char *socket_path, int hz,
                            char *status, int status_len);
/* HDMI output: status (connected/current mode/supported) + set (0 off/1 4:3/2 stretch). */
int jw_ipc_get_hdmi_status(const char *socket_path, int *out_connected,
                           int *out_mode, bool *out_supported);
int jw_ipc_set_hdmi_output(const char *socket_path, int mode,
                           char *status, int status_len);

typedef struct {
    bool valid;           /* credentials confirmed by ScreenScraper */
    bool rejected;        /* explicit credential rejection (vs transport error) */
    int  max_threads;
    int  requests_today;
    int  max_requests;
    int  user_level;
    char message[256];    /* daemon-side error detail when !valid */
} jw_ipc_scrape_validate_info;

/* Ask jawakad to validate ScreenScraper user credentials against the API
   (the daemon owns the dev-credential half of the auth). Returns 0 when the
   request completed (check out->valid / out->rejected), -1 when the daemon
   was unreachable or replied malformed. */
int jw_ipc_scrape_validate(const char *socket_path, const char *username,
                           const char *password,
                           jw_ipc_scrape_validate_info *out);

typedef struct {
    char state[16];           /* "idle" | "running" | "paused-quota" */
    int  total;
    int  done;
    int  found;
    int  not_found;
    int  failed;
    int  cancelled;
    int  queued;
    int  active;
    char current_name[256];
    char current_system[64];
    char message[256];
} jw_ipc_scrape_status_info;

#define JW_IPC_SCRAPE_QUEUE_MAX_ROWS 256

typedef enum {
    JW_IPC_SCRAPE_ROW_QUEUED = 0,
    JW_IPC_SCRAPE_ROW_HASH,
    JW_IPC_SCRAPE_ROW_SEARCH,
    JW_IPC_SCRAPE_ROW_DOWNLOAD,
    JW_IPC_SCRAPE_ROW_SAVE,
    JW_IPC_SCRAPE_ROW_DONE,
    JW_IPC_SCRAPE_ROW_NOT_FOUND,
    JW_IPC_SCRAPE_ROW_ERROR,
    JW_IPC_SCRAPE_ROW_CANCELLED,
} jw_ipc_scrape_row_state;

typedef struct {
    unsigned id;
    jw_ipc_scrape_row_state state;
    char display_name[256];
    char system[64];
    char rom_path[512];
    char output_path[512];
    char message[256];
} jw_ipc_scrape_queue_row;

typedef struct {
    char state[16];           /* "idle" | "running" | "paused-quota" */
    int total;
    int done;
    int found;
    int not_found;
    int failed;
    int cancelled;
    int queued;
    int active;
    int requests_today;
    int max_requests;
    int max_threads;
    int permits;
    int eta_seconds;          /* -1 when unavailable */
    int row_count;
    char message[256];
    jw_ipc_scrape_queue_row rows[JW_IPC_SCRAPE_QUEUE_MAX_ROWS];
} jw_ipc_scrape_queue_info;

/* Queue scraping. scope "game" needs rom_path (always re-fetches/replaces);
   scope "system" honors missing_only. On success returns 0 and sets
   *out_enqueued; on failure returns -1 with a daemon message in
   status[status_len]. */
int jw_ipc_scrape_start(const char *socket_path, const char *scope,
                        const char *system, const char *rom_path,
                        bool missing_only, int *out_enqueued,
                        char *status, int status_len);
int jw_ipc_scrape_status(const char *socket_path,
                         jw_ipc_scrape_status_info *out);
int jw_ipc_scrape_queue(const char *socket_path, int offset, int limit,
                        jw_ipc_scrape_queue_info *out);
/* True in *out_pending when the system (rom_path NULL) or game has queued
   or in-flight scrape work. */
int jw_ipc_scrape_pending(const char *socket_path, const char *system,
                          const char *rom_path, bool *out_pending);
/* scope "all" | "system" | "game"; system/rom_path as required by scope. */
int jw_ipc_scrape_cancel(const char *socket_path, const char *scope,
                         const char *system, const char *rom_path,
                         int *out_removed);
int jw_ipc_scrape_stop_all(const char *socket_path, int *out_stopped);
int jw_ipc_scrape_clear_done(const char *socket_path, int *out_cleared);

#define JW_IPC_MISSING_MAX_SYSTEMS 128
typedef struct {
    char system[64];   /* system code (e.g. "GB") */
    int  missing;      /* games still needing art */
    int  total;        /* games in this system */
} jw_ipc_scrape_missing_row;
typedef struct {
    int total_missing;
    int system_count;
    jw_ipc_scrape_missing_row systems[JW_IPC_MISSING_MAX_SYSTEMS];
} jw_ipc_scrape_missing_info;
/* Per-system missing/total art counts (mapped systems with games only). */
int jw_ipc_scrape_missing_counts(const char *socket_path,
                                 jw_ipc_scrape_missing_info *out);

/* LED ring. set-led applies + persists in jawakad; get-led reads the cached
   state back from platform-status. mode is "FOREVER"/"BREATH"/"RAINBOW". */
int jw_ipc_set_led(const char *socket_path, int enabled, const char *mode,
                   int r, int g, int b, int brightness, int speed,
                   char *status, int status_len);
int jw_ipc_get_led(const char *socket_path, int *enabled, char *mode, int mode_len,
                   int *r, int *g, int *b, int *brightness, int *speed);

#endif /* JW_IPC_CLIENT_H */
