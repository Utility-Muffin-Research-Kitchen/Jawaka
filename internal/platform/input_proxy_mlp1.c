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
#define JW_MLP1_BRIGHTNESS_REPEAT_MS 120u

typedef struct {
    int input_fd;
    int uinput_fd;
    char physical_path[JW_INPUT_PROXY_MAX_PATH];
    bool menu_held;
    bool menu_forwarded;
    bool chord_active;
    uint64_t last_brightness_ms;
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

static int jw__open_loong_gamepad(char *out_path, size_t out_size) {
    for (int i = 0; i < 32; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }

        char name[128];
        memset(name, 0, sizeof(name));
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 &&
            strcmp(name, JW_MLP1_INPUT_NAME) == 0) {
            snprintf(out_path, out_size, "%s", path);
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

            char name[128];
            memset(name, 0, sizeof(name));
            bool match = ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 &&
                         strcmp(name, JW_MLP1_INPUT_NAME) == 0;
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
    jw__write_event(data, EV_KEY, BTN_MODE, 0);
    jw__emit_syn(data);
}

static bool jw__volume_key(uint16_t code) {
    return code == KEY_VOLUMEUP || code == KEY_VOLUMEDOWN;
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
                jw__emit_deferred_menu_tap(data);
            }
            data->menu_held = false;
            data->menu_forwarded = false;
            data->chord_active = false;
            return;
        }
    }

    if (data->menu_held && jw__volume_key(ev->code)) {
        data->chord_active = true;
        jw__handle_brightness_key(proxy, ev->code, ev->value);
        return;
    }

    if (data->menu_held && !data->menu_forwarded && ev->value > 0) {
        jw__flush_menu_press(data);
    }
    jw__forward_event(data, ev);
}

int jw_input_proxy_init(jw_input_proxy *proxy,
                        jw_input_brightness_delta_cb brightness_delta,
                        void *userdata) {
    if (!proxy) {
        return -1;
    }
    memset(proxy, 0, sizeof(*proxy));
    proxy->brightness_delta = brightness_delta;
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

    data->input_fd = jw__open_loong_gamepad(data->physical_path, sizeof(data->physical_path));
    if (data->input_fd < 0) {
        jw_log_warn("input proxy: Loong Gamepad not found");
        free(data);
        return 0;
    }

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

    proxy->backend_data = data;
    proxy->enabled = true;
    jw_log_info("input proxy: grabbed %s, virtual=%s",
                data->physical_path,
                proxy->virtual_event_path[0] ? proxy->virtual_event_path : "(unknown)");
    return 0;
}

void jw_input_proxy_tick(jw_input_proxy *proxy) {
    if (!proxy || !proxy->enabled || !proxy->backend_data) {
        return;
    }

    jw_mlp1_input_proxy_data *data = (jw_mlp1_input_proxy_data *)proxy->backend_data;
    while (1) {
        struct input_event ev;
        ssize_t n = read(data->input_fd, &ev, sizeof(ev));
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                jw_log_warn("input proxy: read failed: %s", strerror(errno));
            }
            return;
        }
        if (n != (ssize_t)sizeof(ev)) {
            return;
        }

        if (ev.type == EV_KEY) {
            jw__handle_key(proxy, &ev);
        } else {
            jw__forward_event(data, &ev);
        }
    }
}

void jw_input_proxy_shutdown(jw_input_proxy *proxy) {
    if (!proxy || !proxy->backend_data) {
        return;
    }

    jw_mlp1_input_proxy_data *data = (jw_mlp1_input_proxy_data *)proxy->backend_data;
    if (data->input_fd >= 0) {
        ioctl(data->input_fd, EVIOCGRAB, 0);
        close(data->input_fd);
    }
    if (data->uinput_fd >= 0) {
        ioctl(data->uinput_fd, UI_DEV_DESTROY);
        close(data->uinput_fd);
    }
    free(data);
    memset(proxy, 0, sizeof(*proxy));
}
