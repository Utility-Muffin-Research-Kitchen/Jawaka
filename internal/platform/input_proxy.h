#ifndef JW_PLATFORM_INPUT_PROXY_H
#define JW_PLATFORM_INPUT_PROXY_H

#include <stdbool.h>
#include <stddef.h>

#define JW_INPUT_PROXY_MAX_PATH 256

typedef void (*jw_input_brightness_delta_cb)(void *userdata, int delta_percent);

typedef struct {
    bool enabled;
    char virtual_event_path[JW_INPUT_PROXY_MAX_PATH];
    void *backend_data;
    jw_input_brightness_delta_cb brightness_delta;
    void *userdata;
} jw_input_proxy;

int  jw_input_proxy_init(jw_input_proxy *proxy,
                         jw_input_brightness_delta_cb brightness_delta,
                         void *userdata);
void jw_input_proxy_tick(jw_input_proxy *proxy);
void jw_input_proxy_shutdown(jw_input_proxy *proxy);

#endif /* JW_PLATFORM_INPUT_PROXY_H */
