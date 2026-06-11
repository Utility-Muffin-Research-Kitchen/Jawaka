#include "internal/platform/input_proxy.h"

#include <string.h>

int jw_input_proxy_init(jw_input_proxy *proxy,
                        jw_input_brightness_delta_cb brightness_delta,
                        jw_input_volume_delta_cb volume_delta,
                        jw_input_menu_tap_cb menu_tap,
                        jw_input_game_switcher_cb game_switcher,
                        void *userdata) {
    if (!proxy) {
        return -1;
    }
    memset(proxy, 0, sizeof(*proxy));
    proxy->brightness_delta = brightness_delta;
    proxy->volume_delta = volume_delta;
    proxy->menu_tap = menu_tap;
    proxy->game_switcher = game_switcher;
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

uint64_t jw_input_proxy_idle_ms(const jw_input_proxy *proxy) {
    (void)proxy;
    return 0;   /* mock: never idle (auto-sleep is a no-op off-device) */
}

void jw_input_proxy_mark_activity(jw_input_proxy *proxy) {
    (void)proxy;
}

void jw_input_proxy_flush(jw_input_proxy *proxy) {
    (void)proxy;
}

void jw_input_proxy_set_swallow(jw_input_proxy *proxy, bool swallow) {
    (void)proxy;
    (void)swallow;
}

void jw_input_proxy_take_power_edges(jw_input_proxy *proxy, bool *down, bool *up) {
    (void)proxy;
    if (down) *down = false;
    if (up) *up = false;
}

void jw_input_proxy_shutdown(jw_input_proxy *proxy) {
    if (proxy) {
        memset(proxy, 0, sizeof(*proxy));
    }
}
