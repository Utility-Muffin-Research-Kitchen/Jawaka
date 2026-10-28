#ifndef JW_GAME_FOLDERS_H
#define JW_GAME_FOLDERS_H

#include <stdbool.h>
#include <stddef.h>

/* Show Folders: one system's loaded game list presented as the folder tree its
   ROMs sit in. Pure model, no SDL and no DB: the launcher hands in each game's
   rom_relpath ("<physical system folder>/<subpath>/<file>") and asks for the
   rows of one level at a time.

   The tree is a function of its input alone, so the result never depends on
   card order or scan order:
     1. Merge. A folder is its path below the system folder, compared without
        ASCII case, so the same folder on two cards or under an alias system
        folder is one folder. It shows the spelling that sorts first by bytes.
     2. Collapse. Every merged folder is judged once, before any collapse:
        - a folder-marker game (EasyRPG RPG_RT.ldb) is listed in the parent of
          its marker folder, which never becomes a folder of its own;
        - a folder holding exactly one game directly and no subfolder collapses
          when that game's file name equals the folder name once both are
          normalized: drop the file extension, strip trailing "(...)" and
          "[...]" groups, ignore ASCII case, collapse runs of spaces, trim.
          A marker game is named by its marker folder instead, so that
          folder's name (no extension to drop) is what must match.
          An empty normalized name never matches.
     3. Promote one level. A collapsed folder's game is listed in its parent;
        the parent is never re-judged, so nothing cascades.

   Rows of a level: its uncollapsed subfolders first, ordered by name without
   ASCII case (bytes break ties), then every game listed in that level in input
   order. The root is folder 0 and is never collapsed. */

/* Whether the file <rel_subpath>/<filename> under its system folder is a
   folder-marker game. rel_subpath is "" for a game at the system root. */
typedef bool (*jw_game_folder_marker_fn)(void *userdata, const char *rel_subpath,
                                         const char *filename);

/* The rom_relpath of input game i, 0 <= i < count. */
typedef const char *(*jw_game_folder_relpath_fn)(void *userdata, int i);

typedef struct jw_game_folder_tree jw_game_folder_tree;

typedef enum {
    JW_GAME_FOLDER_ROW_GAME = 0,
    JW_GAME_FOLDER_ROW_FOLDER,
} jw_game_folder_row_kind;

typedef struct {
    jw_game_folder_row_kind kind;
    int index;   /* GAME: input game index. FOLDER: folder id. */
} jw_game_folder_row;

/* Build the tree for count games. is_marker may be NULL (no marker games).
   Returns NULL on allocation failure or bad arguments; count 0 is a valid,
   empty tree. Costs O(n log n) in the number of games. */
jw_game_folder_tree *jw_game_folder_tree_build(int count,
                                               jw_game_folder_relpath_fn relpath,
                                               jw_game_folder_marker_fn is_marker,
                                               void *userdata);
void jw_game_folder_tree_free(jw_game_folder_tree *tree);

/* Folder ids run 0..folder_count-1, collapsed folders included; 0 is the root. */
int  jw_game_folder_count(const jw_game_folder_tree *tree);
int  jw_game_folder_parent(const jw_game_folder_tree *tree, int folder);     /* -1 for the root */
/* Display spelling of the last path segment ("" for the root) and of the whole
   path below the system folder, segments joined by '/'. Valid until free. */
const char *jw_game_folder_name(const jw_game_folder_tree *tree, int folder);
const char *jw_game_folder_path(const jw_game_folder_tree *tree, int folder);
/* Games anywhere below the folder, collapsed descendants included. */
int  jw_game_folder_game_count(const jw_game_folder_tree *tree, int folder);
/* The input index of a collapsed folder's game, or -1 when not collapsed. */
int  jw_game_folder_collapsed_game(const jw_game_folder_tree *tree, int folder);
/* The folder whose rows list input game i, after markers and promotion. */
int  jw_game_folder_of_game(const jw_game_folder_tree *tree, int game);

/* The folder at path, compared without ASCII case, collapsed or not; "" is the
   root. -1 when no folder has that path. */
int  jw_game_folder_lookup(const jw_game_folder_tree *tree, const char *path);
/* The deepest uncollapsed folder at or above path: the folder itself when it
   still exists uncollapsed, otherwise its nearest surviving ancestor, 0 at worst.
   Used by B, resume and reloads. */
int  jw_game_folder_resolve(const jw_game_folder_tree *tree, const char *path);

/* Rows of an uncollapsed folder. Writes up to max rows to out (out may be NULL
   when max is 0) and returns the total row count, or -1 for a collapsed or
   unknown folder. */
int  jw_game_folder_rows(const jw_game_folder_tree *tree, int folder,
                         jw_game_folder_row *out, int max);

/* The row in folder's rows that stands for child_path once you go up from it.
   child_path may lie any depth below folder; what counts is its first segment
   below folder: that subfolder's row, or its game when it collapsed. -1 when
   child_path is not below folder or that segment is no longer a row. */
int  jw_game_folder_row_for_child(const jw_game_folder_tree *tree, int folder,
                                  const char *child_path);

#endif
