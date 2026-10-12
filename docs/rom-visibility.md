# ROM visibility and game deletion

**Hide Game** in a game's **Options** menu removes that source/path identity
from ordinary browsing. **Settings > Games > Hidden Games** lets you restore
it, including when every game is hidden. ROMs, favorites, history, overrides,
saves and artwork remain in place.

The library stores hidden identities in a `hidden_roms` table added to schema
v6 without changing its version, so earlier Leaf releases and other readers
of `library.db` still open the library; they just ignore the preferences,
which return when you upgrade again. A library that a development build
stamped as v7 returns to v6 on its next writable open. A library from an
earlier release gains the table on its first writable open; on a card that is
already read-only, browsing works with nothing hidden and visibility changes
report the read-only card. Hide Game stores the game's own source/path with an
empty `member`. Scans retain preferences even when a file disappears.
Relocation moves the exact file's keys in its existing transaction. A missing
library row still appears in Hidden Games by its stored path so you can clear
its preference.

The v0.13.0 betas also had **Manage Discs**, which stored keys for individual
discs and M3U `archive.zip#member` selectors (a non-empty `member`). It was
removed in favor of whole-game Hide and Delete.
Those keys stay in the table. They no longer affect anything unless one
matches an indexed row, such as a disc the scanner lists on its own, which
then stays hidden like any game. Hidden Games lists every key, by path when
there's no row, and Unhide clears it.

## Browse query audit

| Surface | Visibility boundary |
| --- | --- |
| Systems, Games and Focus Pick | `jw_db_list_systems`, `jw_db_count_games_for_system`, `jw_db_list_games_for_system` filter before counts, ordering and limits. |
| Favorites and Recents | Both shared list queries filter before limits; their metadata remains stored. |
| Search | Both FTS and fallback searches, including pinyin candidates, exclude hidden games. App results are unchanged. |
| Game switcher | Recents use the shared query. Current-game injection checks the game's identity before adding a tile. Each in-game entry reloads the list. B Resume remains independent of selection, including an empty carousel. |
| Library summary and most-played titles | Browse counts, sample titles and ranked titles exclude hidden games. Lifetime statistics retain their historical totals. |
| Batch artwork scraping | System/game enumeration uses the shared browse queries. Explicit single-game requests still resolve by identity. |

These paths deliberately retain access to hidden games:

- `jw_db_get_game_by_id` and `jw_db_get_game_by_rom_path` serve identity lookup,
  launches, active-session metadata, and inspection.
- Focus's locked screen and Arrange resolve remembered IDs. Pick uses the
  filtered browser; its Clear action removes the remembered selection.
- Boot resume resolves its saved ROM path and can resume a hidden game.
- Scanning, relocation, legacy save migration and direct SQL consumers such as
  Central Scrutinizer retain their existing behavior.
- Daemon startup uses total library statistics to recognize a populated cache
  even when its visible game count is zero.

Hide/Unhide writes directly through the DB. The launcher refreshes its cached
lists, cursors, counts and navigation breadcrumb without changing the daemon's
library generation or requesting a scan. A read-only library returns the
existing storage result and opens the SD card warning with its repair action.

## Content inspection

`internal/discovery/content.c` inspects content on demand for Delete Game. It
reads M3U, CUE, GDI, TOC and quoted CMD references without executing commands.
It retains raw descriptor bytes, labels and directives, including `#SAVEDISK`,
and resolves references using actual filesystem lookup rules. Existing paths
use their on-disk spelling; missing referenced files keep their normalized
identity. As in RetroArch, only a `#` directly after `.zip`, `.7z` or `.apk`
starts an archive member; any other `#` is part of the filename, as in
`Game #1 (Disc 1).cue`. Archive selectors stay separate from host filename
case. Unreadable or malformed descriptors, unsafe paths and bounded inspection
limits report an error. A parse error names the innermost descriptor that
failed, so a bad file on either card can be found and fixed. Inspection
doesn't change descriptors, ROMs or scanner grouping.

## Delete Game

**Options > Delete Game** previews permanent removal of your selected game
and its exclusively owned ROM files. It stays last in the menu, after
**Hide Game**. **Hidden Games** offers the same action for eligible indexed
game rows. Unassociated hidden paths and archive members remain available to
unhide.

You see the game, source card, file count (and disc count for a playlist),
expected freed space, shared files kept, and a reminder that deletion is
permanent. **Files** opens the exact file list; **Back** returns to the
summary. **Cancel** is selected initially. Choose **Delete Game** explicitly
to confirm. Preparing the preview is cancellable, and every disc the playlist
lists is included.

Delete is offered only for the following release system IDs, provided their
current catalog formats remain supported:

| Group | System IDs and content |
| --- | --- |
| Single-file games | `32X`, `ATARI2600`, `COLECO`, `FC`, `FDS`, `GB`, `GBA`, `GBC`, `GG`, `GW`, `LYNX`, `MD`, `MS`, `N64`, `NDS`, `NGP`, `NGPC`, `PICO8`, `PSP`, `SEVENTYEIGHTHUNDRED`, `SFC`, `VB`, `VECTREX`, `WS`, `WSC`. Their accepted ZIP/7Z files are treated as whole containers. |
| Disc games | `PS` and `SEGACD`, including supported CUE, TOC and M3U layouts and their referenced tracks. |
| Command descriptors | `PC98`, with quoted CMD file arguments resolved without executing the command. |

The menu checks `jw_delete_supported()` and the catalog without inspecting
files. A content-pak provider, filename-based launch contract, unsupported
system ID or newly accepted format outside that system's supported set omits
**Delete Game**. **Hide Game** stays available. Arcade sets, AMIGA, DOS,
EasyRPG, PORTS, DC, NAOMI, ATOMISWAVE, PCE/PCECD, SATURN and MD32X are excluded.

### Shared files and preserved data

Preview preparation reads M3U/M3U8, CUE, GDI, TOC and CMD descriptors across
every mounted source's ROM tree, including hidden and scan-suppressed files.
macOS AppleDouble metadata files (`._Name` with Apple's binary header, or
empty) are skipped; a text playlist with such a name is still read.
It also includes indexed game identities as owners. Exact normalized paths,
current absolute cross-card references and filesystem identity determine
sharing. A separate game using the selected launch file blocks deletion and
is named in the error. Other shared files remain on disk. Those leftovers can
appear as standalone games or be regrouped by the existing scanner later.
The scanner still treats any `#` in a playlist entry as a member separator, so
a disc such as `Game #1 (Disc 1).cue` can be listed beside its playlist;
Delete Game on that entry is blocked because the playlist uses it.

The search scales roughly linearly with your library. It stops with an error
only past 1,000,000 ROM-tree entries, 65,536 descriptors, 131,072 referenced
files, 65,536 directories, 262,144 cached directory names or 32 MB of
descriptor text (4 MB per descriptor). These bound memory on the MLP1, where a
commit briefly holds two copies of the search.

If a known card isn't inserted, the preview names it and says its playlists
weren't checked. A card is known when your library has games on it or a
playlist on a mounted card refers to it; an empty second card slot doesn't
warn. You can
continue after the usual confirmation. A mounted card with unreadable or
malformed descriptors instead stops the operation.
There are no readers for `.uae`, `.ccd`, `.mds`, `.conf`, `.bat`, `.exe`, `.sh`
or `.dat`. Incoming references from those formats cannot be detected, such as
a PORTS script that launches a ROM in another system. This is an ownership
search limitation even though those systems can't themselves be deleted.

Your save files, memory cards, states, thumbnails and artwork are kept.
Protection includes configured data roots and indexed artwork inside ROM
directories. Deletion never expands a title stem or removes a folder
recursively. A PC98 preview warns that progress stored inside a writable
game image is erased with that image. Containers aren't unpacked or rewritten.

PlayStation's default core, PCSX ReARMed, also reads same-name `.toc`, `.ccd`,
`.mds` or `.cue` files beside a loaded `.img`, `.mdf` or `.iso`, plus `.sub`
subchannel and `.sbi` LibCrypt data. Deletion never matches title stems, so
deleting such an image leaves those sidecars in place and the preview doesn't
list them. The core also falls back to a CUE `FILE` entry's bare filename when
its path doesn't exist; deletion follows the exact path, so a track found only
that way is kept.

### Daemon mutation and retry

The launcher sends a source/path identity to jawakad, never an unlink list.
The daemon retains the reviewed plan under a one-use token on that connection.
Closing the preview or connection, restarting the daemon, or losing a response
requires a fresh preview and confirmation. Destructive requests are never
automatically replayed.

Both the ROM source and library card must be writable before preview and
commit. Read-only results open the existing SD card warning and repair flow.
Commit rebuilds and compares the mounted-source identity, normalized paths,
file existence, sizes and timestamps, descriptor bytes and references before
any removal. It coordinates with scanning, launches, relocation reservations,
unmounting and affected scrape work. A changed plan stops without applying it.

Exclusive payloads are removed before their descriptors, with directory
changes synced before removing the descriptor that records them. The selected
launch file is removed last. An error stops further removal and reports the
files removed and the failure. Deletion isn't atomic: a crash or I/O error can
leave a game with missing tracks. Reopen **Delete Game** for a fresh preview.
Already missing paths can be reconciled, but a missing descriptor can't reveal
its former tracks; unknown files stay in place. A lost commit response reports
that some files may already have been removed.

DB reconciliation removes exact deleted-file rows and their settings,
favorites, recents and Focus selections, including independently indexed disc
rows. Visibility keys are cleared only for removed identities; shared-file
choices remain. The same orphan/Focus cleanup is used by scan pruning. The
daemon publishes the library generation after reconciliation, including
partial removal, and the launcher refreshes its browse and Hidden Games caches.
Re-adding a deleted ROM doesn't recover that removed metadata.

## Checks

- `make content-test` covers descriptors, filesystem case behavior, path
  aliases, selectors, missing references and unsafe or malformed input. Run
  it on both case-sensitive and case-insensitive storage.
- `make schema-v6-test visibility-test relocation-test` covers migration, the
  unchanged v6 version (including a development v7 library), read-only
  libraries from earlier releases, visibility queries, metadata retention,
  rescans and relocation.
- `make visibility-ui-test settings-status-test game-switcher-test` exercises
  the real launcher/settings/menu handlers with disposable databases and SDL's
  dummy renderer, including empty lists, leftover beta disc keys, independent
  B Resume and the longest Options menu at a larger font size.
- `make delete-test` covers eligibility, shared ownership across cards,
  missing-card warnings, protected data, changed previews, dependency ordering
  and partial-failure retry with disposable content. It also checks a
  2,600-game disc library beyond the original search limits, AppleDouble
  sidecars, `#` in filenames and descriptor-named errors.
- `make deletion-db-test focus-test` covers exact-file reconciliation and
  transactional metadata/Focus cleanup, including later scan pruning.
- `make delete-ui-test delete-client-test` covers the real deletion screen,
  initial Cancel selection, inspectable files, Hidden Games, read-only warnings,
  retained IPC sessions, malformed previews, lost-response replay prevention
  and a quick result followed by Back.
- `make rom-delete-ipc-smoke` exercises the daemon with synthetic ROMs and its
  preview/commit protocol, storage changes, coordination and fault injection.
- `WORKSPACE_ROOT=/path/to/UMRK make phase3-fixture-scan-smoke` checks that
  existing nested-folder grouping remains unchanged. Continue running
  `make storage-sources-test relocation-test` for source and reservation changes.

Deferred format readers and system groups are tracked in
`umrk-workspace/plans/Jawaka/rom-visibility-and-deletion.md`. The planned
simplification of Delete Game is in
`umrk-workspace/plans/Jawaka/rom-deletion-simplification.md`.
