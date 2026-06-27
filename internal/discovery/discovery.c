#include "internal/discovery/discovery.h"

#include "cJSON.h"
#include "internal/db/db.h"
#include "internal/platform/platform_id.h"
#include "internal/retroarch/catalog.h"
#include "internal/storage/sources.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static int jw__exec(sqlite3 *db, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        if (err) {
            fprintf(stderr, "sqlite exec failed: %s\n", err);
            sqlite3_free(err);
        }
        return -1;
    }
    return 0;
}

#define JW_SCAN_WRITE_BATCH 64

typedef struct {
    sqlite3 *db;
    int      writes;
    bool     active;
} jw_scan_tx;

static int jw__scan_tx_begin(jw_scan_tx *tx) {
    if (!tx || !tx->db) {
        return -1;
    }
    if (tx->active) {
        return 0;
    }
    if (jw__exec(tx->db, "BEGIN DEFERRED;") != 0) {
        return -1;
    }
    tx->active = true;
    tx->writes = 0;
    return 0;
}

static void jw__scan_tx_rollback(jw_scan_tx *tx) {
    if (!tx || !tx->active) {
        return;
    }
    jw__exec(tx->db, "ROLLBACK;");
    tx->active = false;
    tx->writes = 0;
}

static int jw__scan_tx_commit(jw_scan_tx *tx) {
    if (!tx || !tx->active) {
        return 0;
    }
    if (jw__exec(tx->db, "COMMIT;") != 0) {
        jw__scan_tx_rollback(tx);
        return -1;
    }
    tx->active = false;
    tx->writes = 0;
    return 0;
}

static int jw__scan_tx_note_write(jw_scan_tx *tx) {
    if (!tx) {
        return -1;
    }
    tx->writes += 1;
    if (tx->writes >= JW_SCAN_WRITE_BATCH) {
        return jw__scan_tx_commit(tx);
    }
    return 0;
}

static int jw__scan_insert_game(jw_scan_tx *tx,
                                const char *system,
                                const char *name,
                                const char *rom_path,
                                const char *image_path) {
    if (jw__scan_tx_begin(tx) != 0) {
        return -1;
    }
    if (jw_db_insert_game(tx->db, system, name, rom_path, image_path) != 0) {
        return -1;
    }
    return jw__scan_tx_note_write(tx);
}

static int jw__scan_insert_app(jw_scan_tx *tx,
                               const char *pak_dir,
                               const char *name,
                               const char *icon,
                               const char *platform,
                               const char *pak_version,
                               const char *min_jawaka_version) {
    if (jw__scan_tx_begin(tx) != 0) {
        return -1;
    }
    if (jw_db_insert_app(tx->db, pak_dir, name, icon, platform,
                         pak_version, min_jawaka_version) != 0) {
        return -1;
    }
    return jw__scan_tx_note_write(tx);
}

static int jw__is_directory(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int jw__is_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int jw__is_hidden(const char *name) {
    return name[0] == '.';
}

static int jw__is_private_rom_name(const char *name) {
    return name[0] == '_';
}

static int jw__format_string(char *out, size_t out_size, const char *fmt, ...) {
    if (!out || out_size == 0) {
        return -1;
    }

    va_list args;
    va_start(args, fmt);
    int needed = vsnprintf(out, out_size, fmt, args);
    va_end(args);
    return needed >= 0 && (size_t)needed < out_size ? 0 : -1;
}

static void jw__lower_copy(const char *in, char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    size_t i = 0;
    if (in) {
        for (; in[i] && i + 1u < out_size; i++) {
            out[i] = (char)tolower((unsigned char)in[i]);
        }
    }
    out[i] = '\0';
}

static void jw__extension_lower(const char *filename, char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!filename) {
        return;
    }
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename || !dot[1]) {
        return;
    }
    jw__lower_copy(dot + 1, out, out_size);
}

static void jw__strip_last_extension(char *name) {
    if (!name) {
        return;
    }
    char *dot = strrchr(name, '.');
    if (dot && dot != name) {
        *dot = '\0';
    }
}

static void jw__title_from_filename(const char *filename, char *out, size_t out_size) {
    snprintf(out, out_size, "%s", filename);
    char *dot = strrchr(out, '.');
    if (dot) {
        *dot = '\0';
    }
}

static void jw__title_from_metadata_filename(const jw_ra_system *system,
                                             const char *filename,
                                             char *out,
                                             size_t out_size) {
    snprintf(out, out_size, "%s", filename ? filename : "");

    char outer_ext[64];
    jw__extension_lower(filename, outer_ext, sizeof(outer_ext));
    int archive = jw_ra_string_list_contains_casefold(&system->archive_extensions, outer_ext);
    int playlist = jw_ra_string_list_contains_casefold(&system->playlist_extensions, outer_ext);
    int content = jw_ra_string_list_contains_casefold(&system->extensions, outer_ext);

    if (archive || playlist || content) {
        jw__strip_last_extension(out);
    }

    if (archive || content) {
        /* Also strip a content double-extension ("cart.p8.png") so titles
           don't keep the inner suffix. */
        char inner_ext[64];
        jw__extension_lower(out, inner_ext, sizeof(inner_ext));
        if (jw_ra_string_list_contains_casefold(&system->archive_inner_extensions, inner_ext) ||
            jw_ra_string_list_contains_casefold(&system->extensions, inner_ext)) {
            jw__strip_last_extension(out);
        }
    }
}

static int jw__system_already_counted(char systems[][64], int count, const char *system) {
    for (int i = 0; i < count; i++) {
        if (strcmp(systems[i], system) == 0) {
            return 1;
        }
    }
    return 0;
}

static void jw__count_system_once(jw_scan_result *out, char systems[][64], int *count,
                                  const char *system) {
    if (!out || !systems || !count || !system || !system[0]) {
        return;
    }
    if (jw__system_already_counted(systems, *count, system)) {
        return;
    }
    if (*count < 128) {
        snprintf(systems[*count], 64, "%s", system);
        *count += 1;
    }
    out->system_count += 1;
}

static int jw__metadata_accepts_rom(const jw_ra_system *system,
                                    const char *filename,
                                    int *logged_archive_policy,
                                    int *logged_m3u_policy) {
    if (!system || !filename || !filename[0]) {
        return 0;
    }

    char filename_lower[PATH_MAX];
    jw__lower_copy(filename, filename_lower, sizeof(filename_lower));
    if (jw_ra_string_list_contains_casefold(&system->ignore_file_names, filename_lower)) {
        return 0;
    }

    if (jw_ra_string_list_contains_casefold(&system->file_names, filename)) {
        return 1;
    }

    /* No generic sidecar filtering here: the per-system metadata lists are
       explicit allowlists, and some systems claim classic sidecar extensions
       as content (Pico-8 ships .p8.png carts). Anything unlisted hits the
       final reject below. */
    char ext[64];
    jw__extension_lower(filename, ext, sizeof(ext));
    if (!ext[0]) {
        return 0;
    }

    if (jw_ra_string_list_contains_casefold(&system->playlist_extensions, ext)) {
        if (system->m3u_generation &&
            strcmp(system->m3u_generation, "rescan_hook") == 0 &&
            logged_m3u_policy && !*logged_m3u_policy) {
            fprintf(stderr, "RetroArch discovery: %s declares deferred m3u rescan hook policy\n",
                    system->id);
            *logged_m3u_policy = 1;
        }
        return 1;
    }

    if (jw_ra_string_list_contains_casefold(&system->extensions, ext)) {
        return 1;
    }

    if (jw_ra_string_list_contains_casefold(&system->archive_extensions, ext)) {
        const char *mode = system->archive_mode && system->archive_mode[0]
            ? system->archive_mode
            : "pass_through";
        if (strcmp(mode, "pass_through") == 0) {
            return 1;
        }
        if (strcmp(mode, "ignore") == 0) {
            return 0;
        }
        if (logged_archive_policy && !*logged_archive_policy) {
            fprintf(stderr, "RetroArch discovery: %s declares deferred archive policy %s\n",
                    system->id, mode);
            *logged_archive_policy = 1;
        }
        return 0;
    }

    return 0;
}

static int jw__metadata_system_has_packaged_retroarch_core(const jw_ra_catalog *catalog,
                                                           const jw_ra_system *system) {
    if (!catalog || !system) {
        return 0;
    }

    if (jw_ra_core_is_packaged_retroarch(jw_ra_catalog_find_core(catalog, system->default_core))) {
        return 1;
    }

    for (size_t i = 0; i < system->alternate_cores.count; i++) {
        if (jw_ra_core_is_packaged_retroarch(
                jw_ra_catalog_find_core(catalog, system->alternate_cores.items[i]))) {
            return 1;
        }
    }

    return 0;
}

static int jw__metadata_core_is_packaged_path(const jw_ra_core *core) {
    return core &&
           core->type &&
           strcmp(core->type, "path") == 0 &&
           core->status &&
           strcmp(core->status, "packaged") == 0 &&
           core->path &&
           core->path[0];
}

static int jw__metadata_platform_path(const char *sdcard_root,
                                      char *out,
                                      size_t out_size) {
    const char *platform_path = getenv("UMRK_PLATFORM_PATH");
    if (!platform_path || !platform_path[0]) {
        platform_path = getenv("SYSTEM_PATH");
    }
    if (platform_path && platform_path[0]) {
        return snprintf(out, out_size, "%s", platform_path) < (int)out_size ? 0 : -1;
    }
    if (!sdcard_root || !sdcard_root[0]) {
        return -1;
    }
    return snprintf(out, out_size, "%s/.system/leaf/platforms/%s",
                    sdcard_root, jw_platform_compiled_id()) < (int)out_size ? 0 : -1;
}

static int jw__metadata_path_core_exists(const char *sdcard_root,
                                         const jw_ra_core *core) {
    if (!jw__metadata_core_is_packaged_path(core)) {
        return 0;
    }

    char candidate[PATH_MAX];
    if (core->path[0] == '/') {
        if (snprintf(candidate, sizeof(candidate), "%s", core->path) >=
            (int)sizeof(candidate)) {
            return 0;
        }
    } else {
        char platform_path[PATH_MAX];
        if (jw__metadata_platform_path(sdcard_root, platform_path,
                                       sizeof(platform_path)) != 0 ||
            snprintf(candidate, sizeof(candidate), "%s/%s",
                     platform_path, core->path) >= (int)sizeof(candidate)) {
            return 0;
        }
    }

    return access(candidate, X_OK) == 0;
}

static int jw__metadata_system_has_packaged_launch_target(const jw_ra_catalog *catalog,
                                                          const jw_ra_system *system,
                                                          const char *sdcard_root) {
    if (!catalog || !system) {
        return 0;
    }

    if (jw__metadata_system_has_packaged_retroarch_core(catalog, system)) {
        return 1;
    }

    const jw_ra_core *core = jw_ra_catalog_find_core(catalog, system->default_core);
    if (jw__metadata_path_core_exists(sdcard_root, core)) {
        return 1;
    }

    for (size_t i = 0; i < system->alternate_cores.count; i++) {
        core = jw_ra_catalog_find_core(catalog, system->alternate_cores.items[i]);
        if (jw__metadata_path_core_exists(sdcard_root, core)) {
            return 1;
        }
    }

    return 0;
}

static const char *jw__metadata_image_path(const jw_ra_system *system,
                                           const jw_storage_source *source,
                                           const char *physical_folder,
                                           const char *title,
                                           char *image_abs,
                                           size_t image_abs_size,
                                           char *image_rel,
                                           size_t image_rel_size) {
    const char *canonical_root = system->image_root && system->image_root[0]
        ? system->image_root
        : NULL;
    if (canonical_root &&
        snprintf(image_abs, image_abs_size, "%s/%s/%s.png",
                 source->root, canonical_root, title) < (int)image_abs_size &&
        snprintf(image_rel, image_rel_size, "%s/%s.png",
                 canonical_root, title) < (int)image_rel_size &&
        jw__is_file(image_abs)) {
        return jw_storage_db_path_for_source(source, image_rel, image_abs,
                                             image_rel, image_rel_size) == 0
            ? image_rel
            : NULL;
    }

    if (physical_folder && physical_folder[0] &&
        snprintf(image_abs, image_abs_size, "%s/%s/%s.png",
                 source->images_path, physical_folder, title) < (int)image_abs_size &&
        snprintf(image_rel, image_rel_size, "Images/%s/%s.png",
                 physical_folder, title) < (int)image_rel_size &&
        jw__is_file(image_abs)) {
        return jw_storage_db_path_for_source(source, image_rel, image_abs,
                                             image_rel, image_rel_size) == 0
            ? image_rel
            : NULL;
    }

    if (physical_folder && physical_folder[0] &&
        snprintf(image_abs, image_abs_size, "%s/%s/Imgs/%s.png",
                 source->roms_path, physical_folder, title) < (int)image_abs_size &&
        snprintf(image_rel, image_rel_size, "Roms/%s/Imgs/%s.png",
                 physical_folder, title) < (int)image_rel_size &&
        jw__is_file(image_abs)) {
        return jw_storage_db_path_for_source(source, image_rel, image_abs,
                                             image_rel, image_rel_size) == 0
            ? image_rel
            : NULL;
    }

    return NULL;
}

static int jw__scan_roms_compat(jw_scan_tx *tx, const jw_storage_source *source,
                                jw_scan_result *out) {
    DIR *systems = opendir(source->roms_path);
    if (!systems) {
        return 0;
    }

    struct dirent *system_entry;
    while ((system_entry = readdir(systems)) != NULL) {
        if (jw__is_hidden(system_entry->d_name)) {
            continue;
        }

        char system_dir[PATH_MAX];
        if (snprintf(system_dir, sizeof(system_dir), "%s/%s",
                     source->roms_path, system_entry->d_name) >= (int)sizeof(system_dir)) {
            continue;
        }
        if (!jw__is_directory(system_dir)) {
            continue;
        }

        DIR *files = opendir(system_dir);
        if (!files) {
            continue;
        }

        int system_has_games = 0;
        struct dirent *file_entry;
        while ((file_entry = readdir(files)) != NULL) {
            if (jw__is_hidden(file_entry->d_name)) {
                continue;
            }

            char rom_abs[PATH_MAX];
            if (snprintf(rom_abs, sizeof(rom_abs), "%s/%s",
                         system_dir, file_entry->d_name) >= (int)sizeof(rom_abs)) {
                continue;
            }
            if (!jw__is_file(rom_abs)) {
                continue;
            }

            char title[PATH_MAX];
            char rom_rel[PATH_MAX];
            char image_abs[PATH_MAX];
            char image_rel[PATH_MAX];
            const char *image_path = NULL;

            jw__title_from_filename(file_entry->d_name, title, sizeof(title));
            char rom_rel_candidate[PATH_MAX];
            if (snprintf(rom_rel_candidate, sizeof(rom_rel_candidate), "Roms/%s/%s",
                         system_entry->d_name, file_entry->d_name) >=
                (int)sizeof(rom_rel_candidate) ||
                jw_storage_db_path_for_source(source, rom_rel_candidate, rom_abs,
                                              rom_rel, sizeof(rom_rel)) != 0) {
                continue;
            }
            if (jw__format_string(image_abs, sizeof(image_abs), "%s/%s/%s.png",
                                  source->images_path, system_entry->d_name, title) == 0 &&
                jw__format_string(image_rel, sizeof(image_rel), "Images/%s/%s.png",
                                  system_entry->d_name, title) == 0 &&
                jw__is_file(image_abs) &&
                jw_storage_db_path_for_source(source, image_rel, image_abs,
                                              image_rel, sizeof(image_rel)) == 0) {
                image_path = image_rel;
            } else if (jw__format_string(image_abs, sizeof(image_abs), "%s/%s/Imgs/%s.png",
                                         source->roms_path, system_entry->d_name, title) == 0 &&
                       jw__format_string(image_rel, sizeof(image_rel), "Roms/%s/Imgs/%s.png",
                                         system_entry->d_name, title) == 0 &&
                       jw__is_file(image_abs) &&
                       jw_storage_db_path_for_source(source, image_rel, image_abs,
                                                     image_rel, sizeof(image_rel)) == 0) {
                image_path = image_rel;
            }

            if (jw__scan_insert_game(tx, system_entry->d_name, title,
                                     rom_rel, image_path) == 0) {
                out->game_count += 1;
                system_has_games = 1;
            } else {
                closedir(files);
                closedir(systems);
                return -1;
            }
        }

        if (system_has_games) {
            out->system_count += 1;
        }

        closedir(files);
    }

    closedir(systems);
    return 0;
}

#define JW_M3U_MAX_MEMBERS 128
#define JW_M3U_MEMBER_LEN  512

/* Collect the disc files (.cue/.iso/.chd basenames) referenced by every .m3u in
   a system dir. Those member discs are then hidden from the game list so only
   the .m3u shows — one entry per game, and the m3u drives in-game disc swapping.
   Handles single-disc-with-m3u and multi-disc sets alike. */
static void jw__collect_m3u_members(const jw_ra_system *system, const char *system_dir,
                                    char members[][JW_M3U_MEMBER_LEN], int *member_count) {
    *member_count = 0;
    if (!system->playlist_extensions.count) {
        return;
    }
    DIR *dir = opendir(system_dir);
    if (!dir) {
        return;
    }
    struct dirent *entry;
    char ext[16];
    while ((entry = readdir(dir)) != NULL) {
        if (jw__is_hidden(entry->d_name)) {
            continue;
        }
        jw__extension_lower(entry->d_name, ext, sizeof(ext));
        if (!jw_ra_string_list_contains_casefold(&system->playlist_extensions, ext)) {
            continue;
        }
        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", system_dir, entry->d_name) >= (int)sizeof(path)) {
            continue;
        }
        FILE *fp = fopen(path, "r");
        if (!fp) {
            continue;
        }
        char line[512];
        while (fgets(line, sizeof(line), fp) != NULL) {
            char *s = line;
            while (*s == ' ' || *s == '\t') s++;
            size_t n = strlen(s);
            while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                             s[n - 1] == ' '  || s[n - 1] == '\t')) {
                s[--n] = '\0';
            }
            if (!s[0] || s[0] == '#') {
                continue;
            }
            /* Take the basename — m3u entries may be bare names or relative paths. */
            char *base = s;
            for (char *p = s; *p; p++) {
                if (*p == '/' || *p == '\\') base = p + 1;
            }
            if (base[0] && *member_count < JW_M3U_MAX_MEMBERS) {
                snprintf(members[*member_count], JW_M3U_MEMBER_LEN, "%s", base);
                (*member_count)++;
            }
        }
        fclose(fp);
    }
    closedir(dir);
}

static int jw__name_is_m3u_member(const char *name, char members[][JW_M3U_MEMBER_LEN],
                                  int member_count) {
    for (int i = 0; i < member_count; i++) {
        if (strcasecmp(name, members[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int jw__scan_roms_metadata(jw_scan_tx *tx,
                                  const char *sdcard_root,
                                  const jw_storage_source *source,
                                  const jw_ra_catalog *catalog,
                                  jw_scan_result *out) {
    DIR *systems = opendir(source->roms_path);
    if (!systems) {
        return 0;
    }

    char counted_systems[128][64];
    int counted_system_count = 0;
    struct dirent *system_entry;
    while ((system_entry = readdir(systems)) != NULL) {
        if (jw__is_hidden(system_entry->d_name)) {
            continue;
        }

        char system_dir[PATH_MAX];
        if (snprintf(system_dir, sizeof(system_dir), "%s/%s",
                     source->roms_path, system_entry->d_name) >= (int)sizeof(system_dir)) {
            continue;
        }
        if (!jw__is_directory(system_dir)) {
            continue;
        }

        const jw_ra_system *system = jw_ra_catalog_match_system_folder(catalog, system_entry->d_name);
        if (!system) {
            fprintf(stderr, "RetroArch discovery: skipping unknown ROM folder %s\n",
                    system_entry->d_name);
            continue;
        }
        if (!jw__metadata_system_has_packaged_launch_target(catalog, system, sdcard_root)) {
            fprintf(stderr,
                    "RetroArch discovery: skipping unsupported ROM folder %s "
                    "(%s has no packaged launch target)\n",
                    system_entry->d_name, system->id);
            continue;
        }

        DIR *files = opendir(system_dir);
        if (!files) {
            continue;
        }

        int system_has_games = 0;
        int logged_archive_policy = 0;
        int logged_m3u_policy = 0;

        /* Pre-pass: gather disc files referenced by any .m3u so they can be
           hidden below (one entry per game; the m3u handles disc swapping). */
        char m3u_members[JW_M3U_MAX_MEMBERS][JW_M3U_MEMBER_LEN];
        int m3u_member_count = 0;
        jw__collect_m3u_members(system, system_dir, m3u_members, &m3u_member_count);

        struct dirent *file_entry;
        while ((file_entry = readdir(files)) != NULL) {
            if (jw__is_hidden(file_entry->d_name) ||
                jw__is_private_rom_name(file_entry->d_name)) {
                continue;
            }

            char rom_abs[PATH_MAX];
            if (snprintf(rom_abs, sizeof(rom_abs), "%s/%s",
                         system_dir, file_entry->d_name) >= (int)sizeof(rom_abs)) {
                continue;
            }
            if (!jw__is_file(rom_abs)) {
                continue;
            }

            if (!jw__metadata_accepts_rom(system, file_entry->d_name,
                                          &logged_archive_policy,
                                          &logged_m3u_policy)) {
                continue;
            }

            /* Hide disc files (.cue/.iso/.chd) that an .m3u references — the
               .m3u is the single list entry for that game / multi-disc set. */
            if (m3u_member_count > 0) {
                char this_ext[16];
                jw__extension_lower(file_entry->d_name, this_ext, sizeof(this_ext));
                if (!jw_ra_string_list_contains_casefold(&system->playlist_extensions, this_ext) &&
                    jw__name_is_m3u_member(file_entry->d_name, m3u_members, m3u_member_count)) {
                    continue;
                }
            }

            char title[PATH_MAX];
            char rom_rel[PATH_MAX];
            char image_abs[PATH_MAX];
            char image_rel[PATH_MAX];
            const char *image_path = NULL;

            jw__title_from_metadata_filename(system, file_entry->d_name, title, sizeof(title));
            char rom_rel_candidate[PATH_MAX];
            if (snprintf(rom_rel_candidate, sizeof(rom_rel_candidate), "Roms/%s/%s",
                         system_entry->d_name, file_entry->d_name) >=
                (int)sizeof(rom_rel_candidate) ||
                jw_storage_db_path_for_source(source, rom_rel_candidate, rom_abs,
                                              rom_rel, sizeof(rom_rel)) != 0) {
                continue;
            }
            image_path = jw__metadata_image_path(system,
                                                 source,
                                                 system_entry->d_name,
                                                 title,
                                                 image_abs,
                                                 sizeof(image_abs),
                                                 image_rel,
                                                 sizeof(image_rel));

            if (jw__scan_insert_game(tx, system->id, title, rom_rel, image_path) == 0) {
                out->game_count += 1;
                system_has_games = 1;
            } else {
                closedir(files);
                closedir(systems);
                return -1;
            }
        }

        if (system_has_games) {
            jw__count_system_once(out, counted_systems, &counted_system_count, system->id);
        }

        closedir(files);
    }

    closedir(systems);
    return 0;
}

static int jw__scan_roms(jw_scan_tx *tx, const char *sdcard_root, jw_scan_result *out) {
    jw_storage_source_list sources;
    if (jw_storage_sources_resolve(sdcard_root, &sources) != 0) {
        return -1;
    }

    const char *disable_v2 = getenv("JAWAKA_DISABLE_RETROARCH_V2");
    if (disable_v2 && strcmp(disable_v2, "1") == 0) {
        fprintf(stderr, "RetroArch discovery: metadata disabled, using compatibility scanner\n");
        for (int i = 0; i < sources.count; i++) {
            if (jw__scan_roms_compat(tx, &sources.sources[i], out) != 0) {
                return -1;
            }
        }
        return 0;
    }

    char error[256];
    const jw_ra_catalog *catalog = jw_ra_catalog_get(sdcard_root, error, sizeof(error));
    if (!catalog) {
        fprintf(stderr, "RetroArch discovery: metadata unavailable (%s), using compatibility scanner\n",
                error[0] ? error : "unknown error");
        for (int i = 0; i < sources.count; i++) {
            if (jw__scan_roms_compat(tx, &sources.sources[i], out) != 0) {
                return -1;
            }
        }
        return 0;
    }

    for (int i = 0; i < sources.count; i++) {
        if (jw__scan_roms_metadata(tx, sdcard_root, &sources.sources[i], catalog, out) != 0) {
            return -1;
        }
    }
    return 0;
}

static void jw__trim_pak_suffix(const char *name, char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    if (jw__format_string(out, out_size, "%s", name ? name : "") != 0) {
        out[out_size - 1] = '\0';
    }
    size_t len = strlen(out);
    if (len > 4 && strcmp(out + len - 4, ".pak") == 0) {
        out[len - 4] = '\0';
    }
}

static const char *jw__scan_platform(void) {
    const char *platform = getenv("PLATFORM");
    if (platform && platform[0]) {
        return platform;
    }
    platform = jw_platform_compiled_id();
    return (platform && platform[0]) ? platform : "mac";
}

static bool jw__load_pak_manifest(const char *pak_abs,
                                  char *name_buf,
                                  size_t name_size,
                                  char *icon_buf,
                                  size_t icon_size,
                                  char *platform_buf,
                                  size_t platform_size,
                                  char *pak_version_buf,
                                  size_t pak_version_size,
                                  char *min_version_buf,
                                  size_t min_version_size) {
    char pak_json_path[PATH_MAX];
    if (jw__format_string(pak_json_path, sizeof(pak_json_path), "%s/pak.json", pak_abs) != 0 ||
        !jw__is_file(pak_json_path)) {
        fprintf(stderr, "App discovery: skipping %s (missing pak.json)\n", pak_abs);
        return false;
    }

    FILE *fp = fopen(pak_json_path, "rb");
    if (!fp) {
        fprintf(stderr, "App discovery: skipping %s (cannot open pak.json)\n", pak_abs);
        return false;
    }

    bool ok = false;
    if (fseek(fp, 0, SEEK_END) == 0) {
        long size = ftell(fp);
        if (size > 0 && fseek(fp, 0, SEEK_SET) == 0) {
            char *json = (char *)malloc((size_t)size + 1u);
            if (json) {
                size_t read_count = fread(json, 1, (size_t)size, fp);
                json[read_count] = '\0';
                cJSON *root = cJSON_Parse(json);
                if (root) {
                    cJSON *item = NULL;
                    item = cJSON_GetObjectItem(root, "name");
                    if (cJSON_IsString(item) && item->valuestring) {
                        snprintf(name_buf, name_size, "%s", item->valuestring);
                    }
                    item = cJSON_GetObjectItem(root, "icon");
                    if (cJSON_IsString(item) && item->valuestring) {
                        snprintf(icon_buf, icon_size, "%s", item->valuestring);
                    }
                    item = cJSON_GetObjectItem(root, "platform");
                    if (cJSON_IsString(item) && item->valuestring) {
                        snprintf(platform_buf, platform_size, "%s", item->valuestring);
                    }
                    item = cJSON_GetObjectItem(root, "pak_version");
                    if (cJSON_IsString(item) && item->valuestring) {
                        snprintf(pak_version_buf, pak_version_size, "%s", item->valuestring);
                    }
                    item = cJSON_GetObjectItem(root, "min_jawaka_version");
                    if (cJSON_IsString(item) && item->valuestring) {
                        snprintf(min_version_buf, min_version_size, "%s", item->valuestring);
                    }
                    ok = platform_buf[0] != '\0';
                    cJSON_Delete(root);
                }
                free(json);
            }
        }
    }
    fclose(fp);

    if (!ok) {
        fprintf(stderr, "App discovery: skipping %s (missing or invalid platform in pak.json)\n",
                pak_abs);
    }
    return ok;
}

static int jw__scan_apps_dir(jw_scan_tx *tx, const jw_storage_source *source,
                             const char *platform_dir,
                             const char *expected_platform,
                             jw_scan_result *out) {
    char apps_path[PATH_MAX];
    if (jw__format_string(apps_path, sizeof(apps_path), "%s/%s",
                          source->apps_path, platform_dir) != 0) {
        return -1;
    }

    DIR *apps = opendir(apps_path);
    if (!apps) {
        return 0;
    }

    struct dirent *entry;
    while ((entry = readdir(apps)) != NULL) {
        if (jw__is_hidden(entry->d_name)) {
            continue;
        }

        char pak_abs[PATH_MAX];
        if (jw__format_string(pak_abs, sizeof(pak_abs), "%s/%s",
                              apps_path, entry->d_name) != 0) {
            continue;
        }
        if (!jw__is_directory(pak_abs)) {
            continue;
        }

        char pak_rel[PATH_MAX];
        char pak_rel_candidate[PATH_MAX];
        char default_name[256];
        if (jw__format_string(pak_rel_candidate, sizeof(pak_rel_candidate),
                              "Apps/%s/%s", platform_dir, entry->d_name) != 0 ||
            jw_storage_db_path_for_source(source, pak_rel_candidate, pak_abs,
                                          pak_rel, sizeof(pak_rel)) != 0) {
            continue;
        }
        jw__trim_pak_suffix(entry->d_name, default_name, sizeof(default_name));

        char name_buf[256];
        char icon_buf[256];
        char platform_buf[64];
        char pak_version_buf[64];
        char min_version_buf[64];

        snprintf(name_buf, sizeof(name_buf), "%s", default_name);
        icon_buf[0] = '\0';
        platform_buf[0] = '\0';
        pak_version_buf[0] = '\0';
        min_version_buf[0] = '\0';

        if (!jw__load_pak_manifest(pak_abs,
                                   name_buf, sizeof(name_buf),
                                   icon_buf, sizeof(icon_buf),
                                   platform_buf, sizeof(platform_buf),
                                   pak_version_buf, sizeof(pak_version_buf),
                                   min_version_buf, sizeof(min_version_buf))) {
            continue;
        }

        if (strcmp(platform_buf, expected_platform) != 0) {
            fprintf(stderr,
                    "App discovery: skipping %s (manifest platform=%s, expected=%s)\n",
                    pak_abs, platform_buf, expected_platform);
            continue;
        }

        if (jw__scan_insert_app(tx, pak_rel, name_buf, icon_buf, platform_buf,
                                pak_version_buf, min_version_buf) == 0) {
            out->app_count += 1;
        } else {
            closedir(apps);
            return -1;
        }
    }

    closedir(apps);
    return 0;
}

static int jw__scan_apps_source(jw_scan_tx *tx, const jw_storage_source *source,
                                const char *platform,
                                jw_scan_result *out) {
    if (jw__scan_apps_dir(tx, source, platform, platform, out) != 0) {
        return -1;
    }
    if (strcmp(platform, "shared") != 0 &&
        jw__scan_apps_dir(tx, source, "shared", "shared", out) != 0) {
        return -1;
    }
    return 0;
}

static int jw__scan_apps(jw_scan_tx *tx, const char *sdcard_root, jw_scan_result *out) {
    jw_storage_source_list sources;
    if (jw_storage_sources_resolve(sdcard_root, &sources) != 0) {
        return -1;
    }
    const char *platform = jw__scan_platform();
    for (int i = 0; i < sources.count; i++) {
        if (jw__scan_apps_source(tx, &sources.sources[i], platform, out) != 0) {
            return -1;
        }
    }
    return 0;
}

static void jw__refresh_result_counts(sqlite3 *db, jw_scan_result *out) {
    if (!db || !out) {
        return;
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(DISTINCT system) FROM games;",
                           -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            out->system_count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    /* Recompute from the table rather than trusting the insert tally: alias
       dedup deletes rows after they were counted, so the tally would overcount. */
    stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM games;", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            out->game_count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
}

static int jw__dedup_folder_aliases(sqlite3 *db, const char *sdcard_root) {
    /* After folder folding, a legacy alias folder (e.g. Roms/FC) and the
       canonical public folder (Roms/NES) both resolve to one system, so the
       same title can land twice. Collapse each system's duplicates, preferring
       the copy under the canonical public folder. Only metadata mode folds
       folders, so compat mode (no catalog) has nothing to dedup. */
    char error[256];
    const jw_ra_catalog *catalog = jw_ra_catalog_get(sdcard_root, error, sizeof(error));
    if (!catalog) {
        return 0;
    }
    for (size_t i = 0; i < catalog->system_count; i++) {
        const jw_ra_system *system = &catalog->systems[i];
        if (!system->id || !system->id[0] || !system->rom_root || !system->rom_root[0]) {
            continue;
        }
        if (jw_db_dedup_system_aliases(db, system->id, system->rom_root) != 0) {
            return -1;
        }
    }
    return 0;
}

int jw_scan_library(sqlite3 *db, const char *sdcard_root, jw_scan_result *out) {
    if (!db || !sdcard_root || !out) {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    /* Non-destructive rescan: upsert keeps stable ids so favorites/recents
       survive, then prune only the rows whose ROM/pak vanished from disk. */
    jw_scan_tx tx;
    memset(&tx, 0, sizeof(tx));
    tx.db = db;

    if (jw_db_scan_begin(db) != 0 ||
        jw__scan_roms(&tx, sdcard_root, out) != 0 ||
        jw__scan_apps(&tx, sdcard_root, out) != 0 ||
        jw__scan_tx_commit(&tx) != 0) {
        jw__scan_tx_rollback(&tx);
        return -1;
    }

    if (jw__scan_tx_begin(&tx) != 0 ||
        jw__dedup_folder_aliases(db, sdcard_root) != 0 ||
        jw_db_scan_prune(db) != 0 ||
        jw__scan_tx_commit(&tx) != 0) {
        jw__scan_tx_rollback(&tx);
        return -1;
    }

    jw__refresh_result_counts(db, out);
    return 0;
}
