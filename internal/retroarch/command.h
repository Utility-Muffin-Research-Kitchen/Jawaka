#ifndef JW_RETROARCH_COMMAND_H
#define JW_RETROARCH_COMMAND_H

#include <stdbool.h>
#include <stddef.h>

#define JW_RA_DEFAULT_HOST "127.0.0.1"
#define JW_RA_DEFAULT_PORT 55355u
#define JW_RA_DEFAULT_TIMEOUT_MS 750u
#define JW_RA_REPLY_MAX 1024u

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
    JW_RA_STATE_PAUSED
} jw_ra_play_state;

typedef struct {
    const char *host;
    unsigned port;
    unsigned timeout_ms;
} jw_ra_client;

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

#endif
