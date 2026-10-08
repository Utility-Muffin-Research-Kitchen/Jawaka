#include "internal/discovery/delete.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define JW_DELETE_MAX_DESCRIPTORS 8192
#define JW_DELETE_MAX_ENTRIES 200000

typedef struct {
    struct stat file;
    struct stat directory;
    bool missing;
    char parent[JW_STORAGE_PATH_MAX];
} jw_delete_stamp;

typedef struct {
    jw_content graph;
    jw_storage_source_list sources;
    struct stat mounts[JW_STORAGE_MAX_SOURCES];
    jw_delete_stamp *stamps;
    size_t *indices;
    size_t *order;
    size_t order_count;
    char *replacement;
    size_t replacement_size;
} jw_delete_snapshot;

typedef struct {
    const jw_storage_source_list *sources;
    const jw_ra_catalog *catalog;
    jw_content_root *roots;
    size_t root_count;
    size_t entries;
    jw_delete_cancelled cancelled;
    void *context;
    char *error;
    size_t error_size;
} jw_delete_builder;

static int jw__error(char *error, size_t size, const char *format, ...) {
    if (error && size) {
        va_list args;
        va_start(args, format);
        vsnprintf(error, size, format, args);
        va_end(args);
    }
    return -1;
}

static const char *jw__extension(const char *path) {
    const char *base = strrchr(path, '/');
    const char *dot = strrchr(base ? base + 1 : path, '.');
    return dot ? dot + 1 : "";
}

static bool jw__word(const char *list, const char *word) {
    if (!word || !*word) return false;
    if (*word == '.') word++;
    size_t n = strlen(word);
    for (const char *p = list; *p;) {
        const char *end = strchr(p, ' ');
        size_t length = end ? (size_t)(end - p) : strlen(p);
        if (length == n && !strncasecmp(p, word, n)) return true;
        p += length;
        if (*p) p++;
    }
    return false;
}

bool jw_delete_supported(const jw_ra_system *system) {
    static const struct { const char *id; const char *formats; } allowed[] = {
        {"32X", "32x 68k bin chd gen iso md smd sms"}, {"ATARI2600", "a26 bin"},
        {"COLECO", "bin col rom"}, {"FC", "fds nes unf unif"}, {"FDS", "fds nes"},
        {"GB", "bin dmg gb gbc"}, {"GBA", "bin gba"}, {"GBC", "bin dmg gb gbc"},
        {"GG", "bin gg"}, {"GW", "mgw"}, {"LYNX", "lnx"},
        {"MD", "32x 68k bin chd gen iso md smd sms"}, {"MS", "32x 68k bin chd gen iso md smd sms"},
        {"N64", "n64 v64 z64"}, {"NDS", "nds"}, {"NGP", "ngc ngp"}, {"NGPC", "ngc ngp"},
        {"PICO8", "p8 png"}, {"PSP", "chd cso iso pbp"},
        {"SEVENTYEIGHTHUNDRED", "a78 bin"}, {"SFC", "bs bsx dx2 fig gd3 gd7 sfc smc st swc"},
        {"VB", "vb vboy"}, {"VECTREX", "bin vec"}, {"WS", "pc2 ws wsc"}, {"WSC", "pc2 ws wsc"},
        {"PS", "cbn chd cue img iso mdf pbp toc m3u m3u8"},
        {"SEGACD", "chd cue iso m3u m3u8"},
        {"PC98", "2hd 88d 98d cmd d88 d98 dup fdd fdi hdd hdi hdm hdn nhd tfd thd xdf"},
    };
    if (!system || !system->id || (system->provider && *system->provider) || system->file_names.count)
        return false;
    const char *formats = NULL;
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
        if (!strcmp(system->id, allowed[i].id)) formats = allowed[i].formats;
    if (!formats) return false;
    const jw_ra_string_list *lists[] = {&system->extensions, &system->playlist_extensions,
                                         &system->archive_extensions};
    for (size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); i++)
        for (size_t j = 0; j < lists[i]->count; j++)
            if (!jw__word(formats, lists[i]->items[j]) && !jw__word("zip 7z", lists[i]->items[j])) return false;
    return true;
}

static const jw_ra_system *jw__system(const jw_ra_catalog *catalog, const char *id) {
    if (catalog && id) for (size_t i = 0; i < catalog->system_count; i++)
        if (catalog->systems[i].id && !strcmp(catalog->systems[i].id, id)) return &catalog->systems[i];
    return NULL;
}

static const jw_ra_system *jw__path_system(const jw_ra_catalog *catalog, const char *path) {
    if (!strcasecmp(jw__extension(path), "cmd")) return jw__system(catalog, "PC98");
    size_t length = strcspn(path, "/");
    for (size_t i = 0; catalog && i < catalog->system_count; i++) {
        const jw_ra_system *system = &catalog->systems[i];
        if (system->id && strlen(system->id) == length && !strncasecmp(path, system->id, length)) return system;
        for (size_t j = 0; j < system->patterns.count; j++)
            if (strlen(system->patterns.items[j]) == length && !strncasecmp(path, system->patterns.items[j], length)) return system;
    }
    return NULL;
}

static bool jw__inside(const char *path, const char *root) {
    size_t n = root ? strlen(root) : 0;
    while (n > 1 && root[n - 1] == '/') n--;
    return n && !strncmp(path, root, n) && (!path[n] || path[n] == '/');
}

static int jw__join(char *out, size_t size, const char *root, const char *name) {
    int n = snprintf(out, size, "%s/%s", root, name);
    return n < 0 || (size_t)n >= size ? -1 : 0;
}

static int jw__add_root(jw_delete_builder *b, const char *source_id, const char *path,
                        const jw_ra_system *system) {
    if (!strcasecmp(jw__extension(path), "cmd") && !system)
        return jw__error(b->error, b->error_size, "CMD ownership inspection needs the PC98 catalog formats");
    for (size_t i = 0; i < b->root_count; i++)
        if (!strcmp(b->roots[i].source_id, source_id) && !strcmp(b->roots[i].rom_relpath, path)) return 0;
    if (b->root_count >= JW_DELETE_MAX_DESCRIPTORS)
        return jw__error(b->error, b->error_size, "Too many descriptors to check safely");
    jw_content_root *roots = realloc(b->roots, (b->root_count + 1) * sizeof(*roots));
    if (!roots) return jw__error(b->error, b->error_size, "Out of memory checking ownership");
    b->roots = roots;
    char *id = strdup(source_id), *relative = strdup(path);
    if (!id || !relative) { free(id); free(relative); return jw__error(b->error, b->error_size, "Out of memory checking ownership"); }
    roots[b->root_count++] = (jw_content_root){id, relative, system, 0};
    return 0;
}

/* Walk all mounted ROM trees, including hidden and scanner-suppressed folders.
   shortcut: scripts/UAE/CCD/MDS/DAT have no reader; add their dependency semantics
   before claiming cross-system ownership coverage for those formats. */
static int jw__walk(jw_delete_builder *b, const jw_storage_source *source,
                    const char *relative, unsigned depth) {
    if (b->cancelled && b->cancelled(b->context)) return jw__error(b->error, b->error_size, "Delete preview cancelled");
    if (depth > 64) return jw__error(b->error, b->error_size, "ROM directories are nested too deeply");
    char path[JW_STORAGE_PATH_MAX];
    if (jw__join(path, sizeof(path), source->roms_path, relative))
        return jw__error(b->error, b->error_size, "ROM path is too long");
    DIR *dir = opendir(path);
    if (!dir) {
        if (!*relative && errno == ENOENT) return 0;
        return jw__error(b->error, b->error_size, "Cannot inspect ROM directory %.160s: %s", path, strerror(errno));
    }
    int result = 0;
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) { if (errno) result = jw__error(b->error, b->error_size, "Cannot read ROM directory: %s", strerror(errno)); break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (++b->entries > JW_DELETE_MAX_ENTRIES) { result = jw__error(b->error, b->error_size, "Too many ROM entries to check safely"); break; }
        if (b->cancelled && b->cancelled(b->context)) { result = jw__error(b->error, b->error_size, "Delete preview cancelled"); break; }
        char child[512], absolute[JW_STORAGE_PATH_MAX];
        int n = snprintf(child, sizeof(child), "%s%s%s", relative, *relative ? "/" : "", entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(child) || jw__join(absolute, sizeof(absolute), source->roms_path, child)) {
            result = jw__error(b->error, b->error_size, "ROM path is too long"); break;
        }
        struct stat st;
        if (lstat(absolute, &st)) { result = jw__error(b->error, b->error_size, "Cannot inspect %.160s: %s", absolute, strerror(errno)); break; }
        /* A symlink directory could hide incoming descriptors or leave this card. */
        if (S_ISLNK(st.st_mode)) {
            if (stat(absolute, &st) || S_ISDIR(st.st_mode) || jw_content_is_descriptor_path(child)) {
                result = jw__error(b->error, b->error_size, "Cannot check descriptor ownership through symlink: %.160s", child); break;
            }
            continue;
        }
        if (S_ISDIR(st.st_mode)) result = jw__walk(b, source, child, depth + 1);
        else if (jw_content_is_descriptor_path(child)) {
            if (!S_ISREG(st.st_mode)) result = jw__error(b->error, b->error_size, "Descriptor is not a regular file: %.160s", child);
            else result = jw__add_root(b, source->id, child, jw__path_system(b->catalog, child));
        }
        if (result) break;
    }
    if (closedir(dir) && !result) result = jw__error(b->error, b->error_size, "Cannot close ROM directory: %s", strerror(errno));
    return result;
}

static int jw__root_compare(const void *a, const void *b) {
    const jw_content_root *x = a, *y = b;
    int result = strcmp(x->source_id, y->source_id);
    return result ? result : strcmp(x->rom_relpath, y->rom_relpath);
}

static void jw__mark(const jw_content *graph, size_t root, bool *marked) {
    marked[root] = true;
    bool changed;
    do {
        changed = false;
        for (size_t i = 0; i < graph->reference_count; i++) {
            jw_content_reference ref = graph->references[i];
            if (marked[ref.parent] && !marked[ref.child]) { marked[ref.child] = true; changed = true; }
        }
    } while (changed);
}

static bool jw__same_stat(const struct stat *a, const struct stat *b) {
    if (a->st_dev != b->st_dev || a->st_ino != b->st_ino || a->st_mode != b->st_mode || a->st_size != b->st_size)
        return false;
#ifdef __APPLE__
    return a->st_mtimespec.tv_sec == b->st_mtimespec.tv_sec && a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
           a->st_ctimespec.tv_sec == b->st_ctimespec.tv_sec && a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
#else
    return a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
           a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
#endif
}

static int jw__stamp(const jw_content_file *file, jw_delete_stamp *stamp, char *error, size_t error_size) {
    memset(stamp, 0, sizeof(*stamp));
    stamp->missing = file->missing;
    if (lstat(file->path, &stamp->file)) {
        if (!file->missing || errno != ENOENT) return jw__error(error, error_size, "Content changed during preview: %.160s", file->rom_relpath);
    } else if (file->missing || !S_ISREG(stamp->file.st_mode) || (uint64_t)stamp->file.st_size != file->size)
        return jw__error(error, error_size, "Content changed during preview: %.160s", file->rom_relpath);
    snprintf(stamp->parent, sizeof(stamp->parent), "%s", file->path);
    char *slash = strrchr(stamp->parent, '/');
    if (!slash) return jw__error(error, error_size, "Invalid content directory");
    *slash = '\0';
    if (stat(stamp->parent, &stamp->directory)) {
        if (!file->missing || errno != ENOENT) return jw__error(error, error_size, "Cannot inspect content directory: %s", strerror(errno));
    }
    return 0;
}

static bool jw__protected(const jw_content_file *file, const jw_storage_source_list *sources,
                           const char *const *paths, size_t count, bool pico_cart) {
    for (int i = 0; i < sources->count; i++) {
        const jw_storage_source *s = &sources->sources[i];
        const char *roots[] = {s->saves_path, s->states_path, s->images_path, s->userdata_path,
                              s->shared_userdata_path, s->bios_path, s->cheats_path};
        for (size_t j = 0; j < sizeof(roots) / sizeof(roots[0]); j++) {
            char resolved[JW_STORAGE_PATH_MAX];
            if (*roots[j] && realpath(roots[j], resolved) && jw__inside(file->path, resolved)) return true;
        }
    }
    for (size_t i = 0; i < count; i++) {
        char resolved[JW_STORAGE_PATH_MAX];
        if (paths[i] && realpath(paths[i], resolved) && !strcmp(file->path, resolved)) return true;
    }
    const char *ext = jw__extension(file->path);
    if (jw__word("sav srm rtc state mcr mcd ssv scr jpg jpeg bmp webp", ext)) return true;
    if (!pico_cart && !strcasecmp(ext, "png")) return true;
    const char *base = strrchr(file->path, '/');
    const char *state = base ? strstr(base, ".state") : NULL;
    if (!state) return false;
    const char *suffix = state + strlen(".state");
    if (!strcmp(suffix, ".auto")) return true;
    while (isdigit((unsigned char)*suffix)) suffix++;
    return !*suffix;
}

static void jw__order(const jw_content *graph, size_t index, bool *visited,
                       const bool *selected, size_t *order, size_t *count) {
    if (visited[index]) return;
    visited[index] = true;
    for (size_t i = 0; i < graph->reference_count; i++)
        if (graph->references[i].parent == index)
            jw__order(graph, graph->references[i].child, visited, selected, order, count);
    if (selected[index]) order[(*count)++] = index;
}

void jw_delete_plan_free(jw_delete_plan *plan) {
    if (!plan) return;
    jw_delete_snapshot *snapshot = plan->snapshot;
    if (snapshot) {
        jw_content_free(&snapshot->graph);
        free(snapshot->stamps); free(snapshot->indices); free(snapshot->order); free(snapshot->replacement); free(snapshot);
    }
    free(plan->files);
    memset(plan, 0, sizeof(*plan));
}

static bool jw__same_disc(const jw_content_disc *a, const jw_content_disc *b) {
    return !strcmp(a->source_id, b->source_id) && !strcmp(a->rom_relpath, b->rom_relpath) &&
           !strcmp(a->member, b->member);
}

static int jw__replacement(jw_delete_snapshot *snapshot, const jw_content_disc *selected,
                           char *error, size_t error_size) {
    const jw_content *graph = &snapshot->graph;
    const jw_content_file *root = &graph->files[graph->launch_file];
    bool *omit = calloc(root->descriptor_size ? root->descriptor_size : 1, sizeof(*omit));
    snapshot->replacement = malloc(root->descriptor_size + 1);
    if (!omit || !snapshot->replacement) { free(omit); return jw__error(error, error_size, "Out of memory editing playlist"); }
    size_t previous_end = 0;
    for (size_t i = 0; i < graph->disc_count; i++) {
        const jw_content_disc *disc = &graph->discs[i];
        if (disc->line_start < previous_end || disc->line_end > root->descriptor_size || disc->line_end <= disc->line_start) {
            free(omit); return jw__error(error, error_size, "The playlist line changed during inspection");
        }
        if (jw__same_disc(disc, selected)) {
            memset(omit + disc->line_start, 1, (disc->line_end - disc->line_start) * sizeof(*omit));
            /* EXTINF belongs to the next disc; retain unrelated comments and directives. */
            for (size_t start = previous_end; start < disc->line_start;) {
                size_t end = start;
                while (end < disc->line_start && root->descriptor[end] != '\n') end++;
                if (end < disc->line_start) end++;
                size_t text = start;
                if (!text && end >= 3 && !memcmp(root->descriptor, "\xef\xbb\xbf", 3)) text = 3;
                size_t preserve_bom = text;
                while (text < end && isspace((unsigned char)root->descriptor[text])) text++;
                if (end - text >= 8 && !strncasecmp(root->descriptor + text, "#EXTINF:", 8))
                    memset(omit + preserve_bom, 1, (end - preserve_bom) * sizeof(*omit));
                start = end;
            }
        }
        previous_end = disc->line_end;
    }
    for (size_t i = 0; i < root->descriptor_size; i++)
        if (!omit[i]) snapshot->replacement[snapshot->replacement_size++] = root->descriptor[i];
    snapshot->replacement[snapshot->replacement_size] = '\0';
    free(omit);
    return 0;
}

static int jw__plan_build(const jw_storage_source_list *sources,
                         const jw_ra_catalog *catalog,
                         const char *source_id, const char *rom_relpath,
                         const char *system_id, const jw_content_disc *disc,
                         const jw_delete_owner *owners, size_t owner_count,
                         const char *const *protected_paths, size_t protected_count,
                         jw_delete_cancelled cancelled, void *context,
                         jw_delete_plan *out, char *error, size_t error_size) {
    if (error && error_size) *error = '\0';
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    const jw_ra_system *system = jw__system(catalog, system_id);
    if (!sources || sources->count < 1 || sources->count > JW_STORAGE_MAX_SOURCES ||
        !source_id || !rom_relpath || !jw_delete_supported(system) || (owner_count && !owners))
        return jw__error(error, error_size, "Delete is not supported for this game");
    jw_delete_builder builder = {.sources = sources, .catalog = catalog,
        .cancelled = cancelled, .context = context, .error = error, .error_size = error_size};
    jw_delete_snapshot *snapshot = calloc(1, sizeof(*snapshot));
    bool *selected = NULL, *shared = NULL, *visited = NULL;
    int result = -1;
    if (!snapshot) return jw__error(error, error_size, "Out of memory preparing deletion");
    out->snapshot = snapshot;
    snapshot->sources = *sources;
    for (int i = 0; i < sources->count; i++) {
        if (strcmp(sources->sources[i].id, source_id)) continue;
        char path[JW_STORAGE_PATH_MAX]; struct stat st;
        if (jw__join(path, sizeof(path), sources->sources[i].roms_path, rom_relpath) ||
            (!lstat(path, &st) && S_ISLNK(st.st_mode))) {
            jw__error(error, error_size, "Delete cannot remove a symlink launch file"); goto done;
        }
    }
    if (jw__add_root(&builder, source_id, rom_relpath, system)) goto done;
    for (int i = 0; i < sources->count; i++) {
        const jw_storage_source *source = &sources->sources[i];
        if (!source->available) {
            size_t used = strlen(out->missing_sources);
            snprintf(out->missing_sources + used, sizeof(out->missing_sources) - used, "%s%s", used ? ", " : "", source->id);
            continue;
        }
        if (stat(source->root_abs[0] ? source->root_abs : source->root, &snapshot->mounts[i])) {
            jw__error(error, error_size, "Cannot inspect mounted source %s: %s", source->id, strerror(errno)); goto done;
        }
        if (jw__walk(&builder, source, "", 0)) goto done;
    }
    if (builder.root_count > 2) qsort(builder.roots + 1, builder.root_count - 1, sizeof(*builder.roots), jw__root_compare);
    if (jw_content_inspect_many(sources, builder.roots, builder.root_count, true, cancelled, context,
                               &snapshot->graph, error, error_size)) goto done;
    jw_content *graph = &snapshot->graph;
    const jw_content_file *launch = &graph->files[graph->launch_file];
    if (strcmp(launch->source_id, source_id) || strcmp(launch->rom_relpath, rom_relpath)) {
        jw__error(error, error_size, "The launch file identity changed. Refresh your library before deleting it.");
        goto done;
    }
    selected = calloc(graph->file_count, sizeof(*selected));
    shared = calloc(graph->file_count, sizeof(*shared));
    visited = calloc(graph->file_count, sizeof(*visited));
    snapshot->stamps = calloc(graph->file_count, sizeof(*snapshot->stamps));
    snapshot->indices = calloc(graph->file_count, sizeof(*snapshot->indices));
    snapshot->order = calloc(graph->file_count, sizeof(*snapshot->order));
    if (!selected || !shared || !visited || !snapshot->stamps || !snapshot->indices || !snapshot->order) {
        jw__error(error, error_size, "Out of memory preparing deletion"); goto done;
    }
    jw__mark(graph, graph->launch_file, selected);
    for (size_t i = 0; i < graph->file_count; i++) {
        if (cancelled && cancelled(context)) { jw__error(error, error_size, "Delete preview cancelled"); goto done; }
        bool available = false;
        for (int source = 0; source < sources->count; source++)
            if (!strcmp(sources->sources[source].id, graph->files[i].source_id)) available = sources->sources[source].available;
        if (available && jw__stamp(&graph->files[i], &snapshot->stamps[i], error, error_size)) goto done;
        if (!available) snapshot->stamps[i].missing = true;
    }
    /* Unindexed descriptors outside the selected graph are still independent owners. */
    for (size_t i = 1; i < builder.root_count; i++) {
        if (cancelled && cancelled(context)) { jw__error(error, error_size, "Delete preview cancelled"); goto done; }
        size_t root = builder.roots[i].file_index;
        if (!selected[root]) {
            jw__mark(graph, root, shared);
            if (shared[graph->launch_file] && !graph->files[graph->launch_file].missing) {
                const char *name = builder.roots[i].rom_relpath;
                for (size_t j = 0; j < owner_count; j++)
                    if (!strcmp(owners[j].source_id, builder.roots[i].source_id) &&
                        !strcmp(owners[j].rom_relpath, builder.roots[i].rom_relpath)) name = owners[j].name;
                jw__error(error, error_size, "Delete is blocked because %.160s uses this game's launch file. Hide Game is still available.", name);
                goto done;
            }
        }
    }
    for (size_t i = 0; i < owner_count; i++) {
        if (cancelled && cancelled(context)) { jw__error(error, error_size, "Delete preview cancelled"); goto done; }
        const jw_delete_owner *owner = &owners[i];
        if (!strcmp(owner->source_id, source_id) && !strcmp(owner->rom_relpath, rom_relpath)) continue;
        const jw_storage_source *source = NULL;
        for (int j = 0; j < sources->count; j++) if (!strcmp(sources->sources[j].id, owner->source_id)) source = &sources->sources[j];
        if (!source || !source->available) continue;
        char path[JW_STORAGE_PATH_MAX], resolved[JW_STORAGE_PATH_MAX];
        if (jw__join(path, sizeof(path), source->roms_path, owner->rom_relpath)) { jw__error(error, error_size, "Indexed ROM path is too long"); goto done; }
        bool present = realpath(path, resolved) != NULL;
        if (!present && errno != ENOENT) { jw__error(error, error_size, "Cannot inspect indexed game %.160s: %s", owner->name, strerror(errno)); goto done; }
        struct stat st;
        bool have_stat = present && stat(resolved, &st) == 0;
        for (size_t j = 0; j < graph->file_count; j++) {
            const jw_content_file *file = &graph->files[j];
            bool same = !strcmp(owner->source_id, file->source_id) && !strcmp(owner->rom_relpath, file->rom_relpath);
            if (present && !strcmp(resolved, file->path)) same = true;
            if (have_stat && !file->missing && st.st_dev == snapshot->stamps[j].file.st_dev && st.st_ino == snapshot->stamps[j].file.st_ino) same = true;
            if (!same) continue;
            jw__mark(graph, j, shared);
            if (shared[graph->launch_file] && !graph->files[graph->launch_file].missing) {
                jw__error(error, error_size, "Delete is blocked because %.160s uses this game's launch file. Hide Game is still available.", owner->name); goto done;
            }
        }
    }
    if (disc) {
        if (!graph->is_playlist || !launch->descriptor) {
            jw__error(error, error_size, "Delete Disc needs an existing playlist"); goto done;
        }
        size_t matches = 0;
        for (size_t i = 0; i < graph->disc_count; i++) {
            if (jw__same_disc(&graph->discs[i], disc)) {
                if (!matches) snprintf(out->disc_name, sizeof(out->disc_name), "%s", graph->discs[i].label);
                matches++;
            }
        }
        if (!matches) { jw__error(error, error_size, "The selected disc changed. Open a fresh preview."); goto done; }
        out->remaining_discs = graph->disc_count - matches;
        out->final_disc = out->remaining_discs == 0;
        out->playlist_edit = !out->final_disc;
        if (out->playlist_edit) {
            memset(selected, 0, graph->file_count * sizeof(*selected));
            for (size_t i = 0; i < graph->disc_count; i++) {
                const jw_content_disc *entry = &graph->discs[i];
                jw__mark(graph, entry->file_index, jw__same_disc(entry, disc) ? selected : shared);
            }
            selected[graph->launch_file] = true;
            if (jw__replacement(snapshot, disc, error, error_size)) goto done;
        }
    }
    for (size_t i = 0; i < graph->file_count; i++) if (selected[i]) out->file_count++;
    out->files = calloc(out->file_count, sizeof(*out->files));
    if (!out->files) { jw__error(error, error_size, "Out of memory preparing deletion"); goto done; }
    size_t count = 0;
    for (size_t i = 0; i < graph->file_count; i++) {
        if (!selected[i]) continue;
        const jw_content_file *file = &graph->files[i];
        jw_delete_file *target = &out->files[count];
        snapshot->indices[count] = i;
        if (i == graph->launch_file) out->launch_file = count;
        count++;
        snprintf(target->source_id, sizeof(target->source_id), "%s", file->source_id);
        snprintf(target->rom_relpath, sizeof(target->rom_relpath), "%s", file->rom_relpath);
        snprintf(target->path, sizeof(target->path), "%s", file->path);
        target->size = file->size; target->missing = file->missing;
        target->keep = shared[i] && !(i == graph->launch_file && file->missing) ? JW_DELETE_SHARED : JW_DELETE_REMOVE;
        for (int source = 0; source < sources->count; source++)
            if (!strcmp(sources->sources[source].id, file->source_id) && !sources->sources[source].available)
                target->keep = JW_DELETE_PRESERVED;
        bool pico_cart = !strcmp(system_id, "PICO8") && i == graph->launch_file;
        if (jw__protected(file, sources, protected_paths, protected_count, pico_cart)) target->keep = JW_DELETE_PRESERVED;
        if (i == graph->launch_file && target->keep) { jw__error(error, error_size, "The launch file is protected save, state or artwork data"); goto done; }
        if (i == graph->launch_file && out->playlist_edit) target->keep = JW_DELETE_PRESERVED;
        if (!target->keep && jw__word("uae ccd mds conf bat exe sh dat", jw__extension(file->path))) {
            jw__error(error, error_size, "No dependency reader for %.160s", file->rom_relpath); goto done;
        }
        if (file->missing && jw_content_is_descriptor_path(file->path)) out->missing_descriptor = true;
        if (target->keep) out->kept_count++;
        else { out->remove_count++; out->bytes += target->size; }
        if (target->missing) out->missing_count++;
    }
    jw__order(graph, graph->launch_file, visited, selected, snapshot->order, &snapshot->order_count);
    out->disc_count = out->playlist_edit ? graph->disc_count - out->remaining_discs : graph->disc_count;
    out->writable_image = !strcmp(system_id, "PC98");
    result = 0;
done:
    for (size_t i = 0; i < builder.root_count; i++) { free((char *)builder.roots[i].source_id); free((char *)builder.roots[i].rom_relpath); }
    free(builder.roots); free(selected); free(shared); free(visited);
    if (result) jw_delete_plan_free(out);
    return result;
}

int jw_delete_plan_build(const jw_storage_source_list *sources,
                         const jw_ra_catalog *catalog,
                         const char *source_id, const char *rom_relpath,
                         const char *system_id,
                         const jw_delete_owner *owners, size_t owner_count,
                         const char *const *protected_paths, size_t protected_count,
                         jw_delete_cancelled cancelled, void *context,
                         jw_delete_plan *out, char *error, size_t error_size) {
    return jw__plan_build(sources, catalog, source_id, rom_relpath, system_id, NULL,
        owners, owner_count, protected_paths, protected_count, cancelled, context, out, error, error_size);
}

int jw_delete_disc_plan_build(const jw_storage_source_list *sources,
                              const jw_ra_catalog *catalog,
                              const char *source_id, const char *rom_relpath,
                              const char *system_id, const jw_content_disc *disc,
                              const jw_delete_owner *owners, size_t owner_count,
                              const char *const *protected_paths, size_t protected_count,
                              jw_delete_cancelled cancelled, void *context,
                              jw_delete_plan *out, char *error, size_t error_size) {
    if (!disc || !disc->source_id[0] || !disc->rom_relpath[0]) {
        if (out) memset(out, 0, sizeof(*out));
        return jw__error(error, error_size, "Invalid disc identity");
    }
    return jw__plan_build(sources, catalog, source_id, rom_relpath, system_id, disc,
        owners, owner_count, protected_paths, protected_count, cancelled, context, out, error, error_size);
}

static bool jw__same_sources(const jw_storage_source_list *a, const struct stat *a_mounts,
                             const jw_storage_source_list *b, const struct stat *b_mounts) {
    if (a->count != b->count) return false;
    for (int i = 0; i < a->count; i++) {
        const jw_storage_source *x = &a->sources[i], *y = &b->sources[i];
        if (strcmp(x->id, y->id) || strcmp(x->root, y->root) || strcmp(x->root_abs, y->root_abs) ||
            strcmp(x->roms_path, y->roms_path) || x->available != y->available ||
            x->device_id != y->device_id || x->roms_device_id != y->roms_device_id ||
            x->mount_id != y->mount_id || x->roms_mount_id != y->roms_mount_id ||
            strcmp(x->filesystem_fingerprint, y->filesystem_fingerprint) ||
            strcmp(x->roms_filesystem_fingerprint, y->roms_filesystem_fingerprint) ||
            a_mounts[i].st_dev != b_mounts[i].st_dev || a_mounts[i].st_ino != b_mounts[i].st_ino) return false;
    }
    return true;
}

bool jw_delete_plan_sources_match(const jw_delete_plan *plan,
                                   const jw_storage_source_list *current) {
    if (!plan || !plan->snapshot || !current || current->count < 1 ||
        current->count > JW_STORAGE_MAX_SOURCES) return false;
    struct stat mounts[JW_STORAGE_MAX_SOURCES] = {0};
    for (int i = 0; i < current->count; i++) {
        const jw_storage_source *source = &current->sources[i];
        if (source->available && stat(source->root_abs[0] ? source->root_abs : source->root,
                                      &mounts[i])) return false;
    }
    const jw_delete_snapshot *snapshot = plan->snapshot;
    return jw__same_sources(&snapshot->sources, snapshot->mounts, current, mounts);
}

bool jw_delete_plan_equal(const jw_delete_plan *a, const jw_delete_plan *b) {
    if (!a || !b || !a->snapshot || !b->snapshot || a->file_count != b->file_count ||
        a->disc_count != b->disc_count || a->launch_file != b->launch_file || a->bytes != b->bytes ||
        a->missing_descriptor != b->missing_descriptor || a->writable_image != b->writable_image ||
        a->playlist_edit != b->playlist_edit || a->final_disc != b->final_disc ||
        a->remaining_discs != b->remaining_discs || strcmp(a->disc_name, b->disc_name) ||
        strcmp(a->missing_sources, b->missing_sources)) return false;
    const jw_delete_snapshot *x = a->snapshot, *y = b->snapshot;
    if (!jw__same_sources(&x->sources, x->mounts, &y->sources, y->mounts) || x->graph.file_count != y->graph.file_count ||
        x->graph.reference_count != y->graph.reference_count ||
        x->replacement_size != y->replacement_size ||
        (x->replacement_size && memcmp(x->replacement, y->replacement, x->replacement_size))) return false;
    for (size_t i = 0; i < a->file_count; i++) {
        const jw_delete_file *p = &a->files[i], *q = &b->files[i];
        if (strcmp(p->source_id, q->source_id) || strcmp(p->rom_relpath, q->rom_relpath) ||
            strcmp(p->path, q->path) || p->size != q->size || p->missing != q->missing || p->keep != q->keep) return false;
    }
    for (size_t i = 0; i < x->graph.file_count; i++) {
        const jw_content_file *p = &x->graph.files[i], *q = &y->graph.files[i];
        if (strcmp(p->path, q->path) || p->missing != q->missing || p->descriptor_size != q->descriptor_size ||
            (!p->missing && !jw__same_stat(&x->stamps[i].file, &y->stamps[i].file)) ||
            (p->descriptor_size && memcmp(p->descriptor, q->descriptor, p->descriptor_size))) return false;
    }
    for (size_t i = 0; i < x->graph.reference_count; i++)
        if (x->graph.references[i].parent != y->graph.references[i].parent ||
            x->graph.references[i].child != y->graph.references[i].child) return false;
    return true;
}

void jw_delete_result_free(jw_delete_result *result) {
    if (!result) return;
    free(result->completed);
    memset(result, 0, sizeof(*result));
}

/* Return an opened, pinned parent directory, -2 for an explicitly absent file,
   or -1 on changed content. The caller owns a returned directory descriptor. */
static int jw__checked_file(const jw_delete_plan *plan, size_t slot,
                            jw_delete_guard guard, void *context,
                            char *error, size_t error_size) {
    const jw_delete_snapshot *snapshot = plan->snapshot;
    size_t index = snapshot->indices[slot];
    const jw_delete_file *file = &plan->files[slot];
    if (guard && guard(context, file, error, error_size)) return -1;
    if (!jw_delete_plan_sources_match(plan, &snapshot->sources))
        return jw__error(error, error_size, "Mounted sources changed; open a fresh preview");
    const jw_delete_stamp *stamp = &snapshot->stamps[index];
    char resolved[JW_STORAGE_PATH_MAX];
    struct stat st;
    if (!realpath(stamp->parent, resolved)) {
        if (file->missing && errno == ENOENT && lstat(file->path, &st) && errno == ENOENT) return -2;
        return jw__error(error, error_size, "Content directory changed: %.160s", file->rom_relpath);
    }
    if (strcmp(resolved, stamp->parent)) return jw__error(error, error_size, "Content directory changed: %.160s", file->rom_relpath);
    int directory = open(stamp->parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (directory < 0) return jw__error(error, error_size, "Cannot open content directory: %s", strerror(errno));
    if (fstat(directory, &st) || st.st_dev != stamp->directory.st_dev || st.st_ino != stamp->directory.st_ino) {
        close(directory); return jw__error(error, error_size, "Content directory changed: %.160s", file->rom_relpath);
    }
    const char *name = strrchr(file->path, '/') + 1;
    int stat_result = fstatat(directory, name, &st, AT_SYMLINK_NOFOLLOW);
    if (file->missing && stat_result && errno == ENOENT) { close(directory); return -2; }
    if (stat_result || file->missing || !jw__same_stat(&stamp->file, &st)) {
        close(directory); return jw__error(error, error_size, "Content changed; open a fresh preview: %.160s", file->rom_relpath);
    }
    const jw_content_file *original = &snapshot->graph.files[index];
    if (original->descriptor) {
        int fd = openat(directory, name, O_RDONLY | O_NOFOLLOW);
        if (fd < 0) { close(directory); return jw__error(error, error_size, "Cannot read descriptor %.160s: %s", file->rom_relpath, strerror(errno)); }
        size_t offset = 0;
        char buffer[4096];
        bool bad = false;
        while (offset < original->descriptor_size) {
            size_t want = original->descriptor_size - offset;
            if (want > sizeof(buffer)) want = sizeof(buffer);
            ssize_t got = read(fd, buffer, want);
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0 || memcmp(buffer, original->descriptor + offset, (size_t)got)) { bad = true; break; }
            offset += (size_t)got;
        }
        if (!bad && read(fd, buffer, 1) != 0) bad = true;
        if (close(fd)) bad = true;
        if (bad) { close(directory); return jw__error(error, error_size, "Descriptor changed; open a fresh preview: %.160s", file->rom_relpath); }
    }
    if (fstatat(directory, name, &st, AT_SYMLINK_NOFOLLOW) || !jw__same_stat(&stamp->file, &st)) {
        close(directory); return jw__error(error, error_size, "Content changed; open a fresh preview: %.160s", file->rom_relpath);
    }
    return directory;
}

static int jw__sync_directory(int directory, const jw_delete_file *file,
                              char *error, size_t error_size) {
    int rc;
#ifdef JW_ENABLE_FAULT_INJECTION
    const char *failure = getenv("JAWAKA_TEST_DELETE_SYNC_FAIL");
    if (failure && !strcmp(failure, file->rom_relpath)) { errno = EIO; rc = -1; }
    else
#endif
        rc = fsync(directory);
    return rc ? jw__error(error, error_size, "Could not sync changes to %.160s: %s", file->rom_relpath, strerror(errno)) : 0;
}

static int jw__remove_files(const jw_delete_plan *plan, jw_delete_guard guard, void *context,
                            jw_delete_result *result, char *error, size_t error_size) {
    const jw_delete_snapshot *snapshot = plan->snapshot;
    for (size_t ordinal = 0; ordinal < snapshot->order_count; ordinal++) {
        size_t index = snapshot->order[ordinal], slot = 0;
        while (slot < plan->file_count && snapshot->indices[slot] != index) slot++;
        if (slot == plan->file_count) return jw__error(error, error_size, "Invalid deletion order");
        const jw_delete_file *file = &plan->files[slot];
        if (file->keep) continue;
        result->failed_index = slot;
        int directory = jw__checked_file(plan, slot, guard, context, error, error_size);
        if (directory == -2) { result->completed[slot] = true; result->absent_count++; continue; }
        if (directory < 0) return -1;
        const char *name = strrchr(file->path, '/') + 1;
        if (unlinkat(directory, name, 0)) {
            int saved = errno; close(directory);
            return jw__error(error, error_size, "Could not remove %.160s: %s", file->rom_relpath, strerror(saved));
        }
        result->completed[slot] = true;
        result->removed_count++; result->bytes += file->size;
        int rc = jw__sync_directory(directory, file, error, error_size);
        if (close(directory) && !rc) rc = jw__error(error, error_size, "Could not close removal directory: %s", strerror(errno));
        if (rc) return -1;
    }
    return 0;
}

static int jw__prepare_replacement(const jw_delete_plan *plan, int directory,
                                   char temporary[80], struct stat *stamp,
                                   char *error, size_t error_size) {
    const jw_delete_snapshot *snapshot = plan->snapshot;
    const jw_delete_stamp *root = &snapshot->stamps[snapshot->graph.launch_file];
    int fd = -1;
    for (unsigned attempt = 0; attempt < 1000; attempt++) {
        snprintf(temporary, 80, ".jawaka-delete-%ld-%u.tmp", (long)getpid(), attempt);
        fd = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) { temporary[0] = '\0'; return jw__error(error, error_size, "Could not prepare replacement playlist: %s", strerror(errno)); }
    int failure = fstat(fd, stamp) ? errno : 0;
    if (!failure && fchmod(fd, root->file.st_mode & 0777)) failure = errno;
    size_t written = 0;
    while (!failure && written < snapshot->replacement_size) {
        ssize_t n = write(fd, snapshot->replacement + written, snapshot->replacement_size - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { failure = n < 0 ? errno : EIO; break; }
        written += (size_t)n;
    }
#ifdef JW_ENABLE_FAULT_INJECTION
    const char *marker = getenv("JAWAKA_TEST_DELETE_FAIL_TEMP_FILE");
    if (!failure && marker && access(marker, F_OK) == 0) failure = EIO;
#endif
    if (!failure && fsync(fd)) failure = errno;
    if (!failure && fstat(fd, stamp)) failure = errno;
    if (close(fd) && !failure) failure = errno;
    return failure ? jw__error(error, error_size, "Could not write complete replacement playlist: %s", strerror(failure)) : 0;
}

int jw_delete_execute(const jw_delete_plan *plan, jw_delete_guard guard, void *context,
                      jw_delete_result *result, char *error, size_t error_size) {
    if (error && error_size) *error = '\0';
    if (!result) return -1;
    memset(result, 0, sizeof(*result));
    result->failed_index = (size_t)-1;
    if (!plan || !plan->snapshot) return jw__error(error, error_size, "No reviewed deletion plan");
    result->completed = calloc(plan->file_count, sizeof(*result->completed));
    if (!result->completed) return jw__error(error, error_size, "Out of memory recording deletion");
    char temporary[80] = "";
    struct stat prepared = {0};
    int parent = -1, final_parent = -1, rc = -1;
    if (plan->playlist_edit) {
        result->failed_index = plan->launch_file;
        parent = jw__checked_file(plan, plan->launch_file, guard, context, error, error_size);
        if (parent < 0) goto done;
        if (jw__prepare_replacement(plan, parent, temporary, &prepared, error, error_size)) goto done;
    }
    if (jw__remove_files(plan, guard, context, result, error, error_size)) goto done;
    if (plan->playlist_edit) {
        result->failed_index = plan->launch_file;
        final_parent = jw__checked_file(plan, plan->launch_file, guard, context, error, error_size);
        if (final_parent < 0) goto done;
#ifdef JW_ENABLE_FAULT_INJECTION
        const char *marker = getenv("JAWAKA_TEST_DELETE_FAIL_BEFORE_RENAME_FILE");
        if (marker && access(marker, F_OK) == 0) {
            jw__error(error, error_size, "Injected failure before playlist replacement"); goto done;
        }
#endif
        struct stat st;
        if (fstatat(parent, temporary, &st, AT_SYMLINK_NOFOLLOW) || !jw__same_stat(&prepared, &st)) {
            jw__error(error, error_size, "Replacement playlist changed. Open a fresh preview."); goto done;
        }
        const jw_delete_file *root = &plan->files[plan->launch_file];
        const char *name = strrchr(root->path, '/') + 1;
        if (renameat(parent, temporary, final_parent, name)) {
            jw__error(error, error_size, "Could not replace playlist: %s", strerror(errno)); goto done;
        }
        temporary[0] = '\0';
        result->playlist_replaced = true;
        if (jw__sync_directory(final_parent, root, error, error_size)) goto done;
    }
    result->failed_index = (size_t)-1;
    rc = 0;
done:
    if (temporary[0] && parent >= 0) {
        struct stat current;
        /* A client may replace our temporary file too; never unlink its replacement. */
        if (!fstatat(parent, temporary, &current, AT_SYMLINK_NOFOLLOW) &&
            current.st_dev == prepared.st_dev && current.st_ino == prepared.st_ino &&
            unlinkat(parent, temporary, 0) && !rc)
            rc = jw__error(error, error_size, "Could not remove temporary playlist: %s", strerror(errno));
    }
    if (final_parent >= 0 && close(final_parent) && !rc)
        rc = jw__error(error, error_size, "Could not close playlist directory: %s", strerror(errno));
    if (parent >= 0 && close(parent) && !rc)
        rc = jw__error(error, error_size, "Could not close playlist directory: %s", strerror(errno));
    return rc;
}
