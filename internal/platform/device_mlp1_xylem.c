/* The MLP1 platform backend's xylem side; see device_mlp1_xylem.h. */
#include "internal/platform/device_mlp1_xylem.h"

#include "internal/core/log.h"
#include "internal/platform/aw20036_i2c.h"
#include "internal/platform/power_supply_sysfs.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define JW_XYLEM_BACKLIGHT_DIR        "/sys/class/backlight/backlight"
#define JW_XYLEM_BACKLIGHT_BRIGHTNESS JW_XYLEM_BACKLIGHT_DIR "/brightness"
/* FB_BLANK: 0 unblank, 4 powerdown. */
#define JW_XYLEM_BACKLIGHT_BL_POWER   JW_XYLEM_BACKLIGHT_DIR "/bl_power"

/* rk817_ext is card 0 on mainline. The one speaker is fed from the codec's
   headphone outputs, so "Playback Mux" stays HP whatever plays: SPK drives a
   path with nothing on it and gives only hiss. The speaker amp is the
   "Internal Speakers Switch", off while headphones are in. */
#define JW_XYLEM_MUX_HP \
    "amixer -c 0 cset name='Playback Mux' HP >/dev/null 2>&1"
#define JW_XYLEM_SPEAKERS_ON \
    "amixer -c 0 cset name='Internal Speakers Switch' on >/dev/null 2>&1"
#define JW_XYLEM_SPEAKERS_OFF \
    "amixer -c 0 cset name='Internal Speakers Switch' off >/dev/null 2>&1"
#define JW_XYLEM_HP_JACK_CMD \
    "amixer -c 0 cget name='Headphones Jack' 2>/dev/null"
/* The jack's switch device (SW_HEADPHONE_INSERT). It reports a plug or an
   unplug within half a second. */
#define JW_XYLEM_JACK_INPUT_NAME "rk817_ext Headphones"
/* Without the device, look for it again this often rather than on every pass
   of the daemon loop, which asks for the fd each time. */
#define JW_XYLEM_JACK_RESCAN_MS 5000LL

static long long jw__xylem_monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
}

static int jw__xylem_shell(const char *cmd) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int jw__xylem_write_int(const char *path, int value) {
    FILE *fp = fopen(path, "w");
    if (!fp) {
        return -1;
    }
    int rc = fprintf(fp, "%d\n", value) > 0 ? 0 : -1;
    if (fclose(fp) != 0) {
        rc = -1;
    }
    return rc;
}

/* ── Power supplies ─────────────────────────────────────────────────────── */

static char s_xylem_battery_dir[JW_POWER_SUPPLY_PATH_MAX];

void jw_mlp1_xylem_get_power_status(jw_platform_status *out) {
    if (!out) {
        return;
    }
    int capacity = jw_power_supply_battery_capacity(s_xylem_battery_dir);
    if (capacity >= 0) {
        out->battery_percent = capacity;
    }
    int online = jw_power_supply_charger_online();
    if (online >= 0) {
        out->charging = online;
    }
}

int jw_mlp1_xylem_charger_online(void) {
    return jw_power_supply_charger_online();
}

/* ── Actions ────────────────────────────────────────────────────────────── */

bool jw_mlp1_xylem_action_unsupported(jw_platform_action action) {
    switch (action) {
        /* No Wi-Fi or Bluetooth backend on xylem yet. */
        case JW_PLATFORM_ACTION_WIFI_ON:
        case JW_PLATFORM_ACTION_WIFI_OFF:
        case JW_PLATFORM_ACTION_BLUETOOTH_ON:
        case JW_PLATFORM_ACTION_BLUETOOTH_OFF:
        /* Stock's USB gadget (adbd + usb-gadget) and its boot splash. */
        case JW_PLATFORM_ACTION_ENABLE_ADB:
        case JW_PLATFORM_ACTION_DISABLE_ADB:
        case JW_PLATFORM_ACTION_SET_BOOT_SPLASH:
        /* loong_power's auto-sleep: there is none to keep disabled. jawakad's
           own idle sleep is unaffected. */
        case JW_PLATFORM_ACTION_SET_AUTO_SLEEP:
        /* Stock writes a ~/.config/weston.ini mode override for its Weston. */
        case JW_PLATFORM_ACTION_SET_REFRESH_RATE:
        /* Stock programs the colour LUT with a legacy DRM_IOCTL_MODE_SETGAMMA
           ramp on the vendor 5.10 VOP2 driver. That does not work on the
           mainline kernel, so colour temperature stays off until a mainline
           route (the atomic GAMMA_LUT property, or a compositor shader) is
           tested. */
        case JW_PLATFORM_ACTION_SET_COLOR_TEMP:
        /* HDMI output switching restarts stock's Weston through its init.d
           script and loads an HDMI sink into stock's PulseAudio. */
        case JW_PLATFORM_ACTION_SET_HDMI_OUTPUT:
            return true;
        default:
            return false;
    }
}

/* ── Audio ──────────────────────────────────────────────────────────────── */

/* The jack's input device, opened once by name. -2 = not probed yet (or due a
   rescan), -1 = absent until s_xylem_jack_rescan_at_ms. */
static int s_xylem_jack_fd = -2;
static int s_xylem_jack_state = -1;   /* 1 plugged, 0 unplugged, -1 unknown */
static long long s_xylem_jack_rescan_at_ms = 0;
/* The route last applied, so status polls cost no amixer fork. */
static jw_platform_audio_output s_xylem_route = JW_PLATFORM_AUDIO_OUTPUT_UNKNOWN;

static void jw__xylem_jack_close(void) {
    if (s_xylem_jack_fd >= 0) {
        close(s_xylem_jack_fd);
    }
    s_xylem_jack_fd = -2;
    s_xylem_jack_state = -1;
}

/* Re-read the switch: on open, after SYN_DROPPED, and on reconcile (a wake,
   where events may have been lost). */
static void jw__xylem_jack_resync(void) {
    if (s_xylem_jack_fd < 0) {
        return;
    }
    unsigned long bits[2] = {0};   /* SW_HEADPHONE_INSERT (2) lives in word 0 */
    if (ioctl(s_xylem_jack_fd, EVIOCGSW(sizeof(bits)), bits) < 0) {
        jw__xylem_jack_close();
        return;
    }
    s_xylem_jack_state = (bits[0] >> SW_HEADPHONE_INSERT) & 1UL ? 1 : 0;
}

int jw_mlp1_xylem_jack_fd(void) {
    if (s_xylem_jack_fd >= 0) {
        return s_xylem_jack_fd;
    }
    long long now = jw__xylem_monotonic_ms();
    if (s_xylem_jack_fd == -1 && now < s_xylem_jack_rescan_at_ms) {
        return -1;
    }
    for (int i = 0; i < 32; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        char name[128] = {0};
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 &&
            strstr(name, JW_XYLEM_JACK_INPUT_NAME)) {
            s_xylem_jack_fd = fd;
            jw__xylem_jack_resync();
            if (s_xylem_jack_fd >= 0) {
                jw_log_info("audio: headphone jack is %s (%s)", path, name);
            }
            return s_xylem_jack_fd;
        }
        close(fd);
    }
    /* The codec's input device can appear after jawakad starts. */
    s_xylem_jack_fd = -1;
    s_xylem_jack_rescan_at_ms = now + JW_XYLEM_JACK_RESCAN_MS;
    return -1;
}

/* 1 plugged, 0 unplugged, -1 no device. Drains the queued switch events. */
static int jw__xylem_jack_state(void) {
    int fd = jw_mlp1_xylem_jack_fd();
    if (fd < 0) {
        return -1;
    }
    struct input_event ev;
    for (;;) {
        ssize_t got = read(fd, &ev, sizeof(ev));
        if (got == (ssize_t)sizeof(ev)) {
            if (ev.type == EV_SW && ev.code == SW_HEADPHONE_INSERT) {
                s_xylem_jack_state = ev.value ? 1 : 0;
            } else if (ev.type == EV_SYN && ev.code == SYN_DROPPED) {
                jw__xylem_jack_resync();
                if (s_xylem_jack_fd < 0) {
                    return -1;
                }
            }
            continue;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        /* ENODEV or a short read: the node went stale. */
        jw__xylem_jack_close();
        return -1;
    }
    return s_xylem_jack_state;
}

/* The ALSA "Headphones Jack" control, for when the input device is missing.
   Its value line reads ": values=on" or "=off". */
static bool jw__xylem_jack_amixer(void) {
    FILE *fp = popen(JW_XYLEM_HP_JACK_CMD, "r");
    if (!fp) {
        return false;
    }
    char line[256];
    bool present = false;
    while (fgets(line, sizeof(line), fp)) {
        const char *v = strstr(line, ": values=");
        if (v) {
            v += strlen(": values=");
            present = strncmp(v, "on", 2) == 0 || v[0] == '1';
            break;
        }
    }
    pclose(fp);
    return present;
}

static bool jw__xylem_headphones_in(void) {
    int state = jw__xylem_jack_state();
    return state >= 0 ? state == 1 : jw__xylem_jack_amixer();
}

static jw_platform_audio_output jw__xylem_desired_output(void) {
    return jw__xylem_headphones_in() ? JW_PLATFORM_AUDIO_OUTPUT_HEADSET
                                     : JW_PLATFORM_AUDIO_OUTPUT_SPEAKER;
}

jw_platform_audio_output jw_mlp1_xylem_audio_output(void) {
    return s_xylem_route != JW_PLATFORM_AUDIO_OUTPUT_UNKNOWN ? s_xylem_route
                                                             : jw__xylem_desired_output();
}

unsigned jw_mlp1_xylem_audio_outputs(void) {
    unsigned mask = JW_PLATFORM_AUDIO_OUTPUT_BIT(JW_PLATFORM_AUDIO_OUTPUT_SPEAKER);
    if (jw__xylem_headphones_in()) {
        mask |= JW_PLATFORM_AUDIO_OUTPUT_BIT(JW_PLATFORM_AUDIO_OUTPUT_HEADSET);
    }
    return mask;
}

int jw_mlp1_xylem_route_audio(jw_platform_audio_output output) {
    if (output != JW_PLATFORM_AUDIO_OUTPUT_SPEAKER &&
        output != JW_PLATFORM_AUDIO_OUTPUT_HEADSET) {
        return -1;
    }
    /* Re-asserted on every route: nothing else on xylem owns the codec, but an
       alsactl restore or a mixer client could have moved it. */
    static bool mux_warned = false;
    if (jw__xylem_shell(JW_XYLEM_MUX_HP) != 0) {
        if (!mux_warned) {
            mux_warned = true;
            jw_log_warn("audio: could not set rk817 Playback Mux to HP");
        }
    } else {
        mux_warned = false;
    }
    bool headphones = output == JW_PLATFORM_AUDIO_OUTPUT_HEADSET;
    if (jw__xylem_shell(headphones ? JW_XYLEM_SPEAKERS_OFF : JW_XYLEM_SPEAKERS_ON) != 0) {
        jw_log_warn("audio: could not turn the internal speaker %s",
                    headphones ? "off" : "on");
        return -1;
    }
    s_xylem_route = output;
    return 0;
}

void jw_mlp1_xylem_init(void) {
    jw_platform_audio_output target = jw__xylem_desired_output();
    if (jw_mlp1_xylem_route_audio(target) == 0) {
        jw_log_info("audio: rk817 mux HP, routed to %s",
                    jw_platform_audio_output_label(target));
    }
}

void jw_mlp1_xylem_shutdown(void) {
    jw__xylem_jack_close();
}

unsigned jw_mlp1_xylem_audio_tick(void) {
    static int last_present = -1;
    int present = jw__xylem_jack_state();
    if (present < 0 || present == last_present) {
        return 0;
    }
    last_present = present;
    jw_platform_audio_output target = present ? JW_PLATFORM_AUDIO_OUTPUT_HEADSET
                                              : JW_PLATFORM_AUDIO_OUTPUT_SPEAKER;
    (void)jw_mlp1_xylem_route_audio(target);
    jw_log_info("audio: headphone jack %s, routed to %s",
                present ? "inserted" : "removed",
                jw_platform_audio_output_label(target));
    return JW_PLATFORM_AUDIO_EVENT_OUTPUT_CHANGED;
}

void jw_mlp1_xylem_audio_reconcile(const char *reason) {
    const char *why = (reason && reason[0]) ? reason : "unknown";
    /* Switch events can be lost across a suspend; take the kernel's word. */
    jw__xylem_jack_resync();
    jw_platform_audio_output target = jw__xylem_desired_output();
    if (jw_mlp1_xylem_route_audio(target) != 0) {
        jw_log_warn("audio: reconcile %s -> %s failed",
                    why, jw_platform_audio_output_label(target));
        return;
    }
    jw_log_info("audio: reconcile %s -> %s",
                why, jw_platform_audio_output_label(target));
}

bool jw_mlp1_xylem_sink_open(void) {
    FILE *fp = popen("pactl list short sinks 2>/dev/null", "r");
    if (!fp) {
        return false;
    }
    char line[256];
    bool open_sink = false;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "RUNNING") || strstr(line, "IDLE")) {
            open_sink = true;
            break;
        }
    }
    pclose(fp);
    return open_sink;
}

/* ── Sleep ──────────────────────────────────────────────────────────────── */

int jw_mlp1_xylem_suspend(int restore_brightness_raw) {
    /* The LED ring's chip stays powered through suspend (a GPIO hog holds its
       enable line), so it would stay lit: put it to sleep. jawaka-ledd sees
       the resume and sets the chip up again. No chip, or a kernel driver
       holding it, leaves nothing to do. */
    int ring = jw_aw20036_open();
    if (ring >= 0) {
        if (!jw_aw20036_sleep(ring)) {
            jw_log_warn("platform: LED ring sleep before suspend failed: %s",
                        strerror(errno));
        }
        close(ring);
    }
    /* Mainline keeps the panel lit through `mem` unless it is blanked first.
       jawakad has normally blanked it already (SCREEN_OFF); doing it here
       too keeps a bare SLEEP request from suspending with the panel on. */
    if (jw__xylem_write_int(JW_XYLEM_BACKLIGHT_BL_POWER, 4) != 0) {
        jw_log_warn("platform: backlight blank before suspend failed: %s",
                    strerror(errno));
    }
    int rc = -1;
    int saved_errno = 0;
    int fd = open("/sys/power/state", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        rc = write(fd, "mem\n", 4) == 4 ? 0 : -1;   /* blocks until resume */
        saved_errno = errno;
        close(fd);
    } else {
        saved_errno = errno;
    }
    if (rc != 0) {
        jw_log_warn("platform: suspend to RAM failed: %s", strerror(saved_errno));
    }
    if (jw__xylem_write_int(JW_XYLEM_BACKLIGHT_BL_POWER, 0) != 0) {
        jw_log_warn("platform: backlight unblank after suspend failed: %s",
                    strerror(errno));
    }
    if (restore_brightness_raw > 0 &&
        jw__xylem_write_int(JW_XYLEM_BACKLIGHT_BRIGHTNESS, restore_brightness_raw) != 0) {
        jw_log_warn("platform: brightness restore after suspend failed: %s",
                    strerror(errno));
    }
    return rc;
}

/* ---- Cards ---------------------------------------------------------------- */

/* The second slot is sdmmc2, the SD controller at fe000000 (sdmmc1 is the
   Wi-Fi). On mainline the boot card is mmcblk1 and its first partition the
   boot partition, so stock's /dev/mmcblk1p1 and /dev/mmcblk3p1 guesses would
   pick the wrong card: the card is found by its controller instead. */
#define JW_XYLEM_SECONDARY_MOUNT "/roms"
#define JW_XYLEM_SECONDARY_MMC   "fe000000.mmc"
#define JW_XYLEM_SECONDARY_GLOB \
    "/sys/bus/platform/devices/" JW_XYLEM_SECONDARY_MMC \
    "/mmc_host/mmc*/mmc*:*/block/mmcblk*/mmcblk*p1"

const char *jw_mlp1_xylem_secondary_mount(void) {
    const char *path = getenv("UMRK_SECONDARY_SDCARD_PATH");
    return path && path[0] == '/' ? path : JW_XYLEM_SECONDARY_MOUNT;
}

bool jw_mlp1_xylem_secondary_block(char *out, size_t out_size) {
    bool found = false;
    glob_t g;
    if (glob(JW_XYLEM_SECONDARY_GLOB, 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc && !found; i++) {
            const char *name = strrchr(g.gl_pathv[i], '/');
            char dev[64];
            if (!name || (size_t)snprintf(dev, sizeof(dev), "/dev%s", name) >= sizeof(dev) ||
                access(dev, F_OK) != 0) {
                continue;
            }
            if (out && out_size > 0) {
                snprintf(out, out_size, "%s", dev);
            }
            found = true;
        }
        globfree(&g);
    }
    if (!found && out && out_size > 0) {
        out[0] = '\0';
    }
    return found;
}

bool jw_mlp1_xylem_containing_mount(const char *path, char *device, size_t device_size) {
    if (device && device_size > 0) {
        device[0] = '\0';
    }
    if (!path || path[0] != '/') {
        return false;
    }
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp) {
        return false;
    }
    char dev[256];
    char mount[256];
    size_t best = 0;
    bool found = false;
    while (fscanf(fp, "%255s %255s %*s %*s %*d %*d\n", dev, mount) == 2) {
        size_t len = strlen(mount);
        bool holds = strcmp(mount, "/") == 0 ||
                     (strncmp(path, mount, len) == 0 &&
                      (path[len] == '\0' || path[len] == '/'));
        if (holds && (!found || len > best)) {
            best = len;
            found = true;
            if (device && device_size > 0) {
                snprintf(device, device_size, "%s", dev);
            }
        }
    }
    fclose(fp);
    return found;
}
