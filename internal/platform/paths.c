#include "internal/platform/paths.h"

#include "cJSON.h"

#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static char *jw__dup_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return NULL;
    }

    char *buf = (char *)malloc((size_t)needed + 1u);
    if (!buf) {
        va_end(args);
        return NULL;
    }

    vsnprintf(buf, (size_t)needed + 1u, fmt, args);
    va_end(args);
    return buf;
}

static int jw__mkdir_if_needed(const char *path, mode_t mode) {
    if (mkdir(path, mode) == 0 || errno == EEXIST) {
        return 0;
    }
    return -1;
}

static int jw__path_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

static int jw__is_directory(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static const char *jw__platform_id(void) {
#if defined(PLATFORM_MLP1)
    return "mlp1";
#else
    return "mac";
#endif
}

static char *jw__dup_realpath_or_literal(const char *path) {
    char resolved[PATH_MAX];
    if (path && realpath(path, resolved)) {
        return jw__dup_printf("%s", resolved);
    }
    return path ? jw__dup_printf("%s", path) : NULL;
}

static bool jw__path_is_within(const char *path, const char *root) {
    if (!path || !root) {
        return false;
    }
    size_t root_len = strlen(root);
    return strncmp(path, root, root_len) == 0 &&
           (path[root_len] == '\0' || path[root_len] == '/');
}

static bool jw__option_list_has(const char *list, const char *option) {
    if (!list || !option) {
        return false;
    }

    char needle[64];
    if (snprintf(needle, sizeof(needle), ",%s,", option) >= (int)sizeof(needle)) {
        return false;
    }

    char wrapped[4096];
    if (snprintf(wrapped, sizeof(wrapped), ",%s,", list) >= (int)sizeof(wrapped)) {
        return false;
    }

    return strstr(wrapped, needle) != NULL;
}

static void jw__set_error(char *error, size_t error_size, const char *message) {
    if (error && error_size > 0) {
        snprintf(error, error_size, "%s", message ? message : "");
    }
}

static char *jw__env_or_probe(const char *env_name, const char *relative_path,
                              const char *absolute_path) {
    const char *env = getenv(env_name);
    if (env && env[0]) {
        return jw__dup_realpath_or_literal(env);
    }

    if (relative_path && jw__path_exists(relative_path)) {
        return jw__dup_realpath_or_literal(relative_path);
    }

    if (absolute_path && jw__path_exists(absolute_path)) {
        return jw__dup_realpath_or_literal(absolute_path);
    }

    return absolute_path ? jw__dup_printf("%s", absolute_path) : NULL;
}

static char *jw__read_text_file(const char *path, size_t max_size) {
    if (!path || max_size == 0) {
        return NULL;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }

    long size = ftell(fp);
    if (size < 0 || (size_t)size > max_size) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);

    char *buf = (char *)malloc((size_t)size + 1u);
    if (!buf) {
        fclose(fp);
        return NULL;
    }

    size_t read_count = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    if (read_count != (size_t)size) {
        free(buf);
        return NULL;
    }

    buf[read_count] = '\0';
    return buf;
}

static char *jw__platform_defaults_path(const char *sdcard_root, const char *filename) {
    if (!sdcard_root || !filename || !filename[0]) {
        return NULL;
    }

    char path[PATH_MAX];
    int needed = snprintf(path, sizeof(path), "%s/UMRK/%s/defaults/%s",
                          sdcard_root, jw__platform_id(), filename);
    if (needed < 0 || needed >= (int)sizeof(path)) {
        return NULL;
    }

    return jw__path_exists(path) ? jw__dup_realpath_or_literal(path) : NULL;
}

static int jw__write_text_file_contents(FILE *fp, const char *path) {
    char *text = jw__read_text_file(path, 64u * 1024u);
    if (!text) {
        return -1;
    }

    fputs(text, fp);
    size_t len = strlen(text);
    if (len == 0 || text[len - 1] != '\n') {
        fputc('\n', fp);
    }

    free(text);
    return ferror(fp) ? -1 : 0;
}

static int jw__mkdir_child(const char *root, const char *name) {
    char path[PATH_MAX];
    if (!root || !name || snprintf(path, sizeof(path), "%s/%s", root, name) >= (int)sizeof(path)) {
        return -1;
    }
    return jw__mkdir_if_needed(path, 0755);
}

static const char *jw__username(void) {
    const char *user = getenv("USER");
    if (user && user[0]) {
        return user;
    }

    struct passwd *pwd = getpwuid(getuid());
    if (pwd && pwd->pw_name && pwd->pw_name[0]) {
        return pwd->pw_name;
    }

    return "anon";
}

char *jw_runtime_dir(void) {
    const char *env = getenv("JAWAKA_RUNTIME_DIR");
    char *path = NULL;

    if (env && env[0]) {
        path = jw__dup_printf("%s", env);
    } else {
        path = jw__dup_printf("/tmp/jawaka-%s", jw__username());
    }

    if (!path) {
        return NULL;
    }

    if (jw__mkdir_if_needed(path, 0700) != 0) {
        free(path);
        return NULL;
    }

    return path;
}

char *jw_sdcard_root(void) {
    const char *env = getenv("JAWAKA_SDCARD_ROOT");
    if (env && env[0]) {
        return jw__dup_printf("%s", env);
    }
    return jw__dup_printf("./mock-sdcard");
}

char *jw_state_dir(void) {
    char *sdcard_root = jw_sdcard_root();
    if (!sdcard_root) {
        return NULL;
    }

    char *primary = jw__dup_printf("%s/.umrk", sdcard_root);
    char *legacy = jw__dup_printf("%s/.jawaka", sdcard_root);
    if (!primary || !legacy) {
        free(primary);
        free(legacy);
        free(sdcard_root);
        return NULL;
    }

    char *selected = NULL;
    if (jw__is_directory(primary)) {
        selected = primary;
        primary = NULL;
    } else if (jw__is_directory(legacy)) {
        selected = legacy;
        legacy = NULL;
    } else if (jw__mkdir_if_needed(primary, 0755) == 0) {
        selected = primary;
        primary = NULL;
    }

    free(primary);
    free(legacy);
    free(sdcard_root);
    return selected;
}

char *jw_socket_path(void) {
    char *runtime_dir = jw_runtime_dir();
    if (!runtime_dir) {
        return NULL;
    }

    char *path = jw__dup_printf("%s/jawakad.sock", runtime_dir);
    free(runtime_dir);
    return path;
}

char *jw_db_path(void) {
    char *state_dir = jw_state_dir();
    if (!state_dir) {
        return NULL;
    }

    char *db_path = jw__dup_printf("%s/library.db", state_dir);
    free(state_dir);
    return db_path;
}

char *jw_retroarch_bin_path(void) {
#if defined(PLATFORM_MLP1)
    return jw__env_or_probe(
        "JAWAKA_RETROARCH_BIN",
        NULL,
        "/mnt/sdcard/UMRK/mlp1/bin/retroarch");
#else
    return jw__env_or_probe(
        "JAWAKA_RETROARCH_BIN",
        "../retroarch-builds/output/macos/RetroArch.app/Contents/MacOS/RetroArch",
        "/Volumes/Storage/UMRK/retroarch-builds/output/macos/RetroArch.app/Contents/MacOS/RetroArch");
#endif
}

static const char *jw__core_name_for_system(const char *system) {
    if (!system) return NULL;
#if defined(PLATFORM_MLP1)
    if (strcmp(system, "FC") == 0 || strcmp(system, "NES") == 0) return "fceumm_libretro.so";
    if (strcmp(system, "GB") == 0 || strcmp(system, "GBC") == 0) return "gambatte_libretro.so";
    if (strcmp(system, "GBA") == 0) return "mgba_libretro.so";
    if (strcmp(system, "MD") == 0 || strcmp(system, "GEN") == 0 ||
        strcmp(system, "GENESIS") == 0 || strcmp(system, "GG") == 0 ||
        strcmp(system, "MS") == 0) return "genesis_plus_gx_libretro.so";
    if (strcmp(system, "SFC") == 0 || strcmp(system, "SNES") == 0) return "snes9x_libretro.so";
    if (strcmp(system, "PS") == 0 || strcmp(system, "PSX") == 0) return "pcsx_rearmed_libretro.so";
    return NULL;
#else
    if (strcmp(system, "FC") == 0 || strcmp(system, "NES") == 0) return "fceumm_libretro.dylib";
    if (strcmp(system, "GB") == 0 || strcmp(system, "GBC") == 0) return "gambatte_libretro.dylib";
    if (strcmp(system, "GBA") == 0) return "mgba_libretro.dylib";
    if (strcmp(system, "MD") == 0 || strcmp(system, "GEN") == 0 ||
        strcmp(system, "GENESIS") == 0 || strcmp(system, "GG") == 0 ||
        strcmp(system, "MS") == 0) return "genesis_plus_gx_libretro.dylib";
    if (strcmp(system, "PS") == 0 || strcmp(system, "PSX") == 0) return "swanstation_libretro.dylib";
    return NULL;
#endif
}

static char *jw__core_name_from_defaults(const char *system) {
    if (!system || !system[0]) {
        return NULL;
    }

    char *sdcard_root = jw_sdcard_root();
    if (!sdcard_root) {
        return NULL;
    }

    char defaults_path[PATH_MAX];
    int needed = snprintf(defaults_path, sizeof(defaults_path),
                          "%s/UMRK/%s/defaults/cores.json",
                          sdcard_root, jw__platform_id());
    free(sdcard_root);
    if (needed < 0 || needed >= (int)sizeof(defaults_path)) {
        return NULL;
    }

    char *json = jw__read_text_file(defaults_path, 64u * 1024u);
    if (!json) {
        return NULL;
    }

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) {
        return NULL;
    }

    char *core_name = NULL;
    cJSON *systems = cJSON_GetObjectItemCaseSensitive(root, "systems");
    cJSON *entry = cJSON_IsObject(systems) ? cJSON_GetObjectItemCaseSensitive(systems, system) : NULL;
    if (cJSON_IsObject(entry)) {
        cJSON *core = cJSON_GetObjectItemCaseSensitive(entry, "core");
        if (cJSON_IsString(core) && core->valuestring && core->valuestring[0]) {
            core_name = jw__dup_printf("%s", core->valuestring);
        }
    } else if (cJSON_IsString(entry) && entry->valuestring && entry->valuestring[0]) {
        core_name = jw__dup_printf("%s", entry->valuestring);
    }

    cJSON_Delete(root);
    return core_name;
}

static char *jw__packaged_core_dir(void) {
    char *sdcard_root = jw_sdcard_root();
    if (!sdcard_root) {
        return NULL;
    }

    char path[PATH_MAX];
    int needed = snprintf(path, sizeof(path), "%s/UMRK/%s/cores",
                          sdcard_root, jw__platform_id());
    free(sdcard_root);
    if (needed < 0 || needed >= (int)sizeof(path) || !jw__is_directory(path)) {
        return NULL;
    }

    return jw__dup_realpath_or_literal(path);
}

static char *jw__default_cores_dir(void) {
    char *env_dir = jw__env_or_probe("JAWAKA_RETROARCH_CORES_DIR", NULL, NULL);
    if (env_dir) {
        return env_dir;
    }

    char *packaged_dir = jw__packaged_core_dir();
    if (packaged_dir) {
        return packaged_dir;
    }

#if defined(PLATFORM_MLP1)
    return jw__env_or_probe(
        "JAWAKA_RETROARCH_CORES_DIR",
        NULL,
        "/mnt/sdcard/UMRK/mlp1/cores");
#else
    return jw__env_or_probe(
        "JAWAKA_RETROARCH_CORES_DIR",
        "../Cores-spruce/output/macos/cores",
        "/Volumes/Storage/UMRK/Cores-spruce/output/macos/cores");
#endif
}

char *jw_retroarch_core_path_for_system(const char *system) {
    char *core_name = jw__core_name_from_defaults(system);
    if (!core_name) {
        const char *fallback_name = jw__core_name_for_system(system);
        if (!fallback_name) {
            return NULL;
        }
        core_name = jw__dup_printf("%s", fallback_name);
        if (!core_name) {
            return NULL;
        }
    }

    char *cores_dir = jw__default_cores_dir();
    if (!cores_dir) {
        free(core_name);
        return NULL;
    }

    char *path = jw__dup_printf("%s/%s", cores_dir, core_name);
    free(cores_dir);
    free(core_name);
    return path;
}

bool jw_sdcard_exec_available_for_path(const char *path, char *error, size_t error_size) {
#if !defined(PLATFORM_MLP1)
    (void)path;
    jw__set_error(error, error_size, "");
    return true;
#else
    jw__set_error(error, error_size, "");
    if (!path || !path[0]) {
        return true;
    }

    char *sdcard_root = jw_sdcard_root();
    if (!sdcard_root) {
        jw__set_error(error, error_size, "SD-card exec check failed: could not resolve SD root");
        return false;
    }

    char sdcard_abs[PATH_MAX];
    if (!realpath(sdcard_root, sdcard_abs)) {
        snprintf(sdcard_abs, sizeof(sdcard_abs), "%s", sdcard_root);
    }
    free(sdcard_root);

    char path_abs[PATH_MAX];
    if (!realpath(path, path_abs)) {
        snprintf(path_abs, sizeof(path_abs), "%s", path);
    }

    if (!jw__path_is_within(path_abs, sdcard_abs)) {
        return true;
    }

    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp) {
        jw__set_error(error, error_size,
                      "SD-card exec check failed: could not read /proc/mounts");
        return false;
    }

    char mountpoint[PATH_MAX];
    char opts[2048];
    bool found = false;
    while (fscanf(fp, "%*255s %4095s %*63s %2047s %*d %*d",
                  mountpoint, opts) == 2) {
        if (strcmp(mountpoint, sdcard_abs) == 0 || strcmp(mountpoint, "/mnt/sdcard") == 0) {
            found = true;
            break;
        }
    }
    fclose(fp);

    if (!found) {
        jw__set_error(error, error_size,
                      "SD-card exec check failed: no /mnt/sdcard mount entry");
        return false;
    }

    if (jw__option_list_has(opts, "noexec")) {
        jw__set_error(error, error_size,
                      "SD-card is mounted noexec; switcher remount failed or regressed");
        return false;
    }

    return true;
#endif
}

static void jw__retroarch_cfg_string(FILE *fp, const char *key, const char *value) {
    fprintf(fp, "%s = \"", key);
    for (const char *p = value; p && *p; p++) {
        if (*p == '\\' || *p == '"') {
            fputc('\\', fp);
        }
        fputc(*p, fp);
    }
    fprintf(fp, "\"\n");
}

static char *jw__core_info_dir(const char *core_path) {
    if (!core_path) {
        return NULL;
    }

    char core_dir[PATH_MAX];
    snprintf(core_dir, sizeof(core_dir), "%s", core_path);
    char *slash = strrchr(core_dir, '/');
    if (!slash) {
        return NULL;
    }
    *slash = '\0';

    char *info_dir = jw__dup_printf("%s/../info", core_dir);
    if (!info_dir) {
        return NULL;
    }

    char *resolved = jw__dup_realpath_or_literal(info_dir);
    free(info_dir);
    return resolved;
}

char *jw_write_retroarch_append_config(const char *runtime_dir, const char *sdcard_root,
                                       const char *core_path) {
    if (!runtime_dir || !sdcard_root || !core_path) {
        return NULL;
    }

    char sdroot_abs[PATH_MAX];
    if (!realpath(sdcard_root, sdroot_abs)) {
        snprintf(sdroot_abs, sizeof(sdroot_abs), "%s", sdcard_root);
    }

    if (jw__mkdir_child(sdroot_abs, "BIOS") != 0 ||
        jw__mkdir_child(sdroot_abs, "Saves") != 0 ||
        jw__mkdir_child(sdroot_abs, "States") != 0) {
        return NULL;
    }

    char *path = jw__dup_printf("%s/retroarch-launch-%ld.cfg", runtime_dir, (long)getpid());
    if (!path) {
        return NULL;
    }

    char system_dir[PATH_MAX];
    char saves_dir[PATH_MAX];
    char states_dir[PATH_MAX];
    snprintf(system_dir, sizeof(system_dir), "%s/BIOS", sdroot_abs);
    snprintf(saves_dir, sizeof(saves_dir), "%s/Saves", sdroot_abs);
    snprintf(states_dir, sizeof(states_dir), "%s/States", sdroot_abs);

    char core_dir[PATH_MAX];
    snprintf(core_dir, sizeof(core_dir), "%s", core_path);
    char *slash = strrchr(core_dir, '/');
    if (slash) {
        *slash = '\0';
    }

    char *info_dir = jw__core_info_dir(core_path);
    char *defaults_path = jw__platform_defaults_path(sdroot_abs, "retroarch.cfg");

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        free(defaults_path);
        free(info_dir);
        free(path);
        return NULL;
    }

    if (defaults_path) {
        jw__write_text_file_contents(fp, defaults_path);
    }
    jw__retroarch_cfg_string(fp, "system_directory", system_dir);
    jw__retroarch_cfg_string(fp, "savefile_directory", saves_dir);
    jw__retroarch_cfg_string(fp, "savestate_directory", states_dir);
    jw__retroarch_cfg_string(fp, "libretro_directory", core_dir);
    if (info_dir && jw__is_directory(info_dir)) {
        jw__retroarch_cfg_string(fp, "libretro_info_path", info_dir);
    }
    jw__retroarch_cfg_string(fp, "config_save_on_exit", "false");

    int failed = ferror(fp);
    if (fclose(fp) != 0) {
        failed = 1;
    }

    free(defaults_path);
    free(info_dir);
    if (failed) {
        unlink(path);
        free(path);
        return NULL;
    }

    return path;
}
