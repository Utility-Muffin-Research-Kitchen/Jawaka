#include "cJSON.h"
#include "internal/platform/device_backend.h"
#include "internal/platform/bluetooth.h"
#include "internal/core/log.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <dlfcn.h>
#include <dirent.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* MLP1 backlight usable range: raw 61-135 out of 0-255.
   Below 61 the screen goes black. Above 135 there is no visible change.
   We remap the 0-100% OSD range onto this usable hardware range. */
#define JW_MLP1_BACKLIGHT_RAW_MIN 61
#define JW_MLP1_BACKLIGHT_RAW_MAX 135

#define JW_MLP1_BACKLIGHT_DIR "/sys/class/backlight/backlight"
#define JW_MLP1_BACKLIGHT_BRIGHTNESS JW_MLP1_BACKLIGHT_DIR "/brightness"
/* FB_BLANK control: 0 = unblank (on), 4 = powerdown (off). Independent of the
   brightness value, so blanking/unblanking preserves the user's brightness and
   loong_power doesn't fight it (unlike direct brightness writes to 0). */
#define JW_MLP1_BACKLIGHT_BL_POWER JW_MLP1_BACKLIGHT_DIR "/bl_power"
#define JW_MLP1_BACKLIGHT_ACTUAL JW_MLP1_BACKLIGHT_DIR "/actual_brightness"
#define JW_MLP1_BACKLIGHT_MAX JW_MLP1_BACKLIGHT_DIR "/max_brightness"

#define JW_MLP1_CPUFREQ_POLICY "/sys/devices/system/cpu/cpufreq/policy0"
#define JW_MLP1_GPU_DEVFREQ "/sys/devices/platform/fde60000.gpu/devfreq/fde60000.gpu"
#define JW_MLP1_DMC_DEVFREQ "/sys/devices/platform/dmc/devfreq/dmc"
#define JW_MLP1_SOC_TEMP "/sys/class/thermal/thermal_zone0/temp"

#define JW_MLP1_LOONG_DB_PATH "/oem/loong/loong.db"
#define JW_MLP1_POWER_CFG "/oem/loong/record/config/loong_power.cfg"
#define JW_MLP1_POWER_DAEMON "loong_power"

#define JW_MLP1_PACTL_GET_VOLUME "pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null"
#define JW_MLP1_PACTL_SET_VOLUME "pactl set-sink-volume @DEFAULT_SINK@ %d%% 2>/dev/null"
#define JW_MLP1_PACTL_GET_DEFAULT_SINK "pactl get-default-sink 2>/dev/null"
#define JW_MLP1_PACTL_SET_DEFAULT_RK817 \
    "pactl set-default-sink alsa_output.platform-rk817-sound.stereo-fallback 2>/dev/null"
#define JW_MLP1_PACTL_SET_DEFAULT_HDMI \
    "pactl set-default-sink alsa_output.platform-hdmi-sound.stereo-fallback 2>/dev/null"
#define JW_MLP1_PACTL_RK817_SINK "alsa_output.platform-rk817-sound.stereo-fallback"
#define JW_MLP1_PACTL_HDMI_SINK "alsa_output.platform-hdmi-sound.stereo-fallback"
#define JW_MLP1_PLAYBACK_PATH_CMD "amixer -c 1 cget numid=13 2>/dev/null"
#define JW_MLP1_PLAYBACK_PATH_SPK "amixer -c 1 cset numid=13 2 >/dev/null 2>&1"
#define JW_MLP1_PLAYBACK_PATH_HP  "amixer -c 1 cset numid=13 3 >/dev/null 2>&1"
#define JW_MLP1_PLAYBACK_PATH_BT  "amixer -c 1 cset numid=13 5 >/dev/null 2>&1"
#define JW_MLP1_HDMI_STATUS "/sys/class/drm/card0-HDMI-A-1/status"

/* The rk817 DAC (ALSA numid=16) is the dominant hardware loudness control and is
   pinned to a fixed level at boot (platform.d/00-audio-init.sh). The user-facing
   volume runs through PulseAudio's software sink volume above. PulseAudio can,
   however, drift this hardware element low (we have seen it stuck at 0 = muted
   and at ~167 = barely audible), leaving the speaker silent no matter the sink %.
   This self-heal re-asserts the pin whenever it has fallen below it. Keep the
   value (210) in sync with 00-audio-init.sh. */
#define JW_MLP1_DAC_PIN 210
#define JW_MLP1_ENSURE_DAC_FLOOR \
    "v=$(amixer -c 1 cget numid=16 2>/dev/null | " \
    "sed -n 's/.*: values=\\([0-9]*\\).*/\\1/p' | head -1); " \
    "if [ -n \"$v\" ] && [ \"$v\" -lt 210 ]; then " \
    "amixer -c 1 cset numid=16 210,210 >/dev/null 2>&1; fi"
#define JW_MLP1_WIFI_PROC "/proc/net/wireless"
#define JW_MLP1_SECONDARY_SOURCE_ID "secondary_sd"
#define JW_MLP1_SECONDARY_LABEL "Secondary SD"
#define JW_MLP1_SECONDARY_DEVICE "/dev/mmcblk3p1"
#define JW_MLP1_SECONDARY_MOUNT "/media/sdcard1"
#define JW_MLP1_STORAGE_DEBOUNCE_MS 750

/* The stock loong_light daemon owns the AW20036 LED ring. It reads this JSON
   config and applies it on SIGUSR1 (write cfg, then signal). We cooperate with
   it rather than fighting its refresh loop. */
#define JW_MLP1_LED_CFG  "/oem/loong/record/config/loong_light.cfg"
#define JW_MLP1_LED_DAEMON "loong_light"

#define JW_MLP1_ADBD "/usr/bin/adbd"
#define JW_MLP1_USB_CONFIG "/etc/.usb_config"
#define JW_MLP1_USB_GADGET "/etc/init.d/S50usb-gadget.sh"
#define JW_MLP1_ADB_MARKER_FILE "adb-enabled"
#define JW_MLP1_BOOT_SPLASH_DISABLED_FILE "boot-splash-disabled"
#define JW_MLP1_ROOTFS_RW_CMD \
    "(mount -o remount,rw / >/dev/null 2>&1 || " \
    "mount -o remount,rw /dev/root / >/dev/null 2>&1)"
#define JW_MLP1_ADB_ENABLE_CMD \
    JW_MLP1_ROOTFS_RW_CMD " || exit 1; " \
    "chattr -i " JW_MLP1_USB_CONFIG " >/dev/null 2>&1 || true; " \
    "printf 'usb_adb_en\\n' >" JW_MLP1_USB_CONFIG " && " \
    "chattr +i " JW_MLP1_USB_CONFIG " && sync"
#define JW_MLP1_ADB_DISABLE_CMD \
    JW_MLP1_ROOTFS_RW_CMD " || exit 1; " \
    "chattr -i " JW_MLP1_USB_CONFIG " >/dev/null 2>&1 || true; " \
    "printf 'usb_mtp_en\\n' >" JW_MLP1_USB_CONFIG " && sync"
#define JW_MLP1_USB_RESTART_CMD \
    JW_MLP1_USB_GADGET " restart >/dev/null 2>&1 || " \
    JW_MLP1_USB_GADGET " start >/dev/null 2>&1"

/* Run a shell command via /bin/sh -c (for commands with arguments/pipes). */
static int jw__exec_shell(const char *cmd) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static bool jw__env_truthy(const char *name) {
    const char *value = getenv(name);
    return value && value[0] && strcmp(value, "0") != 0 &&
           strcmp(value, "false") != 0 && strcmp(value, "no") != 0;
}

typedef struct {
    int uevent_fd;
    int last_present;
    int last_mounted;
    bool pending_storage_event;
    long long debounce_until_ms;
} jw_mlp1_platform_data;

static int (*s_event_opend)(const char *id);
/* loong::PowerApi singleton accessor + standby method (C++ symbols; the method
   takes the singleton `this`). Used so suspend goes through loong's own power
   path — keeps loong_power in sync so the power button wakes on a single press
   (vs. our raw `echo mem`, which loong re-suspends on the first wake press). */
static void *(*s_power_get)(void);
static void  (*s_power_standby)(void *self);
static int   (*s_write_config)(const char *param, const char *value,
                               const char *backup, int persist);
static bool s_loong_loaded;

static jw_platform_audio_output jw__mlp1_get_audio_output(void);
static unsigned jw__mlp1_get_audio_available_outputs(void);
static void jw__mlp1_get_audio_volumes(int out[JW_PLATFORM_AUDIO_OUTPUT_COUNT]);
static int jw__mlp1_set_audio_output(jw_platform_audio_output output,
                                     jw_platform_result *out);
static void jw__mlp1_sync_sound_volume(jw_platform_audio_output output, int percent);
static int jw__mlp1_get_bluealsa_volume_percent(void);
static int jw__mlp1_set_bluealsa_volume_percent(int percent);
static int jw__mlp1_apply_power_cfg_live(void);

static long long jw__monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
}

static bool jw__mlp1_source_is_secondary(const char *source_id) {
    return !source_id || !source_id[0] ||
           strcmp(source_id, JW_MLP1_SECONDARY_SOURCE_ID) == 0;
}

static bool jw__mlp1_block_present(void) {
    return access(JW_MLP1_SECONDARY_DEVICE, F_OK) == 0;
}

static bool jw__mlp1_mount_is_active(void) {
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp) {
        return false;
    }

    char dev[256];
    char mount[256];
    char type[64];
    bool active = false;
    while (fscanf(fp, "%255s %255s %63s %*s %*d %*d\n",
                  dev, mount, type) == 3) {
        if (strcmp(mount, JW_MLP1_SECONDARY_MOUNT) == 0) {
            active = true;
            break;
        }
    }
    fclose(fp);
    return active;
}

static bool jw__mlp1_storage_busy(void) {
    if (!jw__mlp1_mount_is_active()) {
        return false;
    }

    if (access("/usr/bin/fuser", X_OK) == 0 ||
        access("/bin/fuser", X_OK) == 0 ||
        access("/sbin/fuser", X_OK) == 0) {
        return jw__exec_shell("fuser -m " JW_MLP1_SECONDARY_MOUNT " >/dev/null 2>&1") == 0;
    }

    if (access("/usr/bin/lsof", X_OK) == 0 ||
        access("/bin/lsof", X_OK) == 0 ||
        access("/sbin/lsof", X_OK) == 0) {
        return jw__exec_shell("lsof +f -- " JW_MLP1_SECONDARY_MOUNT
                              " 2>/dev/null | awk 'NR > 1 { found = 1 } END { exit found ? 0 : 1 }'") == 0;
    }

    return false;
}

static int jw__mlp1_mount_secondary_if_needed(void) {
    if (!jw__mlp1_block_present()) {
        return -1;
    }

    mkdir(JW_MLP1_SECONDARY_MOUNT, 0755);

    if (jw__mlp1_mount_is_active()) {
        return jw__exec_shell("mount -o remount,rw,exec,nosuid,nodev,noatime,nodiratime "
                              JW_MLP1_SECONDARY_MOUNT " >/dev/null 2>&1");
    }

    return jw__exec_shell("mount -t vfat -o rw,exec,nosuid,nodev,noatime,nodiratime,"
                          "fmask=0022,dmask=0022,iocharset=utf8,shortname=mixed,"
                          "errors=remount-ro "
                          JW_MLP1_SECONDARY_DEVICE " " JW_MLP1_SECONDARY_MOUNT
                          " >/dev/null 2>&1");
}

static int jw__mlp1_open_uevent_socket(void) {
    int fd = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = 1;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    return fd;
}

static int jw__mlp1_init(jw_platform_context *ctx) {
    jw_mlp1_platform_data *data = (jw_mlp1_platform_data *)calloc(1, sizeof(*data));
    if (!data) {
        return -1;
    }

    data->uevent_fd = jw__mlp1_open_uevent_socket();
    if (data->uevent_fd < 0) {
        jw_log_warn("storage hotplug: uevent socket unavailable: %s", strerror(errno));
    }

    if (jw__mlp1_block_present()) {
        if (jw__mlp1_mount_secondary_if_needed() == 0) {
            jw_log_info("storage hotplug: secondary SD mounted/remounted");
        } else {
            jw_log_warn("storage hotplug: secondary SD mount/remount failed");
        }
    }

    data->last_present = jw__mlp1_block_present() ? 1 : 0;
    data->last_mounted = jw__mlp1_mount_is_active() ? 1 : 0;
    ctx->backend_data = data;
    return 0;
}

static void jw__mlp1_shutdown(jw_platform_context *ctx) {
    jw_mlp1_platform_data *data = ctx ? (jw_mlp1_platform_data *)ctx->backend_data : NULL;
    if (data) {
        if (data->uevent_fd >= 0) {
            close(data->uevent_fd);
        }
        free(data);
    }
    if (ctx) {
        ctx->backend_data = NULL;
    }
}

static int jw__read_int_file(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    int value = -1;
    if (fscanf(fp, "%d", &value) != 1) {
        value = -1;
    }
    fclose(fp);
    return value;
}

static int jw__write_int_file(const char *path, int value) {
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

static int jw__write_text_file(const char *path, const char *value) {
    FILE *fp = fopen(path, "w");
    if (!fp) {
        return -1;
    }

    int rc = fputs(value ? value : "", fp) >= 0 ? 0 : -1;
    if (fclose(fp) != 0) {
        rc = -1;
    }
    return rc;
}

static void jw__trim_line(char *s) {
    if (!s) {
        return;
    }
    size_t len = strlen(s);
    while (len > 0 &&
           (s[len - 1] == '\n' || s[len - 1] == '\r' ||
            s[len - 1] == ' ' || s[len - 1] == '\t')) {
        s[--len] = '\0';
    }
    char *start = s;
    while (*start == ' ' || *start == '\t') {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
}

static int jw__read_line_file(const char *path, char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }
    if (!fgets(out, (int)out_size, fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    jw__trim_line(out);
    return 0;
}

static int jw__join_sysfs_path(char *out, size_t out_size,
                               const char *base, const char *leaf) {
    if (!out || out_size == 0 || !base || !leaf) {
        return -1;
    }
    int n = snprintf(out, out_size, "%s/%s", base, leaf);
    return (n > 0 && n < (int)out_size) ? 0 : -1;
}

static bool jw__token_list_has(const char *list, const char *token) {
    if (!list || !token || !token[0]) {
        return false;
    }
    size_t token_len = strlen(token);
    const char *p = list;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        const char *start = p;
        while (*p && !isspace((unsigned char)*p)) {
            p++;
        }
        size_t len = (size_t)(p - start);
        if (len == token_len && strncmp(start, token, token_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool jw__freq_list_has(const char *list, int freq) {
    char token[32];
    snprintf(token, sizeof(token), "%d", freq);
    return jw__token_list_has(list, token);
}

static void jw__mlp1_perf_domain_status(jw_platform_perf_domain domain,
                                        const char *base,
                                        jw_platform_perf_domain_status *out) {
    if (!out) {
        return;
    }

    const char *name = "CPU";
    const char *governor_leaf = "scaling_governor";
    const char *cur_leaf = "scaling_cur_freq";
    const char *set_leaf = "scaling_setspeed";
    const char *available_freq_leaf = "scaling_available_frequencies";
    const char *available_governor_leaf = "scaling_available_governors";
    if (domain == JW_PLATFORM_PERF_DOMAIN_GPU) {
        name = "GPU";
        governor_leaf = "governor";
        cur_leaf = "cur_freq";
        set_leaf = "userspace/set_freq";
        available_freq_leaf = "available_frequencies";
        available_governor_leaf = "available_governors";
    } else if (domain == JW_PLATFORM_PERF_DOMAIN_DMC) {
        name = "DMC";
        governor_leaf = "governor";
        cur_leaf = "cur_freq";
        set_leaf = "userspace/set_freq";
        available_freq_leaf = "available_frequencies";
        available_governor_leaf = "available_governors";
    }

    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", name);
    out->current_freq = -1;
    out->set_freq = -1;

    char path[PATH_MAX];
    if (!base || jw__join_sysfs_path(path, sizeof(path), base, governor_leaf) != 0 ||
        access(path, R_OK) != 0) {
        return;
    }

    out->supported = true;
    (void)jw__read_line_file(path, out->governor, sizeof(out->governor));
    if (jw__join_sysfs_path(path, sizeof(path), base, cur_leaf) == 0) {
        out->current_freq = jw__read_int_file(path);
    }
    if (jw__join_sysfs_path(path, sizeof(path), base, set_leaf) == 0) {
        out->set_freq = jw__read_int_file(path);
    }
    if (jw__join_sysfs_path(path, sizeof(path), base, available_governor_leaf) == 0) {
        (void)jw__read_line_file(path, out->available_governors,
                                 sizeof(out->available_governors));
    }
    if (jw__join_sysfs_path(path, sizeof(path), base, available_freq_leaf) == 0) {
        (void)jw__read_line_file(path, out->available_frequencies,
                                 sizeof(out->available_frequencies));
    }
}

static const char *jw__mlp1_perf_base(jw_platform_perf_domain domain) {
    switch (domain) {
        case JW_PLATFORM_PERF_DOMAIN_GPU: return JW_MLP1_GPU_DEVFREQ;
        case JW_PLATFORM_PERF_DOMAIN_DMC: return JW_MLP1_DMC_DEVFREQ;
        case JW_PLATFORM_PERF_DOMAIN_CPU:
        default: return JW_MLP1_CPUFREQ_POLICY;
    }
}

static void jw__mlp1_get_performance_status(jw_platform_context *ctx,
                                            jw_platform_perf_status *out) {
    (void)ctx;
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->soc_temp_c = -1;
    for (int i = 0; i < JW_PLATFORM_PERF_DOMAIN_COUNT; i++) {
        jw__mlp1_perf_domain_status((jw_platform_perf_domain)i,
                                    jw__mlp1_perf_base((jw_platform_perf_domain)i),
                                    &out->domains[i]);
    }

    int temp = jw__read_int_file(JW_MLP1_SOC_TEMP);
    if (temp >= 1000) {
        out->soc_temp_c = temp / 1000;
    } else if (temp >= 0) {
        out->soc_temp_c = temp;
    }

    out->supported = out->domains[JW_PLATFORM_PERF_DOMAIN_CPU].supported &&
                     out->domains[JW_PLATFORM_PERF_DOMAIN_GPU].supported &&
                     out->domains[JW_PLATFORM_PERF_DOMAIN_DMC].supported;
    snprintf(out->message, sizeof(out->message), "%s",
             out->supported ? "performance ready" : "performance partially unavailable");
}

static int jw__mlp1_apply_perf_domain(jw_platform_perf_domain domain,
                                      const jw_platform_perf_domain_request *request,
                                      char *message,
                                      size_t message_size) {
    if (!request || !request->governor[0]) {
        return 0;
    }

    const char *base = jw__mlp1_perf_base(domain);
    jw_platform_perf_domain_status status;
    jw__mlp1_perf_domain_status(domain, base, &status);
    if (!status.supported) {
        snprintf(message, message_size, "%s performance unavailable", status.name);
        return -1;
    }
    if (!jw__token_list_has(status.available_governors, request->governor)) {
        snprintf(message, message_size, "%s governor unsupported: %s",
                 status.name, request->governor);
        return -1;
    }
    if (request->frequency >= 0) {
        if (strcmp(request->governor, "userspace") != 0) {
            snprintf(message, message_size, "%s fixed frequency requires userspace governor",
                     status.name);
            return -1;
        }
        if (!jw__freq_list_has(status.available_frequencies, request->frequency)) {
            snprintf(message, message_size, "%s frequency unsupported: %d",
                     status.name, request->frequency);
            return -1;
        }
    }

    char path[PATH_MAX];
    char value[JW_PLATFORM_PERF_VALUE_MAX + 4];
    const char *governor_leaf = (domain == JW_PLATFORM_PERF_DOMAIN_CPU)
                              ? "scaling_governor"
                              : "governor";
    if (jw__join_sysfs_path(path, sizeof(path), base, governor_leaf) != 0) {
        snprintf(message, message_size, "%s governor path invalid", status.name);
        return -1;
    }
    snprintf(value, sizeof(value), "%s\n", request->governor);
    if (jw__write_text_file(path, value) != 0) {
        snprintf(message, message_size, "%s governor write failed: %s",
                 status.name, strerror(errno));
        return -1;
    }

    if (request->frequency >= 0) {
        const char *set_leaf = (domain == JW_PLATFORM_PERF_DOMAIN_CPU)
                             ? "scaling_setspeed"
                             : "userspace/set_freq";
        if (jw__join_sysfs_path(path, sizeof(path), base, set_leaf) != 0) {
            snprintf(message, message_size, "%s frequency path invalid", status.name);
            return -1;
        }
        if (jw__write_int_file(path, request->frequency) != 0) {
            snprintf(message, message_size, "%s frequency write failed: %s",
                     status.name, strerror(errno));
            return -1;
        }
    }

    return 0;
}

static void jw__mlp1_apply_performance(jw_platform_context *ctx,
                                       const jw_platform_perf_request *request,
                                       jw_platform_result *out) {
    (void)ctx;
    char message[JW_PLATFORM_MAX_MESSAGE];
    for (int i = 0; i < JW_PLATFORM_PERF_DOMAIN_COUNT; i++) {
        message[0] = '\0';
        if (jw__mlp1_apply_perf_domain((jw_platform_perf_domain)i,
                                       &request->domains[i],
                                       message, sizeof(message)) != 0) {
            jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED,
                                   message[0] ? message : "performance apply failed");
            return;
        }
    }
    jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "performance applied");
}

/* Map raw backlight value (61-135) to 0-100% for the user-facing OSD. */
static int jw__brightness_raw_to_percent(int raw, int max_raw) {
    (void)max_raw;
    if (raw < 0) {
        return -1;
    }
    if (raw <= JW_MLP1_BACKLIGHT_RAW_MIN) return 0;
    if (raw >= JW_MLP1_BACKLIGHT_RAW_MAX) return 100;

    int range = JW_MLP1_BACKLIGHT_RAW_MAX - JW_MLP1_BACKLIGHT_RAW_MIN;
    return ((raw - JW_MLP1_BACKLIGHT_RAW_MIN) * 100 + range / 2) / range;
}

/* Map 0-100% OSD value to the usable raw range (61-135). */
static int jw__brightness_percent_to_raw(int percent, int max_raw) {
    (void)max_raw;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    int range = JW_MLP1_BACKLIGHT_RAW_MAX - JW_MLP1_BACKLIGHT_RAW_MIN;
    return JW_MLP1_BACKLIGHT_RAW_MIN + (percent * range + 50) / 100;
}

static int jw__mlp1_get_brightness_percent(void) {
    int max_raw = jw__read_int_file(JW_MLP1_BACKLIGHT_MAX);
    int raw = jw__read_int_file(JW_MLP1_BACKLIGHT_ACTUAL);
    if (raw < 0) {
        raw = jw__read_int_file(JW_MLP1_BACKLIGHT_BRIGHTNESS);
    }
    return jw__brightness_raw_to_percent(raw, max_raw);
}

static int jw__read_command_line(const char *cmd, char *out, size_t out_size) {
    if (!cmd || !out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        return -1;
    }
    char *line = fgets(out, (int)out_size, fp);
    int close_rc = pclose(fp);
    if (!line || close_rc == -1) {
        out[0] = '\0';
        return -1;
    }
    out[strcspn(out, "\r\n")] = '\0';
    return out[0] ? 0 : -1;
}

static int jw__parse_percent_from_stream(FILE *fp) {
    if (!fp) {
        return -1;
    }

    char line[256];
    int percent = -1;
    while (fgets(line, sizeof(line), fp)) {
        char *cursor = line;
        while ((cursor = strchr(cursor, '[')) != NULL) {
            int val = 0;
            if (sscanf(cursor, "[%d%%]", &val) == 1 && val >= 0 && val <= 150) {
                percent = val > 100 ? 100 : val;
                break;
            }
            cursor++;
        }
        if (percent >= 0) {
            break;
        }
    }
    return percent;
}

static int jw__mlp1_get_volume_percent(void) {
    jw_platform_audio_output output = jw__mlp1_get_audio_output();
    if (output == JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH) {
        int bt_percent = jw__mlp1_get_bluealsa_volume_percent();
        if (bt_percent >= 0) {
            return bt_percent;
        }
        int volumes[JW_PLATFORM_AUDIO_OUTPUT_COUNT];
        jw__mlp1_get_audio_volumes(volumes);
        return volumes[JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH];
    }

    FILE *fp = popen(JW_MLP1_PACTL_GET_VOLUME, "r");
    if (!fp) {
        return -1;
    }

    char line[256];
    int percent = -1;
    while (fgets(line, sizeof(line), fp)) {
        /* pactl output: "Volume: front-left: 41943 /  64% / -11.63 dB, ..." */
        char *slash = strstr(line, "/");
        while (slash) {
            int val = 0;
            if (sscanf(slash + 1, " %d%%", &val) == 1 && val >= 0 && val <= 150) {
                percent = val > 100 ? 100 : val;
                break;
            }
            slash = strstr(slash + 1, "/");
        }
        if (percent >= 0) break;
    }
    pclose(fp);
    return percent;
}

static int jw__mlp1_set_volume_percent(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    jw_platform_audio_output output = jw__mlp1_get_audio_output();
    int rc;
    if (output == JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH) {
        rc = jw__mlp1_set_bluealsa_volume_percent(percent);
        if (rc == 0) {
            jw__mlp1_sync_sound_volume(output, percent);
        }
        return rc == 0 ? 0 : -1;
    }

    char cmd[128];
    snprintf(cmd, sizeof(cmd), JW_MLP1_PACTL_SET_VOLUME, percent);
    rc = jw__exec_shell(cmd);

    /* Safety net: ensure the hardware DAC has not been left stuck low/muted,
       which would silence the speaker regardless of the sink %. Re-assert the
       boot pin if it has drifted below it. Best-effort; its result does not
       affect whether the volume change itself succeeded. */
    (void)jw__exec_shell(JW_MLP1_ENSURE_DAC_FLOOR);

    if (rc == 0) {
        jw__mlp1_sync_sound_volume(output, percent);
    }
    return rc == 0 ? 0 : -1;
}

static void jw__mlp1_get_wifi_status(jw_platform_status *out) {
    FILE *fp = fopen(JW_MLP1_WIFI_PROC, "r");
    if (!fp) {
        return;
    }

    char line[256];
    /* Skip header lines */
    if (!fgets(line, sizeof(line), fp)) { fclose(fp); return; }
    if (!fgets(line, sizeof(line), fp)) { fclose(fp); return; }

    /* Data line: " wlan0: 0000   95.   23.    0. ..." */
    if (fgets(line, sizeof(line), fp)) {
        int link = 0;
        if (sscanf(line, " %*[^:]: %*d %d.", &link) == 1) {
            out->wifi_connected = (link > 0) ? 1 : 0;
            if (link <= 0) {
                out->wifi_strength = 0;
            } else if (link < 40) {
                out->wifi_strength = 1;
            } else if (link < 70) {
                out->wifi_strength = 2;
            } else {
                out->wifi_strength = 3;
            }
        }
    }
    fclose(fp);
}

static void jw__loong_load(void) {
    if (s_loong_loaded) {
        return;
    }
    s_loong_loaded = true;

    void *handle = dlopen("/usr/lib/libloong_sdk.so", RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        jw_log_error("loong: dlopen libloong_sdk.so failed: %s", dlerror());
        return;
    }

    *(void **)(&s_event_opend) = dlsym(handle, "EventOpend");
    if (!s_event_opend) {
        jw_log_error("loong: EventOpend symbol not found: %s", dlerror());
    }

    *(void **)(&s_power_get)     = dlsym(handle, "_ZN5loong8PowerApi3getEv");
    *(void **)(&s_power_standby) = dlsym(handle, "_ZN5loong8PowerApi12powerStandbyEv");
    if (!s_power_get || !s_power_standby) {
        jw_log_warn("loong: PowerApi standby symbols not found (will fall back to echo mem)");
    }

    *(void **)(&s_write_config) = dlsym(handle, "WriteConfig");
    if (!s_write_config) {
        jw_log_warn("loong: WriteConfig symbol not found (stock settings sync will use DB fallback)");
    }
}

/* Suspend via loong's own PowerApi so loong_power owns the sleep and the wake is
   consistent (single power press). Returns 0 if the call was made, -1 if the
   symbols are unavailable so the caller can fall back to the kernel write. */
static int jw__loong_standby(void) {
    jw__loong_load();
    if (!s_power_get || !s_power_standby) {
        return -1;
    }
    void *api = s_power_get();
    if (!api) {
        return -1;
    }
    s_power_standby(api);
    return 0;
}

/* Reboot or power off via the reboot(2) syscall directly.
   The stock busybox reboot/poweroff applets signal PID 1, but in Leaf mode init
   is blocked in rcS (the umrk-leaf-session supervisor holds the boot), so those
   signals are never serviced and nothing happens. Magic SysRq is also disabled
   by default (/proc/sys/kernel/sysrq = 0). reboot(2) goes straight to the kernel
   and works regardless of init state (we run as root with CAP_SYS_BOOT).
   Done in a forked child after a short delay so the IPC reply reaches the menu
   before the system goes down. cmd is RB_AUTOBOOT or RB_POWER_OFF. */
static int jw__mlp1_power_transition_async(int cmd) {
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        usleep(250000);   /* let the IPC reply flush and the menu close */

        /* Take the filesystems down cleanly before the abrupt reboot(2). The SD
           is FAT32 and busy — our own binary executes from it and the library DB
           is open — so `mount -o remount,ro` returns EBUSY. The kernel's
           emergency remount-ro (magic SysRq 'u') forces every mounted fs
           read-only regardless of open files, flushing the FAT and directory
           entries so an immediate reboot/power-off can't corrupt the card.
           Without this, repeated reboots have produced FAT32 corruption.
           Sequence mirrors REISUB's tail: enable sysrq, Sync, Unmount(ro), Sync. */
        (void)jw__write_text_file("/proc/sys/kernel/sysrq", "1\n");
        sync();
        (void)jw__write_text_file("/proc/sysrq-trigger", "s\n");   /* sync */
        (void)jw__write_text_file("/proc/sysrq-trigger", "u\n");   /* remount-ro all */
        (void)jw__write_text_file("/proc/sysrq-trigger", "s\n");   /* sync */
        usleep(400000);   /* let the emergency remount-ro and flush settle */

        reboot(cmd);
        /* reboot(2) only returns on failure; fall back to a magic SysRq reboot. */
        sleep(1);
        (void)jw__write_text_file("/proc/sysrq-trigger",
                                  cmd == RB_POWER_OFF ? "o\n" : "b\n");
        _exit(0);
    }
    return 0;
}

static char *jw__read_text_file(const char *path, long max_bytes) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long len = ftell(fp);
    if (len < 0 || len > max_bytes || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    buf[got] = '\0';
    return buf;
}

static int jw__write_text_file_atomic(const char *path, const char *text) {
    char tmp[512];
    int needed = snprintf(tmp, sizeof(tmp), "%s.umrk.%ld", path, (long)getpid());
    if (needed < 0 || needed >= (int)sizeof(tmp)) {
        return -1;
    }

    FILE *fp = fopen(tmp, "w");
    if (!fp) {
        return -1;
    }
    int ok = fputs(text, fp) >= 0 && fputc('\n', fp) != EOF;
    ok = fclose(fp) == 0 && ok;
    if (!ok) {
        unlink(tmp);
        return -1;
    }
    chmod(tmp, 0644);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int jw__mkdir_p(const char *dir) {
    if (!dir || !dir[0]) {
        return -1;
    }

    char tmp[JW_PLATFORM_MAX_PATH];
    int needed = snprintf(tmp, sizeof(tmp), "%s", dir);
    if (needed < 0 || needed >= (int)sizeof(tmp)) {
        return -1;
    }

    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') {
        tmp[--len] = '\0';
    }

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
            *p = '/';
            return -1;
        }
        *p = '/';
    }

    return (mkdir(tmp, 0755) == 0 || errno == EEXIST) ? 0 : -1;
}

static int jw__mkdir_parent(const char *path) {
    if (!path || !path[0]) {
        return -1;
    }

    char dir[JW_PLATFORM_MAX_PATH];
    int needed = snprintf(dir, sizeof(dir), "%s", path);
    if (needed < 0 || needed >= (int)sizeof(dir)) {
        return -1;
    }

    char *slash = strrchr(dir, '/');
    if (!slash) {
        return 0;
    }
    if (slash == dir) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    return jw__mkdir_p(dir);
}

static int jw__mlp1_state_marker_path(jw_platform_context *ctx,
                                      const char *override_env,
                                      const char *file_name,
                                      char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return -1;
    }

    const char *override = override_env ? getenv(override_env) : NULL;
    if (override && override[0]) {
        int needed = snprintf(out, out_size, "%s", override);
        return needed >= 0 && needed < (int)out_size ? 0 : -1;
    }

    const char *state = getenv("UMRK_INTERNAL_DATA_PATH");
    if (state && state[0]) {
        int needed = snprintf(out, out_size, "%s/%s", state, file_name);
        return needed >= 0 && needed < (int)out_size ? 0 : -1;
    }

    const char *platform_root = getenv("UMRK_PLATFORM_PATH");
    if (!platform_root || !platform_root[0]) {
        platform_root = getenv("SYSTEM_PATH");
    }
    if (platform_root && platform_root[0]) {
        int needed = snprintf(out, out_size, "%s/state/%s", platform_root,
                              file_name);
        return needed >= 0 && needed < (int)out_size ? 0 : -1;
    }

    const char *sd = (ctx && ctx->sdcard_root[0]) ? ctx->sdcard_root : "/mnt/sdcard";
    int needed = snprintf(out, out_size, "%s/.system/leaf/platforms/mlp1/state/%s",
                          sd, file_name);
    return needed >= 0 && needed < (int)out_size ? 0 : -1;
}

static int jw__mlp1_adb_marker_path(jw_platform_context *ctx,
                                    char *out, size_t out_size) {
    return jw__mlp1_state_marker_path(ctx, "UMRK_ADB_MARKER_PATH",
                                      JW_MLP1_ADB_MARKER_FILE,
                                      out, out_size);
}

static bool jw__mlp1_adb_intent_enabled(jw_platform_context *ctx) {
    char path[JW_PLATFORM_MAX_PATH];
    return jw__mlp1_adb_marker_path(ctx, path, sizeof(path)) == 0 &&
           access(path, F_OK) == 0;
}

static int jw__mlp1_set_adb_intent(jw_platform_context *ctx, bool enabled) {
    char path[JW_PLATFORM_MAX_PATH];
    if (jw__mlp1_adb_marker_path(ctx, path, sizeof(path)) != 0) {
        return -1;
    }

    if (!enabled) {
        return (unlink(path) == 0 || errno == ENOENT) ? 0 : -1;
    }

    if (jw__mkdir_parent(path) != 0) {
        return -1;
    }

    FILE *fp = fopen(path, "w");
    if (!fp) {
        return -1;
    }
    int ok = fputs("1\n", fp) >= 0;
    return fclose(fp) == 0 && ok ? 0 : -1;
}

static int jw__mlp1_boot_splash_disabled_path(jw_platform_context *ctx,
                                              char *out, size_t out_size) {
    return jw__mlp1_state_marker_path(ctx, "UMRK_BOOT_SPLASH_DISABLED_PATH",
                                      JW_MLP1_BOOT_SPLASH_DISABLED_FILE,
                                      out, out_size);
}

static bool jw__mlp1_boot_splash_enabled(jw_platform_context *ctx) {
    char path[JW_PLATFORM_MAX_PATH];
    if (jw__mlp1_boot_splash_disabled_path(ctx, path, sizeof(path)) != 0) {
        return true;
    }
    return access(path, F_OK) != 0;
}

static int jw__mlp1_set_boot_splash(jw_platform_context *ctx, bool enabled) {
    char path[JW_PLATFORM_MAX_PATH];
    if (jw__mlp1_boot_splash_disabled_path(ctx, path, sizeof(path)) != 0) {
        return -1;
    }

    if (enabled) {
        return (unlink(path) == 0 || errno == ENOENT) ? 0 : -1;
    }

    if (jw__mkdir_parent(path) != 0) {
        return -1;
    }

    FILE *fp = fopen(path, "w");
    if (!fp) {
        return -1;
    }
    int ok = fputs("1\n", fp) >= 0;
    return fclose(fp) == 0 && ok ? 0 : -1;
}

static bool jw__mlp1_adb_supported(void) {
    return access(JW_MLP1_ADBD, X_OK) == 0 &&
           access(JW_MLP1_USB_GADGET, X_OK) == 0;
}

static bool jw__mlp1_usb_config_is_adb(void) {
    char *text = jw__read_text_file(JW_MLP1_USB_CONFIG, 64);
    if (!text) {
        return false;
    }
    text[strcspn(text, "\r\n")] = '\0';
    bool result = strcmp(text, "usb_adb_en") == 0;
    free(text);
    return result;
}

static bool jw__mlp1_usb_config_is_immutable(void) {
    FILE *fp = popen("lsattr " JW_MLP1_USB_CONFIG " 2>/dev/null", "r");
    if (!fp) {
        return false;
    }

    char line[128];
    bool immutable = false;
    if (fgets(line, sizeof(line), fp)) {
        char attrs[64];
        if (sscanf(line, "%63s", attrs) == 1 && strchr(attrs, 'i')) {
            immutable = true;
        }
    }
    pclose(fp);
    return immutable;
}

static bool jw__mlp1_adb_is_pinned(void) {
    return jw__mlp1_adb_supported() &&
           jw__mlp1_usb_config_is_adb() &&
           jw__mlp1_usb_config_is_immutable();
}

static void jw__mlp1_enable_adb(jw_platform_context *ctx,
                                jw_platform_result *out) {
    if (!jw__mlp1_adb_supported()) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_UNAVAILABLE,
                               "ADB support unavailable");
        return;
    }

    if (jw__exec_shell(JW_MLP1_ADB_ENABLE_CMD) != 0 ||
        !jw__mlp1_adb_is_pinned()) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED,
                               "ADB pin failed");
        return;
    }

    if (jw__mlp1_set_adb_intent(ctx, true) != 0) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED,
                               "ADB enabled, restore marker failed");
        return;
    }

    if (jw__exec_shell(JW_MLP1_USB_RESTART_CMD) != 0) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_OK,
                               "ADB enabled; USB restart failed");
        return;
    }

    jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "ADB enabled");
}

static void jw__mlp1_disable_adb(jw_platform_context *ctx,
                                 jw_platform_result *out) {
    if (!jw__mlp1_adb_supported()) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_UNAVAILABLE,
                               "ADB support unavailable");
        return;
    }

    if (jw__exec_shell(JW_MLP1_ADB_DISABLE_CMD) != 0 ||
        jw__mlp1_adb_is_pinned()) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED,
                               "ADB disable failed");
        return;
    }

    if (jw__mlp1_set_adb_intent(ctx, false) != 0) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED,
                               "ADB disabled, marker removal failed");
        return;
    }

    if (jw__exec_shell(JW_MLP1_USB_RESTART_CMD) != 0) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_OK,
                               "ADB disabled; USB restart failed");
        return;
    }

    jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "ADB disabled");
}

static int jw__json_put_string(cJSON *object, const char *key, const char *value) {
    if (!object || !key || !value) {
        return -1;
    }
    cJSON *item = cJSON_CreateString(value);
    if (!item) {
        return -1;
    }
    if (cJSON_GetObjectItemCaseSensitive(object, key)) {
        if (!cJSON_ReplaceItemInObjectCaseSensitive(object, key, item)) {
            cJSON_Delete(item);
            return -1;
        }
        return 0;
    }
    if (!cJSON_AddItemToObject(object, key, item)) {
        cJSON_Delete(item);
        return -1;
    }
    return 0;
}

static int jw__json_put_number(cJSON *object, const char *key, int value) {
    if (!object || !key) {
        return -1;
    }
    cJSON *item = cJSON_CreateNumber(value);
    if (!item) {
        return -1;
    }
    if (cJSON_GetObjectItemCaseSensitive(object, key)) {
        if (!cJSON_ReplaceItemInObjectCaseSensitive(object, key, item)) {
            cJSON_Delete(item);
            return -1;
        }
        return 0;
    }
    if (!cJSON_AddItemToObject(object, key, item)) {
        cJSON_Delete(item);
        return -1;
    }
    return 0;
}

static int jw__json_ensure_number(cJSON *object, const char *key, int value) {
    return cJSON_GetObjectItemCaseSensitive(object, key)
               ? 0
               : jw__json_put_number(object, key, value);
}

static int jw__json_ensure_string(cJSON *object, const char *key, const char *value) {
    return cJSON_GetObjectItemCaseSensitive(object, key)
               ? 0
               : jw__json_put_string(object, key, value);
}

static cJSON *jw__load_json_object_file(const char *path, long max_bytes) {
    char *text = jw__read_text_file(path, max_bytes);
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
    }
    return root;
}

static int jw__mlp1_update_power_cfg(int seconds) {
    if (seconds < 0) seconds = 0;
    if (seconds > 86400) seconds = 86400;

    cJSON *root = jw__load_json_object_file(JW_MLP1_POWER_CFG, 4096);
    if (!root) {
        return -1;
    }

    bool disabled = seconds <= 0;
    long timeout_ms = disabled ? 0L : (long)seconds * 1000L;
    char timeout[32];
    snprintf(timeout, sizeof(timeout), "%ld", timeout_ms);

    int rc = 0;
    rc |= jw__json_ensure_string(root, "autoOffLight", "20");
    rc |= jw__json_put_string(root, "hibernateDisable", disabled ? "1" : "0");
    rc |= jw__json_put_string(root, "screenLockDisable", disabled ? "1" : "0");
    rc |= jw__json_put_string(root, "screenLockTimeout", timeout);
    rc |= jw__json_ensure_string(root, "standbyMuteDisable", "0");

    char *printed = rc == 0 ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (!printed) {
        return -1;
    }

    rc = jw__write_text_file_atomic(JW_MLP1_POWER_CFG, printed);
    cJSON_free(printed);
    if (rc == 0) {
        sync();
    }
    return rc;
}

static bool jw__mlp1_json_string_equals(const cJSON *object, const char *key,
                                        const char *expected) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring &&
           strcmp(item->valuestring, expected) == 0;
}

/* True when the on-disk power cfg already encodes `seconds`, so the rewrite
   (and the loong_power restart it forces) can be skipped. Fields written by
   jw__mlp1_update_power_cfg must match exactly; ensure-only fields just need
   to exist. Missing/corrupt cfg yields false and falls back to the write path. */
static bool jw__mlp1_power_cfg_matches(int seconds) {
    cJSON *root = jw__load_json_object_file(JW_MLP1_POWER_CFG, 4096);
    if (!root) {
        return false;
    }

    bool disabled = seconds <= 0;
    char timeout[32];
    snprintf(timeout, sizeof(timeout), "%ld",
             disabled ? 0L : (long)seconds * 1000L);

    bool matches =
        cJSON_GetObjectItemCaseSensitive(root, "autoOffLight") != NULL &&
        cJSON_GetObjectItemCaseSensitive(root, "standbyMuteDisable") != NULL &&
        jw__mlp1_json_string_equals(root, "hibernateDisable", disabled ? "1" : "0") &&
        jw__mlp1_json_string_equals(root, "screenLockDisable", disabled ? "1" : "0") &&
        jw__mlp1_json_string_equals(root, "screenLockTimeout", timeout);
    cJSON_Delete(root);
    return matches;
}

static char *jw__mlp1_read_system_config(const char *param) {
    sqlite3 *db = NULL;
    if (sqlite3_open(JW_MLP1_LOONG_DB_PATH, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 1000);

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT value FROM system_config WHERE param = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_bind_text(stmt, 1, param, -1, SQLITE_TRANSIENT);

    char *out = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        if (text) {
            out = strdup((const char *)text);
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return out;
}

static int jw__mlp1_write_system_config(const char *param, const char *json) {
    sqlite3 *db = NULL;
    if (sqlite3_open(JW_MLP1_LOONG_DB_PATH, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -1;
    }
    sqlite3_busy_timeout(db, 1000);

    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO system_config (param, value, backup) VALUES (?, ?, ?) "
        "ON CONFLICT(param) DO UPDATE SET value = excluded.value, backup = excluded.backup;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, param, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, json, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, json, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc;
}

static char *jw__mlp1_make_display_param(int seconds) {
    if (seconds < 0) seconds = 0;
    if (seconds > 86400) seconds = 86400;

    char *current = jw__mlp1_read_system_config("DISPLAY_PARAM");
    cJSON *root = current ? cJSON_Parse(current) : NULL;
    free(current);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
    }
    if (!root) {
        return NULL;
    }

    int rc = 0;
    rc |= jw__json_ensure_number(root, "dark", 1);
    rc |= jw__json_ensure_number(root, "color", 0);
    rc |= jw__json_ensure_number(root, "temperature", 10);
    rc |= jw__json_ensure_number(root, "brightness", 7);
    rc |= jw__json_put_number(root, "lock", seconds > 0 ? seconds : 0);

    char *printed = rc == 0 ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    return printed;
}

/* Counterpart of jw__mlp1_power_cfg_matches for the loong DISPLAY_PARAM row:
   `lock` (the only field jw__mlp1_make_display_param overwrites) must equal
   `seconds`; the ensure-only fields just need to exist. */
static bool jw__mlp1_display_param_matches(int seconds) {
    if (seconds < 0) seconds = 0;
    if (seconds > 86400) seconds = 86400;

    char *current = jw__mlp1_read_system_config("DISPLAY_PARAM");
    cJSON *root = current ? cJSON_Parse(current) : NULL;
    free(current);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }

    const cJSON *lock = cJSON_GetObjectItemCaseSensitive(root, "lock");
    bool matches =
        cJSON_IsNumber(lock) && lock->valueint == (seconds > 0 ? seconds : 0) &&
        cJSON_GetObjectItemCaseSensitive(root, "dark") != NULL &&
        cJSON_GetObjectItemCaseSensitive(root, "color") != NULL &&
        cJSON_GetObjectItemCaseSensitive(root, "temperature") != NULL &&
        cJSON_GetObjectItemCaseSensitive(root, "brightness") != NULL;
    cJSON_Delete(root);
    return matches;
}

static int jw__mlp1_update_display_param(int seconds) {
    char *json = jw__mlp1_make_display_param(seconds);
    if (!json) {
        return -1;
    }

    jw__loong_load();
    if (s_write_config) {
        (void)s_write_config("DISPLAY_PARAM", json, json, 1);
    }
    int rc = jw__mlp1_write_system_config("DISPLAY_PARAM", json);
    cJSON_free(json);
    return rc;
}

static const char *jw__mlp1_sound_output_key(jw_platform_audio_output output) {
    switch (output) {
        case JW_PLATFORM_AUDIO_OUTPUT_SPEAKER: return "SPEAKER";
        case JW_PLATFORM_AUDIO_OUTPUT_HEADSET: return "HEADSET";
        case JW_PLATFORM_AUDIO_OUTPUT_HDMI: return "HDMI";
        case JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH: return "BLUETOOTH";
        default: return NULL;
    }
}

static int jw__mlp1_percent_to_stock_level(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    return (percent + 5) / 10;
}

static int jw__mlp1_stock_level_to_percent(int level) {
    if (level < 0) level = 0;
    if (level > 10) level = 10;
    return level * 10;
}

static cJSON *jw__mlp1_load_sound_param(void) {
    char *current = jw__mlp1_read_system_config("SOUND_PARAM");
    cJSON *root = current ? cJSON_Parse(current) : NULL;
    free(current);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
    }
    if (!root) {
        return NULL;
    }

    (void)jw__json_ensure_number(root, "bgm", 0);
    (void)jw__json_ensure_number(root, "sysVolume", 10);
    (void)jw__json_ensure_number(root, "toneVolume", 10);
    cJSON *devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (!cJSON_IsObject(devices)) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "devices");
        devices = cJSON_CreateObject();
        if (!devices || !cJSON_AddItemToObject(root, "devices", devices)) {
            cJSON_Delete(devices);
            cJSON_Delete(root);
            return NULL;
        }
    }
    for (int i = 0; i < JW_PLATFORM_AUDIO_OUTPUT_COUNT; i++) {
        const char *key = jw__mlp1_sound_output_key((jw_platform_audio_output)i);
        if (key) {
            (void)jw__json_ensure_number(devices, key, 10);
        }
    }
    return root;
}

static int jw__mlp1_write_sound_param(cJSON *root) {
    if (!root) {
        return -1;
    }
    char *printed = cJSON_PrintUnformatted(root);
    if (!printed) {
        return -1;
    }

    jw__loong_load();
    if (s_write_config) {
        (void)s_write_config("SOUND_PARAM", printed, printed, 1);
    }
    int rc = jw__mlp1_write_system_config("SOUND_PARAM", printed);
    cJSON_free(printed);
    return rc;
}

static void jw__mlp1_get_audio_volumes(int out[JW_PLATFORM_AUDIO_OUTPUT_COUNT]) {
    if (!out) {
        return;
    }
    for (int i = 0; i < JW_PLATFORM_AUDIO_OUTPUT_COUNT; i++) {
        out[i] = -1;
    }

    cJSON *root = jw__mlp1_load_sound_param();
    if (!root) {
        return;
    }
    cJSON *devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
    for (int i = 0; i < JW_PLATFORM_AUDIO_OUTPUT_COUNT; i++) {
        const char *key = jw__mlp1_sound_output_key((jw_platform_audio_output)i);
        const cJSON *level = key ? cJSON_GetObjectItemCaseSensitive(devices, key) : NULL;
        if (cJSON_IsNumber(level)) {
            out[i] = jw__mlp1_stock_level_to_percent(level->valueint);
        }
    }
    cJSON_Delete(root);
}

static void jw__mlp1_sync_sound_volume(jw_platform_audio_output output, int percent) {
    const char *key = jw__mlp1_sound_output_key(output);
    if (!key) {
        return;
    }
    cJSON *root = jw__mlp1_load_sound_param();
    if (!root) {
        return;
    }
    cJSON *devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (cJSON_IsObject(devices)) {
        (void)jw__json_put_number(devices, key,
                                  jw__mlp1_percent_to_stock_level(percent));
    }
    (void)jw__mlp1_write_sound_param(root);
    cJSON_Delete(root);
}

static int jw__mlp1_playback_path(void) {
    FILE *fp = popen(JW_MLP1_PLAYBACK_PATH_CMD, "r");
    if (!fp) {
        return -1;
    }
    char line[256];
    int value = -1;
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, " : values=%d", &value) == 1 ||
            sscanf(line, "  : values=%d", &value) == 1 ||
            sscanf(line, ": values=%d", &value) == 1) {
            break;
        }
    }
    pclose(fp);
    return value;
}

static bool jw__mlp1_pulse_sink_exists(const char *needle) {
    if (!needle || !needle[0]) {
        return false;
    }
    FILE *fp = popen("pactl list short sinks 2>/dev/null", "r");
    if (!fp) {
        return false;
    }
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, needle)) {
            found = true;
            break;
        }
    }
    pclose(fp);
    return found;
}

static bool jw__mlp1_hdmi_connected(void) {
    char *text = jw__read_text_file(JW_MLP1_HDMI_STATUS, 64);
    if (!text) {
        return false;
    }
    text[strcspn(text, "\r\n")] = '\0';
    bool connected = strcmp(text, "connected") == 0;
    free(text);
    return connected;
}

static bool jw__mlp1_bluealsa_control(char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    FILE *fp = popen("amixer -D bluealsa scontrols 2>/dev/null", "r");
    if (!fp) {
        return false;
    }
    char line[256];
    bool ok = false;
    while (fgets(line, sizeof(line), fp)) {
        char ctl[128];
        if (sscanf(line, "Simple mixer control '%127[^']'", ctl) != 1) {
            continue;
        }
        bool safe = true;
        for (const char *p = ctl; *p; p++) {
            if (!( (*p >= 'A' && *p <= 'Z') ||
                   (*p >= 'a' && *p <= 'z') ||
                   (*p >= '0' && *p <= '9') ||
                   *p == ':' || *p == '_' || *p == '-' || *p == '.' || *p == ' ')) {
                safe = false;
                break;
            }
        }
        if (safe) {
            snprintf(out, out_size, "%s", ctl);
            ok = true;
            break;
        }
    }
    pclose(fp);
    return ok;
}

static int jw__mlp1_get_bluealsa_volume_percent(void) {
    char ctl[128];
    if (!jw__mlp1_bluealsa_control(ctl, sizeof(ctl))) {
        return -1;
    }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "amixer -D bluealsa sget '%s' 2>/dev/null", ctl);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        return -1;
    }
    int percent = jw__parse_percent_from_stream(fp);
    pclose(fp);
    return percent;
}

static int jw__mlp1_set_bluealsa_volume_percent(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    char ctl[128];
    if (!jw__mlp1_bluealsa_control(ctl, sizeof(ctl))) {
        return -1;
    }
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "amixer -D bluealsa sset '%s' %d%% >/dev/null 2>&1",
             ctl, percent);
    return jw__exec_shell(cmd);
}

static unsigned jw__mlp1_get_audio_available_outputs(void) {
    unsigned mask = JW_PLATFORM_AUDIO_OUTPUT_BIT(JW_PLATFORM_AUDIO_OUTPUT_SPEAKER) |
                    JW_PLATFORM_AUDIO_OUTPUT_BIT(JW_PLATFORM_AUDIO_OUTPUT_HEADSET);
    if (jw__mlp1_hdmi_connected() &&
        jw__mlp1_pulse_sink_exists(JW_MLP1_PACTL_HDMI_SINK)) {
        mask |= JW_PLATFORM_AUDIO_OUTPUT_BIT(JW_PLATFORM_AUDIO_OUTPUT_HDMI);
    }
    if (jw_bt_audio_connected() == 1) {
        mask |= JW_PLATFORM_AUDIO_OUTPUT_BIT(JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH);
    }
    return mask;
}

static jw_platform_audio_output jw__mlp1_get_audio_output(void) {
    int path = jw__mlp1_playback_path();
    if (path == 5) {
        return JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH;
    }

    char sink[256];
    if (jw__read_command_line(JW_MLP1_PACTL_GET_DEFAULT_SINK, sink, sizeof(sink)) == 0 &&
        strstr(sink, "hdmi")) {
        return JW_PLATFORM_AUDIO_OUTPUT_HDMI;
    }

    if (path == 3 || path == 4) {
        return JW_PLATFORM_AUDIO_OUTPUT_HEADSET;
    }
    return JW_PLATFORM_AUDIO_OUTPUT_SPEAKER;
}

static int jw__mlp1_apply_stored_audio_volume(jw_platform_audio_output output) {
    int volumes[JW_PLATFORM_AUDIO_OUTPUT_COUNT];
    jw__mlp1_get_audio_volumes(volumes);
    int percent = (output >= 0 && output < JW_PLATFORM_AUDIO_OUTPUT_COUNT)
                    ? volumes[output]
                    : -1;
    if (percent < 0) {
        return 0;
    }
    if (output == JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH) {
        (void)jw__mlp1_set_bluealsa_volume_percent(percent);
        return 0;
    }
    return jw__mlp1_set_volume_percent(percent);
}

static int jw__mlp1_set_audio_output(jw_platform_audio_output output,
                                     jw_platform_result *out) {
    if (output < 0 || output >= JW_PLATFORM_AUDIO_OUTPUT_COUNT) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_INVALID,
                               "audio output invalid");
        return -1;
    }

    unsigned available = jw__mlp1_get_audio_available_outputs();
    if ((available & JW_PLATFORM_AUDIO_OUTPUT_BIT(output)) == 0) {
        char message[JW_PLATFORM_MAX_MESSAGE];
        snprintf(message, sizeof(message), "%s output unavailable",
                 jw_platform_audio_output_label(output));
        jw_platform_result_set(out, JW_PLATFORM_RESULT_UNAVAILABLE, message);
        return -1;
    }

    int rc = -1;
    switch (output) {
        case JW_PLATFORM_AUDIO_OUTPUT_SPEAKER:
            rc = jw__exec_shell(JW_MLP1_PACTL_SET_DEFAULT_RK817 " && "
                                JW_MLP1_PLAYBACK_PATH_SPK " && "
                                JW_MLP1_ENSURE_DAC_FLOOR);
            break;
        case JW_PLATFORM_AUDIO_OUTPUT_HEADSET:
            rc = jw__exec_shell(JW_MLP1_PACTL_SET_DEFAULT_RK817 " && "
                                JW_MLP1_PLAYBACK_PATH_HP " && "
                                JW_MLP1_ENSURE_DAC_FLOOR);
            break;
        case JW_PLATFORM_AUDIO_OUTPUT_HDMI:
            rc = jw__exec_shell(JW_MLP1_PACTL_SET_DEFAULT_HDMI);
            break;
        case JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH:
            rc = jw__exec_shell(JW_MLP1_PLAYBACK_PATH_BT);
            break;
        default:
            break;
    }

    if (rc != 0) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED,
                               "audio output route failed");
        return -1;
    }

    (void)jw__mlp1_apply_stored_audio_volume(output);
    char message[JW_PLATFORM_MAX_MESSAGE];
    snprintf(message, sizeof(message), "audio output: %s",
             jw_platform_audio_output_label(output));
    jw_platform_result_set_value(out, JW_PLATFORM_RESULT_OK, message, (int)output);
    return 0;
}

static void jw__mlp1_set_auto_sleep(int seconds, jw_platform_result *out) {
    if (seconds < 0) seconds = 0;
    if (seconds > 86400) seconds = 86400;

    /* Boot-time sync calls this with an unchanged setting on every boot; when
       the stock stores already match, skip the rewrites and especially the
       loong_power restart, which otherwise lands while the launcher is waiting
       on its hello and adds ~300ms to time-to-first-frame. */
    if (jw__mlp1_display_param_matches(seconds) && jw__mlp1_power_cfg_matches(seconds)) {
        jw_log_info("stock auto-sleep already in sync (%ds)", seconds);
        char synced[JW_PLATFORM_MAX_MESSAGE];
        if (seconds <= 0) {
            snprintf(synced, sizeof(synced), "stock auto-sleep disabled");
        } else {
            snprintf(synced, sizeof(synced), "stock auto-sleep set to %ds", seconds);
        }
        jw_platform_result_set_value(out, JW_PLATFORM_RESULT_OK, synced, seconds);
        return;
    }

    int display_rc = jw__mlp1_update_display_param(seconds);
    int power_rc = jw__mlp1_update_power_cfg(seconds);
    int live_rc = (display_rc == 0 && power_rc == 0) ? jw__mlp1_apply_power_cfg_live() : 0;
    if (display_rc != 0 || power_rc != 0 || live_rc != 0) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED,
                               "stock auto-sleep sync failed");
        return;
    }

    char message[JW_PLATFORM_MAX_MESSAGE];
    if (seconds <= 0) {
        snprintf(message, sizeof(message), "stock auto-sleep disabled");
    } else {
        snprintf(message, sizeof(message), "stock auto-sleep set to %ds", seconds);
    }
    jw_platform_result_set_value(out, JW_PLATFORM_RESULT_OK, message, seconds);
}

static void jw__mlp1_get_status(jw_platform_context *ctx, jw_platform_status *out) {
    if (!out) {
        return;
    }

    int battery = jw__read_int_file("/sys/class/power_supply/battery/capacity");
    if (battery >= 0 && battery <= 100) {
        out->battery_percent = battery;
    }

    int charger = jw__read_int_file("/sys/class/power_supply/ac/online");
    if (charger != 1) {
        charger = jw__read_int_file("/sys/class/power_supply/usb/online");
    }
    if (charger >= 0) {
        out->charging = (charger == 1) ? 1 : 0;
    }

    out->brightness_percent = jw__mlp1_get_brightness_percent();
    out->volume_percent = jw__mlp1_get_volume_percent();
    jw__mlp1_get_wifi_status(out);

    if (jw__mlp1_adb_supported()) {
        out->adb_enabled = jw__mlp1_adb_is_pinned() ? 1 : 0;
        out->adb_intent_enabled = jw__mlp1_adb_intent_enabled(ctx) ? 1 : 0;
    }
    out->boot_splash_enabled = jw__mlp1_boot_splash_enabled(ctx) ? 1 : 0;
}

static void jw__mlp1_get_audio_status(jw_platform_context *ctx, jw_platform_status *out) {
    (void)ctx;
    if (!out) {
        return;
    }
    out->volume_percent = jw__mlp1_get_volume_percent();
    out->audio_output = jw__mlp1_get_audio_output();
    out->audio_available_outputs = jw__mlp1_get_audio_available_outputs();
    jw__mlp1_get_audio_volumes(out->audio_volume_percent);
    if (out->audio_output >= 0 && out->audio_output < JW_PLATFORM_AUDIO_OUTPUT_COUNT &&
        out->volume_percent >= 0) {
        out->audio_volume_percent[out->audio_output] = out->volume_percent;
    }
}

static void jw__mlp1_frontend_ready(jw_platform_context *ctx, const char *role,
                                    jw_platform_result *out) {
    if (strcmp(role, "launcher") != 0) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "frontend ready noted");
        return;
    }

    if (ctx->home_ready_sent) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "home ready already sent");
        return;
    }

    if (jw__env_truthy("UMRK_LEAF_MODE")) {
        (void)jw__exec_shell("killall loong_transition >/dev/null 2>&1 || true");
        jw_log_info("loong: dismissed Leaf boot transition");
        ctx->home_ready_sent = true;
        jw_platform_result_set(out, JW_PLATFORM_RESULT_OK,
                               "leaf boot transition dismissed");
        return;
    }

    jw__loong_load();
    if (!s_event_opend) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_UNAVAILABLE,
                               "loong SDK EventOpend unavailable");
        return;
    }

    int rc = s_event_opend("HOME");
    jw_log_info("loong: EventOpend(\"HOME\") -> %d (dismissing boot transition)", rc);
    (void)jw__exec_shell("killall loong_transition >/dev/null 2>&1 || true");
    ctx->home_ready_sent = true;
    jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "home ready sent");
}

static void jw__mlp1_perform_action(jw_platform_context *ctx, jw_platform_action action,
                                    int value, jw_platform_result *out) {
    if (action == JW_PLATFORM_ACTION_POWEROFF) {
        jw_log_info("platform: poweroff requested");
        if (jw__mlp1_power_transition_async(RB_POWER_OFF) != 0) {
            jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED, "poweroff failed");
            return;
        }
        jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "powering off");
        return;
    }

    if (action == JW_PLATFORM_ACTION_REBOOT) {
        jw_log_info("platform: reboot requested");
        if (jw__mlp1_power_transition_async(RB_AUTOBOOT) != 0) {
            jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED, "reboot failed");
            return;
        }
        jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "rebooting");
        return;
    }

    if (action == JW_PLATFORM_ACTION_SLEEP) {
        jw_log_info("platform: sleep requested");
        /* Prefer loong's own standby so loong_power stays in sync (single-press
           wake). Fall back to the raw kernel write if the SDK symbols are absent. */
        if (jw__loong_standby() == 0) {
            jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "suspending (loong)");
            return;
        }
        FILE *fp = fopen("/sys/power/state", "w");
        if (fp) {
            fprintf(fp, "mem\n");
            fclose(fp);
            jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "suspending");
        } else {
            jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED, "sleep failed");
        }
        return;
    }

    if (action == JW_PLATFORM_ACTION_SCREEN_OFF ||
        action == JW_PLATFORM_ACTION_SCREEN_ON) {
        int blank = (action == JW_PLATFORM_ACTION_SCREEN_OFF) ? 4 : 0;
        if (jw__write_int_file(JW_MLP1_BACKLIGHT_BL_POWER, blank) != 0) {
            jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED,
                                   "backlight blank write failed");
            return;
        }
        jw_platform_result_set(out, JW_PLATFORM_RESULT_OK,
                               blank ? "screen off" : "screen on");
        return;
    }

    if (action == JW_PLATFORM_ACTION_SET_AUTO_SLEEP) {
        jw__mlp1_set_auto_sleep(value, out);
        return;
    }

    if (action == JW_PLATFORM_ACTION_SET_VOLUME) {
        int percent = value;
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        if (jw__mlp1_set_volume_percent(percent) != 0) {
            jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED,
                                   "volume set failed");
            return;
        }
        char message[JW_PLATFORM_MAX_MESSAGE];
        snprintf(message, sizeof(message), "volume set to %d%%", percent);
        jw_platform_result_set_value(out, JW_PLATFORM_RESULT_OK, message, percent);
        return;
    }

    if (action == JW_PLATFORM_ACTION_SET_AUDIO_OUTPUT) {
        (void)jw__mlp1_set_audio_output((jw_platform_audio_output)value, out);
        return;
    }

    if (action == JW_PLATFORM_ACTION_BLUETOOTH_ON ||
        action == JW_PLATFORM_ACTION_BLUETOOTH_OFF) {
        bool on = (action == JW_PLATFORM_ACTION_BLUETOOTH_ON);
        if (jw_bt_set_radio(on) != 0) {
            jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED,
                                   on ? "bluetooth enable failed"
                                      : "bluetooth disable failed");
            return;
        }
        jw_platform_result_set(out, JW_PLATFORM_RESULT_OK,
                               on ? "bluetooth enabled" : "bluetooth disabled");
        return;
    }

    if (action == JW_PLATFORM_ACTION_ENABLE_ADB) {
        jw__mlp1_enable_adb(ctx, out);
        return;
    }

    if (action == JW_PLATFORM_ACTION_DISABLE_ADB) {
        jw__mlp1_disable_adb(ctx, out);
        return;
    }

    if (action == JW_PLATFORM_ACTION_SET_BOOT_SPLASH) {
        bool enabled = value != 0;
        if (jw__mlp1_set_boot_splash(ctx, enabled) != 0) {
            jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED,
                                   enabled ? "boot splash enable failed"
                                           : "boot splash disable failed");
            return;
        }
        jw_platform_result_set(out, JW_PLATFORM_RESULT_OK,
                               enabled ? "boot splash enabled"
                                       : "boot splash disabled");
        return;
    }

    if (action != JW_PLATFORM_ACTION_SET_BRIGHTNESS) {
        jw_platform_result_unsupported(action, ctx ? ctx->platform_id : "mlp1", out);
        return;
    }

    int percent = jw_platform_clamp_brightness_percent(value);
    int max_raw = jw__read_int_file(JW_MLP1_BACKLIGHT_MAX);
    int raw = jw__brightness_percent_to_raw(percent, max_raw);
    if (raw < 0) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_UNAVAILABLE,
                               "backlight max brightness unavailable");
        return;
    }
    if (jw__write_int_file(JW_MLP1_BACKLIGHT_BRIGHTNESS, raw) != 0) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED,
                               "backlight brightness write failed");
        return;
    }

    char message[JW_PLATFORM_MAX_MESSAGE];
    snprintf(message, sizeof(message), "brightness set to %d%%", percent);
    jw_platform_result_set_value(out, JW_PLATFORM_RESULT_OK, message, percent);
}

static bool jw__mlp1_storage_tick(jw_platform_context *ctx) {
    jw_mlp1_platform_data *data = ctx ? (jw_mlp1_platform_data *)ctx->backend_data : NULL;
    if (!data) {
        return false;
    }

    if (data->uevent_fd >= 0) {
        char buf[4096];
        while (1) {
            ssize_t n = recv(data->uevent_fd, buf, sizeof(buf) - 1, 0);
            if (n < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    jw_log_warn("storage hotplug: uevent recv failed: %s", strerror(errno));
                }
                break;
            }
            if (n == 0) {
                break;
            }
            buf[n] = '\0';
            if (strstr(buf, "mmcblk3") || strstr(buf, JW_MLP1_SECONDARY_DEVICE)) {
                data->pending_storage_event = true;
                data->debounce_until_ms = jw__monotonic_ms() + JW_MLP1_STORAGE_DEBOUNCE_MS;
            }
        }
    }

    bool changed = false;
    int present = jw__mlp1_block_present() ? 1 : 0;
    int mounted = jw__mlp1_mount_is_active() ? 1 : 0;

    if (data->pending_storage_event &&
        jw__monotonic_ms() >= data->debounce_until_ms) {
        data->pending_storage_event = false;
        if (present) {
            if (jw__mlp1_mount_secondary_if_needed() == 0) {
                jw_log_info("storage hotplug: secondary SD mounted/remounted");
            } else {
                jw_log_warn("storage hotplug: secondary SD mount/remount failed");
            }
        }
        present = jw__mlp1_block_present() ? 1 : 0;
        mounted = jw__mlp1_mount_is_active() ? 1 : 0;
        changed = true;
    }

    if (present != data->last_present || mounted != data->last_mounted) {
        changed = true;
    }

    if (changed) {
        jw_log_info("storage hotplug: secondary SD present=%d mounted=%d",
                    present, mounted);
        data->last_present = present;
        data->last_mounted = mounted;
    }
    return changed;
}

static void jw__mlp1_get_storage_status(jw_platform_context *ctx,
                                        const char *source_id,
                                        jw_platform_storage_status *out) {
    (void)ctx;
    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));
    snprintf(out->source_id, sizeof(out->source_id), "%s",
             source_id && source_id[0] ? source_id : JW_MLP1_SECONDARY_SOURCE_ID);
    snprintf(out->label, sizeof(out->label), "%s", JW_MLP1_SECONDARY_LABEL);
    snprintf(out->mount_path, sizeof(out->mount_path), "%s", JW_MLP1_SECONDARY_MOUNT);
    snprintf(out->device_path, sizeof(out->device_path), "%s", JW_MLP1_SECONDARY_DEVICE);

    if (!jw__mlp1_source_is_secondary(source_id)) {
        snprintf(out->message, sizeof(out->message), "%s", "storage source unavailable");
        return;
    }

    out->present = jw__mlp1_block_present();
    out->mounted = jw__mlp1_mount_is_active();
    out->busy = out->mounted ? jw__mlp1_storage_busy() : false;
    out->can_unmount = out->mounted && !out->busy;
    snprintf(out->message, sizeof(out->message), "%s",
             out->busy ? "Busy" : (out->mounted ? "Mounted" : "Not mounted"));
}

static void jw__mlp1_safe_unmount_storage(jw_platform_context *ctx,
                                          const char *source_id,
                                          jw_platform_result *out) {
    jw_mlp1_platform_data *data = ctx ? (jw_mlp1_platform_data *)ctx->backend_data : NULL;
    if (!jw__mlp1_source_is_secondary(source_id)) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_INVALID,
                               "only secondary SD can be unmounted");
        return;
    }

    if (!jw__mlp1_mount_is_active()) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_UNAVAILABLE,
                               "Secondary SD is not mounted");
        return;
    }

    if (jw__mlp1_storage_busy()) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_UNAVAILABLE,
                               "Secondary SD is busy");
        return;
    }

    sync();
    if (jw__exec_shell("umount " JW_MLP1_SECONDARY_MOUNT " >/dev/null 2>&1") != 0) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED,
                               "Secondary SD unmount failed");
        return;
    }

    if (data) {
        data->last_present = jw__mlp1_block_present() ? 1 : 0;
        data->last_mounted = 0;
    }
    jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "Secondary SD unmounted");
}

/* Send a signal to every process whose /proc/<pid>/comm matches name.
   Avoids a pgrep dependency. Returns the number of processes signalled. */
static int jw__mlp1_signal_by_name(const char *name, int sig) {
    DIR *proc = opendir("/proc");
    if (!proc) return 0;
    struct dirent *entry;
    char path[300];
    char comm[128];
    int signalled = 0;
    while ((entry = readdir(proc)) != NULL) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        if (fgets(comm, sizeof(comm), fp)) {
            comm[strcspn(comm, "\n")] = '\0';
            if (strcmp(comm, name) == 0) {
                if (kill((pid_t)atoi(entry->d_name), sig) == 0) signalled++;
            }
        }
        fclose(fp);
    }
    closedir(proc);
    return signalled;
}

static int jw__mlp1_spawn_loong_power(void) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        chdir("/loong");
        const char *old_ld = getenv("LD_LIBRARY_PATH");
        char ld[512];
        if (old_ld && old_ld[0]) {
            snprintf(ld, sizeof(ld), "./:%s", old_ld);
        } else {
            snprintf(ld, sizeof(ld), "%s", "./");
        }
        setenv("LD_LIBRARY_PATH", ld, 1);
        execl("/loong/loong_power", "loong_power", JW_MLP1_POWER_CFG, (char *)NULL);
        _exit(127);
    }
    return 0;
}

static int jw__mlp1_restart_leaf_loong_power(void) {
    int signalled = jw__mlp1_signal_by_name(JW_MLP1_POWER_DAEMON, SIGTERM);
    if (signalled > 0) {
        usleep(300000);
        jw__mlp1_signal_by_name(JW_MLP1_POWER_DAEMON, SIGKILL);
    }
    return jw__mlp1_spawn_loong_power();
}

static int jw__mlp1_apply_power_cfg_live(void) {
    if (!jw__env_truthy("UMRK_LEAF_MODE")) {
        return 0;
    }
    if (jw__mlp1_restart_leaf_loong_power() != 0) {
        jw_log_warn("loong_power: restart failed after power cfg update");
        return -1;
    }
    jw_log_info("loong_power: restarted after power cfg update");
    return 0;
}

static void jw__mlp1_set_led(jw_platform_context *ctx, const jw_led_config *cfg,
                             jw_platform_result *out) {
    (void)ctx;
    if (!cfg) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_INVALID, "no led config");
        return;
    }
    int brightness = cfg->brightness;
    if (brightness < 0) brightness = 0;
    if (brightness > JW_LED_BRIGHTNESS_MAX) brightness = JW_LED_BRIGHTNESS_MAX;
    int speed = cfg->speed;
    if (speed < 0) speed = 0;
    if (speed > JW_LED_SPEED_MAX) speed = JW_LED_SPEED_MAX;

    FILE *fp = fopen(JW_MLP1_LED_CFG, "w");
    if (!fp) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED, "led cfg write failed");
        return;
    }
    /* colors is a JSON STRING holding an array of {a,r,g,b} (loong's schema). */
    fprintf(fp,
            "{\"brightness\":\"%d\","
            "\"colors\":\"[{\\\"a\\\":0,\\\"b\\\":%u,\\\"g\\\":%u,\\\"r\\\":%u}]\","
            "\"enable\":\"%d\",\"mode\":\"%s\",\"speed\":\"%d\"}",
            brightness,
            (unsigned)cfg->b, (unsigned)cfg->g, (unsigned)cfg->r,
            cfg->enabled ? 1 : 0, jw_led_mode_name(cfg->mode), speed);
    fclose(fp);

    if (jw__env_truthy("UMRK_LEAF_MODE")) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_OK,
                               "led cfg written (leaf driver owns live state)");
        return;
    }

    if (jw__mlp1_signal_by_name(JW_MLP1_LED_DAEMON, SIGUSR1) <= 0) {
        /* cfg is written; daemon will pick it up on its next reload anyway. */
        jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "led cfg written (daemon not signalled)");
        return;
    }
    jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "led applied");
}

const jw_platform_backend *jw_platform_get_backend(void) {
    static const jw_platform_backend backend = {
        .platform_id = "mlp1",
        .platform_name = "Miniloong Pocket 1",
        .capabilities = {
            .battery = true,
            .charging = true,
            .sleep = true,
            .poweroff = true,
            .reboot = true,
            .brightness = true,
            .volume = true,
            .wifi = true,
            .bluetooth = true,
            .adb = true,
            .boot_splash = true,
            .led = true,
            .performance = true,
        },
        .init = jw__mlp1_init,
        .shutdown = jw__mlp1_shutdown,
        .get_status = jw__mlp1_get_status,
        .get_audio_status = jw__mlp1_get_audio_status,
        .frontend_ready = jw__mlp1_frontend_ready,
        .perform_action = jw__mlp1_perform_action,
        .get_performance_status = jw__mlp1_get_performance_status,
        .apply_performance = jw__mlp1_apply_performance,
        .storage_tick = jw__mlp1_storage_tick,
        .get_storage_status = jw__mlp1_get_storage_status,
        .safe_unmount_storage = jw__mlp1_safe_unmount_storage,
        .set_led = jw__mlp1_set_led,
    };
    return &backend;
}
