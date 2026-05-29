#include "internal/platform/device_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int brightness_percent;
} jw_mock_platform_data;

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
    if (parsed < min_value) parsed = min_value;
    if (parsed > max_value) parsed = max_value;
    return (int)parsed;
}

static jw_mock_platform_data *jw__mock_data(jw_platform_context *ctx) {
    return ctx ? (jw_mock_platform_data *)ctx->backend_data : NULL;
}

static int jw__mock_init(jw_platform_context *ctx) {
    jw_mock_platform_data *data = (jw_mock_platform_data *)calloc(1, sizeof(*data));
    if (!data) {
        return -1;
    }

    data->brightness_percent = jw__env_int("CAT_PREVIEW_BRIGHTNESS_PERCENT",
                                           JW_PLATFORM_BRIGHTNESS_MIN_PERCENT,
                                           JW_PLATFORM_BRIGHTNESS_MAX_PERCENT);
    if (data->brightness_percent < 0) {
        data->brightness_percent = 50;
    }
    ctx->backend_data = data;
    return 0;
}

static void jw__mock_shutdown(jw_platform_context *ctx) {
    free(ctx ? ctx->backend_data : NULL);
    if (ctx) {
        ctx->backend_data = NULL;
    }
}

static void jw__mock_get_status(jw_platform_context *ctx, jw_platform_status *out) {
    jw_mock_platform_data *data = jw__mock_data(ctx);
    if (!data || !out) {
        return;
    }

    out->battery_percent = jw__env_int("CAT_PREVIEW_BATTERY_PERCENT", 0, 100);
    int charging = jw__env_int("CAT_PREVIEW_CHARGING", 0, 1);
    if (charging >= 0) {
        out->charging = charging ? 1 : 0;
    }
    out->brightness_percent = data->brightness_percent;
}

static void jw__mock_frontend_ready(jw_platform_context *ctx, const char *role,
                                    jw_platform_result *out) {
    (void)role;
    if (ctx) {
        ctx->home_ready_sent = true;
    }
    jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "frontend ready noted");
}

static void jw__mock_perform_action(jw_platform_context *ctx, jw_platform_action action,
                                    int value, jw_platform_result *out) {
    jw_mock_platform_data *data = jw__mock_data(ctx);
    if (action != JW_PLATFORM_ACTION_SET_BRIGHTNESS) {
        jw_platform_result_unsupported(action, ctx ? ctx->platform_id : "mac", out);
        return;
    }
    if (!data) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_INVALID, "platform not initialized");
        return;
    }

    int percent = jw_platform_clamp_brightness_percent(value);
    data->brightness_percent = percent;

    char message[JW_PLATFORM_MAX_MESSAGE];
    snprintf(message, sizeof(message), "brightness preview set to %d%%", percent);
    jw_platform_result_set_value(out, JW_PLATFORM_RESULT_OK, message, percent);
}

const jw_platform_backend *jw_platform_get_backend(void) {
    static const jw_platform_backend backend = {
        .platform_id = "mac",
        .platform_name = "Mac Preview",
        .capabilities = {
            .battery = true,
            .charging = true,
            .brightness = true,
        },
        .init = jw__mock_init,
        .shutdown = jw__mock_shutdown,
        .get_status = jw__mock_get_status,
        .frontend_ready = jw__mock_frontend_ready,
        .perform_action = jw__mock_perform_action,
    };
    return &backend;
}
