#ifndef JW_LAUNCHER_GAME_SWITCHER_H
#define JW_LAUNCHER_GAME_SWITCHER_H

#include "internal/db/db.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

/* JW_MAX_RECENTS (64, defined in the launcher) plus one slot for the in-game
   current-game injection that may not have reached Recents yet. */
#define JW_GAME_SWITCHER_MAX 65

/* Shared recents/resume carousel used by both the launcher (Select opens a
   dedicated switcher view) and the in-game overlay (Menu + Select pauses the
   game and overlays the same carousel). The module owns the entry list,
   selection, and slide animation, and renders the carousel art/title/system
   into a caller-provided content rect. The caller owns the backdrop (launcher:
   clear + status bar; in-game: paused still + scrim) and the footer hints,
   which differ per context. */
typedef struct {
    jw_game_entry entries[JW_GAME_SWITCHER_MAX];
    int           count;
    int           cursor;          /* logical selected index */
    bool          in_game;         /* true = in-game overlay, false = launcher view */
    int           current_index;   /* index of the running game, or -1 when none */
    char          sdcard_root[PATH_MAX];
    char          states_dir[PATH_MAX]; /* RetroArch States/ root for thumbnails */
    /* Carousel slide animation (logical cursor lerps to the new selection). */
    float         anim_from_visual;
    int           anim_to_cursor;
    uint32_t      anim_start_ms;
    bool          anim_active;
} jw_game_switcher;

/* Reset to an empty carousel. in_game selects the overlay footer/behavior;
   sdcard_root resolves relative cover-art paths; states_dir (may be NULL/empty)
   is the RetroArch States/ root searched for savestate thumbnails. */
void jw_game_switcher_reset(jw_game_switcher *sw, bool in_game,
                            const char *sdcard_root, const char *states_dir);

/* Load the recents list from the library DB (most-recently-played first).
   Returns 0 on success. Safe to call repeatedly to refresh after a removal. */
int  jw_game_switcher_load(jw_game_switcher *sw, const char *db_path);

/* Inject the currently running game (in-game overlay only) and start the
   carousel on it. If a recents entry already matches (same system + ROM file)
   that entry becomes the current one; otherwise a synthetic entry is inserted
   at the front. The current entry carries id < 0 so Y removal is refused. */
void jw_game_switcher_set_current(jw_game_switcher *sw, const char *system,
                                  const char *rom_path, const char *name,
                                  const char *image_path);

/* Prefer a savestate thumbnail over the cover for any entry that has one. Uses
   the resolved storage sources when available so recents from secondary cards
   search their own States/ root first. Call after load (and set_current) so
   every entry is considered. */
void jw_game_switcher_resolve_thumbnails(jw_game_switcher *sw);

/* Move the selection by delta with wrap-around, animating the slide. */
void jw_game_switcher_move(jw_game_switcher *sw, int delta);

/* The selected entry, or NULL when the carousel is empty. */
const jw_game_entry *jw_game_switcher_selected(const jw_game_switcher *sw);

/* True when the selection is the running game (in-game overlay). */
bool jw_game_switcher_selected_is_current(const jw_game_switcher *sw);

bool jw_game_switcher_is_empty(const jw_game_switcher *sw);

/* Drop the selected entry from the in-memory list after its Recents row was
   deleted by the caller. Refuses (returns false) when the selection is the
   running game. Keeps the cursor and current_index in range. */
bool jw_game_switcher_remove_selected(jw_game_switcher *sw);

/* Draw the carousel (cover art + title + system) within the content rect.
   Renders a quiet centered empty state when there are no entries, and requests
   an animation frame while a slide is in flight. */
void jw_game_switcher_render(jw_game_switcher *sw, int x, int y, int w, int h);

#endif /* JW_LAUNCHER_GAME_SWITCHER_H */
