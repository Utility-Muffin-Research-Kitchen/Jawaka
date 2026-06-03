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

#endif /* JW_PLATFORM_WIFI_H */
