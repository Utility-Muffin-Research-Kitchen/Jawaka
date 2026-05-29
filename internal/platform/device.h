#ifndef JW_PLATFORM_DEVICE_H
#define JW_PLATFORM_DEVICE_H

#include <stdbool.h>
#include <stddef.h>

#define JW_PLATFORM_MAX_PATH    4096
#define JW_PLATFORM_MAX_MESSAGE 256

typedef struct {
    bool battery;
    bool charging;
    bool sleep;
    bool poweroff;
    bool reboot;
    bool brightness;
    bool volume;
    bool wifi;
    bool bluetooth;
} jw_platform_capabilities;

typedef struct {
    char platform_id[32];
    char platform_name[64];
    char runtime_dir[JW_PLATFORM_MAX_PATH];
    char sdcard_root[JW_PLATFORM_MAX_PATH];
    char script_dir[JW_PLATFORM_MAX_PATH];
    jw_platform_capabilities capabilities;
    bool home_ready_sent;
} jw_platform_context;

typedef struct {
    int battery_percent;       /* -1 when unknown */
    int charging;              /* -1 unknown, 0 no, 1 yes */
    int brightness_percent;    /* -1 when unknown */
    int volume_percent;        /* -1 when unknown */
    int wifi_connected;        /* -1 unknown, 0 no, 1 yes */
    int wifi_strength;         /* -1 unknown, 0 off/disconnected, 1..3 strength */
    int bluetooth_connected;   /* -1 unknown, 0 no, 1 yes */
} jw_platform_status;

typedef enum {
    JW_PLATFORM_ACTION_SLEEP = 0,
    JW_PLATFORM_ACTION_POWEROFF,
    JW_PLATFORM_ACTION_REBOOT,
    JW_PLATFORM_ACTION_SET_BRIGHTNESS,
    JW_PLATFORM_ACTION_SET_VOLUME,
    JW_PLATFORM_ACTION_WIFI_ON,
    JW_PLATFORM_ACTION_WIFI_OFF,
    JW_PLATFORM_ACTION_BLUETOOTH_ON,
    JW_PLATFORM_ACTION_BLUETOOTH_OFF
} jw_platform_action;

typedef enum {
    JW_PLATFORM_RESULT_OK = 0,
    JW_PLATFORM_RESULT_UNSUPPORTED,
    JW_PLATFORM_RESULT_UNAVAILABLE,
    JW_PLATFORM_RESULT_FAILED,
    JW_PLATFORM_RESULT_INVALID
} jw_platform_result_code;

typedef struct {
    jw_platform_result_code code;
    char message[JW_PLATFORM_MAX_MESSAGE];
} jw_platform_result;

static inline const char *jw_platform_compiled_id(void) {
#if defined(PLATFORM_MLP1)
    return "mlp1";
#else
    return "mac";
#endif
}

int  jw_platform_init(jw_platform_context *ctx, const char *runtime_dir, const char *sdcard_root);
void jw_platform_get_status(jw_platform_context *ctx, jw_platform_status *out);
void jw_platform_frontend_ready(jw_platform_context *ctx, const char *role, jw_platform_result *out);

bool jw_platform_parse_action(const char *name, jw_platform_action *out);
const char *jw_platform_action_name(jw_platform_action action);
const char *jw_platform_result_code_name(jw_platform_result_code code);
void jw_platform_perform_action(jw_platform_context *ctx, jw_platform_action action,
                                int value, jw_platform_result *out);

#endif /* JW_PLATFORM_DEVICE_H */
