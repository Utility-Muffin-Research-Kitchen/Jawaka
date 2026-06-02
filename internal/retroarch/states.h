#ifndef JW_RETROARCH_STATES_H
#define JW_RETROARCH_STATES_H

#include <stdbool.h>
#include <stddef.h>

/* RetroArch savestate thumbnail lookup.

   RetroArch writes a PNG beside each savestate (e.g. <rom>.state1.png, and
   <rom>.state.auto.png for the autosave) and may sort states into per-core
   subfolders under the States/ root (States/<Core Name>/<rom>.state1.png). The
   in-game menu's Save/Load preview and the recents/resume switcher carousel
   both surface these, so the lookup lives here as a shared helper. */

/* Slot-specific lookup, for callers that know the exact slot (the in-game
   Save/Load preview). slot < 0 = the auto slot, 0 = ".state", N = ".stateN".
   Searches states_dir flat, then one level of per-core subfolders. Writes an
   existing path to out and returns true, else false. */
bool jw_ra_find_slot_thumb(const char *states_dir, const char *rom_path,
                           int slot, char *out, size_t out_size);

/* Thumbnail index: scans a States/ root once (flat + one level of subfolders)
   so the carousel can look up many ROMs without re-walking the tree per game. */
typedef struct jw_state_thumb_index jw_state_thumb_index;

jw_state_thumb_index *jw_state_thumb_index_build(const char *states_dir);
void jw_state_thumb_index_free(jw_state_thumb_index *idx);

/* Find any savestate thumbnail for the ROM (matched by filename stem; the slot
   is unknown). Prefers the auto slot. Writes the path to out and returns true
   when one exists. */
bool jw_state_thumb_index_find(const jw_state_thumb_index *idx,
                               const char *rom_path, char *out, size_t out_size);

#endif /* JW_RETROARCH_STATES_H */
