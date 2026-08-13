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
/* Menu + L1 chord: take a screenshot. Same contract as the switcher — return
   true when consumed (proxy swallows L1 + the Menu tap), false to forward the
   events normally (e.g. the feature is disabled). Optional; leave NULL to skip.
   Assign directly on the proxy struct after init; it is read on the next tick. */
typedef bool (*jw_input_screenshot_cb)(void *userdata);

/* Menu + R1 chord: start or stop a game recording. Same contract as the
   screenshot hook -- return true to consume the chord, false to let R1 forward
   to the game as normal (no RetroArch session, feature off). Deliberately a
   Leaf-level chord rather than a RetroArch hotkey: RetroArch's own hotkeys need
   a modifier held on the pad, and it only blocks that modifier from the core for
   input_hotkey_block_delay frames, so Select leaks through to the game. Menu is
   never a game input and jawakad consumes the whole chord. */
typedef bool (*jw_input_record_cb)(void *userdata);
/* Force feedback played on the virtual gamepad. The pad advertises FF_RUMBLE, so
   any emulator that rumbles through SDL (or evdev directly) reaches the motor
   here instead of needing its own sysfs sink. `magnitude` is the effect's two
   channels collapsed to one 0..0xFFFF value -- the MLP1 has a single motor, so
   there is no strong/weak to drive separately -- and 0 means stop. `duration_ms`
   is the effect's replay length; 0 means "until stopped". Optional; assign
   directly on the proxy struct after init, like the screenshot hook. */
typedef void (*jw_input_rumble_cb)(void *userdata, uint16_t magnitude,
                                   uint32_t duration_ms);

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
    jw_input_screenshot_cb screenshot;
    jw_input_record_cb record;
    jw_input_rumble_cb rumble;
    void *userdata;
} jw_input_proxy;

int  jw_input_proxy_init(jw_input_proxy *proxy,
                         jw_input_brightness_delta_cb brightness_delta,
                         jw_input_volume_delta_cb volume_delta,
                         jw_input_menu_tap_cb menu_tap,
                         jw_input_game_switcher_cb game_switcher,
                         void *userdata);
/* Watch-only: observe the physical pad for hotkeys (volume/brightness/Menu)
   without grabbing it or creating a virtual device, so a standalone emulator
   reads the pad directly while jawakad keeps the hotkeys. */
int  jw_input_proxy_init_watch(jw_input_proxy *proxy,
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
/* Emit a release on the virtual pad for every button currently held-forwarded,
 * so a consumer reading it sees a neutral state. Call right before unpausing
 * RetroArch when closing the in-game menu: the button that triggered the menu
 * action (e.g. A on Save State) is often still physically down, and without this
 * the core reads it as a fresh in-game press on resume. evdev is edge-based, so
 * a still-held physical button will not re-fire until released and pressed again.
 * Safe to call when disabled. */
void     jw_input_proxy_release_buttons(jw_input_proxy *proxy);
/* Synthesise a Menu tap on the virtual pad, exactly as the handheld Menu button
 * produces when nothing inside the daemon claims it (no game running, so the
 * press belongs to the launcher UI). External controllers are watched without
 * being grabbed, so their Guide press cannot be forwarded the way the built-in
 * pad's is -- without this the UI never sees Guide at all. The matching release
 * is emitted by the proxy tick, so this is safe to call from an event callback.
 * No-op when the proxy is watch-only or a tap is already in flight. */
void     jw_input_proxy_emit_menu_tap(jw_input_proxy *proxy);
/* One power-key press or release edge, stamped with when the key actually moved
 * (kernel event time, CLOCK_MONOTONIC ms) — not when the daemon got around to
 * consuming it. The distinction matters for long-press detection: if a tick
 * stalls long enough for a press AND its release to queue up, consume-time
 * stamps would make any hold look like a short tap. */
typedef struct {
    bool     down;   /* true = press edge, false = release edge */
    uint64_t ms;     /* CLOCK_MONOTONIC ms of the edge */
} jw_power_edge;

/* The proxy grabs the power key exclusively at init (jawakad owns sleep/wake).
 * This pops the oldest unconsumed power-key edge into *edge and returns true,
 * or returns false when none are pending. The daemon routes them in order:
 * wake on press when the screen is off, sleep on release when it's on, clean
 * power-off when press→release spans the long-press threshold. Drain fully
 * once per tick. */
bool     jw_input_proxy_take_power_edge(jw_input_proxy *proxy, jw_power_edge *edge);

#endif /* JW_PLATFORM_INPUT_PROXY_H */
