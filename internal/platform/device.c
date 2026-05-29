#include "internal/platform/device.h"
#include "internal/core/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(PLATFORM_MLP1)
#include <dlfcn.h>
#endif

static void jw__platform_result_set(jw_platform_result *out,
                                    jw_platform_result_code code,
                                    const char *message) {
    if (!out) {
        return;
    }
    out->code = code;
    snprintf(out->message, sizeof(out->message), "%s", message ? message : "");
}

#if defined(PLATFORM_MLP1)
static int jw__read_int_file(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    int value = -1;
    if (fscanf(fp, "%d", &value) != 1) {
        value = -1;
    }
    fclose(fp);
    return value;
}
#endif

#if !defined(PLATFORM_MLP1)
static int jw__env_int(const char *name, int min_value, int max_value) {
    const char *value = getenv(name);
    if (!value || !value[0]) {
        return -1;
    }

    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || (end && *end != '\0')) {
        return -1;
    }
    if (parsed < min_value) {
        parsed = min_value;
    }
    if (parsed > max_value) {
        parsed = max_value;
    }
    return (int)parsed;
}
#endif

int jw_platform_init(jw_platform_context *ctx, const char *runtime_dir, const char *sdcard_root) {
    if (!ctx || !runtime_dir || !sdcard_root) {
        return -1;
    }

    memset(ctx, 0, sizeof(*ctx));
    snprintf(ctx->platform_id, sizeof(ctx->platform_id), "%s", jw_platform_compiled_id());
    snprintf(ctx->runtime_dir, sizeof(ctx->runtime_dir), "%s", runtime_dir);
    snprintf(ctx->sdcard_root, sizeof(ctx->sdcard_root), "%s", sdcard_root);

#if defined(PLATFORM_MLP1)
    snprintf(ctx->platform_name, sizeof(ctx->platform_name), "%s", "Miniloong Pocket 1");
    ctx->capabilities.battery = true;
    ctx->capabilities.charging = true;
#else
    snprintf(ctx->platform_name, sizeof(ctx->platform_name), "%s", "Mac Preview");
    ctx->capabilities.battery = true;
    ctx->capabilities.charging = true;
#endif

    const char *script_env = getenv("JAWAKA_PLATFORM_SCRIPT_DIR");
    if (script_env && script_env[0]) {
        snprintf(ctx->script_dir, sizeof(ctx->script_dir), "%s", script_env);
    } else {
        snprintf(ctx->script_dir, sizeof(ctx->script_dir), "%s/UMRK/%s/platform.d",
                 sdcard_root, ctx->platform_id);
    }

    return 0;
}

void jw_platform_get_status(jw_platform_context *ctx, jw_platform_status *out) {
    if (!ctx || !out) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->battery_percent = -1;
    out->charging = -1;
    out->brightness_percent = -1;
    out->volume_percent = -1;
    out->wifi_connected = -1;
    out->wifi_strength = -1;
    out->bluetooth_connected = -1;

#if defined(PLATFORM_MLP1)
    int battery = jw__read_int_file("/sys/class/power_supply/battery/capacity");
    if (battery >= 0 && battery <= 100) {
        out->battery_percent = battery;
    }

    int charger = jw__read_int_file("/sys/class/power_supply/ac/online");
    if (charger != 1) {
        charger = jw__read_int_file("/sys/class/power_supply/usb/online");
    }
    if (charger >= 0) {
        out->charging = (charger == 1) ? 1 : 0;
    }
#else
    out->battery_percent = jw__env_int("CAT_PREVIEW_BATTERY_PERCENT", 0, 100);
    int charging = jw__env_int("CAT_PREVIEW_CHARGING", 0, 1);
    if (charging >= 0) {
        out->charging = charging ? 1 : 0;
    }
#endif
}

#if defined(PLATFORM_MLP1)
static int (*s_event_opend)(const char *id);
static bool s_loong_loaded;

static void jw__loong_load(void) {
    if (s_loong_loaded) {
        return;
    }
    s_loong_loaded = true;

    void *handle = dlopen("/usr/lib/libloong_sdk.so", RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        jw_log_error("loong: dlopen libloong_sdk.so failed: %s", dlerror());
        return;
    }

    *(void **)(&s_event_opend) = dlsym(handle, "EventOpend");
    if (!s_event_opend) {
        jw_log_error("loong: EventOpend symbol not found: %s", dlerror());
    }
}

static void jw__mlp1_home_ready(jw_platform_context *ctx, jw_platform_result *out) {
    if (ctx->home_ready_sent) {
        jw__platform_result_set(out, JW_PLATFORM_RESULT_OK, "home ready already sent");
        return;
    }

    jw__loong_load();
    if (!s_event_opend) {
        jw__platform_result_set(out, JW_PLATFORM_RESULT_UNAVAILABLE,
                                "loong SDK EventOpend unavailable");
        return;
    }

    int rc = s_event_opend("HOME");
    jw_log_info("loong: EventOpend(\"HOME\") -> %d (dismissing boot transition)", rc);
    ctx->home_ready_sent = true;
    jw__platform_result_set(out, JW_PLATFORM_RESULT_OK, "home ready sent");
}
#endif

void jw_platform_frontend_ready(jw_platform_context *ctx, const char *role, jw_platform_result *out) {
    if (!ctx || !role || !role[0]) {
        jw__platform_result_set(out, JW_PLATFORM_RESULT_INVALID, "missing frontend role");
        return;
    }

    if (strcmp(role, "launcher") != 0) {
        jw__platform_result_set(out, JW_PLATFORM_RESULT_OK, "frontend ready noted");
        return;
    }

#if defined(PLATFORM_MLP1)
    jw__mlp1_home_ready(ctx, out);
#else
    ctx->home_ready_sent = true;
    jw__platform_result_set(out, JW_PLATFORM_RESULT_OK, "frontend ready noted");
#endif
}

bool jw_platform_parse_action(const char *name, jw_platform_action *out) {
    if (!name || !out) {
        return false;
    }

    if (strcmp(name, "sleep") == 0) {
        *out = JW_PLATFORM_ACTION_SLEEP;
    } else if (strcmp(name, "poweroff") == 0) {
        *out = JW_PLATFORM_ACTION_POWEROFF;
    } else if (strcmp(name, "reboot") == 0) {
        *out = JW_PLATFORM_ACTION_REBOOT;
    } else if (strcmp(name, "set-brightness") == 0) {
        *out = JW_PLATFORM_ACTION_SET_BRIGHTNESS;
    } else if (strcmp(name, "set-volume") == 0) {
        *out = JW_PLATFORM_ACTION_SET_VOLUME;
    } else if (strcmp(name, "wifi-on") == 0) {
        *out = JW_PLATFORM_ACTION_WIFI_ON;
    } else if (strcmp(name, "wifi-off") == 0) {
        *out = JW_PLATFORM_ACTION_WIFI_OFF;
    } else if (strcmp(name, "bluetooth-on") == 0) {
        *out = JW_PLATFORM_ACTION_BLUETOOTH_ON;
    } else if (strcmp(name, "bluetooth-off") == 0) {
        *out = JW_PLATFORM_ACTION_BLUETOOTH_OFF;
    } else {
        return false;
    }

    return true;
}

const char *jw_platform_action_name(jw_platform_action action) {
    switch (action) {
        case JW_PLATFORM_ACTION_SLEEP: return "sleep";
        case JW_PLATFORM_ACTION_POWEROFF: return "poweroff";
        case JW_PLATFORM_ACTION_REBOOT: return "reboot";
        case JW_PLATFORM_ACTION_SET_BRIGHTNESS: return "set-brightness";
        case JW_PLATFORM_ACTION_SET_VOLUME: return "set-volume";
        case JW_PLATFORM_ACTION_WIFI_ON: return "wifi-on";
        case JW_PLATFORM_ACTION_WIFI_OFF: return "wifi-off";
        case JW_PLATFORM_ACTION_BLUETOOTH_ON: return "bluetooth-on";
        case JW_PLATFORM_ACTION_BLUETOOTH_OFF: return "bluetooth-off";
        default: return "unknown";
    }
}

const char *jw_platform_result_code_name(jw_platform_result_code code) {
    switch (code) {
        case JW_PLATFORM_RESULT_OK: return "ok";
        case JW_PLATFORM_RESULT_UNSUPPORTED: return "unsupported";
        case JW_PLATFORM_RESULT_UNAVAILABLE: return "unavailable";
        case JW_PLATFORM_RESULT_FAILED: return "failed";
        case JW_PLATFORM_RESULT_INVALID: return "invalid";
        default: return "unknown";
    }
}

void jw_platform_perform_action(jw_platform_context *ctx, jw_platform_action action,
                                int value, jw_platform_result *out) {
    (void)value;

    if (!ctx) {
        jw__platform_result_set(out, JW_PLATFORM_RESULT_INVALID, "platform not initialized");
        return;
    }

    char message[JW_PLATFORM_MAX_MESSAGE];
    snprintf(message, sizeof(message), "%s is unsupported on %s",
             jw_platform_action_name(action), ctx->platform_id);
    jw__platform_result_set(out, JW_PLATFORM_RESULT_UNSUPPORTED, message);
}
