#ifndef JW_PLATFORM_WIFI_H
#define JW_PLATFORM_WIFI_H

#include <stdbool.h>

/* Wi-Fi status, read from the wpa_supplicant control interface via `wpa_cli`.
 * Phase 1 is read-only (status display); scanning/connect come in later phases. */
typedef struct {
    bool valid;        /* false if wpa_cli/status could not be read at all */
    bool connected;    /* wpa_state == COMPLETED */
    char state[24];    /* raw wpa_state, e.g. "COMPLETED", "SCANNING", "DISCONNECTED" */
    char ssid[64];     /* current SSID, "" if none */
    char ip[40];       /* assigned IPv4, "" if none */
    int  rssi;         /* signal in dBm; 0 = unknown/disconnected */
    int  strength;     /* 0=none, 1=weak, 2=good, 3=strong — same RSSI thresholds
                          as the status-bar wifi icon, so the two agree */
} jw_wifi_status_t;

/* Populate *out from `wpa_cli -i wlan0 status` (+ `signal_poll` for RSSI).
 * Returns 0 on success (out->valid = true), -1 if wpa_cli is missing or returned
 * nothing usable (out is still zeroed with valid = false). */
int jw_wifi_status(jw_wifi_status_t *out);

/* ── Scanning (Phase 2) ──────────────────────────────────────────────────── */

#define JW_WIFI_MAX_NETWORKS 32

typedef struct {
    char ssid[64];
    int  rssi;       /* dBm */
    int  strength;   /* 0..3, same thresholds as the status icon */
    bool secured;    /* WPA/WPA2/WPA3/WEP present in the flags */
    bool current;    /* matches the currently-connected SSID */
    bool saved;      /* a saved wpa_supplicant profile exists for this SSID */
} jw_wifi_network_t;

/* Kick off a background scan (`wpa_cli scan`); results land over the next ~1-3s.
 * Returns 0 if the command was accepted, -1 otherwise. */
int jw_wifi_scan_start(void);

/* Read the latest scan results (`wpa_cli scan_results`) into out[]: blank SSIDs
 * skipped, deduped by SSID keeping the strongest, sorted by signal descending.
 * current_ssid (may be NULL/"") flags the in-use network. Returns the count
 * written (0..max), or -1 on failure. */
int jw_wifi_scan_results(const char *current_ssid, jw_wifi_network_t *out, int max);

/* ── Connect (Phase 3) ───────────────────────────────────────────────────── */

typedef enum {
    JW_WIFI_CONNECT_OK = 0,         /* association requested (watch status to confirm) */
    JW_WIFI_CONNECT_NEED_PASSWORD,  /* secured network with no saved profile (Phase 4) */
    JW_WIFI_CONNECT_FAILED,         /* wpa_cli error */
} jw_wifi_connect_result;

/* Connect by SSID. If a saved profile exists, re-selects it. Else if the network
 * is open, creates an open profile and connects. A secured network with no saved
 * profile returns NEED_PASSWORD without changing anything (Phase 4 supplies the
 * key). On OK, a DHCP client is kicked for the interface. */
jw_wifi_connect_result jw_wifi_connect(const char *ssid, bool secured);

/* Connect to a secured network with the given pre-shared key: creates (or reuses)
 * a profile, sets ssid + psk, connects, saves config, and kicks DHCP. A wrong key
 * still returns OK here (the association request was sent) — the caller polls
 * jw_wifi_join_state_for() to detect success vs. a wrong key. (Phase 4) */
jw_wifi_connect_result jw_wifi_connect_psk(const char *ssid, const char *psk);

/* ── Join monitoring via the wpa control-event socket ────────────────────────
 * Wrong-PSK is only reliably signalled by wpa_supplicant's CTRL-EVENT stream
 * (reason=WRONG_KEY); it does NOT show up in `status`/`list_networks` polling.
 * So a connect attempt attaches to the event socket and watches it. */

/* Attach to the wpa event socket. Returns an fd to pass to the poll/close calls,
 * or -1 on failure. Open it right when a connect attempt starts. */
int  jw_wifi_monitor_open(void);

/* Result of draining the event socket during a join attempt. This hardware does
 * NOT emit reason=WRONG_KEY (esp. on SAE/WPA3, where a bad key fails at the auth
 * phase), so we distinguish a definitive wrong key from a generic auth failure. */
typedef enum {
    JW_WIFI_EVT_NONE = 0,    /* nothing conclusive yet */
    JW_WIFI_EVT_WRONG_KEY,   /* explicit WRONG_KEY (plain WPA2-PSK 4-way failure) */
    JW_WIFI_EVT_AUTH_FAIL,   /* auth timed out / assoc-reject — likely bad key, not certain */
} jw_wifi_evt;

/* Drain buffered events (non-blocking) and report the strongest failure seen.
 * Success is detected separately via status COMPLETED on the target SSID. */
jw_wifi_evt jw_wifi_monitor_poll(int fd);

/* Detach + close the monitor fd (and clean up its local socket). */
void jw_wifi_monitor_close(int fd);

/* Recover connectivity after a failed attempt: a connect uses select_network,
 * which disables the previously-connected network, so on failure re-enable all
 * saved networks and reconnect (wpa picks the best reachable one). */
void jw_wifi_recover(void);

/* ── Manage (Phase 5) ────────────────────────────────────────────────────── */

/* Remove the saved profile for ssid (`remove_network` + `save_config`, persisted
 * to the durable conf). Returns 0 on success, -1 if no saved profile / error. */
int jw_wifi_forget(const char *ssid);

/* Disconnect from the current network (`wpa_cli disconnect`). It will not
 * auto-reconnect until a network is selected again. Returns 0/-1. */
int jw_wifi_disconnect(void);

/* Re-add any saved networks from the durable SD store (<sdcard>/.umrk/wifi.conf)
 * that the running wpa_supplicant doesn't already know — used at startup so
 * networks survive a reboot (the live wpa conf is on tmpfs and is wiped). Adds
 * missing profiles, saves, re-exports, and reconnects. Returns the number added
 * (0 if none/no store), -1 on error. Idempotent. */
int jw_wifi_restore(void);

/* Rewrite any saved networks whose key is stored as a plaintext passphrase into
 * the derived PMK hash (via wpa_passphrase), so the reusable passphrase never
 * sits on disk. Runs at startup after restore; saves + re-exports. Returns the
 * number migrated (0 if none), -1 on error. Idempotent (hashed entries skipped). */
int jw_wifi_harden(void);

#endif /* JW_PLATFORM_WIFI_H */
