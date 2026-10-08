# ROM visibility

**Hide Game** in a game's **Options** menu removes that source/path identity
from ordinary browsing. **Settings > Games > Hidden Games** lets you restore
it, including when every game is hidden. ROMs, favorites, history, overrides,
saves and artwork remain in place.

The library stores hidden identities in schema v7's `hidden_roms` table. An
empty `member` identifies the physical launch path. Non-empty member keys are
reserved for the separate disc-visibility change; game Hide/Unhide leaves them
alone. Scans retain preferences even when a file disappears. Relocation moves
the exact file's keys in its existing transaction. A missing library row still
appears in Hidden Games by its stored path so you can clear its preference.

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

## Checks

- `make schema-v6-test visibility-test relocation-test` covers migration,
  visibility queries, metadata retention, rescans and relocation.
- `make visibility-ui-test settings-status-test game-switcher-test` exercises
  the real launcher/settings/menu handlers with disposable databases and SDL's
  dummy renderer, including an empty library and independent B Resume.

Disc management and permanent deletion are separate changes in the workspace
plan at `umrk-workspace/plans/Jawaka/rom-visibility-and-deletion.md`.
