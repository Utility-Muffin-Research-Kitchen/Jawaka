#include "internal/platform/input_proxy.h"

#include <string.h>

int jw_input_proxy_init(jw_input_proxy *proxy,
                        jw_input_brightness_delta_cb brightness_delta,
                        jw_input_volume_delta_cb volume_delta,
                        void *userdata) {
    if (!proxy) {
        return -1;
    }
    memset(proxy, 0, sizeof(*proxy));
    proxy->brightness_delta = brightness_delta;
    proxy->volume_delta = volume_delta;
    proxy->userdata = userdata;
    return 0;
}

int jw_input_proxy_retroarch_joypad_index(const jw_input_proxy *proxy) {
    (void)proxy;
    return -1;
}

void jw_input_proxy_tick(jw_input_proxy *proxy) {
    (void)proxy;
}

void jw_input_proxy_shutdown(jw_input_proxy *proxy) {
    if (proxy) {
        memset(proxy, 0, sizeof(*proxy));
    }
}
