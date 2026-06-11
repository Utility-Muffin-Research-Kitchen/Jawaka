#ifndef JW_PLATFORM_INPUT_PROXY_H
#define JW_PLATFORM_INPUT_PROXY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/* Milliseconds since the last physical button input (for auto-sleep idle
 * tracking). Counts EV_KEY only — stick drift (EV_ABS) doesn't reset it. */
uint64_t jw_input_proxy_idle_ms(const jw_input_proxy *proxy);
/* Reset the idle timer to "now" (e.g. after resuming from suspend, whose wake
 * key arrives outside the proxied input device). */
void     jw_input_proxy_mark_activity(jw_input_proxy *proxy);
/* Discard any pending input without forwarding it (e.g. on resume from suspend,
 * to drop button/d-pad presses that queued while asleep so they don't replay into
 * the launcher on wake). Safe to call when disabled. */
void     jw_input_proxy_flush(jw_input_proxy *proxy);
/* While "swallow" is on (the auto-sleep screen-off stage), gamepad input still
 * resets the idle timer (so a press wakes the screen) but is NOT forwarded to the
 * launcher — so the wake press only wakes the screen instead of also firing a
 * navigation action. Turn it off once the screen is back on. */
void     jw_input_proxy_set_swallow(jw_input_proxy *proxy, bool swallow);
/* The proxy grabs the power key exclusively at init (jawakad owns sleep/wake). This
 * reports the power-key press/release edges seen since the last call (and clears
 * them), for the daemon to route: wake on press when the screen is off, sleep on
 * release when it's on (so a long hold powers off via the PMIC without sleeping
 * first). Call once per tick. */
void     jw_input_proxy_take_power_edges(jw_input_proxy *proxy, bool *down, bool *up);

#endif /* JW_PLATFORM_INPUT_PROXY_H */
