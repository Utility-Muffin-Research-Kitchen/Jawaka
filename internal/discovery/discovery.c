#include "internal/discovery/discovery.h"

#include "cJSON.h"
#include "internal/db/db.h"
#include "internal/retroarch/catalog.h"

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

static int jw__sidecar_extension(const char *ext) {
    static const char *kSidecars[] = {
        "txt", "nfo", "db", "json", "xml", "png", "jpg", "jpeg",
        "bmp", "gif", "sav", "srm", "state", NULL
    };
    if (!ext || !ext[0]) {
        return 0;
    }
    for (size_t i = 0; kSidecars[i]; i++) {
        if (strcmp(ext, kSidecars[i]) == 0) {
            return 1;
        }
    }
    return 0;
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

    if (archive) {
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

    char ext[64];
    jw__extension_lower(filename, ext, sizeof(ext));
    if (!ext[0] || jw__sidecar_extension(ext)) {
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

static const char *jw__metadata_image_path(const jw_ra_system *system,
                                           const char *physical_folder,
                                           const char *title,
                                           const char *sdcard_root,
                                           char *image_abs,
                                           size_t image_abs_size,
                                           char *image_rel,
                                           size_t image_rel_size) {
    const char *canonical_root = system->image_root && system->image_root[0]
        ? system->image_root
        : NULL;
    if (canonical_root &&
        snprintf(image_abs, image_abs_size, "%s/%s/%s.png",
                 sdcard_root, canonical_root, title) < (int)image_abs_size &&
        snprintf(image_rel, image_rel_size, "%s/%s.png",
                 canonical_root, title) < (int)image_rel_size &&
        jw__is_file(image_abs)) {
        return image_rel;
    }

    if (physical_folder && physical_folder[0] &&
        snprintf(image_abs, image_abs_size, "%s/Images/%s/%s.png",
                 sdcard_root, physical_folder, title) < (int)image_abs_size &&
        snprintf(image_rel, image_rel_size, "Images/%s/%s.png",
                 physical_folder, title) < (int)image_rel_size &&
        jw__is_file(image_abs)) {
        return image_rel;
    }

    return NULL;
}

static int jw__scan_roms_compat(sqlite3 *db, const char *sdcard_root, jw_scan_result *out) {
    char roms_root[PATH_MAX];
    char images_root[PATH_MAX];
    if (jw__format_string(roms_root, sizeof(roms_root), "%s/Roms", sdcard_root) != 0 ||
        jw__format_string(images_root, sizeof(images_root), "%s/Images", sdcard_root) != 0) {
        return -1;
    }

    DIR *systems = opendir(roms_root);
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
                     roms_root, system_entry->d_name) >= (int)sizeof(system_dir)) {
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
            if (snprintf(rom_rel, sizeof(rom_rel), "Roms/%s/%s",
                         system_entry->d_name, file_entry->d_name) >= (int)sizeof(rom_rel)) {
                continue;
            }
            if (jw__format_string(image_abs, sizeof(image_abs), "%s/%s/%s.png",
                                  images_root, system_entry->d_name, title) == 0 &&
                jw__format_string(image_rel, sizeof(image_rel), "Images/%s/%s.png",
                                  system_entry->d_name, title) == 0 &&
                jw__is_file(image_abs)) {
                image_path = image_rel;
            }

            if (jw_db_insert_game(db, system_entry->d_name, title, rom_rel, image_path) == 0) {
                out->game_count += 1;
                system_has_games = 1;
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

static int jw__scan_roms_metadata(sqlite3 *db,
                                  const char *sdcard_root,
                                  const jw_ra_catalog *catalog,
                                  jw_scan_result *out) {
    char roms_root[PATH_MAX];
    if (snprintf(roms_root, sizeof(roms_root), "%s/Roms", sdcard_root) >=
        (int)sizeof(roms_root)) {
        return -1;
    }

    DIR *systems = opendir(roms_root);
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
                     roms_root, system_entry->d_name) >= (int)sizeof(system_dir)) {
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
        if (!jw__metadata_system_has_packaged_retroarch_core(catalog, system)) {
            fprintf(stderr,
                    "RetroArch discovery: skipping unsupported ROM folder %s "
                    "(%s has no packaged RetroArch core)\n",
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

            char title[PATH_MAX];
            char rom_rel[PATH_MAX];
            char image_abs[PATH_MAX];
            char image_rel[PATH_MAX];
            const char *image_path = NULL;

            jw__title_from_metadata_filename(system, file_entry->d_name, title, sizeof(title));
            if (snprintf(rom_rel, sizeof(rom_rel), "Roms/%s/%s",
                         system_entry->d_name, file_entry->d_name) >= (int)sizeof(rom_rel)) {
                continue;
            }
            image_path = jw__metadata_image_path(system,
                                                 system_entry->d_name,
                                                 title,
                                                 sdcard_root,
                                                 image_abs,
                                                 sizeof(image_abs),
                                                 image_rel,
                                                 sizeof(image_rel));

            if (jw_db_insert_game(db, system->id, title, rom_rel, image_path) == 0) {
                out->game_count += 1;
                system_has_games = 1;
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

static int jw__scan_roms(sqlite3 *db, const char *sdcard_root, jw_scan_result *out) {
    const char *disable_v2 = getenv("JAWAKA_DISABLE_RETROARCH_V2");
    if (disable_v2 && strcmp(disable_v2, "1") == 0) {
        fprintf(stderr, "RetroArch discovery: metadata disabled, using compatibility scanner\n");
        return jw__scan_roms_compat(db, sdcard_root, out);
    }

    char error[256];
    const jw_ra_catalog *catalog = jw_ra_catalog_get(sdcard_root, error, sizeof(error));
    if (!catalog) {
        fprintf(stderr, "RetroArch discovery: metadata unavailable (%s), using compatibility scanner\n",
                error[0] ? error : "unknown error");
        return jw__scan_roms_compat(db, sdcard_root, out);
    }

    return jw__scan_roms_metadata(db, sdcard_root, catalog, out);
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

static int jw__scan_apps(sqlite3 *db, const char *sdcard_root, jw_scan_result *out) {
    char apps_root[PATH_MAX];
    if (jw__format_string(apps_root, sizeof(apps_root), "%s/Apps", sdcard_root) != 0) {
        return -1;
    }

    DIR *apps = opendir(apps_root);
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
                              apps_root, entry->d_name) != 0) {
            continue;
        }
        if (!jw__is_directory(pak_abs)) {
            continue;
        }

        char pak_rel[PATH_MAX];
        char default_name[256];
        if (jw__format_string(pak_rel, sizeof(pak_rel), "Apps/%s", entry->d_name) != 0) {
            continue;
        }
        jw__trim_pak_suffix(entry->d_name, default_name, sizeof(default_name));

        char pak_json_path[PATH_MAX];
        int has_pak_json_path =
            jw__format_string(pak_json_path, sizeof(pak_json_path), "%s/pak.json", pak_abs) == 0;

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

        if (has_pak_json_path && jw__is_file(pak_json_path)) {
            FILE *fp = fopen(pak_json_path, "rb");
            if (fp) {
                fseek(fp, 0, SEEK_END);
                long size = ftell(fp);
                fseek(fp, 0, SEEK_SET);
                if (size > 0) {
                    char *json = (char *)malloc((size_t)size + 1u);
                    if (json) {
                        size_t read_count = fread(json, 1, (size_t)size, fp);
                        json[read_count] = '\0';
                        cJSON *root = cJSON_Parse(json);
                        if (root) {
                            cJSON *item = NULL;
                            item = cJSON_GetObjectItem(root, "name");
                            if (cJSON_IsString(item) && item->valuestring) snprintf(name_buf, sizeof(name_buf), "%s", item->valuestring);
                            item = cJSON_GetObjectItem(root, "icon");
                            if (cJSON_IsString(item) && item->valuestring) snprintf(icon_buf, sizeof(icon_buf), "%s", item->valuestring);
                            item = cJSON_GetObjectItem(root, "platform");
                            if (cJSON_IsString(item) && item->valuestring) snprintf(platform_buf, sizeof(platform_buf), "%s", item->valuestring);
                            item = cJSON_GetObjectItem(root, "pak_version");
                            if (cJSON_IsString(item) && item->valuestring) snprintf(pak_version_buf, sizeof(pak_version_buf), "%s", item->valuestring);
                            item = cJSON_GetObjectItem(root, "min_jawaka_version");
                            if (cJSON_IsString(item) && item->valuestring) snprintf(min_version_buf, sizeof(min_version_buf), "%s", item->valuestring);
                            cJSON_Delete(root);
                        }
                        free(json);
                    }
                }
                fclose(fp);
            }
        }

        if (jw_db_insert_app(db, pak_rel, name_buf, icon_buf, platform_buf, pak_version_buf, min_version_buf) == 0) {
            out->app_count += 1;
        }
    }

    closedir(apps);
    return 0;
}

int jw_scan_library(sqlite3 *db, const char *sdcard_root, jw_scan_result *out) {
    if (!db || !sdcard_root || !out) {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    if (jw__exec(db, "BEGIN IMMEDIATE;") != 0) {
        return -1;
    }

    /* Non-destructive rescan: upsert keeps stable ids so favorites/recents
       survive, then prune only the rows whose ROM/pak vanished from disk. */
    if (jw_db_scan_begin(db) != 0 ||
        jw__scan_roms(db, sdcard_root, out) != 0 ||
        jw__scan_apps(db, sdcard_root, out) != 0 ||
        jw_db_scan_prune(db) != 0 ||
        jw__exec(db, "COMMIT;") != 0) {
        jw__exec(db, "ROLLBACK;");
        return -1;
    }

    return 0;
}
