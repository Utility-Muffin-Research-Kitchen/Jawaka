#include "internal/platform/wifi.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

#define JW_WIFI_IFACE       "wlan0"

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

    /* Parent: read the child's stdout. */
    close(pipefd[1]);
    size_t total = 0;
    for (;;) {
        if (total + 1 >= buf_size) {
            break;
        }
        ssize_t n = read(pipefd[0], buf + total, buf_size - 1 - total);
        if (n > 0) {
            total += (size_t)n;
        } else if (n == 0) {
            break;
        } else {
            break;
        }
    }
    buf[total] = '\0';
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
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
            execlp("udhcpc", "udhcpc", "-t", "5", "-n", "-i", JW_WIFI_IFACE,
                   (char *)NULL);
            _exit(127);
        }
        _exit(0);   /* intermediate child exits immediately; grandchild reparents to init */
    }
    int status = 0;
    waitpid(pid, &status, 0);   /* reap the intermediate child only */
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

    jw__wifi_dhcp();
    return JW_WIFI_CONNECT_OK;
}
