#include "internal/launcher/game_folders.h"

#include "internal/core/title_order.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* One merged folder. Ids are handed out as the first game reaching a folder
   walks down from the root, so a parent's id is always below its children's:
   every bottom-up or top-down pass is a plain loop over the ids. */
typedef struct {
    int parent;
    int depth;
    uint32_t hash;        /* of (parent, case-folded name), for the child table */
    size_t name_off;      /* into the build arena, then into tree->text */
    size_t name_len;
    size_t path_len;
    const char *name;
    const char *path;
    int direct;           /* games whose own folder this is, before promotion */
    int children;         /* subfolders, before any collapse */
    int last_direct;      /* the direct game when direct == 1 */
    int game_count;
    int collapsed_game;
    int row_start;
    int row_count;        /* -1 when collapsed */
    int row_fill;
    int row_in_parent;    /* -1 for the root and collapsed folders */
    int rank;             /* 0 when not pinned */
} jw__folder;

struct jw_game_folder_tree {
    jw__folder *folders;
    int folder_count;
    int folder_cap;
    int game_total;
    int *game_folder;     /* of_game */
    int *game_row;        /* the game's row index within of_game's rows */
    int *game_rank;       /* NULL when no game can be pinned */
    jw_game_folder_row *rows;
    /* Open-addressed child table keyed on (parent, case-folded name), so a path
       resolves one segment at a time without scanning siblings. */
    int *slots;
    size_t slot_mask;
    char *text;           /* every name and path, NUL-terminated */
};

typedef struct {
    jw_game_folder_tree *tree;
    char *names;          /* folder names while building; spelling updated in place */
    size_t names_len;
    size_t names_cap;
    char *scratch;
    size_t scratch_cap;
    int *home;            /* each game's folder before promotion */
    bool *marker;         /* game i is a folder-marker game */
} jw__build;

static unsigned char jw__fold(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c - 'A' + 'a') : c;
}

static bool jw__fold_equal(const char *a, const char *b, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (jw__fold((unsigned char)a[i]) != jw__fold((unsigned char)b[i])) {
            return false;
        }
    }
    return true;
}

/* The next non-empty '/'-separated segment at or after *cursor. Empty segments
   ("a//b", a leading or trailing '/') are skipped rather than trusted. */
static bool jw__next_segment(const char **cursor, const char **seg, size_t *len) {
    const char *p = *cursor;
    while (*p == '/') {
        p++;
    }
    if (!*p) {
        *cursor = p;
        return false;
    }
    const char *start = p;
    while (*p && *p != '/') {
        p++;
    }
    *seg = start;
    *len = (size_t)(p - start);
    *cursor = p;
    return true;
}

static uint32_t jw__child_hash(int parent, const char *seg, size_t len) {
    uint32_t hash = 2166136261u;
    uint32_t id = (uint32_t)parent;
    for (int i = 0; i < 4; i++) {
        hash ^= (id >> (i * 8)) & 0xffu;
        hash *= 16777619u;
    }
    for (size_t i = 0; i < len; i++) {
        hash ^= jw__fold((unsigned char)seg[i]);
        hash *= 16777619u;
    }
    /* FNV's low bits are weak and the table is indexed by them. */
    return hash ^ (hash >> 16);
}

/* The slot holding parent's child named seg, or the empty slot where it
   belongs. names is the arena the folders' name_off point into. */
static size_t jw__child_slot(const jw_game_folder_tree *tree, const char *names,
                             int parent, const char *seg, size_t len,
                             uint32_t hash) {
    size_t slot = hash & tree->slot_mask;
    while (tree->slots[slot] >= 0) {
        const jw__folder *f = &tree->folders[tree->slots[slot]];
        if (f->hash == hash && f->parent == parent && f->name_len == len &&
            jw__fold_equal(names + f->name_off, seg, len)) {
            break;
        }
        slot = (slot + 1) & tree->slot_mask;
    }
    return slot;
}

static int jw__find_child(const jw_game_folder_tree *tree, int parent,
                          const char *seg, size_t len) {
    uint32_t hash = jw__child_hash(parent, seg, len);
    return tree->slots[jw__child_slot(tree, tree->text, parent, seg, len, hash)];
}

static bool jw__grow_slots(jw_game_folder_tree *tree, size_t capacity) {
    int *slots = malloc(capacity * sizeof(*slots));
    if (!slots) {
        return false;
    }
    for (size_t i = 0; i < capacity; i++) {
        slots[i] = -1;
    }
    size_t mask = capacity - 1;
    for (int id = 1; id < tree->folder_count; id++) {
        size_t slot = tree->folders[id].hash & mask;
        while (slots[slot] >= 0) {
            slot = (slot + 1) & mask;
        }
        slots[slot] = id;
    }
    free(tree->slots);
    tree->slots = slots;
    tree->slot_mask = mask;
    return true;
}

static bool jw__reserve(void **buffer, size_t *cap, size_t need, size_t item) {
    if (need <= *cap) {
        return true;
    }
    size_t next = *cap ? *cap : 64;
    while (next < need) {
        if (next > SIZE_MAX / 2 / item) {
            return false;
        }
        next *= 2;
    }
    void *grown = realloc(*buffer, next * item);
    if (!grown) {
        return false;
    }
    *buffer = grown;
    *cap = next;
    return true;
}

static int jw__new_folder(jw__build *b, int parent) {
    jw_game_folder_tree *tree = b->tree;
    if (tree->folder_count == INT_MAX) {
        return -1;
    }
    size_t cap = (size_t)tree->folder_cap;
    if (!jw__reserve((void **)&tree->folders, &cap,
                     (size_t)tree->folder_count + 1, sizeof(*tree->folders))) {
        return -1;
    }
    tree->folder_cap = cap > INT_MAX ? INT_MAX : (int)cap;
    int id = tree->folder_count++;
    jw__folder *f = &tree->folders[id];
    memset(f, 0, sizeof(*f));
    f->parent = parent;
    f->depth = parent >= 0 ? tree->folders[parent].depth + 1 : 0;
    f->last_direct = -1;
    f->collapsed_game = -1;
    f->row_in_parent = -1;
    return id;
}

/* parent's child named seg, created when this is the first game below it. A
   folder shows the byte-smallest spelling seen, so every merge keeps the
   smaller one; case-folded equal ASCII names always have equal lengths. */
static int jw__child(jw__build *b, int parent, const char *seg, size_t len) {
    jw_game_folder_tree *tree = b->tree;
    if ((size_t)tree->folder_count + 1 > (tree->slot_mask + 1) / 2) {
        if (tree->slot_mask + 1 > SIZE_MAX / 2 / sizeof(int) ||
            !jw__grow_slots(tree, (tree->slot_mask + 1) * 2)) {
            return -1;
        }
    }
    uint32_t hash = jw__child_hash(parent, seg, len);
    size_t slot = jw__child_slot(tree, b->names, parent, seg, len, hash);
    if (tree->slots[slot] >= 0) {
        jw__folder *f = &tree->folders[tree->slots[slot]];
        if (memcmp(seg, b->names + f->name_off, len) < 0) {
            memcpy(b->names + f->name_off, seg, len);
        }
        return tree->slots[slot];
    }

    if (len > SIZE_MAX - b->names_len - 1 ||
        !jw__reserve((void **)&b->names, &b->names_cap, b->names_len + len + 1, 1)) {
        return -1;
    }
    int id = jw__new_folder(b, parent);
    if (id < 0) {
        return -1;
    }
    jw__folder *f = &tree->folders[id];
    f->hash = hash;
    f->name_off = b->names_len;
    f->name_len = len;
    memcpy(b->names + b->names_len, seg, len);
    b->names[b->names_len + len] = '\0';
    b->names_len += len + 1;
    tree->slots[slot] = id;
    return id;
}

/* Comparable form of a file or folder name: drop the extension (files only),
   strip trailing "(...)" and "[...]" groups, collapse spaces, trim and fold
   case. out holds len + 1 bytes. Returns the length written. */
static size_t jw__normalize(const char *name, size_t len, bool drop_extension,
                            char *out) {
    if (drop_extension) {
        for (size_t i = len; i > 0; i--) {
            if (name[i - 1] == '.') {
                len = i - 1;
                break;
            }
        }
    }
    for (;;) {
        while (len > 0 && name[len - 1] == ' ') {
            len--;
        }
        if (len == 0) {
            break;
        }
        char close = name[len - 1];
        char open = close == ')' ? '(' : close == ']' ? '[' : '\0';
        if (!open) {
            break;
        }
        /* Remove back to the bracket that opens this group, nesting included;
           an unbalanced group is part of the name. */
        size_t depth = 0;
        size_t at = len;
        bool matched = false;
        while (at > 0) {
            at--;
            if (name[at] == close) {
                depth++;
            } else if (name[at] == open && --depth == 0) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            break;
        }
        len = at;
    }
    size_t n = 0;
    bool space = false;
    for (size_t i = 0; i < len; i++) {
        if (name[i] == ' ') {
            space = n > 0;
            continue;
        }
        if (space) {
            out[n++] = ' ';
            space = false;
        }
        out[n++] = (char)jw__fold((unsigned char)name[i]);
    }
    out[n] = '\0';
    return n;
}

static bool jw__scratch(jw__build *b, size_t need) {
    return jw__reserve((void **)&b->scratch, &b->scratch_cap, need, 1);
}

/* Place game i: its folder below the system folder, minus the marker folder
   for a folder-marker game. The relpath is copied first so a callback that
   reuses one buffer cannot change it under the walk. */
static bool jw__place_game(jw__build *b, int i, jw_game_folder_relpath_fn relpath,
                           jw_game_folder_marker_fn is_marker, void *userdata) {
    const char *rel = relpath(userdata, i);
    if (!rel) {
        rel = "";
    }
    size_t len = strlen(rel);
    if (len > (SIZE_MAX - 2) / 2 || !jw__scratch(b, 2 * len + 2)) {
        return false;
    }
    char *copy = b->scratch;
    memcpy(copy, rel, len + 1);

    const char *cursor = copy;
    const char *seg = NULL;
    size_t seg_len = 0;
    size_t segments = 0;
    while (jw__next_segment(&cursor, &seg, &seg_len)) {
        segments++;
    }
    /* <system folder>/<folders...>/<file>; a lone segment is a root file. */
    size_t folders = segments >= 2 ? segments - 2 : 0;
    size_t use = folders;

    if (is_marker && folders > 0) {
        char *sub = copy + len + 1;
        size_t n = 0;
        cursor = copy;
        jw__next_segment(&cursor, &seg, &seg_len);
        for (size_t k = 0; k < folders; k++) {
            jw__next_segment(&cursor, &seg, &seg_len);
            if (k > 0) {
                sub[n++] = '/';
            }
            memcpy(sub + n, seg, seg_len);
            n += seg_len;
        }
        sub[n] = '\0';
        /* The file name goes right behind the subpath; it may need its own
           terminator when the relpath ends in '/'. */
        jw__next_segment(&cursor, &seg, &seg_len);
        char *file = sub + n + 1;
        memmove(file, seg, seg_len);
        file[seg_len] = '\0';
        if (is_marker(userdata, sub, file)) {
            use--;
            b->marker[i] = true;
        }
    }

    int node = 0;
    cursor = copy;
    jw__next_segment(&cursor, &seg, &seg_len);
    for (size_t k = 0; k < use; k++) {
        jw__next_segment(&cursor, &seg, &seg_len);
        node = jw__child(b, node, seg, seg_len);
        if (node < 0) {
            return false;
        }
    }
    b->home[i] = node;
    return true;
}

/* Whether a one-game folder is that game's own folder. The relpath is asked
   for again rather than kept for every game: only candidates need it.

   A folder-marker game is named by its marker folder, not by the marker file
   (every EasyRPG game is RPG_RT.ldb), so that folder's name is what must
   match: "My Game/My Game/RPG_RT.ldb" collapses, "RPG_RT/Ib/RPG_RT.ldb" does
   not. */
static bool jw__names_match(jw__build *b, const jw__folder *folder, int game,
                            jw_game_folder_relpath_fn relpath, void *userdata) {
    const char *rel = relpath(userdata, game);
    if (!rel) {
        return false;
    }
    size_t end = strlen(rel);
    while (end > 0 && rel[end - 1] == '/') {
        end--;
    }
    size_t start = end;
    while (start > 0 && rel[start - 1] != '/') {
        start--;
    }
    bool marker = b->marker[game];
    if (marker) {
        /* Step back over the file to the marker folder, skipping empty
           segments the way placement does. */
        end = start;
        while (end > 0 && rel[end - 1] == '/') {
            end--;
        }
        start = end;
        while (start > 0 && rel[start - 1] != '/') {
            start--;
        }
    }
    size_t file_len = end - start;
    if (file_len > SIZE_MAX - folder->name_len - 2 ||
        !jw__scratch(b, file_len + folder->name_len + 2)) {
        return false;
    }
    char *file_norm = b->scratch;
    char *folder_norm = b->scratch + file_len + 1;
    size_t a = jw__normalize(rel + start, file_len, !marker, file_norm);
    size_t c = jw__normalize(b->names + folder->name_off, folder->name_len, false,
                             folder_norm);
    return a > 0 && a == c && memcmp(file_norm, folder_norm, a) == 0;
}

/* Names and paths move from the build arena into one block that lives with
   the tree; name_off is rebased so the child table keeps working after. */
static bool jw__layout_text(jw__build *b) {
    jw_game_folder_tree *tree = b->tree;
    size_t total = 0;
    for (int id = 0; id < tree->folder_count; id++) {
        jw__folder *f = &tree->folders[id];
        if (id == 0) {
            f->path_len = 0;
        } else {
            const jw__folder *parent = &tree->folders[f->parent];
            f->path_len = parent->path_len + (f->parent ? 1 : 0) + f->name_len;
        }
        size_t need = f->name_len + f->path_len + 2;
        if (need > SIZE_MAX - total) {
            return false;
        }
        total += need;
    }
    char *text = malloc(total ? total : 1);
    if (!text) {
        return false;
    }
    size_t at = 0;
    for (int id = 0; id < tree->folder_count; id++) {
        jw__folder *f = &tree->folders[id];
        char *name = text + at;
        if (f->name_len) {
            memcpy(name, b->names + f->name_off, f->name_len);
        }
        name[f->name_len] = '\0';
        f->name_off = at;
        at += f->name_len + 1;

        char *path = text + at;
        size_t n = 0;
        if (id > 0 && f->parent > 0) {
            const jw__folder *parent = &tree->folders[f->parent];
            memcpy(path, parent->path, parent->path_len);
            n = parent->path_len;
            path[n++] = '/';
        }
        memcpy(path + n, name, f->name_len);
        path[f->path_len] = '\0';
        at += f->path_len + 1;
        f->name = name;
        f->path = path;
    }
    tree->text = text;
    return true;
}

typedef struct {
    int parent;
    int id;
    const char *name;
} jw__sibling;

static int jw__sibling_order(const void *a, const void *b) {
    const jw__sibling *x = a;
    const jw__sibling *y = b;
    if (x->parent != y->parent) {
        return x->parent < y->parent ? -1 : 1;
    }
    int order = jw_title_compare(x->name, strlen(x->name), y->name, strlen(y->name));
    if (order) {
        return order;
    }
    return x->id < y->id ? -1 : x->id > y->id;
}

/* One flat row array; each uncollapsed folder owns a slice of it. Folder rows
   are placed first (siblings sorted by name), then every game in input order,
   so a level's games keep input order whether they were direct or promoted. */
static bool jw__layout_rows(jw_game_folder_tree *tree) {
    int shown = 0;
    for (int id = 1; id < tree->folder_count; id++) {
        if (tree->folders[id].collapsed_game < 0) {
            tree->folders[tree->folders[id].parent].row_count++;
            shown++;
        }
    }
    for (int i = 0; i < tree->game_total; i++) {
        tree->folders[tree->game_folder[i]].row_count++;
    }
    int start = 0;
    for (int id = 0; id < tree->folder_count; id++) {
        jw__folder *f = &tree->folders[id];
        if (f->collapsed_game >= 0) {
            f->row_count = -1;
            continue;
        }
        f->row_start = start;
        start += f->row_count;
    }

    tree->rows = malloc((size_t)(start ? start : 1) * sizeof(*tree->rows));
    jw__sibling *siblings = malloc((size_t)(shown ? shown : 1) * sizeof(*siblings));
    if (!tree->rows || !siblings) {
        free(siblings);
        return false;
    }
    int n = 0;
    for (int id = 1; id < tree->folder_count; id++) {
        const jw__folder *f = &tree->folders[id];
        if (f->collapsed_game < 0) {
            siblings[n].parent = f->parent;
            siblings[n].id = id;
            siblings[n].name = f->name;
            n++;
        }
    }
    qsort(siblings, (size_t)n, sizeof(*siblings), jw__sibling_order);
    for (int k = 0; k < n; k++) {
        jw__folder *parent = &tree->folders[siblings[k].parent];
        int row = parent->row_fill++;
        tree->rows[parent->row_start + row].kind = JW_GAME_FOLDER_ROW_FOLDER;
        tree->rows[parent->row_start + row].index = siblings[k].id;
        tree->folders[siblings[k].id].row_in_parent = row;
    }
    free(siblings);
    for (int i = 0; i < tree->game_total; i++) {
        jw__folder *f = &tree->folders[tree->game_folder[i]];
        int row = f->row_fill++;
        tree->rows[f->row_start + row].kind = JW_GAME_FOLDER_ROW_GAME;
        tree->rows[f->row_start + row].index = i;
        tree->game_row[i] = row;
    }
    return true;
}

int jw_game_folder_row_rank(const jw_game_folder_tree *tree, jw_game_folder_row row) {
    if (!tree) {
        return 0;
    }
    if (row.kind == JW_GAME_FOLDER_ROW_FOLDER) {
        return row.index > 0 && row.index < tree->folder_count ? tree->folders[row.index].rank : 0;
    }
    return tree->game_rank && row.index >= 0 && row.index < tree->game_total ? tree->game_rank[row.index] : 0;
}

typedef struct {
    jw_game_folder_row row;
    int rank;
    int position;
} jw__ranked_row;

static int jw__ranked_order(const void *a, const void *b) {
    const jw__ranked_row *x = a;
    const jw__ranked_row *y = b;
    if (x->rank != y->rank) {
        return x->rank < y->rank ? -1 : 1;
    }
    return x->position - y->position;
}

/* Move each level's pinned rows to its top. Rows already stand folders first by
   name, then games in input order, so a row's position settles a tie in rank, and
   a level with no pins is left as it is. */
static bool jw__apply_pins(jw_game_folder_tree *tree) {
    int widest = 0;
    bool any = false;
    for (int id = 0; id < tree->folder_count; id++) {
        const jw__folder *f = &tree->folders[id];
        if (f->row_count > widest) {
            widest = f->row_count;
        }
        for (int k = 0; k < f->row_count && !any; k++) {
            any = jw_game_folder_row_rank(tree, tree->rows[f->row_start + k]) > 0;
        }
    }
    if (!any) {
        return true;
    }
    jw__ranked_row *pinned = malloc((size_t)widest * sizeof(*pinned));
    jw_game_folder_row *rest = malloc((size_t)widest * sizeof(*rest));
    if (!pinned || !rest) {
        free(pinned);
        free(rest);
        return false;
    }
    for (int id = 0; id < tree->folder_count; id++) {
        const jw__folder *f = &tree->folders[id];
        int pins = 0;
        int others = 0;
        for (int k = 0; k < f->row_count; k++) {
            jw_game_folder_row row = tree->rows[f->row_start + k];
            int rank = jw_game_folder_row_rank(tree, row);
            if (rank > 0) {
                pinned[pins].row = row;
                pinned[pins].rank = rank;
                pinned[pins].position = k;
                pins++;
            } else {
                rest[others++] = row;
            }
        }
        if (pins == 0) {
            continue;
        }
        qsort(pinned, (size_t)pins, sizeof(*pinned), jw__ranked_order);
        for (int k = 0; k < f->row_count; k++) {
            jw_game_folder_row row = k < pins ? pinned[k].row : rest[k - pins];
            tree->rows[f->row_start + k] = row;
            if (row.kind == JW_GAME_FOLDER_ROW_FOLDER) {
                tree->folders[row.index].row_in_parent = k;
            } else {
                tree->game_row[row.index] = k;
            }
        }
    }
    free(pinned);
    free(rest);
    return true;
}

jw_game_folder_tree *jw_game_folder_tree_build_ranked(int count,
                                                      jw_game_folder_relpath_fn relpath,
                                                      jw_game_folder_marker_fn is_marker,
                                                      jw_game_folder_game_rank_fn game_rank,
                                                      jw_game_folder_path_rank_fn folder_rank,
                                                      void *userdata) {
    if (count < 0 || (count > 0 && !relpath)) {
        return NULL;
    }
    jw_game_folder_tree *tree = calloc(1, sizeof(*tree));
    if (!tree) {
        return NULL;
    }
    jw__build b;
    memset(&b, 0, sizeof(b));
    b.tree = tree;
    tree->game_total = count;
    size_t games = (size_t)(count ? count : 1);
    if (game_rank && count > 0) {
        tree->game_rank = malloc(games * sizeof(*tree->game_rank));
        if (!tree->game_rank) {
            free(tree);
            return NULL;
        }
        for (int i = 0; i < count; i++) {
            tree->game_rank[i] = game_rank(userdata, i);
        }
    }
    tree->game_folder = malloc(games * sizeof(*tree->game_folder));
    tree->game_row = malloc(games * sizeof(*tree->game_row));
    b.home = malloc(games * sizeof(*b.home));
    b.marker = calloc(games, sizeof(*b.marker));
    bool ok = tree->game_folder && tree->game_row && b.home && b.marker &&
              jw__grow_slots(tree, 64) && jw__new_folder(&b, -1) == 0;

    /* 1. Merge: every game walks its folder path from the root through the
          case-folded child table, so equal paths from any card or alias
          system folder meet in the same folder. */
    for (int i = 0; ok && i < count; i++) {
        ok = jw__place_game(&b, i, relpath, is_marker, userdata);
    }

    if (ok) {
        for (int i = 0; i < count; i++) {
            jw__folder *f = &tree->folders[b.home[i]];
            f->direct++;
            f->last_direct = i;
        }
        for (int id = 1; id < tree->folder_count; id++) {
            tree->folders[tree->folders[id].parent].children++;
        }
        /* 2. Collapse, judged on the merged tree before anything moves. A
              folder exists only because some game sits at or below it, so a
              folder without subfolders always holds a game directly. */
        for (int id = 1; id < tree->folder_count; id++) {
            jw__folder *f = &tree->folders[id];
            if (f->children == 0 && f->direct == 1 &&
                jw__names_match(&b, f, f->last_direct, relpath, userdata)) {
                f->collapsed_game = f->last_direct;
            }
        }
        /* 3. Promote one level. A collapsed folder has no subfolders, so its
              parent, which has one, could never have collapsed: nothing
              cascades. */
        for (int i = 0; i < count; i++) {
            const jw__folder *f = &tree->folders[b.home[i]];
            tree->game_folder[i] = f->collapsed_game >= 0 ? f->parent : b.home[i];
        }
        for (int id = 0; id < tree->folder_count; id++) {
            tree->folders[id].game_count = tree->folders[id].direct;
        }
        for (int id = tree->folder_count - 1; id > 0; id--) {
            tree->folders[tree->folders[id].parent].game_count +=
                tree->folders[id].game_count;
        }
        ok = jw__layout_text(&b);
        if (ok && folder_rank) {
            for (int id = 1; id < tree->folder_count; id++) {
                tree->folders[id].rank = folder_rank(userdata, tree->folders[id].path);
            }
        }
        ok = ok && jw__layout_rows(tree) && jw__apply_pins(tree);
    }

    free(b.names);
    free(b.scratch);
    free(b.home);
    free(b.marker);
    if (!ok) {
        jw_game_folder_tree_free(tree);
        return NULL;
    }
    return tree;
}

jw_game_folder_tree *jw_game_folder_tree_build(int count,
                                               jw_game_folder_relpath_fn relpath,
                                               jw_game_folder_marker_fn is_marker,
                                               void *userdata) {
    return jw_game_folder_tree_build_ranked(count, relpath, is_marker, NULL, NULL, userdata);
}

void jw_game_folder_tree_free(jw_game_folder_tree *tree) {
    if (!tree) {
        return;
    }
    free(tree->game_rank);
    free(tree->folders);
    free(tree->game_folder);
    free(tree->game_row);
    free(tree->rows);
    free(tree->slots);
    free(tree->text);
    free(tree);
}

static const jw__folder *jw__folder_at(const jw_game_folder_tree *tree, int folder) {
    if (!tree || folder < 0 || folder >= tree->folder_count) {
        return NULL;
    }
    return &tree->folders[folder];
}

int jw_game_folder_count(const jw_game_folder_tree *tree) {
    return tree ? tree->folder_count : 0;
}

int jw_game_folder_parent(const jw_game_folder_tree *tree, int folder) {
    const jw__folder *f = jw__folder_at(tree, folder);
    return f ? f->parent : -1;
}

const char *jw_game_folder_name(const jw_game_folder_tree *tree, int folder) {
    const jw__folder *f = jw__folder_at(tree, folder);
    return f ? f->name : "";
}

const char *jw_game_folder_path(const jw_game_folder_tree *tree, int folder) {
    const jw__folder *f = jw__folder_at(tree, folder);
    return f ? f->path : "";
}

int jw_game_folder_game_count(const jw_game_folder_tree *tree, int folder) {
    const jw__folder *f = jw__folder_at(tree, folder);
    return f ? f->game_count : 0;
}

int jw_game_folder_collapsed_game(const jw_game_folder_tree *tree, int folder) {
    const jw__folder *f = jw__folder_at(tree, folder);
    return f ? f->collapsed_game : -1;
}

int jw_game_folder_of_game(const jw_game_folder_tree *tree, int game) {
    if (!tree || game < 0 || game >= tree->game_total) {
        return -1;
    }
    return tree->game_folder[game];
}

int jw_game_folder_lookup(const jw_game_folder_tree *tree, const char *path) {
    if (!tree || !path) {
        return -1;
    }
    int node = 0;
    const char *cursor = path;
    const char *seg = NULL;
    size_t len = 0;
    while (node >= 0 && jw__next_segment(&cursor, &seg, &len)) {
        node = jw__find_child(tree, node, seg, len);
    }
    return node;
}

int jw_game_folder_resolve(const jw_game_folder_tree *tree, const char *path) {
    if (!tree) {
        return -1;
    }
    /* Collapsed folders are leaves, so the walk stops at the first segment
       that is missing or collapsed and everything above it survives. */
    int node = 0;
    const char *cursor = path ? path : "";
    const char *seg = NULL;
    size_t len = 0;
    while (jw__next_segment(&cursor, &seg, &len)) {
        int child = jw__find_child(tree, node, seg, len);
        if (child < 0 || tree->folders[child].collapsed_game >= 0) {
            break;
        }
        node = child;
    }
    return node;
}

int jw_game_folder_rows(const jw_game_folder_tree *tree, int folder,
                        jw_game_folder_row *out, int max) {
    const jw__folder *f = jw__folder_at(tree, folder);
    if (!f || f->collapsed_game >= 0) {
        return -1;
    }
    int n = out && max > 0 ? (max < f->row_count ? max : f->row_count) : 0;
    if (n > 0) {
        memcpy(out, tree->rows + f->row_start, (size_t)n * sizeof(*out));
    }
    return f->row_count;
}

int jw_game_folder_row_for_child(const jw_game_folder_tree *tree, int folder,
                                 const char *child_path) {
    const jw__folder *f = jw__folder_at(tree, folder);
    if (!f || f->collapsed_game >= 0 || !child_path) {
        return -1;
    }
    /* Walking the folder's depth from the root lands on the folder itself
       exactly when child_path starts with its path, ignoring case. */
    int node = 0;
    const char *cursor = child_path;
    const char *seg = NULL;
    size_t len = 0;
    for (int d = 0; d < f->depth; d++) {
        if (!jw__next_segment(&cursor, &seg, &len)) {
            return -1;
        }
        node = jw__find_child(tree, node, seg, len);
        if (node < 0) {
            return -1;
        }
    }
    if (node != folder || !jw__next_segment(&cursor, &seg, &len)) {
        return -1;
    }
    int child = jw__find_child(tree, folder, seg, len);
    if (child < 0) {
        return -1;
    }
    const jw__folder *c = &tree->folders[child];
    return c->collapsed_game >= 0 ? tree->game_row[c->collapsed_game]
                                  : c->row_in_parent;
}
