#include "internal/platform/platform_id.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/utsname.h>
#include <sys/statvfs.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

const char *jw_platform_compiled_id(void) {
    return "mlp1";
}

/* Read a whole file into out, trimming trailing whitespace/newlines/nulls. */
static int jw__read_file_trim(const char *path, char *out, size_t out_size) {
    if (!out || out_size == 0) return -1;
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(out, 1, out_size - 1, f);
    fclose(f);
    out[n] = '\0';
    while (n > 0) {
        char c = out[n - 1];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t' || c == '\0') out[--n] = '\0';
        else break;
    }
    return 0;
}

static long jw__read_long_file(const char *path) {
    char buf[64];
    if (jw__read_file_trim(path, buf, sizeof(buf)) != 0) return -1;
    return strtol(buf, NULL, 10);
}

/* Stock LoongOS version lives in /loong/loong_version as JSON:
   {"verInner":...,"verShow":"1.3.0.32"}. Extract verShow without a JSON dep. */
static void jw__read_stock_version(char *out, size_t out_size) {
    if (out_size) out[0] = '\0';
    char buf[256];
    if (jw__read_file_trim("/loong/loong_version", buf, sizeof(buf)) != 0) return;
    char *p = strstr(buf, "\"verShow\"");
    if (!p) return;
    p = strchr(p + 9, '"');           /* opening quote of the value */
    if (!p) return;
    p++;
    char *end = strchr(p, '"');
    if (!end) return;
    size_t len = (size_t)(end - p);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, p, len);
    out[len] = '\0';
}

static void jw__read_meminfo(long *total_kb, long *avail_kb) {
    *total_kb = -1;
    *avail_kb = -1;
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        long v;
        if (sscanf(line, "MemTotal: %ld kB", &v) == 1) *total_kb = v;
        else if (sscanf(line, "MemAvailable: %ld kB", &v) == 1) *avail_kb = v;
    }
    fclose(f);
}

/* Find the first power-supply node exposing a capacity; read percent + charging. */
static void jw__read_battery(int *percent, int *charging) {
    *percent = -1;
    *charging = 0;
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return;
    struct dirent *e;
    char path[512];
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/capacity", e->d_name);
        long cap = jw__read_long_file(path);
        if (cap < 0) continue;
        *percent = (int)cap;
        char status[32];
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/status", e->d_name);
        if (jw__read_file_trim(path, status, sizeof(status)) == 0 &&
            strcmp(status, "Charging") == 0) {
            *charging = 1;
        }
        break;
    }
    closedir(d);
}

static void jw__read_ip(char *out, size_t out_size) {
    if (out_size) out[0] = '\0';
    struct ifaddrs *ifaces = NULL;
    if (getifaddrs(&ifaces) != 0) return;
    for (struct ifaddrs *p = ifaces; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || !p->ifa_name) continue;
        if (strncmp(p->ifa_name, "lo", 2) == 0) continue;
        struct sockaddr_in *addr = (struct sockaddr_in *)p->ifa_addr;
        inet_ntop(AF_INET, &addr->sin_addr, out, (socklen_t)out_size);
        if (strncmp(p->ifa_name, "wlan", 4) == 0) break;   /* prefer Wi-Fi */
    }
    freeifaddrs(ifaces);
}

void jw_platform_system_info(const char *fs_path, jw_system_info *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->mem_total_kb = -1;
    out->mem_avail_kb = -1;
    out->sd_total_mb = -1;
    out->sd_free_mb = -1;
    out->battery_percent = -1;
    out->cpu_temp_c = -1;
    out->uptime_s = -1;

    jw__read_stock_version(out->os_version, sizeof(out->os_version));

    struct utsname uts;
    if (uname(&uts) == 0)
        snprintf(out->kernel, sizeof(out->kernel), "%.63s", uts.release);

    jw__read_file_trim("/proc/device-tree/model", out->device, sizeof(out->device));

    jw__read_meminfo(&out->mem_total_kb, &out->mem_avail_kb);

    if (fs_path && fs_path[0]) {
        struct statvfs vfs;
        if (statvfs(fs_path, &vfs) == 0) {
            unsigned long long block = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
            out->sd_total_mb = (long)((block * (unsigned long long)vfs.f_blocks) / (1024ULL * 1024ULL));
            out->sd_free_mb  = (long)((block * (unsigned long long)vfs.f_bavail) / (1024ULL * 1024ULL));
        }
    }

    jw__read_ip(out->ip, sizeof(out->ip));
    jw__read_battery(&out->battery_percent, &out->charging);

    long milli = jw__read_long_file("/sys/class/thermal/thermal_zone0/temp");
    if (milli > 0) out->cpu_temp_c = (int)(milli / 1000);

    char up[64];
    if (jw__read_file_trim("/proc/uptime", up, sizeof(up)) == 0)
        out->uptime_s = strtol(up, NULL, 10);
}
