#ifndef JW_UPDATE_UPDATE_H
#define JW_UPDATE_UPDATE_H

#include "cJSON.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <sys/types.h>
#include <time.h>

#define JW_UPDATE_PLATFORM_ID_MAX 32
#define JW_UPDATE_RELEASE_ID_MAX 128
#define JW_UPDATE_VERSION_MAX 128
#define JW_UPDATE_PATH_MAX 512
#define JW_UPDATE_URL_MAX 1024
#define JW_UPDATE_NAME_MAX 256
#define JW_UPDATE_SHA256_MAX 65
#define JW_UPDATE_KIND_MAX 64
#define JW_UPDATE_MESSAGE_MAX 256
#define JW_UPDATE_INSTALL_REASON_MAX 64
#define JW_UPDATE_MAX_OPTIONS 20

typedef enum {
    JW_UPDATE_STATUS_IDLE = 0,
    JW_UPDATE_STATUS_AVAILABLE,
    JW_UPDATE_STATUS_UP_TO_DATE,
    JW_UPDATE_STATUS_INCOMPATIBLE,
    JW_UPDATE_STATUS_DOWNLOADING,
    JW_UPDATE_STATUS_DOWNLOADED,
    JW_UPDATE_STATUS_CANCELLED,
    JW_UPDATE_STATUS_INSTALLING,
    JW_UPDATE_STATUS_ARMED,
    JW_UPDATE_STATUS_ERROR,
    JW_UPDATE_STATUS_CHECKING
} jw_update_status_code;

typedef struct {
    int index;
    bool installed;
    char release_id[JW_UPDATE_RELEASE_ID_MAX];
    char version[JW_UPDATE_VERSION_MAX];
    char published_at[64];
    char notes_url[JW_UPDATE_PATH_MAX];
    char manifest_url[JW_UPDATE_URL_MAX];
    char artifact_kind[JW_UPDATE_KIND_MAX];
    char artifact_name[JW_UPDATE_NAME_MAX];
    char artifact_url[JW_UPDATE_URL_MAX];
    char artifact_sha256[JW_UPDATE_SHA256_MAX];
    long long artifact_size;
    long long installed_size;
    char recovery_name[JW_UPDATE_NAME_MAX];
    char recovery_url[JW_UPDATE_URL_MAX];
    char handoff_type[JW_UPDATE_KIND_MAX];
    char handoff_completion[JW_UPDATE_KIND_MAX];
    char handoff_trigger_file[JW_UPDATE_NAME_MAX];
    int managed_apps_count;
    int migrations_count;
} jw_update_option;

typedef struct {
    jw_update_status_code status;
    bool compatible;
    bool has_update;
    bool current_unknown;
    int installed_schema;

    char platform_id[JW_UPDATE_PLATFORM_ID_MAX];
    char current_release_id[JW_UPDATE_RELEASE_ID_MAX];
    char current_version[JW_UPDATE_VERSION_MAX];

    char release_id[JW_UPDATE_RELEASE_ID_MAX];
    char version[JW_UPDATE_VERSION_MAX];
    char published_at[64];
    char notes_url[JW_UPDATE_PATH_MAX];

    char source_manifest[JW_UPDATE_PATH_MAX];
    char manifest_url[JW_UPDATE_URL_MAX];
    char artifact_kind[JW_UPDATE_KIND_MAX];
    char artifact_name[JW_UPDATE_NAME_MAX];
    char artifact_url[JW_UPDATE_URL_MAX];
    char artifact_sha256[JW_UPDATE_SHA256_MAX];
    long long artifact_size;
    long long installed_size;
    bool downloaded;
    bool download_active;
    long long download_received;
    long long download_total;
    int download_percent;
    char download_path[JW_UPDATE_PATH_MAX];

    char recovery_name[JW_UPDATE_NAME_MAX];
    char recovery_url[JW_UPDATE_URL_MAX];
    char handoff_type[JW_UPDATE_KIND_MAX];
    char handoff_completion[JW_UPDATE_KIND_MAX];
    char handoff_trigger_file[JW_UPDATE_NAME_MAX];
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
    char install_result_state[JW_UPDATE_KIND_MAX];
    char install_result_release_id[JW_UPDATE_RELEASE_ID_MAX];
    char install_result_message[JW_UPDATE_MESSAGE_MAX];
    char install_request_path[JW_UPDATE_PATH_MAX];
    char install_result_path[JW_UPDATE_PATH_MAX];
    char install_reason[JW_UPDATE_INSTALL_REASON_MAX];
    char install_message[JW_UPDATE_MESSAGE_MAX];

    char message[JW_UPDATE_MESSAGE_MAX];
    time_t checked_at;
    int option_count;
    int selected_option;
    jw_update_option options[JW_UPDATE_MAX_OPTIONS];
} jw_update_status;

typedef struct {
    bool active;
    pid_t pid;
    char tmp_path[JW_UPDATE_PATH_MAX];
    char out_path[JW_UPDATE_PATH_MAX];
    char artifact_name[JW_UPDATE_NAME_MAX];
    char expected_sha256[JW_UPDATE_SHA256_MAX];
    long long expected_size;
} jw_update_download_job;

typedef struct {
    bool active;
    pid_t pid;
    char request_path[JW_UPDATE_PATH_MAX];
    char result_path[JW_UPDATE_PATH_MAX];
} jw_update_install_job;

/* Async release check. The GitHub fetch is a blocking libcurl call (up to 15s on
   bad Wi-Fi); running it on the daemon's request path froze the launcher render
   loop, so we thread it. The worker fills `scratch`; the main loop's poll copies
   it back once `done` is atomically published and `pthread_join` completes. */
typedef struct {
    bool active;
    atomic_bool done;
    pthread_t thread;
    int result;
    jw_update_status scratch;
    char state_dir[JW_UPDATE_PATH_MAX];
    char platform_id[JW_UPDATE_PLATFORM_ID_MAX];
} jw_update_check_job;

const char *jw_update_status_name(jw_update_status_code status);

void jw_update_status_init(jw_update_status *status,
                           const char *platform_id,
                           const char *state_dir);
void jw_update_refresh_installed(jw_update_status *status,
                                 const char *state_dir);
void jw_update_refresh_install_result(jw_update_status *status,
                                      const char *state_dir,
                                      const char *sdcard_root);

int jw_update_check_local_manifest(jw_update_status *status,
                                   const char *state_dir,
                                   const char *platform_id,
                                   const char *manifest_path);
int jw_update_check_github(jw_update_status *status,
                           const char *state_dir,
                           const char *platform_id);
void jw_update_check_job_init(jw_update_check_job *job);
/* Kick off an async GitHub release check. Sets status to CHECKING and returns
   immediately; the result lands via jw_update_check_poll(). No-op (returns 0) if
   a check is already in flight. */
int jw_update_check_start(jw_update_status *status,
                          jw_update_check_job *job,
                          const char *state_dir);
void jw_update_check_poll(jw_update_status *status,
                          jw_update_check_job *job);
void jw_update_check_job_wait(jw_update_check_job *job);
int jw_update_select_option(jw_update_status *status, int option_index);
int jw_update_download_candidate(jw_update_status *status,
                                 const char *state_dir);
void jw_update_download_job_init(jw_update_download_job *job);
int jw_update_download_start(jw_update_status *status,
                             jw_update_download_job *job,
                             const char *state_dir);
void jw_update_download_poll(jw_update_status *status,
                             jw_update_download_job *job);
int jw_update_download_cancel(jw_update_status *status,
                              jw_update_download_job *job);
void jw_update_install_job_init(jw_update_install_job *job);
int jw_update_install_start(jw_update_status *status,
                            jw_update_install_job *job,
                            const char *state_dir,
                            const char *sdcard_root,
                            const char *runner_path);
void jw_update_install_poll(jw_update_status *status,
                            jw_update_install_job *job);
int jw_update_install_preflight(jw_update_status *status,
                                const char *state_dir,
                                const char *install_root,
                                bool device_idle,
                                int battery_percent,
                                int charging,
                                bool confirm_unknown_battery);

cJSON *jw_update_status_to_json(const jw_update_status *status);

#endif /* JW_UPDATE_UPDATE_H */
