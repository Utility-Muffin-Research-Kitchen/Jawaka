#ifndef JW_PLATFORM_DEVICE_H
#define JW_PLATFORM_DEVICE_H

#include <stdbool.h>
#include <stddef.h>

#include "internal/platform/platform_id.h"

#define JW_PLATFORM_MAX_PATH    4096
#define JW_PLATFORM_MAX_MESSAGE 256
#define JW_PLATFORM_BRIGHTNESS_MIN_PERCENT 5
#define JW_PLATFORM_BRIGHTNESS_MAX_PERCENT 100
#define JW_PLATFORM_BRIGHTNESS_STEP_PERCENT 5
#define JW_PLATFORM_VOLUME_STEP_PERCENT 5

#define JW_LED_BRIGHTNESS_MAX 10
#define JW_LED_SPEED_MAX 10

#define JW_PLATFORM_AUDIO_OUTPUT_COUNT 4
#define JW_PLATFORM_PERF_DOMAIN_COUNT 3
#define JW_PLATFORM_PERF_VALUE_MAX 64
#define JW_PLATFORM_PERF_LIST_MAX 512

typedef enum {
    /* Stock modes are driven by the active platform LED backend. */
    JW_LED_MODE_STATIC = 0,   /* solid color */
    JW_LED_MODE_BREATH,       /* pulse the color */
    JW_LED_MODE_RAINBOW,      /* cycle hues (color ignored) */
    /* Custom effects may be driven by a platform-specific helper. */
    JW_LED_MODE_COMET,
    JW_LED_MODE_SWEEP,
    JW_LED_MODE_FOUNTAIN,
    JW_LED_MODE_HICCUP,
    JW_LED_MODE_COUNT
} jw_led_mode;

/* True for custom effects rather than stock platform LED modes. */
#define jw_led_mode_is_effect(m) ((m) >= JW_LED_MODE_COMET && (m) < JW_LED_MODE_COUNT)

typedef enum {
    JW_PLATFORM_AUDIO_OUTPUT_UNKNOWN = -1,
    JW_PLATFORM_AUDIO_OUTPUT_SPEAKER = 0,
    JW_PLATFORM_AUDIO_OUTPUT_HEADSET,
    JW_PLATFORM_AUDIO_OUTPUT_HDMI,
    JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH
} jw_platform_audio_output;

#define JW_PLATFORM_AUDIO_OUTPUT_BIT(o) (1u << (unsigned)(o))

typedef struct {
    bool enabled;
    jw_led_mode mode;
    unsigned char r, g, b;    /* color for static/breath */
    int brightness;           /* 0..JW_LED_BRIGHTNESS_MAX */
    int speed;                /* 0..JW_LED_SPEED_MAX (breath/rainbow) */
} jw_led_config;

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
    bool adb;
    bool boot_splash;
    bool led;
    bool performance;
} jw_platform_capabilities;

typedef struct {
    char platform_id[32];
    char platform_name[64];
    char runtime_dir[JW_PLATFORM_MAX_PATH];
    char sdcard_root[JW_PLATFORM_MAX_PATH];
    char script_dir[JW_PLATFORM_MAX_PATH];
    jw_platform_capabilities capabilities;
    bool home_ready_sent;
    void *backend_data;
} jw_platform_context;

typedef struct {
    int battery_percent;       /* -1 when unknown */
    int charging;              /* -1 unknown, 0 no, 1 yes */
    int brightness_percent;    /* -1 when unknown */
    int volume_percent;        /* -1 when unknown */
    jw_platform_audio_output audio_output;
    unsigned audio_available_outputs;  /* bitmask of JW_PLATFORM_AUDIO_OUTPUT_BIT(output) */
    int audio_volume_percent[JW_PLATFORM_AUDIO_OUTPUT_COUNT]; /* -1 when unknown */
    int wifi_connected;        /* -1 unknown, 0 no, 1 yes */
    int wifi_strength;         /* -1 unknown, 0 off/disconnected, 1..3 strength */
    int bluetooth_connected;   /* -1 unknown, 0 no, 1 yes */
    int adb_enabled;           /* -1 unknown/unavailable, 0 no, 1 yes */
    int adb_intent_enabled;    /* -1 unknown/unavailable, 0 no, 1 yes */
    int boot_splash_enabled;   /* -1 unknown/unavailable, 0 no, 1 yes */
} jw_platform_status;

typedef struct {
    char source_id[32];
    char label[64];
    char mount_path[JW_PLATFORM_MAX_PATH];
    char device_path[JW_PLATFORM_MAX_PATH];
    char message[JW_PLATFORM_MAX_MESSAGE];
    bool present;
    bool mounted;
    bool busy;
    bool can_unmount;
} jw_platform_storage_status;

typedef enum {
    JW_PLATFORM_PERF_DOMAIN_CPU = 0,
    JW_PLATFORM_PERF_DOMAIN_GPU,
    JW_PLATFORM_PERF_DOMAIN_DMC
} jw_platform_perf_domain;

typedef enum {
    JW_PLATFORM_PERF_PROFILE_AUTO = 0,
    JW_PLATFORM_PERF_PROFILE_FRONTEND,
    JW_PLATFORM_PERF_PROFILE_BALANCED,
    JW_PLATFORM_PERF_PROFILE_PERFORMANCE,
    JW_PLATFORM_PERF_PROFILE_BATTERY_SAVER,
    JW_PLATFORM_PERF_PROFILE_SLEEP,
    JW_PLATFORM_PERF_PROFILE_CUSTOM,
    JW_PLATFORM_PERF_PROFILE_COUNT
} jw_platform_perf_profile;

typedef struct {
    bool supported;
    char name[JW_PLATFORM_PERF_VALUE_MAX];
    char governor[JW_PLATFORM_PERF_VALUE_MAX];
    int current_freq;
    int set_freq;
    char available_governors[JW_PLATFORM_PERF_LIST_MAX];
    char available_frequencies[JW_PLATFORM_PERF_LIST_MAX];
} jw_platform_perf_domain_status;

typedef struct {
    bool supported;
    int soc_temp_c;
    char message[JW_PLATFORM_MAX_MESSAGE];
    jw_platform_perf_domain_status domains[JW_PLATFORM_PERF_DOMAIN_COUNT];
} jw_platform_perf_status;

typedef struct {
    char governor[JW_PLATFORM_PERF_VALUE_MAX];
    int frequency; /* -1 = no explicit userspace frequency */
} jw_platform_perf_domain_request;

typedef struct {
    jw_platform_perf_domain_request domains[JW_PLATFORM_PERF_DOMAIN_COUNT];
} jw_platform_perf_request;

typedef enum {
    JW_PLATFORM_ACTION_SLEEP = 0,
    JW_PLATFORM_ACTION_POWEROFF,
    JW_PLATFORM_ACTION_REBOOT,
    JW_PLATFORM_ACTION_SET_BRIGHTNESS,
    JW_PLATFORM_ACTION_SET_VOLUME,
    JW_PLATFORM_ACTION_WIFI_ON,
    JW_PLATFORM_ACTION_WIFI_OFF,
    JW_PLATFORM_ACTION_BLUETOOTH_ON,
    JW_PLATFORM_ACTION_BLUETOOTH_OFF,
    JW_PLATFORM_ACTION_SET_AUDIO_OUTPUT,
    JW_PLATFORM_ACTION_SET_AUTO_SLEEP,
    JW_PLATFORM_ACTION_SCREEN_OFF,   /* blank the backlight (display stays composed) */
    JW_PLATFORM_ACTION_SCREEN_ON,    /* unblank the backlight */
    JW_PLATFORM_ACTION_ENABLE_ADB,
    JW_PLATFORM_ACTION_DISABLE_ADB,
    JW_PLATFORM_ACTION_SET_BOOT_SPLASH
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
    bool has_value;
    int value;
} jw_platform_result;

int  jw_platform_init(jw_platform_context *ctx, const char *runtime_dir, const char *sdcard_root);
void jw_platform_shutdown(jw_platform_context *ctx);
void jw_platform_get_status(jw_platform_context *ctx, jw_platform_status *out);
void jw_platform_get_audio_status(jw_platform_context *ctx, jw_platform_status *out);
void jw_platform_frontend_ready(jw_platform_context *ctx, const char *role, jw_platform_result *out);

bool jw_platform_parse_action(const char *name, jw_platform_action *out);
const char *jw_platform_action_name(jw_platform_action action);
bool jw_platform_parse_audio_output(const char *name, jw_platform_audio_output *out);
const char *jw_platform_audio_output_name(jw_platform_audio_output output);
const char *jw_platform_audio_output_label(jw_platform_audio_output output);
bool jw_platform_parse_perf_profile(const char *name, jw_platform_perf_profile *out);
const char *jw_platform_perf_profile_name(jw_platform_perf_profile profile);
const char *jw_platform_perf_profile_label(jw_platform_perf_profile profile);
const char *jw_platform_result_code_name(jw_platform_result_code code);
int  jw_platform_clamp_brightness_percent(int percent);
void jw_platform_perform_action(jw_platform_context *ctx, jw_platform_action action,
                                int value, jw_platform_result *out);
void jw_platform_get_performance_status(jw_platform_context *ctx,
                                        jw_platform_perf_status *out);
void jw_platform_apply_performance(jw_platform_context *ctx,
                                   const jw_platform_perf_request *request,
                                   jw_platform_result *out);
bool jw_platform_storage_tick(jw_platform_context *ctx);
void jw_platform_get_storage_status(jw_platform_context *ctx, const char *source_id,
                                    jw_platform_storage_status *out);
void jw_platform_safe_unmount_storage(jw_platform_context *ctx, const char *source_id,
                                      jw_platform_result *out);

const char *jw_led_mode_name(jw_led_mode mode);     /* "FOREVER"/"BREATH"/"RAINBOW" */
bool        jw_led_mode_parse(const char *name, jw_led_mode *out);
void jw_platform_set_led(jw_platform_context *ctx, const jw_led_config *cfg,
                         jw_platform_result *out);

#endif /* JW_PLATFORM_DEVICE_H */
