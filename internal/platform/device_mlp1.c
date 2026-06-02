#include "internal/platform/device_backend.h"
#include "internal/core/log.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <dlfcn.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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
#define JW_MLP1_BACKLIGHT_ACTUAL JW_MLP1_BACKLIGHT_DIR "/actual_brightness"
#define JW_MLP1_BACKLIGHT_MAX JW_MLP1_BACKLIGHT_DIR "/max_brightness"

#define JW_MLP1_PACTL_GET_VOLUME "pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null"
#define JW_MLP1_PACTL_SET_VOLUME "pactl set-sink-volume @DEFAULT_SINK@ %d%% 2>/dev/null"
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

/* Run a command by path with fork/exec instead of system(). */
static int jw__exec_command(const char *path) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execl(path, path, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

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

typedef struct {
    int uevent_fd;
    int last_present;
    int last_mounted;
    bool pending_storage_event;
    long long debounce_until_ms;
} jw_mlp1_platform_data;

static int (*s_event_opend)(const char *id);
static bool s_loong_loaded;

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

static int jw__mlp1_get_volume_percent(void) {
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

    char cmd[128];
    snprintf(cmd, sizeof(cmd), JW_MLP1_PACTL_SET_VOLUME, percent);
    return jw__exec_shell(cmd) == 0 ? 0 : -1;
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
}

static void jw__mlp1_get_status(jw_platform_context *ctx, jw_platform_status *out) {
    (void)ctx;
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

    jw__loong_load();
    if (!s_event_opend) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_UNAVAILABLE,
                               "loong SDK EventOpend unavailable");
        return;
    }

    int rc = s_event_opend("HOME");
    jw_log_info("loong: EventOpend(\"HOME\") -> %d (dismissing boot transition)", rc);
    ctx->home_ready_sent = true;
    jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "home ready sent");
}

static void jw__mlp1_perform_action(jw_platform_context *ctx, jw_platform_action action,
                                    int value, jw_platform_result *out) {
    if (action == JW_PLATFORM_ACTION_POWEROFF) {
        jw_log_info("platform: poweroff requested");
        sync();
        if (jw__exec_command("/usr/sbin/poweroff") != 0) {
            jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED, "poweroff failed");
            return;
        }
        jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "powering off");
        return;
    }

    if (action == JW_PLATFORM_ACTION_REBOOT) {
        jw_log_info("platform: reboot requested");
        sync();
        if (jw__exec_command("/usr/sbin/reboot") != 0) {
            jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED, "reboot failed");
            return;
        }
        jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "rebooting");
        return;
    }

    if (action == JW_PLATFORM_ACTION_SLEEP) {
        jw_log_info("platform: sleep requested");
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
            .led = true,
        },
        .init = jw__mlp1_init,
        .shutdown = jw__mlp1_shutdown,
        .get_status = jw__mlp1_get_status,
        .frontend_ready = jw__mlp1_frontend_ready,
        .perform_action = jw__mlp1_perform_action,
        .storage_tick = jw__mlp1_storage_tick,
        .get_storage_status = jw__mlp1_get_storage_status,
        .safe_unmount_storage = jw__mlp1_safe_unmount_storage,
        .set_led = jw__mlp1_set_led,
    };
    return &backend;
}
