#ifndef JW_RETROARCH_COMMAND_H
#define JW_RETROARCH_COMMAND_H

#include <stdbool.h>
#include <stddef.h>

#define JW_RA_DEFAULT_HOST "127.0.0.1"
#define JW_RA_DEFAULT_PORT 55355u
#define JW_RA_DEFAULT_TIMEOUT_MS 750u
#define JW_RA_REPLY_MAX 1024u

/* Shader-specific, because a first apply compiles and links on the GPU while
 * every other command answers immediately. Measured on MLP1 over 25 first
 * applies of the shipped recommendations plus one shader that fails to link:
 * round trip p50 98ms, p95 115ms, max 117ms (including ~13ms of process spawn
 * in the harness); the shader work alone is p50 85ms, p95 102ms, max 104ms.
 * 1000ms is roughly 9x the measured p95 and still bounds the worst-case UI
 * stall at one second. Unrelated commands keep JW_RA_DEFAULT_TIMEOUT_MS; there
 * is no evidence to change those. */
#define JW_RA_SHADER_TIMEOUT_MS 1000u

/* Bounded lowercase hex, generated per exchange. */
#define JW_RA_REQUEST_ID_MAX 17u

typedef enum {
    JW_RA_OK = 0,
    JW_RA_TIMEOUT,
    JW_RA_UNSUPPORTED,
    JW_RA_PARSE_ERROR,
    JW_RA_SOCKET_ERROR
} jw_ra_result;

typedef enum {
    JW_RA_STATE_UNKNOWN = 0,
    JW_RA_STATE_CONTENTLESS,
    JW_RA_STATE_PLAYING,
    JW_RA_STATE_PAUSED,
    JW_RA_STATE_MENU
} jw_ra_play_state;

typedef struct {
    const char *host;
    unsigned port;
    unsigned timeout_ms;
} jw_ra_client;

/* The four automatic-preset scopes RetroArch itself understands. Kept as an
 * enum so a caller cannot pass a path where a scope belongs. */
typedef enum {
    JW_RA_SHADER_SCOPE_GAME = 0,
    JW_RA_SHADER_SCOPE_PARENT,
    JW_RA_SHADER_SCOPE_CORE,
    JW_RA_SHADER_SCOPE_GLOBAL
} jw_ra_shader_scope;

/* What RetroArch reported. Distinct from jw_ra_result, which says whether the
 * exchange itself worked: a shader that fails to link is a successful exchange
 * carrying JW_RA_SHADER_ERR_APPLY. */
typedef enum {
    JW_RA_SHADER_OK = 0,
    JW_RA_SHADER_NONE,            /* GET: nothing is loaded */
    JW_RA_SHADER_ABSENT,          /* REMOVE: there was nothing to remove */
    JW_RA_SHADER_ERR_MISSING,     /* SET: the preset is not on disk */
    JW_RA_SHADER_ERR_UNSUPPORTED, /* SET: not a preset this driver can load */
    JW_RA_SHADER_ERR_APPLY,       /* SET/CLEAR: compile, link or apply failed */
    JW_RA_SHADER_ERR             /* SAVE/REMOVE failed */
} jw_ra_shader_outcome;

typedef struct {
    jw_ra_play_state state;
    char system[64];
    char content[256];
    char raw[JW_RA_REPLY_MAX];
} jw_ra_status;

typedef struct {
    int disk_count;
    int disk_slot;
    bool savestate_supported;
    int state_slot;
    char raw[JW_RA_REPLY_MAX];
} jw_ra_info;

const char *jw_ra_result_string(jw_ra_result result);
const char *jw_ra_play_state_string(jw_ra_play_state state);

jw_ra_client jw_ra_client_default(void);

bool jw_ra_raw_command_supported(const char *command);
jw_ra_result jw_ra_send_raw(const jw_ra_client *client, const char *command);
jw_ra_result jw_ra_request_raw(const jw_ra_client *client, const char *command,
                               char *reply, size_t reply_size);

jw_ra_result jw_ra_get_status(const jw_ra_client *client, jw_ra_status *status);
jw_ra_result jw_ra_get_info(const jw_ra_client *client, jw_ra_info *info);
jw_ra_result jw_ra_pause(const jw_ra_client *client);
jw_ra_result jw_ra_resume(const jw_ra_client *client);
jw_ra_result jw_ra_pause_direct(const jw_ra_client *client);
jw_ra_result jw_ra_resume_direct(const jw_ra_client *client);
jw_ra_result jw_ra_menu_toggle(const jw_ra_client *client);
jw_ra_result jw_ra_open_menu(const jw_ra_client *client);
jw_ra_result jw_ra_open_shader_menu(const jw_ra_client *client);
jw_ra_result jw_ra_quit(const jw_ra_client *client);
jw_ra_result jw_ra_reset(const jw_ra_client *client);
jw_ra_result jw_ra_audio_reinit(const jw_ra_client *client);
/* Ask RetroArch to write a screenshot of the current frame to its configured
 * screenshot_directory. Fire-and-forget; RA writes the PNG asynchronously. */
jw_ra_result jw_ra_screenshot(const jw_ra_client *client);
jw_ra_result jw_ra_save_state(const jw_ra_client *client);
jw_ra_result jw_ra_load_state(const jw_ra_client *client);
/* slot == -1 loads RetroArch's auto state by selecting slot -1, then using
 * LOAD_STATE. slot >= 0 uses LOAD_STATE_SLOT. */
jw_ra_result jw_ra_load_state_slot(const jw_ra_client *client, int slot,
                                   char *reply, size_t reply_size);
jw_ra_result jw_ra_set_state_slot(const jw_ra_client *client, int slot);
jw_ra_result jw_ra_get_state_slot(const jw_ra_client *client, int *out_slot,
                                  bool *out_supported);
jw_ra_result jw_ra_save_state_slot(const jw_ra_client *client, int slot,
                                   char *reply, size_t reply_size);
jw_ra_result jw_ra_state_slot_plus(const jw_ra_client *client);
jw_ra_result jw_ra_state_slot_minus(const jw_ra_client *client);
jw_ra_result jw_ra_get_disk_count(const jw_ra_client *client, int *out_count);
jw_ra_result jw_ra_get_disk_slot(const jw_ra_client *client, int *out_slot);
jw_ra_result jw_ra_set_disk_slot(const jw_ra_client *client, int slot);
jw_ra_result jw_ra_disk_eject_toggle(const jw_ra_client *client);
jw_ra_result jw_ra_disk_next(const jw_ra_client *client);
jw_ra_result jw_ra_disk_prev(const jw_ra_client *client);
jw_ra_result jw_ra_get_path(const jw_ra_client *client, const char *kind,
                            char *out, size_t out_size);
jw_ra_result jw_ra_get_savestate_path(const jw_ra_client *client,
                                      char *out, size_t out_size);
jw_ra_result jw_ra_show_message(const jw_ra_client *client, const char *message);
jw_ra_result jw_ra_load_content_current_core(const jw_ra_client *client,
                                             const char *content_path,
                                             char *reply, size_t reply_size);


/* Namespaced shader commands. Each generates one request ID, validates the
 * reply source, ignores replies carrying any other ID until a single absolute
 * deadline, and parses only the exact documented reply forms. */
jw_ra_result jw_ra_get_shader(const jw_ra_client *client,
                              jw_ra_shader_outcome *outcome,
                              char *path, size_t path_size);
jw_ra_result jw_ra_set_shader(const jw_ra_client *client,
                              const char *preset_path,
                              jw_ra_shader_outcome *outcome);
jw_ra_result jw_ra_clear_shader(const jw_ra_client *client,
                                jw_ra_shader_outcome *outcome);
jw_ra_result jw_ra_save_shader_preset(const jw_ra_client *client,
                                      jw_ra_shader_scope scope,
                                      jw_ra_shader_outcome *outcome);
jw_ra_result jw_ra_remove_shader_preset(const jw_ra_client *client,
                                        jw_ra_shader_scope scope,
                                        jw_ra_shader_outcome *outcome);

/* Exposed for tests. */
const char *jw_ra_shader_scope_token(jw_ra_shader_scope scope);
jw_ra_result jw_ra_parse_status_reply(const char *reply, jw_ra_status *status);
jw_ra_result jw_ra_parse_shader_reply(const char *reply,
                                      const char *request_id,
                                      const char *operation,
                                      jw_ra_shader_outcome *outcome,
                                      char *path, size_t path_size);

#endif
