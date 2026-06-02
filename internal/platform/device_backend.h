#ifndef JW_PLATFORM_DEVICE_BACKEND_H
#define JW_PLATFORM_DEVICE_BACKEND_H

#include "internal/platform/device.h"

typedef struct {
    const char *platform_id;
    const char *platform_name;
    jw_platform_capabilities capabilities;

    int  (*init)(jw_platform_context *ctx);
    void (*shutdown)(jw_platform_context *ctx);
    void (*get_status)(jw_platform_context *ctx, jw_platform_status *out);
    void (*frontend_ready)(jw_platform_context *ctx, const char *role,
                           jw_platform_result *out);
    void (*perform_action)(jw_platform_context *ctx, jw_platform_action action,
                           int value, jw_platform_result *out);
    void (*set_led)(jw_platform_context *ctx, const jw_led_config *cfg,
                    jw_platform_result *out);
} jw_platform_backend;

const jw_platform_backend *jw_platform_get_backend(void);

void jw_platform_result_set(jw_platform_result *out,
                            jw_platform_result_code code,
                            const char *message);
void jw_platform_result_set_value(jw_platform_result *out,
                                  jw_platform_result_code code,
                                  const char *message,
                                  int value);
void jw_platform_result_unsupported(jw_platform_action action,
                                    const char *platform_id,
                                    jw_platform_result *out);

#endif /* JW_PLATFORM_DEVICE_BACKEND_H */
