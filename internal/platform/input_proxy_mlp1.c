/* pipe2, for an atomically close-on-exec pipe. Plain pipe() plus fcntl would
   leave a window in which a fork on the daemon's main thread inherits the fds,
   which is the race this is closing. Same pattern as legacy_migration.c. */
#define _GNU_SOURCE

#include "internal/platform/input_proxy.h"
#include "internal/platform/calibration.h"
#include "internal/core/log.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define JW_MLP1_INPUT_NAME "Loong Gamepad"
#define JW_MLP1_PWRKEY_NAME "rk805 pwrkey"   /* physical power button (KEY_POWER) */
#define JW_MLP1_BRIGHTNESS_REPEAT_MS 120u
#define JW_MLP1_MENU_TAP_MS 80u
#define JW_MLP1_POWER_EDGE_MAX 8   /* pending press/release edges (4 full taps) */
/* Force-feedback slots the virtual pad offers. SDL only ever holds one rumble
   effect per joystick, but a client may re-upload before erasing the old one, so
   leave headroom rather than making a re-upload fail. */
#define JW_MLP1_FF_EFFECTS_MAX 8

/* Older kernel uapi headers predate the y2038 input_event_sec accessors. */
#ifndef input_event_sec
#define input_event_sec  time.tv_sec
#define input_event_usec time.tv_usec
#endif

typedef struct {
    int input_fd;
    int uinput_fd;
    int power_fd;     /* physical power key, watched read-only for auto-sleep wake (-1 = none) */
    char physical_path[JW_INPUT_PROXY_MAX_PATH];
    bool menu_held;
    bool menu_forwarded;
    bool chord_active;
    bool select_chord_consumed;   /* a Menu+Select chord ate the Select press;
                                     swallow its matching release too */
    bool screenshot_chord_consumed; /* a Menu+L1 screenshot chord ate the L1 press;
                                       swallow its matching release too */
    bool record_chord_consumed;   /* a Menu+R1 record chord ate the R1 press;
                                     swallow its release too so R1 does not
                                     stick down in the game */
    bool deferred_menu_release;
    uint64_t deferred_menu_release_at_ms;
    /* Action buttons forwarded as part of the current Menu chord, and the
       virtual Menu-up they are holding back. RetroArch stops masking libretro
       input the frame its hotkey modifier goes up, so an action still held at
       that moment turns into a live press inside the running game. */
    unsigned char chord_forwarded_keys[(KEY_MAX + 8) / 8];
    bool menu_up_pending;
    uint64_t last_brightness_ms;
    uint64_t last_activity_ms;     /* monotonic ms of the last EV_KEY (auto-sleep) */
    bool swallow;                  /* screen-off stage: wake on input but don't forward it */
    bool power_grabbed;            /* we hold the power key exclusively (jawakad owns sleep) */
    bool power_evdev_clock;        /* EVIOCSCLOCKID(CLOCK_MONOTONIC) took on power_fd, so
                                      event timestamps are usable as edge times */
    jw_power_edge power_edges[JW_MLP1_POWER_EDGE_MAX];  /* ring of unconsumed edges */
    int power_edge_head;
    int power_edge_count;
    /* Resting value per ABS axis, so a release can neutralize the stick and the
       D-pad hat, not just the buttons. Hats rest at 0; sticks at their midpoint. */
    bool abs_present[ABS_CNT];
    int  abs_neutral[ABS_CNT];
    jw_stick_calibration cal;      /* analog-stick range normalization (if loaded) */
    int32_t obs_x_min, obs_x_max;  /* observed stick extremes (measure mode only) */
    int32_t obs_y_min, obs_y_max;
    uint64_t last_cal_log_ms;      /* throttle for the measure-mode extremes log */
    unsigned char held_keys[(KEY_MAX + 8) / 8];  /* buttons currently forwarded-down */
    /* Force-feedback effects uploaded by whoever holds the virtual pad. The
       kernel hands us the effect on upload and only the id on playback, so the
       magnitude has to be remembered here to be there when the play arrives. */
    struct {
        bool     used;
        uint16_t magnitude;   /* strong and weak collapsed to one 0..0xFFFF */
        uint32_t length_ms;   /* replay length; 0 = until stopped */
    } ff_effects[JW_MLP1_FF_EFFECTS_MAX];
    /* Which effect is driving the motor, or -1. An id rather than a bool: a
       client may hold several effects at once (RetroArch's udev joypad driver
       uses one per rumble channel), and with only a flag a stop for effect B
       would silence effect A and nothing would ever restart it. */
    int ff_playing_id;
    pthread_t ff_thread;
    bool      ff_thread_running;
    int       ff_quit_pipe[2];
} jw_mlp1_input_proxy_data;

static bool jw__bit_is_set(const unsigned char *bits, int bit) {
    return (bits[bit / 8] & (1u << (bit % 8))) != 0;
}

static void jw__bit_set(unsigned char *bits, int bit) {
    bits[bit / 8] |= (unsigned char)(1u << (bit % 8));
}

static void jw__bit_clear(unsigned char *bits, int bit) {
    bits[bit / 8] &= (unsigned char)~(1u << (bit % 8));
}

static bool jw__bits_any(const unsigned char *bits, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (bits[i]) {
            return true;
        }
    }
    return false;
}

static uint64_t jw__monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static bool jw__event_name_matches(int fd, const char *expected) {
    if (!expected || !expected[0]) {
        return false;
    }

    char name[128];
    memset(name, 0, sizeof(name));
    return ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 &&
           strcmp(name, expected) == 0;
}

static int jw__open_loong_gamepad(char *out_path, size_t out_size) {
    for (int i = 0; i < 32; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }

        if (jw__event_name_matches(fd, JW_MLP1_INPUT_NAME)) {
            snprintf(out_path, out_size, "%s", path);
            return fd;
        }
        close(fd);
    }
    return -1;
}

/* Open the physical power key read-only (no grab — stock loong_power still owns
   it). We only watch it so a power press counts as activity and wakes the
   auto-sleep screen-off stage. Returns fd or -1 if not present. */
static int jw__open_power_key(void) {
    for (int i = 0; i < 32; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        if (jw__event_name_matches(fd, JW_MLP1_PWRKEY_NAME)) {
            return fd;
        }
        close(fd);
    }
    return -1;
}

static int jw__uinput_copy_capabilities(int input_fd, int uinput_fd) {
    unsigned char ev_bits[(EV_MAX + 8) / 8];
    unsigned char key_bits[(KEY_MAX + 8) / 8];
    unsigned char abs_bits[(ABS_MAX + 8) / 8];
    memset(ev_bits, 0, sizeof(ev_bits));
    memset(key_bits, 0, sizeof(key_bits));
    memset(abs_bits, 0, sizeof(abs_bits));

    if (ioctl(input_fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) {
        return -1;
    }

    if (jw__bit_is_set(ev_bits, EV_KEY)) {
        if (ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY) < 0 ||
            ioctl(input_fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) {
            return -1;
        }
        for (int code = 0; code <= KEY_MAX; code++) {
            if (jw__bit_is_set(key_bits, code)) {
                if (ioctl(uinput_fd, UI_SET_KEYBIT, code) < 0) {
                    return -1;
                }
            }
        }
    }

    if (jw__bit_is_set(ev_bits, EV_ABS)) {
        if (ioctl(uinput_fd, UI_SET_EVBIT, EV_ABS) < 0 ||
            ioctl(input_fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0) {
            return -1;
        }
        for (int code = 0; code <= ABS_MAX; code++) {
            if (!jw__bit_is_set(abs_bits, code)) {
                continue;
            }
            if (ioctl(uinput_fd, UI_SET_ABSBIT, code) < 0) {
                return -1;
            }
            struct input_absinfo absinfo;
            memset(&absinfo, 0, sizeof(absinfo));
            if (ioctl(input_fd, EVIOCGABS(code), &absinfo) == 0) {
                struct uinput_abs_setup setup;
                memset(&setup, 0, sizeof(setup));
                setup.code = (uint16_t)code;
                setup.absinfo = absinfo;
                if (ioctl(uinput_fd, UI_ABS_SETUP, &setup) < 0) {
                    return -1;
                }
            }
        }
    }

    return 0;
}

/* Record where each ABS axis rests, for jw_input_proxy_release_buttons. A hat
   rests at 0; a stick rests at the midpoint of its range. */
static void jw__capture_abs_neutrals(jw_mlp1_input_proxy_data *data) {
    unsigned char abs_bits[(ABS_MAX + 8) / 8];
    memset(abs_bits, 0, sizeof(abs_bits));
    if (ioctl(data->input_fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0) {
        return;
    }
    for (int code = 0; code <= ABS_MAX; code++) {
        if (!jw__bit_is_set(abs_bits, code)) {
            continue;
        }
        data->abs_present[code] = true;
        if (code >= ABS_HAT0X && code <= ABS_HAT3Y) {
            data->abs_neutral[code] = 0;
            continue;
        }
        struct input_absinfo absinfo;
        memset(&absinfo, 0, sizeof(absinfo));
        if (ioctl(data->input_fd, EVIOCGABS(code), &absinfo) == 0) {
            data->abs_neutral[code] = (absinfo.minimum + absinfo.maximum) / 2;
        }
    }
}

static int jw__create_virtual_gamepad(int input_fd) {
    /* O_RDWR, not O_WRONLY: force-feedback upload requests and playback commands
       come back to us *through* this fd, so the write-only handle the pad used
       before FF existed can no longer serve the device. */
    int ufd = open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (ufd < 0) {
        return -1;
    }

    if (jw__uinput_copy_capabilities(input_fd, ufd) != 0) {
        close(ufd);
        return -1;
    }

    /* FF_RUMBLE is *added*, not copied: the physical Loong Gamepad has no force
       feedback at all -- the motor hangs off a PWM channel jawakad owns. Putting
       it on the virtual pad is what lets an emulator rumble through the ordinary
       SDL/evdev path and reach that channel, with no per-emulator sysfs sink. */
    bool have_ff = ioctl(ufd, UI_SET_EVBIT, EV_FF) >= 0 &&
                   ioctl(ufd, UI_SET_FFBIT, FF_RUMBLE) >= 0;
    if (!have_ff) {
        jw_log_warn("input proxy: force feedback unavailable on the virtual pad: %s",
                    strerror(errno));
    }

    struct input_id id;
    memset(&id, 0, sizeof(id));
    if (ioctl(input_fd, EVIOCGID, &id) != 0) {
        id.bustype = BUS_VIRTUAL;
        id.vendor = 0x9903;
        id.product = 0x9913;
        id.version = 0x0102;
    }

    struct uinput_setup setup;
    memset(&setup, 0, sizeof(setup));
    snprintf(setup.name, sizeof(setup.name), "%s", JW_MLP1_INPUT_NAME);
    setup.id = id;
    setup.ff_effects_max = have_ff ? JW_MLP1_FF_EFFECTS_MAX : 0;
    if (ioctl(ufd, UI_DEV_SETUP, &setup) < 0 ||
        ioctl(ufd, UI_DEV_CREATE) < 0) {
        close(ufd);
        return -1;
    }

    return ufd;
}

static bool jw__same_rdev(const char *a, const char *b) {
    struct stat sa;
    struct stat sb;
    return stat(a, &sa) == 0 && stat(b, &sb) == 0 && sa.st_rdev == sb.st_rdev;
}

static bool jw__same_event_path(const char *a, const char *b) {
    if (!a || !b || !a[0] || !b[0]) {
        return false;
    }
    return strcmp(a, b) == 0 || jw__same_rdev(a, b);
}

static int jw__find_virtual_event(const char *physical_path, char *out, size_t out_size) {
    for (int attempt = 0; attempt < 50; attempt++) {
        for (int i = 0; i < 64; i++) {
            char path[64];
            snprintf(path, sizeof(path), "/dev/input/event%d", i);
            if (physical_path && jw__same_rdev(path, physical_path)) {
                continue;
            }

            int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) {
                continue;
            }

            bool match = jw__event_name_matches(fd, JW_MLP1_INPUT_NAME);
            close(fd);
            if (match) {
                snprintf(out, out_size, "%s", path);
                return 0;
            }
        }
        usleep(20000);
    }
    return -1;
}

static void jw__write_event(jw_mlp1_input_proxy_data *data,
                            uint16_t type, uint16_t code, int32_t value) {
    if (data->uinput_fd < 0) {
        return;   /* watch-only mode: observe hotkeys, forward nothing */
    }
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    gettimeofday(&ev.time, NULL);
    ev.type = type;
    ev.code = code;
    ev.value = value;
    if (write(data->uinput_fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
        jw_log_warn("input proxy: uinput write failed: %s", strerror(errno));
    }
}

static void jw__forward_event(jw_mlp1_input_proxy_data *data,
                              const struct input_event *ev) {
    if (data->uinput_fd < 0) {
        return;   /* watch-only mode: observe hotkeys, forward nothing */
    }
    /* Track which buttons are held-down on the virtual pad so the in-game menu
       can neutralize them before unpausing (see jw_input_proxy_release_buttons). */
    if (ev->type == EV_KEY && ev->code <= KEY_MAX) {
        if (ev->value > 0) {
            data->held_keys[ev->code / 8] |= (unsigned char)(1u << (ev->code % 8));
        } else {
            data->held_keys[ev->code / 8] &= (unsigned char)~(1u << (ev->code % 8));
        }
    }
    if (write(data->uinput_fd, ev, sizeof(*ev)) != (ssize_t)sizeof(*ev)) {
        jw_log_warn("input proxy: uinput forward failed: %s", strerror(errno));
    }
}

static void jw__emit_syn(jw_mlp1_input_proxy_data *data) {
    jw__write_event(data, EV_SYN, SYN_REPORT, 0);
}

/* Measure mode (no calibration profile): track this unit's real stick throw and
   log it (throttled) so a profile can be hand-written from the device log. */
static void jw__cal_observe(jw_mlp1_input_proxy_data *data,
                            const struct input_event *ev) {
    int32_t v = ev->value;
    bool changed = false;
    if (ev->code == ABS_X) {
        if (v < data->obs_x_min) { data->obs_x_min = v; changed = true; }
        if (v > data->obs_x_max) { data->obs_x_max = v; changed = true; }
    } else { /* ABS_Y */
        if (v < data->obs_y_min) { data->obs_y_min = v; changed = true; }
        if (v > data->obs_y_max) { data->obs_y_max = v; changed = true; }
    }
    if (!changed) {
        return;
    }
    uint64_t now = jw__monotonic_ms();
    if (data->last_cal_log_ms != 0 && now - data->last_cal_log_ms < 400u) {
        return;
    }
    data->last_cal_log_ms = now;
    jw_log_info("calibration[measure]: ABS_X[%d..%d] ABS_Y[%d..%d] "
                "(no profile; roll the stick fully to capture range)",
                data->obs_x_min, data->obs_x_max,
                data->obs_y_min, data->obs_y_max);
}

/* Forward an analog-stick axis event. With a loaded profile, remap the value to
   the normalized output range before forwarding; otherwise forward raw and feed
   the measure-mode observer. Non-stick ABS events never reach here. */
static void jw__forward_stick_abs(jw_mlp1_input_proxy_data *data,
                                  const struct input_event *ev) {
    if (data->uinput_fd < 0) {
        /* Watch-only mode: there is nothing to forward, and measure mode exists
           only to author a profile for the remap directly below -- which never
           runs here. Left on, it logged a range line every 400ms for the whole
           of a standalone session every time the stick moved. */
        return;
    }
    if (data->cal.loaded) {
        struct input_event out = *ev;
        out.value = jw_calibration_axis(&data->cal, ev->code == ABS_X, ev->value);
        jw__forward_event(data, &out);
        return;
    }
    jw__cal_observe(data, ev);
    jw__forward_event(data, ev);
}

static void jw__flush_menu_press(jw_mlp1_input_proxy_data *data) {
    if (!data->menu_held || data->menu_forwarded || data->chord_active) {
        return;
    }
    jw__write_event(data, EV_KEY, BTN_MODE, 1);
    jw__emit_syn(data);
    data->menu_forwarded = true;
}

/* Emit the virtual Menu-up a forwarded chord was holding back, once the last
   action button it forwarded is up. Forced from the reset paths, where the
   releases being waited on are never going to arrive. */
static void jw__release_pending_menu_up(jw_mlp1_input_proxy_data *data,
                                        bool force) {
    if (!data->menu_up_pending) {
        return;
    }
    if (!force &&
        jw__bits_any(data->chord_forwarded_keys,
                     sizeof(data->chord_forwarded_keys))) {
        return;
    }
    jw__write_event(data, EV_KEY, BTN_MODE, 0);
    jw__emit_syn(data);
    data->menu_up_pending = false;
    memset(data->chord_forwarded_keys, 0, sizeof(data->chord_forwarded_keys));
}

static void jw__emit_deferred_menu_tap(jw_mlp1_input_proxy_data *data) {
    jw__write_event(data, EV_KEY, BTN_MODE, 1);
    jw__emit_syn(data);
    data->deferred_menu_release = true;
    data->deferred_menu_release_at_ms = jw__monotonic_ms() + JW_MLP1_MENU_TAP_MS;
}

static void jw__release_deferred_menu_tap(jw_mlp1_input_proxy_data *data, bool force) {
    if (!data->deferred_menu_release) {
        return;
    }
    if (!force && jw__monotonic_ms() < data->deferred_menu_release_at_ms) {
        return;
    }

    jw__write_event(data, EV_KEY, BTN_MODE, 0);
    jw__emit_syn(data);
    data->deferred_menu_release = false;
    data->deferred_menu_release_at_ms = 0;
}

static bool jw__volume_key(uint16_t code) {
    return code == KEY_VOLUMEUP || code == KEY_VOLUMEDOWN;
}

static void jw__handle_volume_key(jw_input_proxy *proxy, uint16_t code, int32_t value) {
    if (value <= 0 || !proxy->volume_delta) {
        return;
    }

    int delta = (code == KEY_VOLUMEUP) ? 5 : -5;
    proxy->volume_delta(proxy->userdata, delta);
}

static void jw__handle_brightness_key(jw_input_proxy *proxy, uint16_t code, int32_t value) {
    if (value <= 0 || !proxy->brightness_delta) {
        return;
    }

    jw_mlp1_input_proxy_data *data = (jw_mlp1_input_proxy_data *)proxy->backend_data;
    uint64_t now = jw__monotonic_ms();
    if (value == 2 && data->last_brightness_ms != 0 &&
        now - data->last_brightness_ms < JW_MLP1_BRIGHTNESS_REPEAT_MS) {
        return;
    }

    data->last_brightness_ms = now;
    int delta = (code == KEY_VOLUMEUP) ? 5 : -5;
    proxy->brightness_delta(proxy->userdata, delta);
}

static void jw__handle_key(jw_input_proxy *proxy, const struct input_event *ev) {
    jw_mlp1_input_proxy_data *data = (jw_mlp1_input_proxy_data *)proxy->backend_data;

    if (ev->code == BTN_MODE) {
        if (ev->value > 0 && !data->menu_held) {
            data->menu_held = true;
            /* The virtual modifier can still be down from a chord whose
               release was deferred. Adopt it instead of deferring a second
               press: the re-flush would be a duplicate the input core drops,
               and the old chord's last action release would then emit a
               Menu-up underneath this new hold. */
            data->menu_forwarded = data->menu_up_pending;
            data->menu_up_pending = false;
            data->chord_active = false;
            return;
        }
        if (ev->value == 0 && data->menu_held) {
            if (data->menu_forwarded) {
                /* Physical Menu up while an action it forwarded is still down.
                   Sending the modifier release now would unmask libretro input
                   with that action held, and the core would read it as a fresh
                   press. Hold the virtual Menu down; the action's own release
                   below lets it go, so RetroArch always sees action-up before
                   modifier-up. */
                if (jw__bits_any(data->chord_forwarded_keys,
                                 sizeof(data->chord_forwarded_keys))) {
                    data->menu_up_pending = true;
                } else {
                    jw__forward_event(data, ev);
                }
            } else if (!data->chord_active) {
                bool handled = proxy->menu_tap && proxy->menu_tap(proxy->userdata);
                if (!handled) {
                    jw__emit_deferred_menu_tap(data);
                }
            }
            data->menu_held = false;
            data->menu_forwarded = false;
            data->chord_active = false;
            return;
        }
    }

    /* Menu + Select: open the in-game switcher. Mirrors Menu + Volume — the
       chord is consumed by jawakad and neither Menu nor Select reaches the
       running game. */
    if (ev->code == BTN_SELECT) {
        /* Swallow the release that pairs with a consumed chord press. */
        if (ev->value == 0 && data->select_chord_consumed) {
            data->select_chord_consumed = false;
            return;
        }
        if (ev->value > 0 && data->menu_held && !data->menu_forwarded) {
            bool handled = proxy->game_switcher &&
                           proxy->game_switcher(proxy->userdata);
            if (handled) {
                data->chord_active = true;          /* suppress the Menu tap */
                data->select_chord_consumed = true; /* suppress Select release */
                return; /* keep the deferred Menu unflushed; drop Select press */
            }
            /* Not handled: fall through so the deferred Menu flushes and Select
               forwards as an ordinary Menu+key chord. */
        }
    }

    /* Menu + L1: take a screenshot. Mirrors Menu + Select — jawakad consumes the
       chord and neither Menu nor L1 reaches the running game. If the callback
       declines (feature disabled), fall through so L1 forwards normally. */
    if (ev->code == BTN_TL) {
        /* Once the chord has eaten the press, eat EVERYTHING for this code until
           the release. Only swallowing value==0 was not enough: holding L1 past the
           autorepeat delay emits value==2, which matched neither guard, fell to the
           bottom of this function and was forwarded -- and jw__forward_event sets
           held_keys for any value > 0. The real release was then swallowed here, so
           the game saw L1 held down forever. */
        if (data->screenshot_chord_consumed) {
            if (ev->value == 0) {
                data->screenshot_chord_consumed = false;
            }
            return;
        }
        /* value==1 only: consuming an autorepeat would leave the earlier real press
           forwarded-down and then swallow its release, the same stuck key by the
           other route. */
        if (ev->value == 1 && data->menu_held && !data->menu_forwarded) {
            bool handled = proxy->screenshot &&
                           proxy->screenshot(proxy->userdata);
            if (handled) {
                data->chord_active = true;             /* suppress the Menu tap */
                data->screenshot_chord_consumed = true; /* suppress L1 release */
                return;                                 /* drop the L1 press */
            }
            /* Not handled: fall through so the deferred Menu flushes and L1
               forwards as an ordinary Menu+key chord. */
        }
    }

    /* Menu + R1: start or stop a recording. Same shape as Menu + L1 above --
       jawakad consumes the chord so neither Menu nor R1 reaches the game. This
       lives here rather than as a RetroArch hotkey because RetroArch's hotkeys
       need a pad modifier held, and it stops blocking that modifier from the
       core after input_hotkey_block_delay frames, so Select reaches the game and
       presses buttons in it. */
    if (ev->code == BTN_TR) {
        /* Swallow every event for this code until the release -- see the L1 block
           above. R1 is run or aim in most cores, so a stuck one is worse there. */
        if (data->record_chord_consumed) {
            if (ev->value == 0) {
                data->record_chord_consumed = false;
            }
            return;
        }
        /* value==1 only: consuming an autorepeat would leave the earlier real press
           forwarded-down, sticking R1 by the other route. */
        if (ev->value == 1 && data->menu_held && !data->menu_forwarded) {
            bool handled = proxy->record &&
                           proxy->record(proxy->userdata);
            if (handled) {
                data->chord_active = true;          /* suppress the Menu tap */
                data->record_chord_consumed = true; /* suppress R1 release */
                return;                             /* drop the R1 press */
            }
            /* Not handled: fall through so the deferred Menu flushes and R1
               forwards as an ordinary Menu+key chord. */
        }
    }

    if (jw__volume_key(ev->code)) {
        if (data->menu_held) {
            data->chord_active = true;
            jw__handle_brightness_key(proxy, ev->code, ev->value);
        } else {
            jw__handle_volume_key(proxy, ev->code, ev->value);
        }
        return;
    }

    if (data->menu_held && !data->menu_forwarded && ev->value > 0) {
        jw__flush_menu_press(data);
    }
    jw__forward_event(data, ev);

    /* Bookkeeping for the ordering guard above. Only real presses under an
       already-flushed Menu join the chord: a repeat would re-add a code the
       release is about to clear, and a button held from before Menu went down
       was never masked in the first place. BTN_MODE reaches here only as its
       own autorepeat, and must never wait on itself. */
    if (ev->code != BTN_MODE) {
        if (ev->value == 1 && data->menu_held && data->menu_forwarded) {
            jw__bit_set(data->chord_forwarded_keys, ev->code);
        } else if (ev->value == 0) {
            jw__bit_clear(data->chord_forwarded_keys, ev->code);
            jw__release_pending_menu_up(data, false);
        }
    }
}

/* ---- Force feedback -----------------------------------------------------
 *
 * uinput turns evdev force feedback inside out. A client that uploads an effect
 * with EVIOCSFF, or plays one by writing EV_FF, is not talking to the kernel --
 * the kernel relays the request back to whoever created the device, here. So the
 * pad's own creator has to answer them.
 *
 * That makes latency the whole design. EVIOCSFF *blocks the calling emulator*
 * until we answer it, and SDL re-uploads on every magnitude change, so serving
 * this from the daemon's 50ms housekeeping loop would stall the emulation thread
 * for up to three frames every time rumble starts or stops. It gets its own
 * thread blocked in poll() instead, which answers in well under a millisecond.
 */
static void jw__ff_handle_upload(jw_mlp1_input_proxy_data *data, int32_t request_id) {
    struct uinput_ff_upload upload;
    memset(&upload, 0, sizeof(upload));
    upload.request_id = (uint32_t)request_id;
    if (ioctl(data->uinput_fd, UI_BEGIN_FF_UPLOAD, &upload) < 0) {
        jw_log_warn("input proxy: FF upload begin failed: %s", strerror(errno));
        return;
    }

    int id = upload.effect.id;
    if (id < 0 || id >= JW_MLP1_FF_EFFECTS_MAX || upload.effect.type != FF_RUMBLE) {
        /* FF_RUMBLE is all the pad claims, so anything else is a client bug --
           and an out-of-range id would be a kernel one. Refuse rather than
           silently accepting an effect we would never play. */
        upload.retval = -EINVAL;
    } else {
        /* One motor, two magnitudes. Take the louder channel rather than
           averaging: a mix would dilute a strong-only effect and could leave a
           weak-only one below the motor's stiction floor, so the two most
           common single-channel cases both come out wrong. */
        uint16_t strong = upload.effect.u.rumble.strong_magnitude;
        uint16_t weak   = upload.effect.u.rumble.weak_magnitude;
        data->ff_effects[id].used      = true;
        data->ff_effects[id].magnitude = strong > weak ? strong : weak;
        data->ff_effects[id].length_ms = upload.effect.replay.length;
        upload.retval = 0;
    }

    if (ioctl(data->uinput_fd, UI_END_FF_UPLOAD, &upload) < 0) {
        jw_log_warn("input proxy: FF upload end failed: %s", strerror(errno));
    }
}

static void jw__ff_handle_erase(jw_input_proxy *proxy,
                                jw_mlp1_input_proxy_data *data,
                                int32_t request_id) {
    struct uinput_ff_erase erase;
    memset(&erase, 0, sizeof(erase));
    erase.request_id = (uint32_t)request_id;
    if (ioctl(data->uinput_fd, UI_BEGIN_FF_ERASE, &erase) < 0) {
        jw_log_warn("input proxy: FF erase begin failed: %s", strerror(errno));
        return;
    }

    if (erase.effect_id < JW_MLP1_FF_EFFECTS_MAX) {
        /* Erasing the effect that is driving the motor has to stop it. The
           client is entitled to erase without stopping first, and after this
           the slot is unused -- so any stop it does send afterwards would be
           dropped, and nothing else would turn the motor off until the hold
           timed out or the session ended. */
        if (data->ff_playing_id == (int)erase.effect_id) {
            data->ff_playing_id = -1;
            if (proxy->rumble) {
                proxy->rumble(proxy->userdata, 0, 0);
            }
        }
        data->ff_effects[erase.effect_id].used = false;
        erase.retval = 0;
    } else {
        erase.retval = -EINVAL;
    }

    if (ioctl(data->uinput_fd, UI_END_FF_ERASE, &erase) < 0) {
        jw_log_warn("input proxy: FF erase end failed: %s", strerror(errno));
    }
}

/* EV_FF: code is the effect id, value the repeat count (0 = stop). Note that
   SDL never sends a stop -- it stops by re-uploading the effect at magnitude 0
   and playing that -- so a zero magnitude has to mean stop as surely as a zero
   value does. */
static void jw__ff_handle_play(jw_input_proxy *proxy,
                               jw_mlp1_input_proxy_data *data,
                               uint16_t effect_id, int32_t value) {
    /* EV_FF also carries FF_GAIN and FF_AUTOCENTER, whose codes sit well above
       any effect id. The bound check is what keeps them out. */
    if (effect_id >= JW_MLP1_FF_EFFECTS_MAX) {
        return;
    }

    /* A stop is honored even for a slot we no longer have, and that ordering
       matters: refusing unknown effects first would swallow the stop for an
       effect erased while it was still playing, and leave the motor running. */
    bool known = data->ff_effects[effect_id].used;
    uint16_t magnitude = known ? data->ff_effects[effect_id].magnitude : 0u;
    if (value == 0 || magnitude == 0) {
        /* Only the effect actually holding the motor may stop it. Otherwise a
           client winding down one of several channels silences the others. */
        if (data->ff_playing_id != (int)effect_id) {
            return;
        }
        data->ff_playing_id = -1;
        if (proxy->rumble) {
            proxy->rumble(proxy->userdata, 0, 0);
        }
        return;
    }

    if (!known) {
        return;   /* play of an effect we were never given */
    }

    data->ff_playing_id = (int)effect_id;
    if (proxy->rumble) {
        proxy->rumble(proxy->userdata, magnitude,
                      data->ff_effects[effect_id].length_ms);
    }
}

static void jw__ff_drain(jw_input_proxy *proxy, jw_mlp1_input_proxy_data *data) {
    struct input_event ev;
    while (read(data->uinput_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_UINPUT) {
            if (ev.code == UI_FF_UPLOAD) {
                jw__ff_handle_upload(data, ev.value);
            } else if (ev.code == UI_FF_ERASE) {
                jw__ff_handle_erase(proxy, data, ev.value);
            }
        } else if (ev.type == EV_FF) {
            jw__ff_handle_play(proxy, data, ev.code, ev.value);
        }
    }
}

static void *jw__ff_thread_main(void *arg) {
    jw_input_proxy *proxy = (jw_input_proxy *)arg;
    jw_mlp1_input_proxy_data *data = (jw_mlp1_input_proxy_data *)proxy->backend_data;

    for (;;) {
        struct pollfd fds[2];
        fds[0].fd = data->uinput_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = data->ff_quit_pipe[0];
        fds[1].events = POLLIN;
        fds[1].revents = 0;

        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            jw_log_warn("input proxy: FF poll failed: %s", strerror(errno));
            break;
        }
        if (fds[1].revents) {
            break;              /* shutdown asked us to stop */
        }
        /* Leave on any error condition. Without this an fd reporting POLLERR but
           not POLLIN would neither drain nor break, and poll() would return
           immediately every time round: a silent 100% CPU spin inside the
           daemon, which is a far worse failure than losing rumble. */
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            jw_log_warn("input proxy: FF fd error (revents=0x%x); stopping",
                        (unsigned)fds[0].revents);
            break;
        }
        if (fds[0].revents & POLLIN) {
            jw__ff_drain(proxy, data);
        }
    }

    /* Never hand the motor back still running: if the emulator is killed
       mid-rumble there is no one left to send the stop. */
    if (data->ff_playing_id >= 0) {
        data->ff_playing_id = -1;
        if (proxy->rumble) {
            proxy->rumble(proxy->userdata, 0, 0);
        }
    }
    return NULL;
}

static void jw__ff_thread_start(jw_input_proxy *proxy,
                                jw_mlp1_input_proxy_data *data) {
    if (data->uinput_fd < 0) {
        return;
    }
    /* O_CLOEXEC, like every other fd here: the daemon forks and execs launchers,
       emulators and third-party pak code constantly, and a plain pipe() would
       hand all of them both ends of the daemon's own shutdown channel. */
    if (pipe2(data->ff_quit_pipe, O_CLOEXEC) != 0) {
        jw_log_warn("input proxy: FF quit pipe failed: %s", strerror(errno));
        data->ff_quit_pipe[0] = data->ff_quit_pipe[1] = -1;
        return;
    }
    if (pthread_create(&data->ff_thread, NULL, jw__ff_thread_main, proxy) != 0) {
        jw_log_warn("input proxy: FF thread failed to start; rumble unavailable");
        close(data->ff_quit_pipe[0]);
        close(data->ff_quit_pipe[1]);
        data->ff_quit_pipe[0] = data->ff_quit_pipe[1] = -1;
        return;
    }
    data->ff_thread_running = true;
}

/* Stop the thread before anything it touches goes away. It must be joined, not
   just signalled: it owns the uinput fd for reads and the rumble callback, and
   both are torn down the instant this returns. */
static void jw__ff_thread_stop(jw_mlp1_input_proxy_data *data) {
    if (!data->ff_thread_running) {
        return;
    }
    if (data->ff_quit_pipe[1] >= 0) {
        ssize_t ignored = write(data->ff_quit_pipe[1], "q", 1);
        (void)ignored;
    }
    pthread_join(data->ff_thread, NULL);
    data->ff_thread_running = false;
    if (data->ff_quit_pipe[0] >= 0) close(data->ff_quit_pipe[0]);
    if (data->ff_quit_pipe[1] >= 0) close(data->ff_quit_pipe[1]);
    data->ff_quit_pipe[0] = data->ff_quit_pipe[1] = -1;
}

static int jw__input_proxy_init_impl(jw_input_proxy *proxy,
                                     jw_input_brightness_delta_cb brightness_delta,
                                     jw_input_volume_delta_cb volume_delta,
                                     jw_input_menu_tap_cb menu_tap,
                                     jw_input_game_switcher_cb game_switcher,
                                     void *userdata,
                                     bool watch_only) {
    if (!proxy) {
        return -1;
    }
    memset(proxy, 0, sizeof(*proxy));
    proxy->brightness_delta = brightness_delta;
    proxy->volume_delta = volume_delta;
    proxy->menu_tap = menu_tap;
    proxy->game_switcher = game_switcher;
    proxy->userdata = userdata;

    const char *enabled = getenv("JAWAKA_INPUT_PROXY");
    if (enabled && strcmp(enabled, "0") == 0) {
        jw_log_info("input proxy: disabled by JAWAKA_INPUT_PROXY=0");
        return 0;
    }

    jw_mlp1_input_proxy_data *data = (jw_mlp1_input_proxy_data *)calloc(1, sizeof(*data));
    if (!data) {
        return -1;
    }
    data->input_fd = -1;
    data->uinput_fd = -1;
    data->power_fd = -1;
    data->ff_quit_pipe[0] = data->ff_quit_pipe[1] = -1;
    /* Explicit: calloc leaves this 0, which would read as "effect 0 is playing"
       and let a stray stop for effect 0 through before anything was ever sent. */
    data->ff_playing_id = -1;
    data->last_activity_ms = jw__monotonic_ms();   /* don't count boot as idle */
    data->obs_x_min = INT32_MAX;
    data->obs_x_max = INT32_MIN;
    data->obs_y_min = INT32_MAX;
    data->obs_y_max = INT32_MIN;
    jw_calibration_load(&data->cal);   /* loaded=false → forward raw + measure */

    data->input_fd = jw__open_loong_gamepad(data->physical_path, sizeof(data->physical_path));
    if (data->input_fd < 0) {
        jw_log_warn("input proxy: Loong Gamepad not found");
        free(data);
        return 0;
    }
    snprintf(proxy->physical_event_path, sizeof(proxy->physical_event_path),
             "%s", data->physical_path);
    snprintf(proxy->device_name, sizeof(proxy->device_name), "%s", JW_MLP1_INPUT_NAME);

    if (!watch_only) {
        jw__capture_abs_neutrals(data);
        data->uinput_fd = jw__create_virtual_gamepad(data->input_fd);
        if (data->uinput_fd < 0) {
            jw_log_warn("input proxy: could not create virtual gamepad: %s", strerror(errno));
            close(data->input_fd);
            free(data);
            return 0;
        }

        if (jw__find_virtual_event(data->physical_path,
                                   proxy->virtual_event_path,
                                   sizeof(proxy->virtual_event_path)) != 0) {
            jw_log_warn("input proxy: virtual event path not found");
        }

        if (ioctl(data->input_fd, EVIOCGRAB, 1) < 0) {
            jw_log_warn("input proxy: EVIOCGRAB failed: %s", strerror(errno));
            ioctl(data->uinput_fd, UI_DEV_DESTROY);
            close(data->uinput_fd);
            close(data->input_fd);
            free(data);
            return 0;
        }
    }

    /* Take over the power key: EVIOCGRAB it so stock loong_power never sees a press
       and jawakad owns the whole sleep/wake story (power = sleep when the screen is
       on, wake when it's off — all through jawakad's own real-suspend path). The
       PMIC still hard-powers-off on a long hold regardless of this grab. */
    data->power_fd = jw__open_power_key();
    if (data->power_fd >= 0 && ioctl(data->power_fd, EVIOCGRAB, 1) == 0) {
        data->power_grabbed = true;
        /* Stamp edges with kernel event time in the clock jawakad measures hold
           durations in, so a press/release that queues during a stalled daemon
           tick still reports its true duration (long-press vs tap). */
        int clk = CLOCK_MONOTONIC;
        data->power_evdev_clock = ioctl(data->power_fd, EVIOCSCLOCKID, &clk) == 0;
        if (!data->power_evdev_clock) {
            jw_log_warn("input proxy: EVIOCSCLOCKID failed on power key; "
                        "edge times fall back to read time");
        }
    }
    jw_log_info("input proxy: power key %s",
                data->power_grabbed ? "grabbed (jawakad owns it)"
                                    : (data->power_fd >= 0 ? "open (grab failed)"
                                                           : "not found"));

    proxy->backend_data = data;
    proxy->enabled = true;
    /* Safe to start before the caller assigns proxy->rumble: nothing has opened
       the pad yet, so no effect can arrive until well after that write. */
    jw__ff_thread_start(proxy, data);
    if (watch_only) {
        jw_log_info("input proxy: watching %s (no grab; hotkeys only)",
                    data->physical_path);
    } else {
        jw_log_info("input proxy: grabbed %s, virtual=%s",
                    data->physical_path,
                    proxy->virtual_event_path[0] ? proxy->virtual_event_path : "(unknown)");
    }
    return 0;
}

int jw_input_proxy_init(jw_input_proxy *proxy,
                        jw_input_brightness_delta_cb brightness_delta,
                        jw_input_volume_delta_cb volume_delta,
                        jw_input_menu_tap_cb menu_tap,
                        jw_input_game_switcher_cb game_switcher,
                        void *userdata) {
    return jw__input_proxy_init_impl(proxy, brightness_delta, volume_delta,
                                     menu_tap, game_switcher, userdata, false);
}

/* Watch-only variant for standalone emulator sessions: the emulator reads the
   physical gamepad directly (no grab, no virtual device), while jawakad still
   observes the same device for volume/brightness and Menu hotkeys. The emulator
   also receives those hotkey presses, but PPSSPP's guide button mapping is
   patched out so Jawaka can open its pause menu via SIGUSR2 instead. */
int jw_input_proxy_init_watch(jw_input_proxy *proxy,
                              jw_input_brightness_delta_cb brightness_delta,
                              jw_input_volume_delta_cb volume_delta,
                              jw_input_menu_tap_cb menu_tap,
                              jw_input_game_switcher_cb game_switcher,
                              void *userdata) {
    return jw__input_proxy_init_impl(proxy, brightness_delta, volume_delta,
                                     menu_tap, game_switcher, userdata, true);
}

int jw_input_proxy_retroarch_joypad_index(const jw_input_proxy *proxy) {
    if (!proxy || !proxy->enabled || !proxy->virtual_event_path[0]) {
        return -1;
    }

    const char *device_name = proxy->device_name[0] ? proxy->device_name : JW_MLP1_INPUT_NAME;
    int joypad_index = 0;
    for (int i = 0; i < 64; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);

        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }

        bool match = jw__event_name_matches(fd, device_name);
        close(fd);
        if (!match) {
            continue;
        }

        if (jw__same_event_path(path, proxy->virtual_event_path)) {
            return joypad_index;
        }
        joypad_index++;
    }

    return -1;
}

int jw_input_proxy_poll_fd(const jw_input_proxy *proxy) {
    if (!proxy || !proxy->enabled || !proxy->backend_data) {
        return -1;
    }
    const jw_mlp1_input_proxy_data *data =
        (const jw_mlp1_input_proxy_data *)proxy->backend_data;
    return data->input_fd;
}

void jw_input_proxy_tick(jw_input_proxy *proxy) {
    if (!proxy || !proxy->enabled || !proxy->backend_data) {
        return;
    }

    jw_mlp1_input_proxy_data *data = (jw_mlp1_input_proxy_data *)proxy->backend_data;
    jw__release_deferred_menu_tap(data, false);

    while (1) {
        struct input_event ev;
        ssize_t n = read(data->input_fd, &ev, sizeof(ev));
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                jw_log_warn("input proxy: read failed: %s", strerror(errno));
            }
            jw__release_deferred_menu_tap(data, false);
            break;   /* gamepad drained — fall through to the power-key handling */
        }
        if (n != (ssize_t)sizeof(ev)) {
            jw__release_deferred_menu_tap(data, false);
            break;
        }

        if (data->swallow) {
            /* Screen-off stage: a press should wake the screen (reset idle) but not
               also fire a navigation action, so stamp activity and discard. */
            if (ev.type == EV_KEY ||
                (ev.type == EV_ABS &&
                 (ev.code == ABS_HAT0X || ev.code == ABS_HAT0Y) && ev.value != 0)) {
                data->last_activity_ms = jw__monotonic_ms();
            }
            continue;
        }
        if (ev.type == EV_KEY) {
            data->last_activity_ms = jw__monotonic_ms();   /* auto-sleep idle reset */
            jw__handle_key(proxy, &ev);
        } else {
            /* The d-pad is an EV_ABS hat (ABS_HAT0X/Y) — count it as activity so
               menu navigation resets the idle timer. The analog stick (ABS_X/Y)
               is deliberately NOT counted, so stick drift can't block sleep. */
            if (ev.type == EV_ABS &&
                (ev.code == ABS_HAT0X || ev.code == ABS_HAT0Y) && ev.value != 0) {
                data->last_activity_ms = jw__monotonic_ms();
            }
            /* Analog stick axes get calibration-normalized (or measured); every
               other ABS/event forwards verbatim. */
            if (ev.type == EV_ABS && (ev.code == ABS_X || ev.code == ABS_Y)) {
                jw__forward_stick_abs(data, &ev);
            } else {
                jw__forward_event(data, &ev);
            }
        }
    }
    /* Power key (we hold it exclusively): queue press/release edges for jawakad,
       which decides sleep vs wake from screen state — wake on press, sleep on
       release. We don't act here. (value 2 = autorepeat, ignored.) Edges keep
       their kernel timestamps so a hold whose press AND release queued behind a
       stalled daemon tick still measures as a long press, not a 0ms tap. */
    if (data->power_grabbed && data->power_fd >= 0) {
        struct input_event pev;
        while (read(data->power_fd, &pev, sizeof(pev)) == (ssize_t)sizeof(pev)) {
            if (pev.type != EV_KEY || pev.code != KEY_POWER ||
                (pev.value != 0 && pev.value != 1)) {
                continue;
            }
            if (data->power_edge_count == JW_MLP1_POWER_EDGE_MAX) {
                /* Full (the daemon is badly stalled): drop the oldest edge. */
                data->power_edge_head =
                    (data->power_edge_head + 1) % JW_MLP1_POWER_EDGE_MAX;
                data->power_edge_count--;
            }
            int tail = (data->power_edge_head + data->power_edge_count) %
                       JW_MLP1_POWER_EDGE_MAX;
            data->power_edges[tail].down = pev.value == 1;
            data->power_edges[tail].ms = data->power_evdev_clock
                ? (uint64_t)pev.input_event_sec * 1000u +
                  (uint64_t)pev.input_event_usec / 1000u
                : jw__monotonic_ms();
            data->power_edge_count++;
        }
    }
}

uint64_t jw_input_proxy_idle_ms(const jw_input_proxy *proxy) {
    if (!proxy || !proxy->backend_data) {
        return 0;
    }
    const jw_mlp1_input_proxy_data *data =
        (const jw_mlp1_input_proxy_data *)proxy->backend_data;
    uint64_t now = jw__monotonic_ms();
    return (now > data->last_activity_ms) ? (now - data->last_activity_ms) : 0;
}

void jw_input_proxy_mark_activity(jw_input_proxy *proxy) {
    if (!proxy || !proxy->backend_data) {
        return;
    }
    jw_mlp1_input_proxy_data *data = (jw_mlp1_input_proxy_data *)proxy->backend_data;
    data->last_activity_ms = jw__monotonic_ms();
}

void jw_input_proxy_set_swallow(jw_input_proxy *proxy, bool swallow) {
    if (!proxy || !proxy->backend_data) {
        return;
    }
    jw_mlp1_input_proxy_data *data = (jw_mlp1_input_proxy_data *)proxy->backend_data;
    /* Entering the screen-off stage drops every event on the floor, including the
       releases the chord state machine is waiting on. Left set, a *_chord_consumed
       flag outlives the press it belongs to and eats the first real press of that
       button after the screen comes back -- R1 is run or aim in most cores, so
       that one is felt. Clear the in-flight chord state instead: with the screen
       off nothing is mid-gesture, and everything here is "waiting for a release
       that is no longer coming". */
    if (swallow && !data->swallow) {
        /* Forced: the un-forced form waits out the hold timer, and that timer is
           driven by a loop that is about to stop delivering events. An unreleased
           BTN_MODE would read as Menu held down for as long as the screen is off. */
        jw__release_deferred_menu_tap(data, true);
        /* The deferred tap is only ONE of the two ways BTN_MODE can be down. A
           Menu+key chord that fell through to an ordinary press flushes it via
           jw__flush_menu_press, which writes through jw__write_event -- and that,
           unlike jw__forward_event, sets no held_keys bit. So the release_buttons
           call both callers make immediately before this cannot release it, and
           menu_forwarded is the only remaining record that it is down. Clearing
           that flag without emitting the release stranded BTN_MODE latched at 1
           with nothing left that knew: the kernel then swallowed the next tap's
           press as a duplicate and the app saw a bare release. */
        if (data->menu_forwarded) {
            jw__write_event(data, EV_KEY, BTN_MODE, 0);
            jw__emit_syn(data);
            data->menu_up_pending = false;
        }
        /* Third way BTN_MODE can be down: a chord whose physical Menu was
           already released is holding the virtual one until its action buttons
           come up. Those releases die with the screen, so let it go now. */
        jw__release_pending_menu_up(data, true);
        memset(data->chord_forwarded_keys, 0, sizeof(data->chord_forwarded_keys));
        data->menu_held                  = false;
        data->menu_forwarded             = false;
        data->chord_active               = false;
        data->select_chord_consumed      = false;
        data->screenshot_chord_consumed  = false;
        data->record_chord_consumed      = false;
    }
    data->swallow = swallow;
}

void jw_input_proxy_emit_menu_tap(jw_input_proxy *proxy) {
    if (!proxy || !proxy->enabled || !proxy->backend_data) {
        return;
    }
    jw_mlp1_input_proxy_data *data = (jw_mlp1_input_proxy_data *)proxy->backend_data;
    if (data->uinput_fd < 0) {
        return; /* watch-only mode: there is no virtual pad to emit onto */
    }
    if (data->deferred_menu_release) {
        return; /* one already in flight; stacking presses would double-toggle */
    }
    jw__emit_deferred_menu_tap(data);
}

void jw_input_proxy_release_buttons(jw_input_proxy *proxy) {
    if (!proxy || !proxy->backend_data) {
        return;
    }
    jw_mlp1_input_proxy_data *data = (jw_mlp1_input_proxy_data *)proxy->backend_data;
    if (data->uinput_fd < 0) {
        return;   /* watch-only mode: nothing forwarded to release */
    }
    bool released_any = false;
    for (int code = 0; code <= KEY_MAX; code++) {
        if (jw__bit_is_set(data->held_keys, code)) {
            jw__write_event(data, EV_KEY, (uint16_t)code, 0);
            data->held_keys[code / 8] &= (unsigned char)~(1u << (code % 8));
            released_any = true;
        }
    }
    /* Buttons alone are not enough: on this pad the D-pad is an ABS hat, so a
       key-only release leaves the hat pinned and the consumer keeps auto-
       repeating a direction that is physically already up. Send every axis back
       to rest as well. */
    for (int code = 0; code <= ABS_MAX; code++) {
        if (data->abs_present[code]) {
            jw__write_event(data, EV_ABS, (uint16_t)code, data->abs_neutral[code]);
            released_any = true;
        }
    }
    if (released_any) {
        jw__emit_syn(data);
    }
    /* Every forwarded action just went up, so nothing is left for a deferred
       Menu-up to wait on. Callers use this to flush input across a transition;
       leaving the virtual modifier latched would strand it for the next app. */
    memset(data->chord_forwarded_keys, 0, sizeof(data->chord_forwarded_keys));
    jw__release_pending_menu_up(data, true);
}

bool jw_input_proxy_take_power_edge(jw_input_proxy *proxy, jw_power_edge *edge) {
    if (!proxy || !proxy->backend_data) {
        return false;
    }
    jw_mlp1_input_proxy_data *data = (jw_mlp1_input_proxy_data *)proxy->backend_data;
    if (data->power_edge_count == 0) {
        return false;
    }
    if (edge) *edge = data->power_edges[data->power_edge_head];
    data->power_edge_head = (data->power_edge_head + 1) % JW_MLP1_POWER_EDGE_MAX;
    data->power_edge_count--;
    return true;
}

void jw_input_proxy_flush(jw_input_proxy *proxy) {
    if (!proxy || !proxy->enabled || !proxy->backend_data) {
        return;
    }
    jw_mlp1_input_proxy_data *data = (jw_mlp1_input_proxy_data *)proxy->backend_data;
    /* Drain the physical gamepad without forwarding — drops presses that queued
       while suspended so they don't replay into the launcher on wake. */
    struct input_event ev;
    while (read(data->input_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        /* discard */
    }
    if (data->power_fd >= 0) {
        struct input_event pev;
        while (read(data->power_fd, &pev, sizeof(pev)) == (ssize_t)sizeof(pev)) {
            /* discard */
        }
    }
    data->power_edge_count = 0;   /* drop already-queued edges too */
}

void jw_input_proxy_shutdown(jw_input_proxy *proxy) {
    if (!proxy || !proxy->backend_data) {
        return;
    }

    jw_mlp1_input_proxy_data *data = (jw_mlp1_input_proxy_data *)proxy->backend_data;
    jw__ff_thread_stop(data);
    jw__release_deferred_menu_tap(data, true);
    jw__release_pending_menu_up(data, true);

    if (data->input_fd >= 0) {
        ioctl(data->input_fd, EVIOCGRAB, 0);
        close(data->input_fd);
    }
    if (data->uinput_fd >= 0) {
        ioctl(data->uinput_fd, UI_DEV_DESTROY);
        close(data->uinput_fd);
    }
    if (data->power_fd >= 0) {
        close(data->power_fd);
    }
    free(data);
    memset(proxy, 0, sizeof(*proxy));
}
