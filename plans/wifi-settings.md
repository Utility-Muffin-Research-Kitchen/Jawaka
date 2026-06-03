# Wi-Fi Settings — phased plan

Goal: a Settings → Network page to view status, scan, and connect to Wi-Fi
networks, so users don't need SSH/ADB to get online.

> ⚠ COORDINATION: Wi-Fi is on Kevin's release must-have list (roadmap.md). No
> branch or code exists yet (checked 2026-06-03). Flag to Kevin before/while
> building so we don't duplicate. This plan is Jawaka-side (launcher UI +
> internal/platform wifi module).

## Device facts (probed 2026-06-03, MLP1 fw 1.3.0.32)
- Tools present: `wpa_cli`, `wpa_supplicant`, `iw`, `iwlist`, `ip`, `ifconfig`,
  `udhcpc`, `dhcpcd` (all in /usr/sbin).
- `wpa_supplicant -B -i wlan0 -c /tmp/wpa_supplicant.conf` already running.
- Control socket: `/var/run/wpa_supplicant/wlan0` (root-owned; launcher is root).
- Interface: `wlan0` (UP, LOWER_UP).
- Signal strength already read from `/proc/net/wireless` (device_mlp1.c:347); the
  status-bar wifi icon uses it.

## Approach
- Drive `wpa_cli -i wlan0 <cmd>` via **fork/exec + pipe** (NO system(), per
  conventions). New module `internal/platform/wifi.{c,h}` (or device_mlp1 wifi
  section) exposing a clean `jw_wifi_*` API; the launcher UI never shells out
  directly.
- DHCP after association: run `udhcpc -i wlan0` (or `dhcpcd wlan0`) via fork/exec
  if the stock path doesn't already pick it up.
- Keep all parsing in the module; the UI consumes structs only.

## Phases (each independently testable on-device + mergeable)

### Phase 1 — Status page (read-only)  ← start here
- New Settings category "Network" + `JW_SETTINGS_NETWORK` page.
- Show current state from `wpa_cli status`: SSID, state (connected/scanning/…),
  signal (from /proc/net/wireless), IP (existing platform_id ip field).
- No scanning, no control. Proves page + nav + reading wpa state.
- API: `jw_wifi_status(jw_wifi_status_t *out)`.

### Phase 2 — Scan + list (read-only)
- Trigger `wpa_cli scan`, poll `scan_results`; parse SSID / signal / security
  flags (WPA/WPA2/open). Dedup by SSID (strongest), sort by signal.
- Render a selectable network list with signal + lock glyph; periodic re-scan.
- API: `jw_wifi_scan_start()`, `jw_wifi_scan_results(list, max, *count)`.

### Phase 3 — Connect (open + already-saved networks)
- Select a network → if open or has a saved profile: `add_network`/
  `set_network ssid`, `enable_network`, `select_network`; show connecting →
  connected; trigger DHCP. No password entry yet.
- API: `jw_wifi_connect_open(ssid)`, `jw_wifi_connect_saved(ssid)`.

### Phase 4 — Password entry (secured networks)
- Reuse the existing on-screen keyboard (`cat_keyboard`, already used by search)
  to capture the PSK → `set_network <id> psk`, connect.
- Handle wrong-password / auth-timeout feedback (watch `status`/events).
- API: `jw_wifi_connect_psk(ssid, psk)`.

### Phase 5 — Manage / persist
- `list_networks`, forget (`remove_network` + `save_config`), disconnect,
  Wi-Fi on/off toggle, auto-reconnect, and persistence across reboot (resolve
  the /tmp vs /etc wpa_supplicant.conf path so saved networks survive). 
- API: `jw_wifi_list_saved`, `jw_wifi_forget(id)`, `jw_wifi_disconnect()`,
  `jw_wifi_set_enabled(bool)`.

## Open questions
- Persistence path: running conf is `/tmp/wpa_supplicant.conf` (ephemeral). Does
  stock restore saved nets on boot? Need `update_config=1` + a writable durable
  conf for `save_config`. Resolve in Phase 5.
- Whether DHCP is auto-run by stock on association or we must invoke it (Phase 3).
- Country/regdomain set? (affects channels/scan) — verify in Phase 2.
