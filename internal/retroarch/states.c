#include "internal/retroarch/states.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static const char *jw__states_basename(const char *path) {
    const char *slash = path ? strrchr(path, '/') : NULL;
    return slash && slash[1] ? slash + 1 : (path ? path : "");
}

static void jw__states_rom_stem(const char *rom_path, char *out, size_t out_size) {
    snprintf(out, out_size, "%s", jw__states_basename(rom_path));
    char *dot = strrchr(out, '.');
    if (dot) {
        *dot = '\0';
    }
}

static bool jw__states_is_regular(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool jw__states_try(const char *dir, const char *name,
                           char *out, size_t out_size) {
    return snprintf(out, out_size, "%s/%s", dir, name) < (int)out_size &&
           jw__states_is_regular(out);
}

bool jw_ra_find_slot_thumb(const char *states_dir, const char *rom_path,
                           int slot, char *out, size_t out_size) {
    if (!states_dir || !states_dir[0] || !rom_path || !rom_path[0] ||
        !out || out_size == 0) {
        return false;
    }

    char stem[512];
    jw__states_rom_stem(rom_path, stem, sizeof(stem));
    char name[576];
    if (slot < 0) {
        snprintf(name, sizeof(name), "%s.state.auto.png", stem);
    } else if (slot == 0) {
        snprintf(name, sizeof(name), "%s.state.png", stem);
    } else {
        snprintf(name, sizeof(name), "%s.state%d.png", stem, slot);
    }

    /* Flat layout first. */
    if (jw__states_try(states_dir, name, out, out_size)) {
        return true;
    }

    /* Per-core subfolders. */
    DIR *d = opendir(states_dir);
    if (!d) {
        return false;
    }
    struct dirent *ent;
    bool found = false;
    while (!found && (ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        char sub[PATH_MAX];
        if (snprintf(sub, sizeof(sub), "%s/%s", states_dir, ent->d_name) >=
                (int)sizeof(sub)) {
            continue;
        }
        struct stat st;
        if (stat(sub, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }
        if (jw__states_try(sub, name, out, out_size)) {
            found = true;
        }
    }
    closedir(d);
    return found;
}

/* ── Thumbnail index ──────────────────────────────────────────────────────── */

struct jw_state_thumb_index {
    char **paths;
    int    count;
    int    cap;
};

/* Accept only savestate thumbnails: a name ending in ".png" that contains
   ".state" (so cover art / box art in the same tree is ignored). */
static bool jw__states_is_thumb_name(const char *name) {
    size_t n = strlen(name);
    if (n < sizeof(".state.png") - 1) {
        return false;
    }
    if (strcmp(name + n - 4, ".png") != 0) {
        return false;
    }
    return strstr(name, ".state") != NULL;
}

static void jw__index_add(jw_state_thumb_index *idx, const char *dir, const char *name) {
    if (!jw__states_is_thumb_name(name)) {
        return;
    }
    char full[PATH_MAX];
    if (snprintf(full, sizeof(full), "%s/%s", dir, name) >= (int)sizeof(full)) {
        return;
    }
    if (idx->count == idx->cap) {
        int ncap = idx->cap ? idx->cap * 2 : 32;
        char **np = realloc(idx->paths, (size_t)ncap * sizeof(*np));
        if (!np) {
            return;
        }
        idx->paths = np;
        idx->cap = ncap;
    }
    char *dup = strdup(full);
    if (dup) {
        idx->paths[idx->count++] = dup;
    }
}

static void jw__index_scan_dir(jw_state_thumb_index *idx, const char *dir, bool recurse) {
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        char sub[PATH_MAX];
        if (snprintf(sub, sizeof(sub), "%s/%s", dir, ent->d_name) >= (int)sizeof(sub)) {
            continue;
        }
        struct stat st;
        if (stat(sub, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (recurse) {
                jw__index_scan_dir(idx, sub, false); /* one level of subfolders */
            }
        } else if (S_ISREG(st.st_mode)) {
            jw__index_add(idx, dir, ent->d_name);
        }
    }
    closedir(d);
}

jw_state_thumb_index *jw_state_thumb_index_build(const char *states_dir) {
    if (!states_dir || !states_dir[0]) {
        return NULL;
    }
    jw_state_thumb_index *idx = calloc(1, sizeof(*idx));
    if (!idx) {
        return NULL;
    }
    jw__index_scan_dir(idx, states_dir, true);
    return idx;
}

void jw_state_thumb_index_free(jw_state_thumb_index *idx) {
    if (!idx) {
        return;
    }
    for (int i = 0; i < idx->count; i++) {
        free(idx->paths[i]);
    }
    free(idx->paths);
    free(idx);
}

/* True when `rest` (the part of a filename after "<stem>.state") is a valid slot
   suffix: ".png" (slot 0), ".auto.png" (auto), or "<digits>.png" (slot N). This
   stops "<stem>" from matching a different ROM that merely shares the prefix. */
static bool jw__states_rest_is_slot(const char *rest) {
    if (strcmp(rest, ".png") == 0 || strcmp(rest, ".auto.png") == 0) {
        return true;
    }
    if (*rest < '0' || *rest > '9') {
        return false;
    }
    while (*rest >= '0' && *rest <= '9') {
        rest++;
    }
    return strcmp(rest, ".png") == 0;
}

bool jw_state_thumb_index_find(const jw_state_thumb_index *idx,
                               const char *rom_path, char *out, size_t out_size) {
    if (!idx || !rom_path || !rom_path[0] || !out || out_size == 0) {
        return false;
    }
    char stem[512];
    jw__states_rom_stem(rom_path, stem, sizeof(stem));
    char prefix[576];
    int plen = snprintf(prefix, sizeof(prefix), "%s.state", stem);
    if (plen <= (int)sizeof(".state") - 1 || plen >= (int)sizeof(prefix)) {
        return false; /* empty stem */
    }

    const char *best = NULL;
    for (int i = 0; i < idx->count; i++) {
        const char *name = jw__states_basename(idx->paths[i]);
        if (strncmp(name, prefix, (size_t)plen) != 0) {
            continue;
        }
        const char *rest = name + plen;
        if (!jw__states_rest_is_slot(rest)) {
            continue;
        }
        best = idx->paths[i];
        if (strcmp(rest, ".auto.png") == 0) {
            break; /* prefer the autosave thumbnail */
        }
    }
    if (!best) {
        return false;
    }
    snprintf(out, out_size, "%s", best);
    return true;
}
