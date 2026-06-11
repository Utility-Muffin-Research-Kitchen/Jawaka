#include "internal/platform/input_proxy.h"
#include "internal/core/log.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
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
    bool deferred_menu_release;
    uint64_t deferred_menu_release_at_ms;
    uint64_t last_brightness_ms;
    uint64_t last_activity_ms;     /* monotonic ms of the last EV_KEY (auto-sleep) */
    bool swallow;                  /* screen-off stage: wake on input but don't forward it */
    bool power_grabbed;            /* we hold the power key exclusively (jawakad owns sleep) */
    bool power_evdev_clock;        /* EVIOCSCLOCKID(CLOCK_MONOTONIC) took on power_fd, so
                                      event timestamps are usable as edge times */
    jw_power_edge power_edges[JW_MLP1_POWER_EDGE_MAX];  /* ring of unconsumed edges */
    int power_edge_head;
    int power_edge_count;
} jw_mlp1_input_proxy_data;

static bool jw__bit_is_set(const unsigned char *bits, int bit) {
    return (bits[bit / 8] & (1u << (bit % 8))) != 0;
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

static int jw__create_virtual_gamepad(int input_fd) {
    int ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (ufd < 0) {
        return -1;
    }

    if (jw__uinput_copy_capabilities(input_fd, ufd) != 0) {
        close(ufd);
        return -1;
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
    if (write(data->uinput_fd, ev, sizeof(*ev)) != (ssize_t)sizeof(*ev)) {
        jw_log_warn("input proxy: uinput forward failed: %s", strerror(errno));
    }
}

static void jw__emit_syn(jw_mlp1_input_proxy_data *data) {
    jw__write_event(data, EV_SYN, SYN_REPORT, 0);
}

static void jw__flush_menu_press(jw_mlp1_input_proxy_data *data) {
    if (!data->menu_held || data->menu_forwarded || data->chord_active) {
        return;
    }
    jw__write_event(data, EV_KEY, BTN_MODE, 1);
    jw__emit_syn(data);
    data->menu_forwarded = true;
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
            data->menu_forwarded = false;
            data->chord_active = false;
            return;
        }
        if (ev->value == 0 && data->menu_held) {
            if (data->menu_forwarded) {
                jw__forward_event(data, ev);
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
}

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
    data->last_activity_ms = jw__monotonic_ms();   /* don't count boot as idle */

    data->input_fd = jw__open_loong_gamepad(data->physical_path, sizeof(data->physical_path));
    if (data->input_fd < 0) {
        jw_log_warn("input proxy: Loong Gamepad not found");
        free(data);
        return 0;
    }
    snprintf(proxy->physical_event_path, sizeof(proxy->physical_event_path),
             "%s", data->physical_path);
    snprintf(proxy->device_name, sizeof(proxy->device_name), "%s", JW_MLP1_INPUT_NAME);

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
    jw_log_info("input proxy: grabbed %s, virtual=%s",
                data->physical_path,
                proxy->virtual_event_path[0] ? proxy->virtual_event_path : "(unknown)");
    return 0;
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
            jw__forward_event(data, &ev);
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
    data->swallow = swallow;
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
    jw__release_deferred_menu_tap(data, true);

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
