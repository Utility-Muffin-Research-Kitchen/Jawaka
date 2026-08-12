#define _GNU_SOURCE

#include "internal/platform/input_roster.h"

#include "internal/core/log.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <linux/input.h>

#define JW__ROSTER_SCAN_MAX 64

static void jw__roster_set_error(char *error, size_t error_size,
                                 const char *code, const char *detail) {
    if (!error || error_size == 0) {
        return;
    }
    snprintf(error, error_size, "%s: %s", code, detail ? detail : "unknown");
}

static bool jw__bit_is_set(const unsigned long *bits, unsigned int bit,
                           size_t bit_count) {
    if (bit / (sizeof(unsigned long) * 8) >= bit_count) {
        return false;
    }
    return (bits[bit / (sizeof(unsigned long) * 8)] &
            (1UL << (bit % (sizeof(unsigned long) * 8)))) != 0;
}

/* A device is a gamepad when it exposes any button in the gamepad block
   (BTN_GAMEPAD/SOUTH..BTN_THUMBR) or the d-pad hats-as-buttons range. */
static bool jw__is_gamepad_capable(int fd) {
    unsigned long key_bits[(KEY_MAX + 8 * sizeof(unsigned long)) /
                           (8 * sizeof(unsigned long))] = {0};
    size_t byte_count = sizeof(key_bits);
    if (ioctl(fd, EVIOCGBIT(EV_KEY, byte_count), key_bits) < 0) {
        return false;
    }
    size_t word_count = byte_count / sizeof(unsigned long);
    for (unsigned int bit = BTN_GAMEPAD; bit <= BTN_THUMBR; bit++) {
        if (jw__bit_is_set(key_bits, bit, word_count)) {
            return true;
        }
    }
    for (unsigned int bit = BTN_DPAD_UP; bit <= BTN_DPAD_RIGHT; bit++) {
        if (jw__bit_is_set(key_bits, bit, word_count)) {
            return true;
        }
    }
    return false;
}

bool jw_input_device_is_gamepad(const char *path) {
    if (!path) {
        return false;
    }
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    bool gamepad = jw__is_gamepad_capable(fd);
    close(fd);
    return gamepad;
}

static bool jw__same_rdev(const char *a, const char *b) {
    struct stat sa;
    struct stat sb;
    return a && b && stat(a, &sa) == 0 && stat(b, &sb) == 0 &&
           sa.st_rdev == sb.st_rdev;
}

static int jw__fill_entry(const char *path, jw_input_roster_entry *entry,
                          bool is_virtual) {
    memset(entry, 0, sizeof(*entry));
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return -1;
    }
    entry->rdev = st.st_rdev;
    entry->is_virtual = is_virtual;
    snprintf(entry->path, sizeof(entry->path), "%s", path);

    char name[JW_INPUT_ROSTER_NAME_MAX];
    memset(name, 0, sizeof(name));
    if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 && name[0]) {
        snprintf(entry->name, sizeof(entry->name), "%s", name);
    } else {
        snprintf(entry->name, sizeof(entry->name), "(unknown)");
    }

    struct input_id id;
    memset(&id, 0, sizeof(id));
    if (ioctl(fd, EVIOCGID, &id) == 0) {
        entry->vendor = id.vendor;
        entry->product = id.product;
    }
    close(fd);
    return 0;
}

bool jw_input_roster_supported(void) {
    return true;
}

int jw_input_roster_build(const jw_input_proxy *proxy, jw_input_roster *roster,
                          char *error, size_t error_size) {
    memset(roster, 0, sizeof(*roster));

    /* Roster contract step 1-3: require the full grab-and-forward proxy, and
       identify the Loong pair strictly by path + st_rdev (both devices share
       the name "Loong Gamepad"). */
    if (!proxy || !proxy->enabled) {
        jw__roster_set_error(error, error_size, JW_INPUT_ROSTER_ERR_PROXY,
                             "input proxy is not active");
        return -1;
    }
    if (!proxy->virtual_event_path[0] || !proxy->physical_event_path[0]) {
        jw__roster_set_error(error, error_size, JW_INPUT_ROSTER_ERR_WATCH,
                             "proxy is watch-only or missing device paths");
        return -1;
    }

    struct stat st_phys;
    struct stat st_virt;
    if (stat(proxy->physical_event_path, &st_phys) != 0 ||
        stat(proxy->virtual_event_path, &st_virt) != 0) {
        jw__roster_set_error(error, error_size, JW_INPUT_ROSTER_ERR_PATHS,
                             "physical or virtual event path missing");
        return -1;
    }
    if (st_phys.st_rdev == st_virt.st_rdev) {
        jw__roster_set_error(error, error_size, JW_INPUT_ROSTER_ERR_PATHS,
                             "physical and virtual paths resolve to the same device");
        return -1;
    }
    snprintf(roster->physical_path, sizeof(roster->physical_path), "%s",
             proxy->physical_event_path);
    roster->physical_rdev = st_phys.st_rdev;

    /* Virtual must be gamepad-capable (contract step 3). */
    {
        int fd = open(proxy->virtual_event_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0 || !jw__is_gamepad_capable(fd)) {
            if (fd >= 0) {
                close(fd);
            }
            jw__roster_set_error(error, error_size, JW_INPUT_ROSTER_ERR_PATHS,
                                 "virtual device is not gamepad-capable");
            return -1;
        }
        close(fd);
    }

    /* Contract steps 4-8: externals in numeric eventN order, max 3, virtual
       always appended last and never displaced. */
    for (int i = 0; i < JW__ROSTER_SCAN_MAX; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        if (access(path, F_OK) != 0) {
            continue;
        }
        if (jw__same_rdev(path, proxy->physical_event_path) ||
            jw__same_rdev(path, proxy->virtual_event_path)) {
            continue;
        }

        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        bool gamepad = jw__is_gamepad_capable(fd);
        close(fd);
        if (!gamepad) {
            continue;
        }

        if (roster->external_count >= JW_INPUT_ROSTER_MAX_EXTERNALS) {
            if (roster->ignored_count < JW_INPUT_ROSTER_MAX_IGNORED) {
                snprintf(roster->ignored[roster->ignored_count],
                         sizeof(roster->ignored[roster->ignored_count]),
                         "%s", path);
                roster->ignored_count++;
            }
            continue;
        }

        jw_input_roster_entry *entry =
            &roster->controllers[roster->external_count];
        if (jw__fill_entry(path, entry, false) != 0) {
            continue;
        }
        roster->external_count++;
    }

    if (jw__fill_entry(proxy->virtual_event_path,
                       &roster->controllers[roster->external_count],
                       true) != 0) {
        jw__roster_set_error(error, error_size, JW_INPUT_ROSTER_ERR_PATHS,
                             "virtual device disappeared during scan");
        return -1;
    }
    roster->count = roster->external_count + 1;
    return 0;
}

size_t jw_input_roster_sdl_devices(const jw_input_roster *roster,
                                   char *out, size_t out_size) {
    size_t needed = 0;
    if (out_size > 0) {
        out[0] = '\0';
    }
    if (!roster) {
        return 0;
    }
    for (int i = 0; i < roster->count; i++) {
        const char *sep = (i > 0) ? ":" : "";
        needed += strlen(sep) + strlen(roster->controllers[i].path);
    }
    if (!out || out_size <= needed + 1) {
        return needed;
    }
    size_t off = 0;
    for (int i = 0; i < roster->count; i++) {
        off += snprintf(out + off, out_size - off, "%s%s",
                        i > 0 ? ":" : "", roster->controllers[i].path);
    }
    return needed;
}

void jw_input_roster_log(const jw_input_roster *roster, const char *tag) {
    if (!roster) {
        return;
    }
    jw_log_info("input roster %s: physical=%s rdev=%u:%u [excluded]",
                tag ? tag : "launch", roster->physical_path,
                (unsigned)major(roster->physical_rdev),
                (unsigned)minor(roster->physical_rdev));
    for (int i = 0; i < roster->count; i++) {
        const jw_input_roster_entry *e = &roster->controllers[i];
        jw_log_info("input roster %s: P%d=%s rdev=%u:%u name=\"%s\" vid=%04x pid=%04x %s%s",
                    tag ? tag : "launch", i + 1, e->path,
                    (unsigned)major(e->rdev), (unsigned)minor(e->rdev),
                    e->name, e->vendor, e->product,
                    e->is_virtual ? "virtual calibrated mandatory"
                                  : "external");
    }
    for (int i = 0; i < roster->ignored_count; i++) {
        jw_log_info("input roster %s: ignored external %s (3-external limit)",
                    tag ? tag : "launch", roster->ignored[i]);
    }
}

int jw_input_namespace_prepare(const jw_input_roster *roster, pid_t child_pid,
                               char *dir_out, size_t dir_out_size) {
    char dir[PATH_MAX];

    if (!roster || child_pid <= 0) {
        return -1;
    }
    if (mkdir(JW_INPUT_ROSTER_STATE_DIR, 0755) != 0 && errno != EEXIST) {
        jw_log_warn("input namespace: mkdir %s failed: %s",
                    JW_INPUT_ROSTER_STATE_DIR, strerror(errno));
        return -1;
    }
    if (snprintf(dir, sizeof(dir), "%s/input-%d", JW_INPUT_ROSTER_STATE_DIR,
                 (int)child_pid) >= (int)sizeof(dir)) {
        return -1;
    }
    if (mkdir(dir, 0700) != 0) {
        jw_log_warn("input namespace: mkdir %s failed: %s", dir, strerror(errno));
        return -1;
    }

    for (int i = 0; i < roster->count; i++) {
        const char *base = strrchr(roster->controllers[i].path, '/');
        base = base ? base + 1 : roster->controllers[i].path;
        char placeholder[PATH_MAX];
        if (snprintf(placeholder, sizeof(placeholder), "%s/%s", dir, base) >=
            (int)sizeof(placeholder)) {
            jw_input_namespace_cleanup_dir(dir);
            return -1;
        }
        int fd = open(placeholder, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) {
            jw_log_warn("input namespace: placeholder %s failed: %s",
                        placeholder, strerror(errno));
            jw_input_namespace_cleanup_dir(dir);
            return -1;
        }
        close(fd);
    }

    if (dir_out && dir_out_size > 0) {
        snprintf(dir_out, dir_out_size, "%s", dir);
    }
    return 0;
}

int jw_input_namespace_enter(const char *dir, const jw_input_roster *roster,
                             char *error, size_t error_size) {
    if (!dir || !roster) {
        jw__roster_set_error(error, error_size, JW_INPUT_ROSTER_ERR_NAMESPACE,
                             "missing namespace inputs");
        return -1;
    }
    if (unshare(CLONE_NEWNS) != 0) {
        jw__roster_set_error(error, error_size, JW_INPUT_ROSTER_ERR_NAMESPACE,
                             strerror(errno));
        return -1;
    }
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        jw__roster_set_error(error, error_size, JW_INPUT_ROSTER_ERR_NAMESPACE,
                             strerror(errno));
        return -1;
    }
    for (int i = 0; i < roster->count; i++) {
        const char *base = strrchr(roster->controllers[i].path, '/');
        base = base ? base + 1 : roster->controllers[i].path;
        char placeholder[PATH_MAX];
        if (snprintf(placeholder, sizeof(placeholder), "%s/%s", dir, base) >=
            (int)sizeof(placeholder)) {
            jw__roster_set_error(error, error_size, JW_INPUT_ROSTER_ERR_NAMESPACE,
                                 "placeholder path too long");
            return -1;
        }
        if (mount(roster->controllers[i].path, placeholder, NULL, MS_BIND,
                  NULL) != 0) {
            char detail[320];
            snprintf(detail, sizeof(detail), "bind %s: %s",
                     roster->controllers[i].path, strerror(errno));
            jw__roster_set_error(error, error_size, JW_INPUT_ROSTER_ERR_NAMESPACE,
                                 detail);
            return -1;
        }
    }
    if (mount(dir, "/dev/input", NULL, MS_BIND, NULL) != 0) {
        jw__roster_set_error(error, error_size, JW_INPUT_ROSTER_ERR_NAMESPACE,
                             strerror(errno));
        return -1;
    }
    return 0;
}

void jw_input_namespace_cleanup_dir(const char *dir) {
    if (!dir || !dir[0]) {
        return;
    }
    DIR *dp = opendir(dir);
    if (!dp) {
        return;
    }
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_name[0] == '.') {
            continue;
        }
        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", dir, de->d_name) <
            (int)sizeof(path)) {
            (void)unlink(path);
        }
    }
    closedir(dp);
    (void)rmdir(dir);
}

void jw_input_namespace_startup_sweep(void) {
    DIR *dp = opendir(JW_INPUT_ROSTER_STATE_DIR);
    if (!dp) {
        return;
    }
    struct dirent *de;
    char dirs[32][PATH_MAX];
    size_t dir_count = 0;
    while ((de = readdir(dp)) != NULL && dir_count < 32) {
        if (strncmp(de->d_name, "input-", 6) != 0) {
            continue;
        }
        if (snprintf(dirs[dir_count], sizeof(dirs[dir_count]), "%s/%s",
                     JW_INPUT_ROSTER_STATE_DIR, de->d_name) <
            (int)sizeof(dirs[dir_count])) {
            dir_count++;
        }
    }
    closedir(dp);
    for (size_t i = 0; i < dir_count; i++) {
        jw_input_namespace_cleanup_dir(dirs[i]);
        jw_log_info("input namespace: swept stale dir %s", dirs[i]);
    }
}
