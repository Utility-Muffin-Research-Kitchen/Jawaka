#include "internal/platform/wifi.h"
#include "internal/platform/paths.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/select.h>
#include <fcntl.h>
#include <dirent.h>
#include <limits.h>
#include <errno.h>
#include <signal.h>
#include <dlfcn.h>
#include <sys/stat.h>

bool jw_wifi_available(void) {
    return true;
}

#define JW_WIFI_CMD_TIMEOUT_MS 6000   /* hard cap so a stuck wpa_cli can't freeze the UI */

#define JW_WIFI_IFACE       "wlan0"
#define JW_WIFI_CTRL_DIR    "/var/run/wpa_supplicant"
#define JW_WIFI_CTRL_SOCK   JW_WIFI_CTRL_DIR "/" JW_WIFI_IFACE
#define JW_WIFI_FLAGS_PATH  "/sys/class/net/" JW_WIFI_IFACE "/flags"

/* Exactly how stock loong launches the supplicant at boot (captured from the
   running process): wpa_supplicant -B -i wlan0 -c <conf> -P <pidfile>. We
   replicate it on the radio-on path because loong's *live* enable only re-ups
   the interface — it doesn't (re)start the supplicant the way boot init does. */
#define JW_WIFI_SUPPLICANT_BIN "/usr/sbin/wpa_supplicant"
#define JW_WIFI_CONF_PATH      JW_WIFI_CTRL_DIR "/wpa_supplicant.conf"
#define JW_WIFI_PIDFILE        "/run/wpa_supplicant.wlan0.pid"

/* Loong's persisted settings store (plain SQLite). WIFI_PARAM = {"enable":0|1}
   is the authoritative radio-enable flag — what loong honors at boot and what
   WriteConfig updates. We read it (rather than the wlan0 IFF_UP bit) because
   loong's watchdog keeps the interface up even when the radio is meant to be
   off, so IFF_UP can lie. */
#define JW_LOONG_DB_PATH "/oem/loong/loong.db"

/* Run `wpa_cli -i wlan0 <args...>` and capture stdout into buf (NUL-terminated).
 * fork/exec + pipe — no system(). Returns bytes read (>=0), or -1 on failure. */
static int jw__wifi_run(const char *const *args, int nargs,
                        char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) {
        return -1;
    }
    buf[0] = '\0';

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        /* Child: stdout -> pipe, then exec wpa_cli. */
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        char *argv[16];
        int a = 0;
        argv[a++] = "wpa_cli";
        argv[a++] = "-i";
        argv[a++] = (char *)JW_WIFI_IFACE;
        for (int i = 0; i < nargs && a < 15; i++) {
            argv[a++] = (char *)args[i];
        }
        argv[a] = NULL;
        execvp("wpa_cli", argv);
        _exit(127);
    }

    /* Parent: read the child's stdout with a hard deadline. wpa_cli can block
       indefinitely when the interface/wpa is in a bad state; without this the
       whole UI freezes (the rescan-while-off lockup). */
    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    size_t total = 0;
    int elapsed_ms = 0;
    int timed_out = 0;
    while (total + 1 < buf_size) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(pipefd[0], &rf);
        struct timeval tv = { 0, 200000 };   /* 200ms slices */
        int s = select(pipefd[0] + 1, &rf, NULL, NULL, &tv);
        if (s > 0) {
            ssize_t n = read(pipefd[0], buf + total, buf_size - 1 - total);
            if (n > 0) {
                total += (size_t)n;
            } else if (n == 0) {
                break;            /* child closed stdout */
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                break;
            }
        } else if (s == 0) {
            elapsed_ms += 200;
            if (elapsed_ms >= JW_WIFI_CMD_TIMEOUT_MS) {
                kill(pid, SIGKILL);
                timed_out = 1;
                break;
            }
        } else if (errno != EINTR) {
            break;
        }
    }
    buf[total] = '\0';
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (timed_out || !(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
        return -1;
    }
    return (int)total;
}

/* Convenience: single-argument wpa_cli command. */
static int jw__wifi_wpa_cli(const char *arg, char *buf, size_t buf_size) {
    const char *args[1] = { arg };
    return jw__wifi_run(args, 1, buf, buf_size);
}

/* Copy the value following "key=" on its line in a wpa_cli status dump. */
static void jw__wifi_field(const char *dump, const char *key,
                           char *out, size_t out_size) {
    if (out_size == 0) {
        return;
    }
    out[0] = '\0';

    size_t key_len = strlen(key);
    const char *p = dump;
    while (p && *p) {
        /* Match "key=" at the start of a line. */
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            const char *val = p + key_len + 1;
            size_t i = 0;
            while (val[i] && val[i] != '\n' && val[i] != '\r' && i + 1 < out_size) {
                out[i] = val[i];
                i++;
            }
            out[i] = '\0';
            return;
        }
        const char *nl = strchr(p, '\n');
        p = nl ? nl + 1 : NULL;
    }
}

/* RSSI (dBm) from `wpa_cli signal_poll`; 0 if unavailable/disconnected. */
static int jw__wifi_rssi(void) {
    char dump[512];
    if (jw__wifi_wpa_cli("signal_poll", dump, sizeof(dump)) < 0) {
        return 0;
    }
    const char *p = strstr(dump, "RSSI=");
    if (!p) {
        return 0;
    }
    return atoi(p + 5);
}

/* Map RSSI to a 0..3 strength using the SAME thresholds as the status-bar wifi
 * icon (catastrophe cat__map_rssi_to_wifi_strength), so the page and icon agree. */
static int jw__wifi_strength(int rssi) {
    if (rssi == 0)   return 0;
    if (rssi >= -60) return 3;
    if (rssi >= -70) return 2;
    return 1;
}

int jw_wifi_strength_now(void) {
    /* Live 0..3 strength from the same RSSI source the Network page uses, so the
       status-bar icon (which renders this) and the page can never disagree.
       Bounded by jw__wifi_run's timeout; 0 when disconnected/radio off. */
    return jw__wifi_strength(jw__wifi_rssi());
}

static int jw__wifi_saved_id(const char *ssid);
static bool jw__wifi_saved_profile_has_security(int id);

int jw_wifi_status(jw_wifi_status_t *out) {
    if (!out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    char dump[2048];
    if (jw__wifi_wpa_cli("status", dump, sizeof(dump)) < 0 || dump[0] == '\0') {
        return -1;
    }

    jw__wifi_field(dump, "wpa_state", out->state, sizeof(out->state));
    jw__wifi_field(dump, "ssid", out->ssid, sizeof(out->ssid));
    jw__wifi_field(dump, "ip_address", out->ip, sizeof(out->ip));
    out->connected = (strcmp(out->state, "COMPLETED") == 0);
    out->rssi = out->connected ? jw__wifi_rssi() : 0;
    out->strength = jw__wifi_strength(out->rssi);
    out->valid = true;
    return 0;
}

int jw_wifi_scan_start(void) {
    char buf[64];
    /* "OK" / "FAIL-BUSY" — either way the command reached wpa_supplicant. */
    return jw__wifi_wpa_cli("scan", buf, sizeof(buf)) < 0 ? -1 : 0;
}

static int jw__wifi_net_cmp(const void *a, const void *b) {
    const jw_wifi_network_t *na = (const jw_wifi_network_t *)a;
    const jw_wifi_network_t *nb = (const jw_wifi_network_t *)b;
    return nb->rssi - na->rssi;   /* strongest first */
}

/* Collect saved-profile SSIDs from `list_networks` (col 2). Returns count. */
static int jw__wifi_saved_ssids(char out[][64], int max) {
    char dump[4096];
    if (jw__wifi_wpa_cli("list_networks", dump, sizeof(dump)) < 0) {
        return 0;
    }
    int count = 0;
    char *save_line = NULL;
    char *line = strtok_r(dump, "\n", &save_line);   /* header */
    if (line) {
        line = strtok_r(NULL, "\n", &save_line);
    }
    for (; line && count < max; line = strtok_r(NULL, "\n", &save_line)) {
        char *st = NULL;
        char *id   = strtok_r(line, "\t", &st);
        char *name = id ? strtok_r(NULL, "\t", &st) : NULL;
        if (name && name[0]) {
            snprintf(out[count], 64, "%s", name);
            count++;
        }
    }
    return count;
}

int jw_wifi_scan_results(const char *current_ssid, jw_wifi_network_t *out, int max) {
    if (!out || max <= 0) {
        return -1;
    }
    char dump[8192];
    if (jw__wifi_wpa_cli("scan_results", dump, sizeof(dump)) < 0) {
        return -1;
    }

    int count = 0;
    char *save_line = NULL;
    /* Header: "bssid / frequency / signal level / flags / ssid". Skip it. */
    char *line = strtok_r(dump, "\n", &save_line);
    if (line) {
        line = strtok_r(NULL, "\n", &save_line);
    }
    for (; line; line = strtok_r(NULL, "\n", &save_line)) {
        /* Tab-separated: bssid, freq, signal(dBm), flags, ssid (rest). */
        char *save_tok = NULL;
        char *bssid = strtok_r(line, "\t", &save_tok);
        char *freq  = bssid ? strtok_r(NULL, "\t", &save_tok) : NULL;
        char *sig   = freq  ? strtok_r(NULL, "\t", &save_tok) : NULL;
        char *flags = sig   ? strtok_r(NULL, "\t", &save_tok) : NULL;
        char *ssid  = flags ? strtok_r(NULL, "\t", &save_tok) : NULL;
        (void)bssid; (void)freq;
        if (!sig || !flags || !ssid || ssid[0] == '\0') {
            continue;
        }
        int rssi = atoi(sig);
        bool secured = (strstr(flags, "WPA") != NULL) ||
                       (strstr(flags, "WEP") != NULL) ||
                       (strstr(flags, "WPA3") != NULL);

        /* Dedup by SSID, keeping the strongest signal. */
        int found = -1;
        for (int i = 0; i < count; i++) {
            if (strcmp(out[i].ssid, ssid) == 0) { found = i; break; }
        }
        if (found >= 0) {
            if (rssi > out[found].rssi) {
                out[found].rssi = rssi;
                out[found].strength = jw__wifi_strength(rssi);
                out[found].secured = secured;
            }
            continue;
        }
        if (count >= max) {
            continue;
        }
        jw_wifi_network_t *n = &out[count++];
        memset(n, 0, sizeof(*n));
        snprintf(n->ssid, sizeof(n->ssid), "%s", ssid);
        n->rssi = rssi;
        n->strength = jw__wifi_strength(rssi);
        n->secured = secured;
        n->current = (current_ssid && current_ssid[0] &&
                      strcmp(current_ssid, ssid) == 0);
    }

    /* Mark networks that have a usable saved profile. A secured AP with a stale
       open profile must prompt for a password instead of looking saved. */
    char saved[JW_WIFI_MAX_NETWORKS][64];
    int nsaved = jw__wifi_saved_ssids(saved, JW_WIFI_MAX_NETWORKS);
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < nsaved; j++) {
            if (strcmp(out[i].ssid, saved[j]) == 0) {
                int id = jw__wifi_saved_id(out[i].ssid);
                out[i].saved = (id >= 0 &&
                                (!out[i].secured || jw__wifi_saved_profile_has_security(id)));
                break;
            }
        }
    }

    qsort(out, (size_t)count, sizeof(out[0]), jw__wifi_net_cmp);
    return count;
}

/* Saved-profile network id for ssid, or -1 if none. Parses `list_networks`:
 *   network id / ssid / bssid / flags   (tab-separated, header line first) */
static int jw__wifi_saved_id(const char *ssid) {
    char dump[4096];
    if (jw__wifi_wpa_cli("list_networks", dump, sizeof(dump)) < 0) {
        return -1;
    }
    char *save_line = NULL;
    char *line = strtok_r(dump, "\n", &save_line);   /* header */
    if (line) {
        line = strtok_r(NULL, "\n", &save_line);
    }
    for (; line; line = strtok_r(NULL, "\n", &save_line)) {
        char *save_tok = NULL;
        char *id   = strtok_r(line, "\t", &save_tok);
        char *name = id ? strtok_r(NULL, "\t", &save_tok) : NULL;
        if (id && name && strcmp(name, ssid) == 0) {
            return atoi(id);
        }
    }
    return -1;
}

static void jw__wifi_trim_line(char *s) {
    if (!s) {
        return;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

static int jw__wifi_run_ok(const char *const *args, int nargs) {
    char buf[128];
    if (jw__wifi_run(args, nargs, buf, sizeof(buf)) < 0) {
        return -1;
    }
    jw__wifi_trim_line(buf);
    return strncmp(buf, "FAIL", 4) == 0 ? -1 : 0;
}

static int jw__wifi_get_network(int id, const char *field,
                                char *out, size_t out_size) {
    if (!out || out_size == 0 || !field) {
        return -1;
    }
    out[0] = '\0';
    char idbuf[16];
    snprintf(idbuf, sizeof(idbuf), "%d", id);
    const char *args[] = { "get_network", idbuf, field };
    if (jw__wifi_run(args, 3, out, out_size) < 0) {
        return -1;
    }
    jw__wifi_trim_line(out);
    if (out[0] == '\0' || strncmp(out, "FAIL", 4) == 0) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

static bool jw__wifi_saved_profile_has_security(int id) {
    char psk[160];
    if (jw__wifi_get_network(id, "psk", psk, sizeof(psk)) == 0 && psk[0]) {
        return true;
    }

    char wep[160];
    if (jw__wifi_get_network(id, "wep_key0", wep, sizeof(wep)) == 0 && wep[0]) {
        return true;
    }

    char key_mgmt[128];
    if (jw__wifi_get_network(id, "key_mgmt", key_mgmt, sizeof(key_mgmt)) == 0 &&
        strstr(key_mgmt, "NONE") == NULL) {
        return true;
    }
    return false;
}

/* Kick a DHCP client for the interface, fire-and-forget (double-fork so we leave
 * no zombie). Mirrors the stock `udhcpc -t 5 -n -i wlan0` invocation. */
static void jw__wifi_dhcp(void) {
    pid_t pid = fork();
    if (pid < 0) {
        return;
    }
    if (pid == 0) {
        pid_t grandchild = fork();
        if (grandchild == 0) {
            /* -t 20: keep sending DISCOVERs (~60s) so we don't give up before the
               (re)association finishes — the stock 5-try -n gave up too early. */
            execlp("udhcpc", "udhcpc", "-t", "20", "-n", "-i", JW_WIFI_IFACE,
                   (char *)NULL);
            _exit(127);
        }
        _exit(0);   /* intermediate child exits immediately; grandchild reparents to init */
    }
    int status = 0;
    waitpid(pid, &status, 0);   /* reap the intermediate child only */
}

/* Durable store on the SD card (survives reboots, unlike the tmpfs wpa conf).
 * Lives in the canonical durable-state dir (jw_state_dir = <primary SD>/.umrk,
 * auto-created), so it routes to the first SD like the rest of our state. */
static int jw__wifi_durable_path(char *out, size_t out_size) {
    char *dir = jw_state_dir();
    if (!dir) {
        return -1;
    }
    int r = snprintf(out, out_size, "%s/wifi.conf", dir);
    free(dir);
    return (r > 0 && (size_t)r < out_size) ? 0 : -1;
}

/* The live wpa_supplicant config file = the running process's -c argument. We
 * discover it (rather than hardcode /tmp vs /var/run) since stock varies it. */
static int jw__wifi_live_conf_path(char *out, size_t out_size) {
    DIR *proc = opendir("/proc");
    if (!proc) {
        return -1;
    }
    int found = -1;
    struct dirent *e;
    while ((e = readdir(proc)) != NULL) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') {
            continue;
        }
        char path[PATH_MAX];
        char comm[64] = { 0 };
        snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
        FILE *cf = fopen(path, "r");
        if (!cf) {
            continue;
        }
        if (!fgets(comm, sizeof(comm), cf)) { fclose(cf); continue; }
        fclose(cf);
        if (strncmp(comm, "wpa_supplicant", 14) != 0) {
            continue;
        }
        snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);
        FILE *xf = fopen(path, "r");
        if (!xf) {
            continue;
        }
        char buf[1024];
        size_t len = fread(buf, 1, sizeof(buf) - 1, xf);
        fclose(xf);
        buf[len] = '\0';
        for (size_t i = 0; i < len; ) {
            size_t al = strlen(buf + i);
            if (strcmp(buf + i, "-c") == 0 && i + al + 1 < len) {
                snprintf(out, out_size, "%s", buf + i + al + 1);
                found = 0;
                break;
            }
            i += al + 1;
        }
        break;
    }
    closedir(proc);
    return found;
}

/* Copy the live wpa conf (all saved networks) to the durable SD store after a
 * change, so they can be restored on next boot. Best-effort, plain C copy. */
static void jw__wifi_export(void) {
    char src[PATH_MAX];
    char dst[PATH_MAX];
    if (jw__wifi_live_conf_path(src, sizeof(src)) != 0 ||
        jw__wifi_durable_path(dst, sizeof(dst)) != 0) {
        return;
    }
    FILE *in = fopen(src, "rb");
    if (!in) {
        return;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return;
    }
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            break;
        }
    }
    fclose(in);
    fclose(out);
}

/* Derive the wpa PMK hex for (ssid, passphrase) via `wpa_passphrase`, so we can
 * store the network-specific hash instead of the reusable passphrase. Writes the
 * 64-hex PMK to out. Returns 0 on success, -1 otherwise. */
static int jw__wifi_derive_psk(const char *ssid, const char *passphrase,
                               char *out, size_t out_size) {
    if (!ssid || !passphrase || !out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execlp("wpa_passphrase", "wpa_passphrase", ssid, passphrase, (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    char dump[512];
    size_t total = 0;
    ssize_t n;
    while (total + 1 < sizeof(dump) &&
           (n = read(pipefd[0], dump + total, sizeof(dump) - 1 - total)) > 0) {
        total += (size_t)n;
    }
    dump[total] = '\0';
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
        return -1;
    }

    /* Output has a commented "#psk=\"...\"" line and the real "psk=<hex>" line.
       Take the uncommented, unquoted psk= line. */
    char *save = NULL;
    for (char *ln = strtok_r(dump, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
        while (*ln == ' ' || *ln == '\t') ln++;
        if (strncmp(ln, "psk=", 4) == 0 && ln[4] != '"') {
            snprintf(out, out_size, "%s", ln + 4);
            return (out[0] != '\0') ? 0 : -1;
        }
    }
    return -1;
}

static bool jw__wifi_psk_is_pmk(const char *psk) {
    if (!psk || strlen(psk) != 64) {
        return false;
    }
    for (const char *p = psk; *p; p++) {
        if (!((*p >= '0' && *p <= '9') ||
              (*p >= 'a' && *p <= 'f') ||
              (*p >= 'A' && *p <= 'F'))) {
            return false;
        }
    }
    return true;
}

jw_wifi_connect_result jw_wifi_connect(const char *ssid, bool secured) {
    if (!ssid || !ssid[0]) {
        return JW_WIFI_CONNECT_FAILED;
    }
    char buf[128];
    char idbuf[16];
    int id = jw__wifi_saved_id(ssid);

    if (id < 0) {
        /* No saved profile. Only open networks can connect without a password. */
        if (secured) {
            return JW_WIFI_CONNECT_NEED_PASSWORD;
        }
        const char *add[] = { "add_network" };
        if (jw__wifi_run(add, 1, buf, sizeof(buf)) < 0) {
            return JW_WIFI_CONNECT_FAILED;
        }
        id = atoi(buf);
        if (id < 0) {
            return JW_WIFI_CONNECT_FAILED;
        }
        snprintf(idbuf, sizeof(idbuf), "%d", id);

        char qssid[72];
        snprintf(qssid, sizeof(qssid), "\"%s\"", ssid);
        const char *set_ssid[] = { "set_network", idbuf, "ssid", qssid };
        const char *set_open[] = { "set_network", idbuf, "key_mgmt", "NONE" };
        if (jw__wifi_run(set_ssid, 4, buf, sizeof(buf)) < 0 ||
            jw__wifi_run(set_open, 4, buf, sizeof(buf)) < 0) {
            return JW_WIFI_CONNECT_FAILED;
        }
    } else {
        if (secured && !jw__wifi_saved_profile_has_security(id)) {
            return JW_WIFI_CONNECT_NEED_PASSWORD;
        }
        snprintf(idbuf, sizeof(idbuf), "%d", id);
    }

    const char *enable[] = { "enable_network", idbuf };
    const char *select[] = { "select_network", idbuf };
    if (jw__wifi_run(enable, 2, buf, sizeof(buf)) < 0 ||
        jw__wifi_run(select, 2, buf, sizeof(buf)) < 0) {
        return JW_WIFI_CONNECT_FAILED;
    }

    const char *save[] = { "save_config" };
    (void)jw__wifi_run(save, 1, buf, sizeof(buf));   /* best-effort persistence */
    jw__wifi_export();

    jw__wifi_dhcp();
    return JW_WIFI_CONNECT_OK;
}

/* Does the SSID advertise SAE (WPA3) in the latest scan? 1=yes, 0=secured but no
 * SAE, -1=not found. SAE can't use a stored PMK hash — it needs the passphrase —
 * so we only hash when this returns 0 (confirmed WPA2-only). */
static int jw__wifi_ssid_sae(const char *ssid) {
    char dump[8192];
    if (jw__wifi_wpa_cli("scan_results", dump, sizeof(dump)) < 0) {
        return -1;
    }
    char *save_line = NULL;
    char *line = strtok_r(dump, "\n", &save_line);   /* header */
    if (line) {
        line = strtok_r(NULL, "\n", &save_line);
    }
    for (; line; line = strtok_r(NULL, "\n", &save_line)) {
        char *st = NULL;
        char *bssid = strtok_r(line, "\t", &st);
        char *freq  = bssid ? strtok_r(NULL, "\t", &st) : NULL;
        char *sig   = freq  ? strtok_r(NULL, "\t", &st) : NULL;
        char *flags = sig   ? strtok_r(NULL, "\t", &st) : NULL;
        char *name  = flags ? strtok_r(NULL, "\t", &st) : NULL;
        if (name && strcmp(name, ssid) == 0) {
            return (flags && strstr(flags, "SAE") != NULL) ? 1 : 0;
        }
    }
    return -1;
}

/* Management-frame protection. WPA2/WPA3 transition APs advertise PMF-capable;
   if the client leaves ieee80211w unset, wpa_supplicant defaults PMF off and the
   AP silently drops data frames after a SAE association (associates fine, then
   DHCP gets no reply). ieee80211w=1 (optional) negotiates PMF with SAE yet still
   lets a pure WPA2-PSK AP associate without it — so it is safe for every secured
   network. =2 (required) would break the WPA2 fallback, so we never use it. */
static void jw__wifi_set_pmf(const char *idbuf, int mode) {
    char val[4];
    snprintf(val, sizeof(val), "%d", mode);
    const char *set_pmf[] = { "set_network", idbuf, "ieee80211w", val };
    (void)jw__wifi_run_ok(set_pmf, 4);
}

static int jw__wifi_set_secured_key_mgmt(const char *idbuf, int sae) {
    const char *primary = (sae == 1) ? "WPA-PSK SAE" : "WPA-PSK";
    const char *set_primary[] = { "set_network", idbuf, "key_mgmt", primary };
    if (jw__wifi_run_ok(set_primary, 4) == 0) {
        jw__wifi_set_pmf(idbuf, sae == 1 ? 1 : 0);
        return 0;
    }
    if (sae == 1) {
        const char *set_fallback[] = { "set_network", idbuf, "key_mgmt", "WPA-PSK" };
        if (jw__wifi_run_ok(set_fallback, 4) == 0) {
            jw__wifi_set_pmf(idbuf, 0);
            return 0;
        }
    }
    return -1;
}

jw_wifi_connect_result jw_wifi_connect_psk(const char *ssid, const char *psk) {
    if (!ssid || !ssid[0] || !psk) {
        return JW_WIFI_CONNECT_FAILED;
    }
    char buf[128];
    char idbuf[16];
    int id = jw__wifi_saved_id(ssid);
    if (id < 0) {
        const char *add[] = { "add_network" };
        if (jw__wifi_run(add, 1, buf, sizeof(buf)) < 0) {
            return JW_WIFI_CONNECT_FAILED;
        }
        id = atoi(buf);
        if (id < 0) {
            return JW_WIFI_CONNECT_FAILED;
        }
    }
    snprintf(idbuf, sizeof(idbuf), "%d", id);

    char qssid[72];
    snprintf(qssid, sizeof(qssid), "\"%s\"", ssid);
    const char *set_ssid[] = { "set_network", idbuf, "ssid", qssid };
    if (jw__wifi_run(set_ssid, 4, buf, sizeof(buf)) < 0) {
        return JW_WIFI_CONNECT_FAILED;
    }

    int sae = jw__wifi_ssid_sae(ssid);
    if (jw__wifi_set_secured_key_mgmt(idbuf, sae) < 0) {
        return JW_WIFI_CONNECT_FAILED;
    }

    /* Key storage depends on the security type:
       - confirmed WPA2-only (sae==0): store the derived PMK hash, so the reusable
         passphrase never hits disk (the security win).
       - SAE/WPA3 (sae==1) or unknown (-1): SAE cannot use a stored PMK hash — it
         needs the actual passphrase — so store it quoted. (Unavoidable plaintext
         for SAE; only applies to WPA3 networks.) */
    char hex[80];
    bool stored = false;
    if (sae == 0 && jw__wifi_derive_psk(ssid, psk, hex, sizeof(hex)) == 0) {
        const char *set_psk[] = { "set_network", idbuf, "psk", hex };
        stored = (jw__wifi_run_ok(set_psk, 4) == 0);
    }
    if (!stored) {
        char qpsk[140];
        snprintf(qpsk, sizeof(qpsk), "\"%s\"", psk);
        const char *set_psk[] = { "set_network", idbuf, "psk", qpsk };
        if (jw__wifi_run_ok(set_psk, 4) < 0) {
            return JW_WIFI_CONNECT_FAILED;
        }
    }

    const char *enable[] = { "enable_network", idbuf };
    const char *select[] = { "select_network", idbuf };
    if (jw__wifi_run(enable, 2, buf, sizeof(buf)) < 0 ||
        jw__wifi_run(select, 2, buf, sizeof(buf)) < 0) {
        return JW_WIFI_CONNECT_FAILED;
    }

    const char *save2[] = { "save_config" };
    (void)jw__wifi_run(save2, 1, buf, sizeof(buf));
    jw__wifi_export();

    jw__wifi_dhcp();
    return JW_WIFI_CONNECT_OK;
}

int jw_wifi_forget(const char *ssid) {
    if (!ssid || !ssid[0]) {
        return -1;
    }
    int id = jw__wifi_saved_id(ssid);
    if (id < 0) {
        return -1;
    }
    char idbuf[16];
    char buf[64];
    snprintf(idbuf, sizeof(idbuf), "%d", id);
    const char *rm[] = { "remove_network", idbuf };
    if (jw__wifi_run(rm, 2, buf, sizeof(buf)) < 0) {
        return -1;
    }
    const char *save[] = { "save_config" };
    (void)jw__wifi_run(save, 1, buf, sizeof(buf));
    jw__wifi_export();
    return 0;
}

int jw_wifi_disconnect(void) {
    char buf[64];
    const char *cmd[] = { "disconnect" };
    return jw__wifi_run(cmd, 1, buf, sizeof(buf)) < 0 ? -1 : 0;
}

static void jw__wifi_monitor_local_path(char *out, size_t n) {
    snprintf(out, n, "/tmp/umrk-wpa-mon-%d", (int)getpid());
}

int jw_wifi_monitor_open(void) {
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    char local_path[64];
    jw__wifi_monitor_local_path(local_path, sizeof(local_path));
    unlink(local_path);

    struct sockaddr_un local;
    memset(&local, 0, sizeof(local));
    local.sun_family = AF_UNIX;
    snprintf(local.sun_path, sizeof(local.sun_path), "%s", local_path);
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        close(fd);
        return -1;
    }
    struct sockaddr_un dest;
    memset(&dest, 0, sizeof(dest));
    dest.sun_family = AF_UNIX;
    snprintf(dest.sun_path, sizeof(dest.sun_path), "%s", JW_WIFI_CTRL_SOCK);
    if (connect(fd, (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        close(fd);
        unlink(local_path);
        return -1;
    }

    /* Attach for unsolicited events; wait briefly for the OK reply. */
    struct timeval tv = { 1, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (send(fd, "ATTACH", 6, 0) < 0) {
        close(fd);
        unlink(local_path);
        return -1;
    }
    char reply[16] = { 0 };
    ssize_t rn = recv(fd, reply, sizeof(reply) - 1, 0);   /* expect "OK\n" */
    if (rn > 0) reply[rn] = '\0';

    (void)reply;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    return fd;
}

jw_wifi_evt jw_wifi_monitor_poll(int fd) {
    if (fd < 0) {
        return JW_WIFI_EVT_NONE;
    }
    jw_wifi_evt result = JW_WIFI_EVT_NONE;
    char buf[512];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            break;   /* drained (EAGAIN) or closed */
        }
        buf[n] = '\0';
        /* Definitive wrong key (WPA2-PSK 4-way handshake failure). */
        if (strstr(buf, "WRONG_KEY") != NULL) {
            result = JW_WIFI_EVT_WRONG_KEY;   /* strongest; keep draining */
        } else if (result != JW_WIFI_EVT_WRONG_KEY &&
                   ((strstr(buf, "Authentication") && strstr(buf, "timed out")) ||
                    strstr(buf, "CTRL-EVENT-ASSOC-REJECT") != NULL)) {
            /* SAE/WPA3 bad key, or assoc rejected — likely-but-not-certain bad key.
               A clean successful connect never emits these, so it's safe to act on
               within the attempt (the monitor is closed before recovery churn). */
            result = JW_WIFI_EVT_AUTH_FAIL;
        }
    }
    return result;
}

void jw_wifi_monitor_close(int fd) {
    if (fd < 0) {
        return;
    }
    (void)send(fd, "DETACH", 6, 0);
    close(fd);
    char local_path[64];
    jw__wifi_monitor_local_path(local_path, sizeof(local_path));
    unlink(local_path);
}


/* True if the wlan0 interface carries the IFF_UP bit. Used only as a fallback
   when loong's enable flag can't be read. */
static bool jw__wifi_iface_up(void) {
    FILE *f = fopen(JW_WIFI_FLAGS_PATH, "r");
    if (!f) {
        return false;   /* no interface = module unloaded / radio off */
    }
    unsigned flags = 0;
    int ok = fscanf(f, "%x", &flags);
    fclose(f);
    return ok == 1 && (flags & 0x1u) != 0;   /* IFF_UP */
}

/* Read loong's persisted WIFI_PARAM enable flag from loong.db via the sqlite3
   CLI (fork/exec, no system()). Returns 1=on, 0=off, -1 if it can't be read
   (DB locked/missing/sqlite3 absent) so the caller can fall back. */
static int jw__wifi_read_enable_flag(void) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        /* .timeout rides out a brief writer lock from loong; the query returns
           the raw JSON value, e.g. {"enable":1}. */
        execlp("sqlite3", "sqlite3", "-cmd", ".timeout 1000", JW_LOONG_DB_PATH,
               "select value from system_config where param='WIFI_PARAM';",
               (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

    char buf[128];
    size_t total = 0;
    int elapsed_ms = 0;
    int timed_out = 0;
    while (total + 1 < sizeof(buf)) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(pipefd[0], &rf);
        struct timeval tv = { 0, 200000 };
        int s = select(pipefd[0] + 1, &rf, NULL, NULL, &tv);
        if (s > 0) {
            ssize_t n = read(pipefd[0], buf + total, sizeof(buf) - 1 - total);
            if (n > 0) {
                total += (size_t)n;
            } else {
                break;
            }
        } else if (s == 0) {
            elapsed_ms += 200;
            if (elapsed_ms >= 2000) {
                kill(pid, SIGKILL);
                timed_out = 1;
                break;
            }
        } else if (errno != EINTR) {
            break;
        }
    }
    buf[total] = '\0';
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (timed_out || !(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
        return -1;
    }

    /* Parse the first 0/1 after "enable". */
    const char *p = strstr(buf, "enable");
    if (!p) {
        return -1;
    }
    p += 6;
    while (*p && *p != '0' && *p != '1') {
        p++;
    }
    if (*p == '0') return 0;
    if (*p == '1') return 1;
    return -1;
}

bool jw_wifi_radio_is_on(void) {
    /* In Leaf mode we bring wlan0 up/down ourselves on every radio toggle, and no
       stock loong daemon runs to keep loong's persisted WIFI_PARAM flag in sync —
       so that flag goes stale (e.g. it stays "1" after an off-toggle, leaving the
       UI stuck showing "on"/"unavailable"). The interface IFF_UP bit reflects the
       real radio state and is authoritative whenever it can be read; fall back to
       the persisted flag only if the interface flags are entirely unreadable. */
    if (access(JW_WIFI_FLAGS_PATH, R_OK) == 0) {
        return jw__wifi_iface_up();
    }
    int flag = jw__wifi_read_enable_flag();
    return flag > 0;
}

/* Loong's generic system_config writer from libloong_sdk. Driving WIFI_PARAM
   through it is exactly how stock toggles the radio: loong applies it live AND
   persists to loong.db, and its watchdog honors the new isEnable state — whereas
   ifconfig is reverted by that watchdog. Signature confirmed by disassembly:
   int WriteConfig(const char *param, const char *value, const char *backup, bool). */
typedef int (*jw_writeconfig_fn)(const char *, const char *, const char *, int);
static jw_writeconfig_fn jw__loong_writeconfig(void) {
    static jw_writeconfig_fn fn = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        void *h = dlopen("/usr/lib/libloong_sdk.so", RTLD_NOW | RTLD_GLOBAL);
        if (h) {
            /* dlsym returns void*; the *(void**)& idiom avoids the ISO-C
               object->function pointer cast warning (matches device_mlp1.c). */
            *(void **)(&fn) = dlsym(h, "WriteConfig");
        }
    }
    return fn;
}

/* PID of the running wpa_supplicant, or -1 if none. (Scans /proc by comm.) */
static int jw__wifi_supplicant_pid(void) {
    DIR *proc = opendir("/proc");
    if (!proc) {
        return -1;
    }
    int found = -1;
    struct dirent *e;
    while ((e = readdir(proc)) != NULL) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') {
            continue;
        }
        char path[PATH_MAX];
        char comm[64] = { 0 };
        snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
        FILE *cf = fopen(path, "r");
        if (!cf) {
            continue;
        }
        int hit = (fgets(comm, sizeof(comm), cf) &&
                   strncmp(comm, "wpa_supplicant", 14) == 0);
        fclose(cf);
        if (hit) {
            found = atoi(e->d_name);
            break;
        }
    }
    closedir(proc);
    return found;
}

/* Ensure the wpa conf exists with at least ctrl_interface + update_config, so a
   freshly-started supplicant exposes its control socket and can save networks.
   loong recreates this at boot, but if the radio booted off it may be absent. */
static void jw__wifi_ensure_conf(void) {
    if (access(JW_WIFI_CONF_PATH, F_OK) == 0) {
        return;
    }
    mkdir(JW_WIFI_CTRL_DIR, 0755);   /* best-effort; may already exist */
    FILE *f = fopen(JW_WIFI_CONF_PATH, "w");
    if (!f) {
        return;
    }
    fputs("ctrl_interface=" JW_WIFI_CTRL_DIR "\nupdate_config=1\n", f);
    fclose(f);
}

/* Start wpa_supplicant the way stock loong does, if it isn't already running,
   and wait briefly for its control socket to appear. No-op if already up. */
static void jw__wifi_start_supplicant(void) {
    if (jw__wifi_supplicant_pid() >= 0) {
        return;   /* already running (loong's or ours) */
    }
    jw__wifi_ensure_conf();
    pid_t pid = fork();
    if (pid < 0) {
        return;
    }
    if (pid == 0) {
        execl(JW_WIFI_SUPPLICANT_BIN, "wpa_supplicant", "-B",
              "-i", JW_WIFI_IFACE, "-c", JW_WIFI_CONF_PATH,
              "-P", JW_WIFI_PIDFILE, (char *)NULL);
        /* Fall back to a PATH lookup if the absolute path differs on a unit. */
        execlp("wpa_supplicant", "wpa_supplicant", "-B",
               "-i", JW_WIFI_IFACE, "-c", JW_WIFI_CONF_PATH,
               "-P", JW_WIFI_PIDFILE, (char *)NULL);
        _exit(127);
    }
    /* -B daemonizes: our direct child returns once backgrounded — reap it. */
    int status = 0;
    waitpid(pid, &status, 0);
    /* Give the control socket up to ~2s to appear before issuing wpa_cli cmds. */
    for (int i = 0; i < 20; i++) {
        if (access(JW_WIFI_CTRL_SOCK, F_OK) == 0) {
            break;
        }
        usleep(100 * 1000);
    }
}

/* Bring wlan0 up (aligns with loong's enable intent, so the watchdog won't
   fight it — unlike `ifconfig down`, which it reverts). */
static void jw__wifi_iface_set_up(void) {
    pid_t pid = fork();
    if (pid < 0) {
        return;
    }
    if (pid == 0) {
        execlp("ifconfig", "ifconfig", JW_WIFI_IFACE, "up", (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
}

/* Bring wlan0 down. In Leaf mode stock loong_service (and its wifi watchdog) is
   not running, so nothing reverts this — unlike on stock. */
static void jw__wifi_iface_set_down(void) {
    pid_t pid = fork();
    if (pid < 0) {
        return;
    }
    if (pid == 0) {
        execlp("ifconfig", "ifconfig", JW_WIFI_IFACE, "down", (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
}

/* Stop wpa_supplicant so the radio stops scanning and won't reconnect. In Leaf
   mode no stock daemon restarts it, so the radio stays off until the next
   jw_wifi_set_radio(true). */
static void jw__wifi_stop_supplicant(void) {
    int pid = jw__wifi_supplicant_pid();
    if (pid <= 0) {
        return;
    }
    kill(pid, SIGTERM);
    for (int i = 0; i < 20; i++) {
        if (jw__wifi_supplicant_pid() != pid) {
            return;
        }
        usleep(100 * 1000);
    }
    kill(pid, SIGKILL);
}

/* Persist the Wi-Fi off-intent so a reboot keeps the radio off. Stock S40network
   re-ups wlan0 on every boot regardless of the last toggle, so without this the
   radio always comes back on. The boot hook (00-wifi-dhcpv4.sh) honors this
   marker and brings the interface back down. Lives in the platform state dir,
   like the other Leaf markers (adb-enabled, boot-splash-disabled). */
static void jw__wifi_set_disabled_marker(bool disabled) {
    const char *dir = getenv("UMRK_INTERNAL_DATA_PATH");
    if (!dir || !dir[0]) {
        return;
    }
    char path[512];
    int n = snprintf(path, sizeof(path), "%s/wifi-disabled", dir);
    if (n <= 0 || (size_t)n >= sizeof(path)) {
        return;
    }
    if (disabled) {
        FILE *fp = fopen(path, "w");
        if (fp) {
            fclose(fp);
        }
    } else {
        unlink(path);
    }
}

int jw_wifi_set_radio(bool on) {
    jw_writeconfig_fn WriteConfig = jw__loong_writeconfig();
    if (!WriteConfig) {
        return -1;
    }
    int rc = WriteConfig("WIFI_PARAM", on ? "{\"enable\":1}" : "{\"enable\":0}", "", 1);
    jw__wifi_set_disabled_marker(!on);
    if (on) {
        /* loong sets the enable flag and re-ups the interface, but its LIVE
           enable path does not (re)start wpa_supplicant the way its boot init
           does — so the radio reads "on" yet there's no scan/SSIDs/IP. Bring the
           interface up, start the supplicant ourselves if absent, then re-add
           saved networks and (re)connect with DHCP. */
        jw__wifi_iface_set_up();
        jw__wifi_start_supplicant();
        jw_wifi_restore();
        jw_wifi_recover();
    } else {
        /* WriteConfig persists enable:0 to loong.db, but in Leaf mode no stock
           loong daemon is running to act on it live — so the radio would stay on.
           Turn it off ourselves: drop the connection, stop the supplicant, and
           bring the interface down. With stock loong_service absent there is no
           watchdog to revert the interface-down. */
        const char *disconnect[] = { "disconnect" };
        (void)jw__wifi_run_ok(disconnect, 1);
        jw__wifi_stop_supplicant();
        jw__wifi_iface_set_down();
    }
    return rc < 0 ? -1 : 0;
}

void jw_wifi_recover(void) {
    char buf[64];
    const char *enable_all[] = { "enable_network", "all" };
    const char *reconnect[]  = { "reconnect" };
    (void)jw__wifi_run(enable_all, 2, buf, sizeof(buf));
    (void)jw__wifi_run(reconnect, 1, buf, sizeof(buf));
    /* Reassociating alone leaves us with no IP (the failed connect's select_network
       tore down the lease) — kick DHCP like the connect path does. */
    jw__wifi_dhcp();
}

/* Add (but don't select) a profile: add_network + ssid + psk/open + enable. */
static int jw__wifi_add_profile(const char *ssid, const char *psk) {
    char buf[128];
    char idbuf[16];
    const char *add[] = { "add_network" };
    if (jw__wifi_run(add, 1, buf, sizeof(buf)) < 0) {
        return -1;
    }
    int id = atoi(buf);
    if (id < 0) {
        return -1;
    }
    snprintf(idbuf, sizeof(idbuf), "%d", id);

    char qssid[72];
    snprintf(qssid, sizeof(qssid), "\"%s\"", ssid);
    const char *set_ssid[] = { "set_network", idbuf, "ssid", qssid };
    if (jw__wifi_run(set_ssid, 4, buf, sizeof(buf)) < 0) {
        return -1;
    }
    if (psk && psk[0]) {
        if (jw__wifi_set_secured_key_mgmt(idbuf, jw__wifi_ssid_sae(ssid)) < 0) {
            return -1;
        }
        if (jw__wifi_psk_is_pmk(psk)) {
            const char *set_psk[] = { "set_network", idbuf, "psk", psk };
            if (jw__wifi_run_ok(set_psk, 4) < 0) {
                return -1;
            }
        } else {
            char qpsk[140];
            snprintf(qpsk, sizeof(qpsk), "\"%s\"", psk);
            const char *set_psk[] = { "set_network", idbuf, "psk", qpsk };
            if (jw__wifi_run_ok(set_psk, 4) < 0) {
                return -1;
            }
        }
    } else {
        const char *set_open[] = { "set_network", idbuf, "key_mgmt", "NONE" };
        if (jw__wifi_run_ok(set_open, 4) < 0) {
            return -1;
        }
    }
    const char *enable[] = { "enable_network", idbuf };
    (void)jw__wifi_run(enable, 2, buf, sizeof(buf));
    return 0;
}

int jw_wifi_restore(void) {
    char path[PATH_MAX];
    if (jw__wifi_durable_path(path, sizeof(path)) != 0) {
        return -1;
    }
    int added = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        char line[256];
        char ssid[64] = { 0 };
        char psk[128] = { 0 };
        int in_block = 0;
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "network={")) {
                in_block = 1;
                ssid[0] = '\0';
                psk[0] = '\0';
                continue;
            }
            if (in_block && strchr(line, '}')) {
                if (ssid[0] && jw__wifi_saved_id(ssid) < 0) {
                    if (jw__wifi_add_profile(ssid, psk[0] ? psk : NULL) == 0) {
                        added++;
                    }
                }
                in_block = 0;
                continue;
            }
            if (in_block) {
                char *p;
                if ((p = strstr(line, "ssid=\"")) != NULL) {
                    sscanf(p, "ssid=\"%63[^\"]\"", ssid);
                } else if ((p = strstr(line, "psk=\"")) != NULL) {
                    sscanf(p, "psk=\"%127[^\"]\"", psk);
                } else if ((p = strstr(line, "psk=")) != NULL) {
                    p += 4;
                    while (*p == ' ' || *p == '\t') p++;
                    if (*p && *p != '"') {
                        size_t i = 0;
                        while (p[i] && p[i] != '\n' && p[i] != '\r' &&
                               p[i] != ' ' && p[i] != '\t' &&
                               i + 1 < sizeof(psk)) {
                            psk[i] = p[i];
                            i++;
                        }
                        psk[i] = '\0';
                    }
                }
            }
        }
        fclose(f);

        if (added > 0) {
            char buf[64];
            const char *save[] = { "save_config" };
            (void)jw__wifi_run(save, 1, buf, sizeof(buf));
            const char *reconnect[] = { "reconnect" };
            (void)jw__wifi_run(reconnect, 1, buf, sizeof(buf));
        }
    }

    /* Always refresh the durable store from the current live config, so it
       captures whatever is saved right now — this seeds it on first run (when no
       store exists yet) and keeps it current after adds. */
    jw__wifi_export();
    return added;
}

int jw_wifi_harden(void) {
    char path[PATH_MAX];
    if (jw__wifi_live_conf_path(path, sizeof(path)) != 0) {
        return -1;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    char line[256];
    char ssid[64] = { 0 };
    char pass[128] = { 0 };
    int in_block = 0;
    int has_plaintext = 0;
    int migrated = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "network={")) {
            in_block = 1;
            ssid[0] = '\0';
            pass[0] = '\0';
            has_plaintext = 0;
            continue;
        }
        if (in_block && strchr(line, '}')) {
            /* Only hash networks we can CONFIRM are WPA2-only (sae==0). SAE/WPA3
               or unknown stays plaintext — hashing would break SAE auth. */
            if (ssid[0] && has_plaintext) {
                char hex[80];
                int id = jw__wifi_saved_id(ssid);
                if (id >= 0 && jw__wifi_ssid_sae(ssid) == 0 &&
                    jw__wifi_derive_psk(ssid, pass, hex, sizeof(hex)) == 0) {
                    char idbuf[16];
                    char buf[64];
                    snprintf(idbuf, sizeof(idbuf), "%d", id);
                    const char *set_psk[] = { "set_network", idbuf, "psk", hex };
                    /* jw__wifi_run returns bytes read (>=0) on success, -1 on error. */
                    if (jw__wifi_run(set_psk, 4, buf, sizeof(buf)) >= 0) {
                        migrated++;
                    }
                }
            }
            in_block = 0;
            continue;
        }
        if (in_block) {
            char *p;
            if ((p = strstr(line, "ssid=\"")) != NULL) {
                sscanf(p, "ssid=\"%63[^\"]\"", ssid);
            } else if ((p = strstr(line, "psk=\"")) != NULL) {
                /* Quoted psk = a plaintext passphrase that needs hashing. */
                sscanf(p, "psk=\"%127[^\"]\"", pass);
                has_plaintext = 1;
            }
        }
    }
    fclose(f);

    if (migrated > 0) {
        char buf[64];
        const char *save[] = { "save_config" };
        (void)jw__wifi_run(save, 1, buf, sizeof(buf));
        jw__wifi_export();
    }
    return migrated;
}
