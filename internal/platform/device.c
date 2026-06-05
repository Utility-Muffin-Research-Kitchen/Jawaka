#include "internal/platform/device.h"
#include "internal/platform/device_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int jw__platform_uses_dot_system(const char *platform_id) {
    return platform_id &&
           (strcmp(platform_id, "tg5040") == 0 ||
            strcmp(platform_id, "tg5050") == 0 ||
            strcmp(platform_id, "my355") == 0);
}

void jw_platform_result_set(jw_platform_result *out,
                            jw_platform_result_code code,
                            const char *message) {
    if (!out) {
        return;
    }
    out->code = code;
    snprintf(out->message, sizeof(out->message), "%s", message ? message : "");
    out->has_value = false;
    out->value = 0;
}

void jw_platform_result_set_value(jw_platform_result *out,
                                  jw_platform_result_code code,
                                  const char *message,
                                  int value) {
    jw_platform_result_set(out, code, message);
    if (out) {
        out->has_value = true;
        out->value = value;
    }
}

void jw_platform_result_unsupported(jw_platform_action action,
                                    const char *platform_id,
                                    jw_platform_result *out) {
    char message[JW_PLATFORM_MAX_MESSAGE];
    snprintf(message, sizeof(message), "%s is unsupported on %s",
             jw_platform_action_name(action), platform_id ? platform_id : "unknown");
    jw_platform_result_set(out, JW_PLATFORM_RESULT_UNSUPPORTED, message);
}

int jw_platform_init(jw_platform_context *ctx, const char *runtime_dir, const char *sdcard_root) {
    if (!ctx || !runtime_dir || !sdcard_root) {
        return -1;
    }

    const jw_platform_backend *backend = jw_platform_get_backend();
    if (!backend || !backend->platform_id || !backend->platform_name) {
        return -1;
    }

    memset(ctx, 0, sizeof(*ctx));
    snprintf(ctx->platform_id, sizeof(ctx->platform_id), "%s", backend->platform_id);
    snprintf(ctx->platform_name, sizeof(ctx->platform_name), "%s", backend->platform_name);
    snprintf(ctx->runtime_dir, sizeof(ctx->runtime_dir), "%s", runtime_dir);
    snprintf(ctx->sdcard_root, sizeof(ctx->sdcard_root), "%s", sdcard_root);
    ctx->capabilities = backend->capabilities;

    const char *script_env = getenv("JAWAKA_PLATFORM_SCRIPT_DIR");
    if (script_env && script_env[0]) {
        snprintf(ctx->script_dir, sizeof(ctx->script_dir), "%s", script_env);
    } else if ((script_env = getenv("UMRK_PLATFORM_PATH")) && script_env[0]) {
        snprintf(ctx->script_dir, sizeof(ctx->script_dir), "%s/platform.d", script_env);
    } else if ((script_env = getenv("SYSTEM_PATH")) && script_env[0]) {
        snprintf(ctx->script_dir, sizeof(ctx->script_dir), "%s/platform.d", script_env);
    } else {
        const char *prefix = jw__platform_uses_dot_system(ctx->platform_id)
            ? ".system"
            : "UMRK";
        snprintf(ctx->script_dir, sizeof(ctx->script_dir), "%s/%s/%s/platform.d",
                 sdcard_root, prefix, ctx->platform_id);
    }

    if (backend->init && backend->init(ctx) != 0) {
        return -1;
    }
    return 0;
}

void jw_platform_shutdown(jw_platform_context *ctx) {
    const jw_platform_backend *backend = jw_platform_get_backend();
    if (ctx && backend && backend->shutdown) {
        backend->shutdown(ctx);
    }
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

    const jw_platform_backend *backend = jw_platform_get_backend();
    if (backend && backend->get_status) {
        backend->get_status(ctx, out);
    }
}

void jw_platform_frontend_ready(jw_platform_context *ctx, const char *role, jw_platform_result *out) {
    if (!ctx || !role || !role[0]) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_INVALID, "missing frontend role");
        return;
    }

    const jw_platform_backend *backend = jw_platform_get_backend();
    if (backend && backend->frontend_ready) {
        backend->frontend_ready(ctx, role, out);
        return;
    }

    jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "frontend ready noted");
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
    } else if (strcmp(name, "set-auto-sleep") == 0) {
        *out = JW_PLATFORM_ACTION_SET_AUTO_SLEEP;
    } else if (strcmp(name, "screen-off") == 0) {
        *out = JW_PLATFORM_ACTION_SCREEN_OFF;
    } else if (strcmp(name, "screen-on") == 0) {
        *out = JW_PLATFORM_ACTION_SCREEN_ON;
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
        case JW_PLATFORM_ACTION_SET_AUTO_SLEEP: return "set-auto-sleep";
        case JW_PLATFORM_ACTION_SCREEN_OFF: return "screen-off";
        case JW_PLATFORM_ACTION_SCREEN_ON: return "screen-on";
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
    if (!ctx) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_INVALID, "platform not initialized");
        return;
    }

    const jw_platform_backend *backend = jw_platform_get_backend();
    if (backend && backend->perform_action) {
        backend->perform_action(ctx, action, value, out);
        return;
    }

    jw_platform_result_unsupported(action, ctx->platform_id, out);
}

bool jw_platform_storage_tick(jw_platform_context *ctx) {
    if (!ctx) {
        return false;
    }

    const jw_platform_backend *backend = jw_platform_get_backend();
    if (backend && backend->storage_tick) {
        return backend->storage_tick(ctx);
    }
    return false;
}

void jw_platform_get_storage_status(jw_platform_context *ctx, const char *source_id,
                                    jw_platform_storage_status *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    snprintf(out->source_id, sizeof(out->source_id), "%s",
             source_id && source_id[0] ? source_id : "secondary_sd");
    snprintf(out->label, sizeof(out->label), "%s", "Secondary SD");
    snprintf(out->message, sizeof(out->message), "%s", "storage source unavailable");

    if (!ctx) {
        return;
    }

    const jw_platform_backend *backend = jw_platform_get_backend();
    if (backend && backend->get_storage_status) {
        backend->get_storage_status(ctx, source_id, out);
    }
}

void jw_platform_safe_unmount_storage(jw_platform_context *ctx, const char *source_id,
                                      jw_platform_result *out) {
    if (!ctx) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_INVALID, "platform not initialized");
        return;
    }

    const jw_platform_backend *backend = jw_platform_get_backend();
    if (backend && backend->safe_unmount_storage) {
        backend->safe_unmount_storage(ctx, source_id, out);
        return;
    }

    jw_platform_result_set(out, JW_PLATFORM_RESULT_UNSUPPORTED,
                           "storage source unavailable");
}

void jw_platform_set_led(jw_platform_context *ctx, const jw_led_config *cfg,
                         jw_platform_result *out) {
    if (!ctx) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_INVALID, "platform not initialized");
        return;
    }
    const jw_platform_backend *backend = jw_platform_get_backend();
    if (backend && backend->set_led) {
        backend->set_led(ctx, cfg, out);
        return;
    }
    jw_platform_result_set(out, JW_PLATFORM_RESULT_UNSUPPORTED, "led not supported");
}
