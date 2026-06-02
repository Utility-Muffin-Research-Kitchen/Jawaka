#ifndef JW_PLATFORM_INPUT_PROXY_H
#define JW_PLATFORM_INPUT_PROXY_H

#include <stdbool.h>
#include <stddef.h>

#define JW_INPUT_PROXY_MAX_PATH 256
#define JW_INPUT_PROXY_MAX_NAME 128

typedef void (*jw_input_brightness_delta_cb)(void *userdata, int delta_percent);
typedef void (*jw_input_volume_delta_cb)(void *userdata, int delta_percent);
typedef bool (*jw_input_menu_tap_cb)(void *userdata);
/* Menu + Select chord: open the in-game game switcher. Return true when the
   chord was consumed (the proxy then swallows Select and the Menu tap); return
   false to let the events forward normally. */
typedef bool (*jw_input_game_switcher_cb)(void *userdata);

typedef struct {
    bool enabled;
    char physical_event_path[JW_INPUT_PROXY_MAX_PATH];
    char virtual_event_path[JW_INPUT_PROXY_MAX_PATH];
    char device_name[JW_INPUT_PROXY_MAX_NAME];
    void *backend_data;
    jw_input_brightness_delta_cb brightness_delta;
    jw_input_volume_delta_cb volume_delta;
    jw_input_menu_tap_cb menu_tap;
    jw_input_game_switcher_cb game_switcher;
    void *userdata;
} jw_input_proxy;

int  jw_input_proxy_init(jw_input_proxy *proxy,
                         jw_input_brightness_delta_cb brightness_delta,
                         jw_input_volume_delta_cb volume_delta,
                         jw_input_menu_tap_cb menu_tap,
                         jw_input_game_switcher_cb game_switcher,
                         void *userdata);
int  jw_input_proxy_retroarch_joypad_index(const jw_input_proxy *proxy);
void jw_input_proxy_tick(jw_input_proxy *proxy);
void jw_input_proxy_shutdown(jw_input_proxy *proxy);

#endif /* JW_PLATFORM_INPUT_PROXY_H */
