#include "internal/discovery/discovery.h"

#include "cJSON.h"
#include "internal/db/db.h"

#include <dirent.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static void jw__title_from_filename(const char *filename, char *out, size_t out_size) {
    snprintf(out, out_size, "%s", filename);
    char *dot = strrchr(out, '.');
    if (dot) {
        *dot = '\0';
    }
}

static int jw__scan_roms(sqlite3 *db, const char *sdcard_root, jw_scan_result *out) {
    char roms_root[PATH_MAX];
    char images_root[PATH_MAX];
    snprintf(roms_root, sizeof(roms_root), "%s/Roms", sdcard_root);
    snprintf(images_root, sizeof(images_root), "%s/Images", sdcard_root);

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
        snprintf(system_dir, sizeof(system_dir), "%s/%s", roms_root, system_entry->d_name);
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
            snprintf(rom_abs, sizeof(rom_abs), "%s/%s", system_dir, file_entry->d_name);
            if (!jw__is_file(rom_abs)) {
                continue;
            }

            char title[PATH_MAX];
            char rom_rel[PATH_MAX];
            char image_abs[PATH_MAX];
            char image_rel[PATH_MAX];
            const char *image_path = NULL;

            jw__title_from_filename(file_entry->d_name, title, sizeof(title));
            snprintf(rom_rel, sizeof(rom_rel), "Roms/%s/%s", system_entry->d_name, file_entry->d_name);
            snprintf(image_abs, sizeof(image_abs), "%s/%s/%s.png", images_root, system_entry->d_name, title);
            snprintf(image_rel, sizeof(image_rel), "Images/%s/%s.png", system_entry->d_name, title);
            if (jw__is_file(image_abs)) {
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

static void jw__trim_pak_suffix(const char *name, char *out, size_t out_size) {
    snprintf(out, out_size, "%s", name);
    size_t len = strlen(out);
    if (len > 4 && strcmp(out + len - 4, ".pak") == 0) {
        out[len - 4] = '\0';
    }
}

static int jw__scan_apps(sqlite3 *db, const char *sdcard_root, jw_scan_result *out) {
    char apps_root[PATH_MAX];
    snprintf(apps_root, sizeof(apps_root), "%s/Apps", sdcard_root);

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
        snprintf(pak_abs, sizeof(pak_abs), "%s/%s", apps_root, entry->d_name);
        if (!jw__is_directory(pak_abs)) {
            continue;
        }

        char pak_rel[PATH_MAX];
        char default_name[PATH_MAX];
        snprintf(pak_rel, sizeof(pak_rel), "Apps/%s", entry->d_name);
        jw__trim_pak_suffix(entry->d_name, default_name, sizeof(default_name));

        char pak_json_path[PATH_MAX];
        snprintf(pak_json_path, sizeof(pak_json_path), "%s/pak.json", pak_abs);

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

        if (jw__is_file(pak_json_path)) {
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

    if (jw_db_reset_library(db) != 0 ||
        jw__scan_roms(db, sdcard_root, out) != 0 ||
        jw__scan_apps(db, sdcard_root, out) != 0 ||
        jw__exec(db, "COMMIT;") != 0) {
        jw__exec(db, "ROLLBACK;");
        return -1;
    }

    return 0;
}
