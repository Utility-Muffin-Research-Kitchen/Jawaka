# Jawaka — Phase 0 + Phase 1 implementation plan

This is an executable plan for a coding agent. It is scoped to the first
commit set only: Phase 0 (repo bootstrap) and the planning-prescribed Phase 1
("desktop foundation and mock SD card"), narrowed to the minimum that proves
the daemon/launcher/menu architecture is real **without** pretending the
supervision, UI, and discovery pieces can be deferred indefinitely.

The agent should treat this plan as canonical. The broader phased roadmap is in
[`jawaka.md`](./jawaka.md) and the binding design decisions are in
[`jawaka-architecture-decisions.md`](#references) (also reproduced in
[`docs/ARCHITECTURE_DECISIONS.md`](#deliverables) inside the Jawaka repo).

Working directory for this plan: `/Volumes/Storage/UMRK/Jawaka/`.

---

## 1. Mission

Land the first commit set for the Jawaka repo with three real binaries that:

- build on macOS (and Linux) with plain `make`
- are launched through `jawakad` as the normal entrypoint
- talk to each other over a real Unix domain socket using length-prefixed JSON
  frames
- agree on an SD-card-shaped data root, defaulting to `./mock-sdcard/`
- apply a real SQLite schema (with FTS5) at daemon startup
- perform one real daemon-owned scan against a mockgen-produced Done-Set-Three-shaped tree
- bring up minimal Catastrophe-backed launcher and menu surfaces
- render a tiny real Jawaka shell instead of blank placeholder windows
- prove that the daemon can supervise the launcher and invoke the menu

The goal is **architectural honesty**, not feature completeness. The three
processes really do exec separately, the IPC really is socket-based, the DB
really opens, the daemon really stays in charge, the launcher/menu really
create minimal UI surfaces, and a real scan pass really populates the DB.
Feature richness, polished browsing UX, advanced supervision, and complete
Catastrophe widget coverage are still deferred.

A reviewer should be able to:

```sh
cd /Volumes/Storage/UMRK/Jawaka
make mockgen
make
make run-daemon
```

…and see `jawakad`:

- start and apply the schema
- launch `jawaka-launcher`
- receive real IPC traffic
- perform one scan into SQLite
- invoke `jawaka-menu`
- shut everything down cleanly

---

## 2. Hard constraints (do not violate)

These come straight from `jawaka-architecture-decisions.md`. The agent should
not relitigate them.

1. **IPC transport is Unix domain socket + length-prefixed JSON.** No pipes,
   no signals, no shared files, no stdio-based RPC. Length prefix is a 4-byte
   big-endian unsigned integer immediately followed by exactly that many bytes
   of UTF-8 JSON. Cap a single frame at 16 MiB and reject anything larger.
2. **Real separate processes.** `jawakad`, `jawaka-launcher`, `jawaka-menu` are
   three separate `main()` entry points compiled to three separate binaries.
   No "single binary with subcommands" shortcut. Even on Mac.
3. **C-first.** All code in this commit set is C11. C++ stays available for
   later use in `jawakad` but is not introduced here.
4. **Mac-first.** Build and verify on macOS. Linux should still work but is not
   the verification target. Cross-compile dispatchers under `ports/*` are
   placeholder Makefiles only.
5. **SD-card layout is Done-Set-Three-inspired.** ROMs at
   `Roms/<SYSTEM_CODE>/<title>.<ext>`, BIOS at `BIOS/`, artwork at a sibling
   top-level `Images/<SYSTEM_CODE>/<title>.png`, apps at `Apps/<Name>.pak/`.
   Do **not** colocate artwork inside `Roms/<SYSTEM>/`.
6. **FAT32-safe for content.** No symlinks, no case-only-differing names, no
   reserved Windows filenames. Runtime sockets live under `/tmp/` (tmpfs),
   not on the SD-card path.
7. **Catastrophe is a future submodule.** The repo has no `origin` yet, so
   the submodule cannot literally be added in this commit. Leave a
   `third_party/README.md` documenting the future `git submodule add` command,
   keep `third_party/catastrophe/` empty, and allow Phase 0+1 builds to point
   at a local Catastrophe checkout via `CATASTROPHE_DIR`.
8. **Single-source-of-truth for the SQL schema.** Either keep the schema only
   in a `.sql` file that the daemon reads at runtime, or only as a `const char *`
   inside `db.c`. Do not duplicate. This plan chooses the embedded-string
   approach and treats `internal/db/schema.sql` as a non-authoritative
   reviewer-facing copy with a comment pointing at `db.c` as canonical, until
   Phase 2 introduces a real codegen step.

---

## 3. Deliverables

Files to create under `/Volumes/Storage/UMRK/Jawaka/`. The directory skeleton
(`cmd/{jawakad,jawaka-launcher,jawaka-menu}`, `internal/{core,ipc,platform,db}`,
`scripts`, `third_party`, `ports/{tg5040,tg5050,my355}`, `docs`) already exists.

| Path | Purpose |
|------|---------|
| `.gitignore` | ignore `build/`, `mock-sdcard/`, `.DS_Store`, `*.o`, `*.dSYM/`, `third_party/catastrophe/*` except its `.gitkeep` |
| `LICENSE` | MIT, year 2026, "Jawaka contributors". Match Catastrophe's tone. |
| `README.md` | short overview, status (Phase 0 + 1), build/run instructions, env vars, layout, license |
| `Makefile` | top-level dispatcher (see §6) |
| `docs/PLAN.md` | verbatim copy of `/Volumes/Storage/UMRK/plans/jawaka.md` at the time of commit |
| `docs/ARCHITECTURE_DECISIONS.md` | verbatim copy of `/Users/kevinvranken/Downloads/jawaka-architecture-decisions.md` |
| `cmd/jawakad/main.c` | daemon entry point (see §7.1) |
| `cmd/jawaka-launcher/main.c` | launcher stub (see §7.2) |
| `cmd/jawaka-menu/main.c` | menu stub (see §7.3) |
| `internal/core/log.h` / `log.c` | tiny stderr logger (see §7.4) |
| `internal/platform/paths.h` / `paths.c` | runtime dir, sdcard root, socket path, db path (see §7.5) |
| `internal/ipc/ipc.h` / `ipc.c` | UDS server/client + length-prefixed framing (see §7.6) |
| `internal/db/db.h` / `db.c` | SQLite open + apply schema (see §7.7) |
| `internal/db/schema.sql` | reviewer copy of the schema, header-commented as non-authoritative |
| `internal/discovery/discovery.h` / `discovery.c` | one real scan pass for games and apps into SQLite |
| `scripts/mockgen.sh` | mock SD-card generator (see §8) |
| `third_party/README.md` | how Catastrophe will be added as a submodule once origin exists |
| `third_party/catastrophe/.gitkeep` | preserve the empty dir |
| `third_party/cjson/cJSON.h` / `cJSON.c` | vendored JSON parser for the Phase 0+1 IPC command set |
| `ports/tg5040/Makefile`, `ports/tg5050/Makefile`, `ports/my355/Makefile` | identical placeholder stubs that `echo "not yet implemented" && exit 1` |

`internal/metadata/` and `internal/search/` are named in the prescribed layout
but contain no code yet — **do not create empty directories or `.gitkeep`
files for them**. They will appear in later phases when richer metadata/search
logic lands.

---

## 4. Architecture summary (compressed)

### 4.1 Process model

- `jawakad`: long-lived coordinator. In Phase 0+1 it binds the IPC socket,
  opens the DB, applies the schema, launches `jawaka-launcher`, stays up to
  accept commands, performs one real scan, can invoke `jawaka-menu`, and shuts
  down cleanly when told.
- `jawaka-launcher`: Catastrophe-backed minimal launcher shell. It connects to
  `jawakad`, performs a hello, requests a scan, renders top-level sections plus
  a tiny scanned-content summary, can request `jawaka-menu`, and can request
  shutdown.
- `jawaka-menu`: Catastrophe-backed minimal menu window invoked by the daemon.
  It performs the same hello flow, proves the process/UI boundary is real, and
  can return control cleanly.

The agent should not introduce threading shortcuts to "make the launcher
embedded in the daemon" — that would violate decision 2.

### 4.2 IPC wire format

```
+---------+-------------------------+
| len:u32 | json body (utf-8 bytes) |
+---------+-------------------------+
   4 B          len bytes
```

- `len` is network byte order (`htonl` / `ntohl`).
- A frame larger than `16 * 1024 * 1024` bytes is rejected.
- Bodies are JSON objects with a `"type"` field. Phase 0+1 sends a deliberately
  tiny but real command set:
  - launcher/menu → daemon: `{"type":"hello","role":"launcher"}` / `{"type":"hello","role":"menu"}`
  - daemon → launcher/menu: `{"type":"hello-ok","version":"0.0.1"}`
  - launcher → daemon: `{"type":"scan-library"}`
  - launcher → daemon: `{"type":"open-menu"}`
  - launcher/menu → daemon: `{"type":"shutdown"}`
  - daemon → launcher/menu: `{"type":"ok","action":"<...>"}` or `{"type":"error","message":"<...>"}`
- Phase 0+1 should include a small real JSON parser rather than rely on
  `strstr` hacks, because the command set now includes multiple message types.
  Vendored cJSON is acceptable in this phase.

### 4.3 Path conventions

| Logical path | Mac default | Env override |
|--------------|-------------|--------------|
| Runtime dir (sockets, pid files) | `/tmp/jawaka-$USER/` | `JAWAKA_RUNTIME_DIR` |
| SD-card root | `./mock-sdcard/` | `JAWAKA_SDCARD_ROOT` |
| IPC socket | `<runtime>/jawakad.sock` | derived |
| DB file | `<sdcard>/.jawaka/library.db` | derived |

`paths.c` is responsible for creating `<runtime>` (mode 0700) and
`<sdcard>/.jawaka/` (mode 0755) if they don't exist, but **not** for creating
the SD-card root itself — that's `mockgen.sh`'s job. If `<sdcard>` doesn't
exist when the daemon starts, log a clear error pointing at `make mockgen`
and exit non-zero.

### 4.4 SD-card layout

```
<sdcard root>/
├── Roms/
│   ├── GBA/
│   │   ├── Advance Wars.zip
│   │   └── ...
│   ├── SFC/
│   ├── PS/
│   └── ...
├── Images/
│   ├── GBA/
│   │   └── Advance Wars.png
│   └── ...
├── BIOS/
├── Apps/
│   ├── HelloApp.pak/
│   │   ├── launch.sh
│   │   └── pak.json
│   └── Tools.pak/
└── .jawaka/
    └── library.db                # created by jawakad at startup
```

Notes:

- ROM extension is whatever Done Set Three uses for that system (mostly `.zip`,
  some `.chd`, `.nds`, `.7z`, `.m3u`). Phase 1 just needs **plausible**
  extensions on stub files; nothing parses them yet.
- A PNG placeholder is fine — write the literal string `PNG` to the file. It's
  not a valid image; Phase 2 metadata code will tolerate that or replace these
  with real 1x1 PNGs.
- The `.jawaka/` directory is FAT32-safe (dotted directories are allowed,
  just not hidden).

### 4.5 System code table (canonical for Phase 1 mockgen and Phase 2 discovery)

Derived from the Done Set Three layout under
`/Volumes/downloads/done-set-three_202501/.../Roms/`. Phase 1 uses a subset for
mockgen content. Phase 2 will load the full table into the discovery module.

| Code | Display name | Common ext |
|------|--------------|------------|
| `ARCADE` | Arcade (FBNeo) | `zip` |
| `ATARI` | Atari 2600 | `zip` |
| `COLECO` | ColecoVision | `zip` |
| `COMMODORE` | Commodore 64 | `zip` |
| `CPS3` | Capcom CPS3 | `zip` |
| `DOS` | DOS | `zip` |
| `FC` | Famicom / NES | `zip` |
| `FDS` | Famicom Disk System | `zip` |
| `FIFTYTWOHUNDRED` | Atari 5200 | `zip` |
| `GB` | Game Boy | `zip` |
| `GBA` | Game Boy Advance | `zip` |
| `GBC` | Game Boy Color | `zip` |
| `GG` | Game Gear | `zip` |
| `GW` | Game & Watch | `zip` |
| `LYNX` | Atari Lynx | `zip` |
| `MD` | Mega Drive / Genesis | `zip` |
| `MS` | Master System | `zip` |
| `NDS` | Nintendo DS | `nds` |
| `NEOGEO` | Neo Geo | `zip` |
| `PCE` | PC Engine / TurboGrafx-16 | `zip` |
| `PCECD` | PC Engine CD | `chd` |
| `PICO` | Sega Pico | `zip` |
| `PORTS` | Native ports | `sh` |
| `PS` | PlayStation | `chd` |
| `SATELLAVIEW` | Satellaview | `zip` |
| `SCUMMVM` | ScummVM | `zip` |
| `SEGACD` | Sega CD | `chd` |
| `SEVENTYEIGHTHUNDRED` | Atari 7800 | `zip` |
| `SFC` | Super Famicom / SNES | `zip` |
| `VECTREX` | Vectrex | `zip` |
| `ZXS` | ZX Spectrum | `zip` |

Phase 1 only needs the codes that `mockgen.sh` actually populates (see §8).
The full table can live inline in `mockgen.sh` and the agent does not need
to commit a separate alias-table file yet.

### 4.6 SQLite schema

```sql
PRAGMA foreign_keys = ON;
PRAGMA user_version = 1;

CREATE TABLE IF NOT EXISTS games (
    id          INTEGER PRIMARY KEY,
    system      TEXT    NOT NULL,
    name        TEXT    NOT NULL,
    rom_path    TEXT    NOT NULL UNIQUE,
    image_path  TEXT,
    last_played INTEGER,
    playtime_s  INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS apps (
    id                  INTEGER PRIMARY KEY,
    pak_dir             TEXT NOT NULL UNIQUE,
    name                TEXT NOT NULL,
    icon                TEXT,
    platform            TEXT,
    pak_version         TEXT,
    min_jawaka_version  TEXT
);

CREATE TABLE IF NOT EXISTS favorites (
    kind       TEXT NOT NULL CHECK (kind IN ('game','app')),
    target_id  INTEGER NOT NULL,
    added_at   INTEGER NOT NULL,
    PRIMARY KEY (kind, target_id)
);

CREATE TABLE IF NOT EXISTS recents (
    kind        TEXT NOT NULL CHECK (kind IN ('game','app')),
    target_id   INTEGER NOT NULL,
    last_opened INTEGER NOT NULL,
    duration_s  INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (kind, target_id)
);

CREATE VIRTUAL TABLE IF NOT EXISTS games_fts USING fts5(
    name, system, content='games', content_rowid='id'
);

CREATE TRIGGER IF NOT EXISTS games_ai AFTER INSERT ON games BEGIN
    INSERT INTO games_fts(rowid, name, system)
        VALUES (new.id, new.name, new.system);
END;

CREATE TRIGGER IF NOT EXISTS games_ad AFTER DELETE ON games BEGIN
    INSERT INTO games_fts(games_fts, rowid, name, system)
        VALUES ('delete', old.id, old.name, old.system);
END;

CREATE TRIGGER IF NOT EXISTS games_au AFTER UPDATE ON games BEGIN
    INSERT INTO games_fts(games_fts, rowid, name, system)
        VALUES ('delete', old.id, old.name, old.system);
    INSERT INTO games_fts(rowid, name, system)
        VALUES (new.id, new.name, new.system);
END;
```

Notes:

- macOS's bundled libsqlite3 and Debian/Ubuntu's `libsqlite3-dev` both ship
  with FTS5 enabled by default. Don't add a configure step to detect it; if
  it's missing the agent should log the SQLite error verbatim and exit.
- `playtime_s` is on `games` to support decision 5 ("playtime tracking, not
  just last-opened"). Phase 4 will write to it; Phase 1 just creates the column.
- The recents table also tracks `duration_s` per row so playtime accumulation
  has a place to land.

---

## 5. Naming, formatting, style

Mirror Catastrophe's conventions so cross-repo readers don't context-switch:

- C11 (`-std=c11`).
- 4-space indentation, no tabs.
- Opening braces on the same line.
- Public symbols: `jw_` prefix (e.g. `jw_log_info`, `jw_ipc_client_send`).
- Internal/static helpers: `jw__` (double underscore) — used sparingly; most
  helpers in this commit set are file-static and don't need the prefix.
- Macros, constants, enum values: `JW_` (e.g. `JW_OK`, `JW_IPC_MAX_FRAME`).
- Log messages are lowercase, no trailing punctuation. Errors prefer
  `"<verb> <subject>: <reason>"`.
- Header guards use `JW_<MODULE>_<FILE>_H` (e.g. `JW_IPC_H`).
- Headers expose declarations only; implementations live in `.c`.
- No `#pragma once` — use `#ifndef` guards.

---

## 6. Makefile spec

Single root Makefile, no recursion except into `ports/<platform>/`.

Required targets:

- `all` (default): build `build/bin/jawakad`, `build/bin/jawaka-launcher`,
  `build/bin/jawaka-menu`.
- `jawakad`, `jawaka-launcher`, `jawaka-menu`: per-binary aliases.
- `mockgen`: shell out to `bash scripts/mockgen.sh`.
- `run-daemon`, `run-launcher`, `run-menu`: convenience runners. `run-daemon`
  depends on `mockgen` as well as the binary so a first-run user gets a tree.
- `clean`: `rm -rf build`.
- `help`: print the target list.
- `tg5040`, `tg5050`, `my355`: `$(MAKE) -C ports/$@ all` (placeholders that
  exit non-zero with a friendly message in this phase).

Variables:

- `CC ?= cc`
- `CSTD := -std=c11`
- `CWARN := -Wall -Wextra -Wpedantic -Wno-unused-parameter`
- `CDEBUG ?= -g -O0`
- `CFLAGS_COMMON := $(CSTD) $(CWARN) $(CDEBUG) -Iinternal`
- `LDLIBS_COMMON := -lsqlite3`
- `BUILD ?= build`
- `CATASTROPHE_DIR ?= third_party/catastrophe` (unused this phase but reserved)

Shared sources:

```
CORE_SRCS := \
    internal/core/log.c \
    internal/ipc/ipc.c \
    internal/platform/paths.c \
    internal/db/db.c
```

Per-binary rule pattern (one option — agent may use object files instead, but
keep it simple for Phase 1):

```make
build/bin/%: cmd/%/main.c $(CORE_SRCS) | build/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $< $(CORE_SRCS) $(LDLIBS_COMMON)
```

Or use a `define`/`eval` loop over `BINS := jawakad jawaka-launcher jawaka-menu`
— either is fine. Just avoid silently sharing object files across binaries
without per-TU compilation, since each `main.c` is a different translation unit.

`run-daemon` recipe should pick up env overrides correctly:

```make
run-daemon: build/bin/jawakad mockgen
	JAWAKA_SDCARD_ROOT="$${JAWAKA_SDCARD_ROOT:-./mock-sdcard}" \
	$(BUILD)/bin/jawakad
```

`make clean` must not touch `mock-sdcard/` (decision: regenerating the mock
tree is opt-in).

---

## 7. Module-by-module spec

### 7.1 `cmd/jawakad/main.c`

Behavior:

1. Install `SIGINT`/`SIGTERM` handlers that set a `volatile sig_atomic_t shutdown` flag.
2. `signal(SIGPIPE, SIG_IGN)` so a peer disconnect doesn't kill the daemon.
3. Resolve `runtime_dir`, `sdcard_root`, `socket_path`, `db_path` via
   `internal/platform/paths.h`.
4. Verify the SD-card root exists. If not, log
   `"sdcard root missing: <path> (run 'make mockgen')"` and exit 2.
5. Open the DB and apply the schema. Failures log the sqlite error and exit 1.
6. Bind and listen on the IPC socket. Failures log and exit 1.
7. Log a 4-line startup banner showing the resolved paths.
8. Spawn `jawaka-launcher` as a child process with the resolved env vars.
9. Enter a small accept loop and handle the Phase 0+1 command set:
   - hello
   - scan-library
   - open-menu
   - shutdown
10. `scan-library` triggers one real scan pass over the mock SD-card tree and
    populates the SQLite tables.
11. `open-menu` launches `jawaka-menu` as a child process if one is not already
    active.
12. On shutdown, terminate/wait for child processes, close the server (which
    unlinks the socket), close the DB, free path strings, log
    `"jawakad exiting"`.

### 7.2 `cmd/jawaka-launcher/main.c`

Behavior:

1. Resolve `socket_path` via paths helper.
2. `jw_ipc_client_connect`. On failure log `"could not connect to jawakad
   at <path>; is the daemon running?"` and exit 1.
3. Send the hello frame: `{"type":"hello","role":"launcher"}`.
4. Create a minimal Catastrophe-backed launcher shell.
5. Send `scan-library`.
6. After the scan completes, render:
   - the top-level Jawaka sections
   - a small count/summary of scanned games and apps
   - a short sample of scanned items or systems
7. On Mac, provide simple keyboard shortcuts:
   - `M` -> `open-menu`
   - `R` -> `scan-library`
   - `Q` -> `shutdown`
8. Close the client and exit 0 when shutdown is acknowledged.

This is still intentionally small, but it is no longer a pure CLI stub or a
blank UI placeholder.

### 7.3 `cmd/jawaka-menu/main.c`

Identical to launcher at the transport layer except the role string is
`"menu"`. It should also create a minimal Catastrophe-backed menu surface and
prove that the daemon can invoke it as a separate process. The menu only needs
to render a tiny contextual shell with a small action set such as refresh,
close, and shutdown.

On Mac, the first menu shortcut contract can stay equally simple:

- `R` -> request `scan-library`
- `Q` -> request `shutdown`
- `Esc` -> close the menu and return control

### 7.4 `internal/core/log.h` + `log.c`

```c
// log.h
#ifndef JW_CORE_LOG_H
#define JW_CORE_LOG_H

void jw_log_impl(const char *level, const char *fmt, ...);

#define jw_log_info(...)  jw_log_impl("INFO",  __VA_ARGS__)
#define jw_log_warn(...)  jw_log_impl("WARN",  __VA_ARGS__)
#define jw_log_error(...) jw_log_impl("ERROR", __VA_ARGS__)

#endif
```

Implementation: stderr only. Line format:

```
YYYY-MM-DD HH:MM:SS LEVEL <message>
```

Use `localtime_r` and `strftime("%Y-%m-%d %H:%M:%S")`. No log file rotation,
no syslog, no `stderr`/`stdout` split. Phase 5+ may swap this for something
richer; for Phase 1 it just needs to be readable.

### 7.5 `internal/platform/paths.h` + `paths.c`

```c
// paths.h
#ifndef JW_PLATFORM_PATHS_H
#define JW_PLATFORM_PATHS_H

// All getters return malloc'd strings owned by the caller.
// Each call may create directories as a side effect:
//   jw_runtime_dir     creates the runtime dir if missing (mode 0700)
//   jw_db_path         creates <sdcard>/.jawaka if missing (mode 0755)
char *jw_runtime_dir(void);
char *jw_sdcard_root(void);   // does NOT create the dir; caller verifies
char *jw_socket_path(void);
char *jw_db_path(void);

#endif
```

Implementation notes:

- `jw_runtime_dir`: read `JAWAKA_RUNTIME_DIR`; fall back to
  `"/tmp/jawaka-" + username`. Username from `getenv("USER")`, falling back to
  `getpwuid(getuid())->pw_name`, falling back to `"anon"`. `mkdir(path, 0700)`,
  ignoring `EEXIST`.
- `jw_sdcard_root`: read `JAWAKA_SDCARD_ROOT`; fall back to `"./mock-sdcard"`.
  No mkdir.
- `jw_socket_path`: `<runtime>/jawakad.sock`.
- `jw_db_path`: `<sdcard>/.jawaka/library.db`; mkdir `<sdcard>/.jawaka` 0755,
  ignoring `EEXIST`.

### 7.6 `internal/ipc/ipc.h` + `ipc.c`

```c
// ipc.h
#ifndef JW_IPC_H
#define JW_IPC_H

#include <stddef.h>

#define JW_IPC_MAX_FRAME (16u * 1024u * 1024u)

typedef struct jw_ipc_server jw_ipc_server;
typedef struct jw_ipc_client jw_ipc_client;

// Server.
int  jw_ipc_server_listen(const char *socket_path, jw_ipc_server **out);
// timeout_ms < 0 blocks forever; returns 0 on accept, 1 on timeout, -1 on error
int  jw_ipc_server_accept(jw_ipc_server *server,
                          jw_ipc_client **out_client,
                          int timeout_ms);
void jw_ipc_server_close(jw_ipc_server *server);

// Client.
int  jw_ipc_client_connect(const char *socket_path, jw_ipc_client **out);
int  jw_ipc_client_send(jw_ipc_client *client, const char *json, size_t len);
// out_json is malloc'd, NUL-terminated, with the exact frame body
int  jw_ipc_client_recv(jw_ipc_client *client, char **out_json, size_t *out_len);
void jw_ipc_client_close(jw_ipc_client *client);

#endif
```

Implementation notes:

- `AF_UNIX`, `SOCK_STREAM`. No `SOCK_CLOEXEC`/`SOCK_NONBLOCK` flags — keep it
  POSIX-baseline since macOS only got those flags recently.
- Server unlinks any stale socket at the path before `bind`.
- Length prefix: `uint32_t` network order via `htonl`/`ntohl`
  (`<arpa/inet.h>`).
- `read_all` / `write_all` helpers loop on `EINTR` and treat short reads as
  fatal.
- `jw_ipc_server` retains its socket path so close-time can `unlink` it.
- Reject incoming frames with `len > JW_IPC_MAX_FRAME`.
- Parse and build message bodies with vendored cJSON from `third_party/cjson/`.

### 7.7 `internal/db/db.h` + `db.c`

```c
// db.h
#ifndef JW_DB_H
#define JW_DB_H

#include <sqlite3.h>

int  jw_db_open(const char *path, sqlite3 **out);
int  jw_db_apply_schema(sqlite3 *db);
void jw_db_close(sqlite3 *db);

#endif
```

Implementation: embed the schema string from §4.6 as a single
`static const char *kSchemaSql = ...;`. Run it via `sqlite3_exec`. On error
log the `sqlite3_errmsg` / `sqlite3_exec` `err` output, `sqlite3_free` the
error message, return -1.

`schema.sql` is the same text in `.sql` form, with a leading comment:

```sql
-- internal/db/schema.sql
-- Non-authoritative reviewer-facing copy. The runtime schema is the
-- embedded string in internal/db/db.c. Keep these in sync by hand until
-- Phase 2 introduces a real codegen step.
```

---

## 8. `scripts/mockgen.sh`

Bash, `#!/usr/bin/env bash`. Must work on macOS's bundled bash 3.2 — that
means no associative arrays, no `mapfile`. Use a here-doc table read with
`while IFS=: read`.

Behavior:

1. Resolve `ROOT="${JAWAKA_SDCARD_ROOT:-./mock-sdcard}"`.
2. If `ROOT` exists and `FORCE != "1"`, log "updating in place"; if `FORCE=1`,
   `rm -rf "$ROOT"` first.
3. `mkdir -p "$ROOT"/{Roms,Images,BIOS,Apps}`.
4. For each row in the embedded system table, create
   `Roms/<SYS>/<Title>.<ext>` and `Images/<SYS>/<Title>.png` (each with stub
   content) if missing.
5. Pad each system up to 20 entries with `<SYS> Filler <N>` so total ROM
   count lands near 300.
6. Create two app paks:
   - `Apps/HelloApp.pak/launch.sh` (prints "Hello from a Jawaka mock pak!"),
     `chmod +x`
   - `Apps/HelloApp.pak/pak.json` containing
     `{ "name": "Hello App", "icon": "", "platform": "mac", "pak_version": "0.0.1", "min_jawaka_version": "0.0.1" }`
   - `Apps/Tools.pak/launch.sh` + `pak.json` ("Tools placeholder")
7. Log `mockgen: generated N fake ROMs across M systems in <ROOT>`.

Embedded system table (subset of §4.5 — these are the ones that get content
generated; full table lands in Phase 2 discovery code):

```
GB:zip:Tetris|Super Mario Land|Pokemon Red|Kirby's Dream Land|Metroid II|Wario Land|Final Fantasy Adventure
GBA:zip:Advance Wars|Golden Sun|Mario Kart Super Circuit|Metroid Fusion|Pokemon Emerald|Castlevania Aria of Sorrow|F-Zero Maximum Velocity
GBC:zip:Pokemon Crystal|Zelda Oracle of Ages|Wario Land 3|Dragon Warrior Monsters|Survival Kids
SFC:zip:Super Mario World|Chrono Trigger|F-Zero|Earthbound|Super Metroid|Final Fantasy VI|Star Fox
FC:zip:Super Mario Bros 3|Metroid|Castlevania|Mega Man 2|Contra|Kirby's Adventure|Final Fantasy
MD:zip:Sonic the Hedgehog|Streets of Rage 2|Gunstar Heroes|Phantasy Star IV|Shining Force|Rocket Knight Adventures
GG:zip:Sonic Triple Trouble|Shinobi|Columns|Wonder Boy
MS:zip:Phantasy Star|Wonder Boy III|Alex Kidd in Miracle World
PCE:zip:Bonk's Adventure|R-Type|Castlevania Rondo of Blood
NEOGEO:zip:Metal Slug|King of Fighters 98|Samurai Shodown II|Garou Mark of the Wolves
LYNX:zip:California Games|Blue Lightning|Chip's Challenge
PS:chd:Castlevania Symphony of the Night|Metal Gear Solid|Final Fantasy VII|Chrono Cross|Silent Hill|Resident Evil 2|Ridge Racer Type 4
NDS:nds:Mario Kart DS|Castlevania Dawn of Sorrow|Advance Wars Dual Strike|Phoenix Wright Ace Attorney
ARCADE:zip:1942|Street Fighter II|Final Fight|Cadillacs and Dinosaurs|Sunset Riders
PORTS:sh:DOOM|Quake|OpenLara
```

The agent should not include filler titles for `PORTS` (those are real
shell scripts later) — pad PORTS only to 5 entries, not 20.

---

## 9. `third_party/README.md`

Body:

> ## Catastrophe submodule
>
> Once Jawaka has a git origin configured, add Catastrophe as a submodule:
>
> ```sh
> git submodule add <CATASTROPHE_REPO_URL> third_party/catastrophe
> git submodule update --init --recursive
> ```
>
> Until then, developers should point the Makefile at a local checkout by
> exporting `CATASTROPHE_DIR=/path/to/Catastrophe` before invoking `make`.
> Phase 0+1 already uses Catastrophe for minimal launcher/menu surfaces, so
> `CATASTROPHE_DIR` is part of the expected developer setup until the
> submodule can be added for real.

---

## 10. `ports/<platform>/Makefile` placeholders

Identical body for `tg5040`, `tg5050`, `my355`:

```make
# ports/<platform>/Makefile
# Cross-compile dispatcher for the <platform> target.
# Phase 6/7 will replace this with a Docker-toolchain-driven build, mirroring
# the Catastrophe approach.

.PHONY: all
all:
	@echo "ports/<platform>: cross-compile not yet implemented (Phase 6/7)."
	@exit 1
```

---

## 11. Acceptance criteria

The commit set is complete when **every** one of these holds, run from a
clean checkout on macOS:

1. `make help` lists the documented targets.
2. `make mockgen` creates `./mock-sdcard/` with at least 200 files under
   `Roms/`, a matching `Images/` tree, two `.pak` directories under `Apps/`,
   and an empty `BIOS/`.
3. `make` produces three executables in `build/bin/` with no warnings under
   `-Wall -Wextra -Wpedantic`.
4. Running `make run-daemon` results in `jawakad` launching `jawaka-launcher`
   as a child rather than requiring the launcher to be started manually.
5. The daemon log shows:
   - startup with resolved paths
   - launcher hello
   - at least one `scan-library` request
   - at least one `open-menu` request
   - menu hello
6. The `mock-sdcard/.jawaka/library.db` file exists after a daemon run and
   contains rows in `games` and `apps`, not just an empty schema.
7. Minimal Catastrophe-backed launcher and menu surfaces both open on Mac.
8. The launcher surface renders a tiny real shell, not just a blank window.
9. The menu surface renders a tiny contextual shell with actions.
10. The Mac shortcut contract works: `M` opens menu, `R` requests scan, `Q`
    requests shutdown.
11. `make clean && make` works from a clean state.
12. `make tg5040` exits non-zero with the friendly "not yet implemented" message
   (same for `tg5050`, `my355`).
13. `git status` shows no tracked changes inside `mock-sdcard/` or `build/`.

---

## 12. Verification commands (copy-paste)

```sh
cd /Volumes/Storage/UMRK/Jawaka

# clean
rm -rf build mock-sdcard

# build
make mockgen
make 2>&1 | tee /tmp/jawaka-build.log
test -x build/bin/jawakad
test -x build/bin/jawaka-launcher
test -x build/bin/jawaka-menu

# run + verify schema/data
( build/bin/jawakad & echo $! > /tmp/jawakad.pid ) 2> /tmp/jawakad.stderr
wait "$(cat /tmp/jawakad.pid)" 2>/dev/null || true

grep 'launcher hello' /tmp/jawakad.stderr
grep 'scan-library'   /tmp/jawakad.stderr
grep 'open-menu'      /tmp/jawakad.stderr
grep 'menu hello'     /tmp/jawakad.stderr
sqlite3 mock-sdcard/.jawaka/library.db 'select count(*) from games;'
sqlite3 mock-sdcard/.jawaka/library.db 'select count(*) from apps;'

# ports stub
make tg5040 || echo "stub exited non-zero (expected)"
```

The agent should run these as a self-check before declaring the commit set
done.

---

## 13. Explicitly out of scope (do not implement)

The agent must resist scope creep into any of these — they belong in Phase 2
or later:

- Full-featured Catastrophe widgets or polished launcher UI
- Advanced JSON/RPC layering beyond the small Phase 0+1 command set
- Scraper/downloader logic
- Rich metadata parsing beyond the initial mock-tree scan
- Advanced supervision, restart policy, crash recovery
- The full system alias table as a committed file
- `tg5040`/`tg5050`/`my355` actual cross-compile flow
- Logs to a file (stderr only)
- Complex background job machinery
- Schema migrations (just `PRAGMA user_version = 1`, no upgrade path yet)
- Rich search UX beyond proving indexed data exists and can be queried later
- `internal/metadata/` and `internal/search/` source files

---

## 14. Conventions for the implementation agent

- Make all parallel-safe Write calls in parallel where possible. There are
  ~20 files; group them by directory and write in batches.
- Use `Bash` only to (a) verify directory structure, (b) run the
  verification commands in §12, (c) `chmod +x` shell scripts where Write
  cannot set the bit.
- Do not introduce a TaskList unless the agent finds itself losing track.
  The work is bounded and the file list in §3 is the natural checklist.
- If something in this plan conflicts with `jawaka.md` or
  `jawaka-architecture-decisions.md`, the decisions document wins, then
  `jawaka.md`, then this plan. If a conflict is unresolvable, stop and ask.
- When running the verification flow, `sleep 0.3` after starting the daemon
  is sufficient on a developer machine. If the agent hits race conditions,
  prefer `wait`-on-PID or a short retry loop over longer sleeps.
- Don't add error handling for impossible paths (e.g. checking
  `malloc` return on a 16-byte allocation in main). Trust C runtime
  guarantees; validate only at external boundaries (socket, fs, sqlite).
- Don't add comments that re-state what well-named code already says. A
  short comment on `db.c`'s embedded schema explaining the
  "not duplicated until Phase 2" relationship is welcome.

---

## 15. References

- `/Volumes/Storage/UMRK/plans/jawaka.md` — canonical phased plan
- `/Users/kevinvranken/Downloads/jawaka-architecture-decisions.md` — binding
  decisions
- `/Volumes/Storage/UMRK/plans/workspace-overview.md` — wider UMRK context
- `/Volumes/Storage/UMRK/Catastrophe/` — the UI toolkit Jawaka will later
  depend on; useful style/convention reference for naming and Makefile shape
- `/Volumes/Storage/GitHub/Allium/` — original architecture inspiration; the
  `alliumd` + `allium-launcher` + `allium-menu` crate split is the model
- `/Volumes/downloads/done-set-three_202501/done-set-three_202501/done-set-three_202501/Roms/`
  — the SD-card layout this commit set targets compatibility with
