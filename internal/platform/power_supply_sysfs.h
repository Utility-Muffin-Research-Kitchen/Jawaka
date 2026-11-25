/* Power supplies found by type, for kernels that do not use stock LoongOS's
 * fixed names.
 *
 * Stock's vendor kernel names its supplies battery, ac and usb. Mainline names
 * them after the driver (rk817-battery, rk817-charger on the MLP1) and may add
 * more (a USB-C port controller registers its own), so xylem looks them up by
 * the POWER_SUPPLY_TYPE each one reports in its `type` attribute: "Battery"
 * for the gauge, "USB" or "Mains" for a charger input.
 *
 * Header-only and plain libc, so jawaka-ledd (one translation unit) can use it
 * as well as jawakad. Every function here is a few sysfs reads; nothing caches
 * across calls except the battery directory a caller hands in. */
#ifndef JW_POWER_SUPPLY_SYSFS_H
#define JW_POWER_SUPPLY_SYSFS_H

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef JW_POWER_SUPPLY_SYSFS_DIR
#define JW_POWER_SUPPLY_SYSFS_DIR "/sys/class/power_supply"
#endif

#define JW_POWER_SUPPLY_PATH_MAX 256

/* <dir>/<attr> as one line without its trailing whitespace. 0 on success. */
static inline int jw_power_supply_read_line(const char *dir, const char *attr,
                                            char *out, size_t out_size) {
    if (!dir || !dir[0] || !attr || !out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';
    char path[JW_POWER_SUPPLY_PATH_MAX + 64];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, attr);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return -1;
    }
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }
    char *got = fgets(out, (int)out_size, fp);
    fclose(fp);
    if (!got) {
        out[0] = '\0';
        return -1;
    }
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' ||
                       out[len - 1] == ' ' || out[len - 1] == '\t')) {
        out[--len] = '\0';
    }
    return 0;
}

/* <dir>/<attr> as a whole decimal number. false when missing or not a number. */
static inline bool jw_power_supply_read_long(const char *dir, const char *attr,
                                             long *out) {
    char line[64];
    if (jw_power_supply_read_line(dir, attr, line, sizeof(line)) != 0 || !line[0]) {
        return false;
    }
    char *end = NULL;
    long value = strtol(line, &end, 10);
    if (end == line || *end != '\0') {
        return false;
    }
    if (out) {
        *out = value;
    }
    return true;
}

static inline bool jw_power_supply_type_is(const char *dir, const char *type) {
    char line[32];
    return jw_power_supply_read_line(dir, "type", line, sizeof(line)) == 0 &&
           strcmp(line, type) == 0;
}

static inline bool jw_power_supply_is_charger(const char *dir) {
    return jw_power_supply_type_is(dir, "USB") || jw_power_supply_type_is(dir, "Mains");
}

/* The first supply whose type is `type`, as its sysfs directory. */
static inline bool jw_power_supply_find(const char *type, char *out, size_t out_size) {
    if (!type || !out || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    DIR *dir = opendir(JW_POWER_SUPPLY_SYSFS_DIR);
    if (!dir) {
        return false;
    }
    bool found = false;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char path[JW_POWER_SUPPLY_PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/%s", JW_POWER_SUPPLY_SYSFS_DIR,
                         entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(path) || (size_t)n >= out_size) {
            continue;
        }
        if (jw_power_supply_type_is(path, type)) {
            memcpy(out, path, (size_t)n + 1);
            found = true;
            break;
        }
    }
    closedir(dir);
    return found;
}

/* The battery's directory, looked up once into `cache` (a caller-owned buffer
   of JW_POWER_SUPPLY_PATH_MAX bytes, "" until found) and again whenever it
   stops answering, so a gauge that probes after the caller started is found. */
static inline const char *jw_power_supply_battery_dir(char *cache) {
    if (!cache) {
        return NULL;
    }
    if (cache[0] && jw_power_supply_type_is(cache, "Battery")) {
        return cache;
    }
    return jw_power_supply_find("Battery", cache, JW_POWER_SUPPLY_PATH_MAX) ? cache : NULL;
}

/* Charge in percent, 0-100, or -1 when unknown. */
static inline int jw_power_supply_battery_capacity(char *cache) {
    long value = -1;
    const char *dir = jw_power_supply_battery_dir(cache);
    if (!dir || !jw_power_supply_read_long(dir, "capacity", &value) ||
        value < 0 || value > 100) {
        return -1;
    }
    return (int)value;
}

/* The battery's POWER_SUPPLY_STATUS ("Charging", "Discharging", "Not charging",
   "Full", "Unknown"). 0 on success. */
static inline int jw_power_supply_battery_status(char *cache, char *out, size_t out_size) {
    const char *dir = jw_power_supply_battery_dir(cache);
    if (!dir) {
        if (out && out_size) {
            out[0] = '\0';
        }
        return -1;
    }
    return jw_power_supply_read_line(dir, "status", out, out_size);
}

/* Is any charger input online? 1 yes, 0 no, -1 when no charger supply exists.
   Every USB and Mains supply counts: a USB-C port controller registers its own
   beside the PMIC's charger, and either may be the one that says online. */
static inline int jw_power_supply_charger_online(void) {
    DIR *dir = opendir(JW_POWER_SUPPLY_SYSFS_DIR);
    if (!dir) {
        return -1;
    }
    int result = -1;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char path[JW_POWER_SUPPLY_PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/%s", JW_POWER_SUPPLY_SYSFS_DIR,
                         entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(path) || !jw_power_supply_is_charger(path)) {
            continue;
        }
        long online = 0;
        if (!jw_power_supply_read_long(path, "online", &online)) {
            continue;
        }
        if (online > 0) {
            result = 1;
            break;
        }
        result = 0;
    }
    closedir(dir);
    return result;
}

#endif /* JW_POWER_SUPPLY_SYSFS_H */
