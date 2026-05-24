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

const char *jw_ra_result_string(jw_ra_result result);
const char *jw_ra_play_state_string(jw_ra_play_state state);

jw_ra_client jw_ra_client_default(void);

bool jw_ra_raw_command_supported(const char *command);
jw_ra_result jw_ra_send_raw(const jw_ra_client *client, const char *command);
jw_ra_result jw_ra_request_raw(const jw_ra_client *client, const char *command,
                               char *reply, size_t reply_size);

jw_ra_result jw_ra_get_status(const jw_ra_client *client, jw_ra_status *status);
jw_ra_result jw_ra_pause(const jw_ra_client *client);
jw_ra_result jw_ra_resume(const jw_ra_client *client);
jw_ra_result jw_ra_menu_toggle(const jw_ra_client *client);
jw_ra_result jw_ra_quit(const jw_ra_client *client);
jw_ra_result jw_ra_save_state(const jw_ra_client *client);
jw_ra_result jw_ra_load_state(const jw_ra_client *client);
jw_ra_result jw_ra_load_state_slot(const jw_ra_client *client, int slot,
                                   char *reply, size_t reply_size);
jw_ra_result jw_ra_set_state_slot(const jw_ra_client *client, int slot);
jw_ra_result jw_ra_state_slot_plus(const jw_ra_client *client);
jw_ra_result jw_ra_state_slot_minus(const jw_ra_client *client);
jw_ra_result jw_ra_disk_eject_toggle(const jw_ra_client *client);
jw_ra_result jw_ra_disk_next(const jw_ra_client *client);
jw_ra_result jw_ra_disk_prev(const jw_ra_client *client);
jw_ra_result jw_ra_show_message(const jw_ra_client *client, const char *message);

#endif
