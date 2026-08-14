#include "internal/platform/paths.h"
#include "internal/platform/platform_id.h"
#include "internal/core/log.h"
#include "internal/retroarch/catalog.h"
#include "internal/retroarch/command.h"

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

#define JW_MLP1_RUNTIME_DIR "/tmp/jawaka-runtime"
#define JW_DESKTOP_RUNTIME_DIR_FMT "/tmp/jawaka-%s"

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

static bool jw__format_string(char *out, size_t out_size, const char *fmt, ...) {
    if (!out || out_size == 0) {
        return false;
    }

    va_list args;
    va_start(args, fmt);
    int needed = vsnprintf(out, out_size, fmt, args);
    va_end(args);
    return needed >= 0 && (size_t)needed < out_size;
}

bool jw_primary_recordings_path(char *out, size_t out_size,
                                const char *primary_sdcard_root) {
    if (!primary_sdcard_root || !primary_sdcard_root[0]) {
        return false;
    }
    return jw__format_string(out, out_size, "%s/Recordings", primary_sdcard_root);
}

static int jw__mkdir_if_needed(const char *path, mode_t mode) {
    if (mkdir(path, mode) == 0 || errno == EEXIST) {
        return 0;
    }
    return -1;
}

static int jw__mkdir_p(const char *path, mode_t mode) {
    if (!path || !path[0]) {
        return -1;
    }

    char buf[PATH_MAX];
    if (snprintf(buf, sizeof(buf), "%s", path) >= (int)sizeof(buf)) {
        return -1;
    }

    size_t len = strlen(buf);
    while (len > 1 && buf[len - 1] == '/') {
        buf[--len] = '\0';
    }

    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (buf[0] && jw__mkdir_if_needed(buf, mode) != 0) {
            return -1;
        }
        *p = '/';
    }

    return jw__mkdir_if_needed(buf, mode);
}

static int jw__path_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

static int jw__is_directory(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static char *jw__dup_realpath_or_literal(const char *path) {
    char resolved[PATH_MAX];
    if (path && realpath(path, resolved)) {
        return jw__dup_printf("%s", resolved);
    }
    return path ? jw__dup_printf("%s", path) : NULL;
}

typedef struct {
    const char *platform_id;
    const char *runtime_dir_absolute;
    const char *retroarch_bin_relative;
    const char *cores_relative;
    const char *core_library_suffix;
    const char *ps_core_name;
    bool requires_sdcard_exec_check;
} jw_path_layout;

static const jw_path_layout JW_PATH_LAYOUTS[] = {
    {
        "mlp1",
        JW_MLP1_RUNTIME_DIR,
        NULL,
        NULL,
        "_libretro.so",
        "pcsx_rearmed_libretro.so",
        true,
    },
    {
        "mac",
        NULL,
        "../retroarch-builds/output/macos/RetroArch.app/Contents/MacOS/RetroArch",
        "../Cores-spruce/output/macos/cores",
        "_libretro.dylib",
        "swanstation_libretro.dylib",
        false,
    },
};

static const jw_path_layout *jw__path_layout(void) {
    const char *platform_id = jw_platform_compiled_id();
    for (size_t i = 0; i < sizeof(JW_PATH_LAYOUTS) / sizeof(JW_PATH_LAYOUTS[0]); i++) {
        if (platform_id && strcmp(platform_id, JW_PATH_LAYOUTS[i].platform_id) == 0) {
            return &JW_PATH_LAYOUTS[i];
        }
    }
    return &JW_PATH_LAYOUTS[sizeof(JW_PATH_LAYOUTS) / sizeof(JW_PATH_LAYOUTS[0]) - 1u];
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

static const char *jw__env_value(const char *env_name) {
    const char *env = getenv(env_name);
    if (env && env[0]) {
        return env;
    }
    return NULL;
}

static bool jw__format_default_system_path(char *out, size_t out_size,
                                           const char *sdcard_root,
                                           const char *platform) {
    if (!out || out_size == 0 || !sdcard_root || !sdcard_root[0] ||
        !platform || !platform[0]) {
        return false;
    }

    return jw__format_string(out, out_size, "%s/.system/leaf/platforms/%s",
                             sdcard_root, platform);
}

static bool jw__format_default_system_child(char *out, size_t out_size,
                                            const char *sdcard_root,
                                            const char *child) {
    char system_path[PATH_MAX];
    if (!jw__format_default_system_path(system_path, sizeof(system_path), sdcard_root,
                                        jw_platform_compiled_id())) {
        return false;
    }
    return jw__format_string(out, out_size, "%s/%s", system_path, child);
}

/* Internal launcher state (library.db, release.json, wifi.conf, RetroArch
   config) lives under the SD root's .umrk/<platform> — separate from the
   release-managed system content under .system. Used only as the fallback when
   UMRK_INTERNAL_DATA_PATH is unset (native dev / tests); on device the session
   exports the real path. */
static bool jw__format_default_internal_data(char *out, size_t out_size,
                                             const char *sdcard_root) {
    if (!out || out_size == 0 || !sdcard_root || !sdcard_root[0]) {
        return false;
    }
    return jw__format_string(out, out_size, "%s/.umrk/%s",
                             sdcard_root, jw_platform_compiled_id());
}

static char *jw__dup_env_value(const char *env_name) {
    const char *env = jw__env_value(env_name);
    return env ? jw__dup_realpath_or_literal(env) : NULL;
}

static char *jw__env_or_probe(const char *env_name, const char *relative_path,
                              const char *fallback_path) {
    char *env = jw__dup_env_value(env_name);
    if (env) {
        return env;
    }

    if (relative_path && jw__path_exists(relative_path)) {
        return jw__dup_realpath_or_literal(relative_path);
    }

    if (fallback_path && jw__path_exists(fallback_path)) {
        return jw__dup_realpath_or_literal(fallback_path);
    }

    return fallback_path ? jw__dup_printf("%s", fallback_path) : NULL;
}

static char *jw__env_or_sd_child(const char *env_name, const char *sdroot_abs,
                                 const char *child) {
    char *env = jw__dup_env_value(env_name);
    if (env) {
        return env;
    }
    return (sdroot_abs && child) ? jw__dup_printf("%s/%s", sdroot_abs, child) : NULL;
}

static int jw__retroarch_storage_dirs(const char *sdroot_abs,
                                      char **system_dir,
                                      char **saves_dir,
                                      char **states_dir,
                                      char *error,
                                      size_t error_size) {
    if (!sdroot_abs || !system_dir || !saves_dir || !states_dir) {
        jw__set_error(error, error_size, "missing RetroArch storage inputs");
        return -1;
    }

    *system_dir = jw__env_or_sd_child("BIOS_PATH", sdroot_abs, "BIOS");
    *saves_dir = jw__env_or_sd_child("SAVES_PATH", sdroot_abs, "Saves");
    *states_dir = jw__env_or_sd_child("STATES_PATH", sdroot_abs, "States");
    if (!*system_dir || !*saves_dir || !*states_dir ||
        jw__mkdir_p(*system_dir, 0755) != 0 ||
        jw__mkdir_p(*saves_dir, 0755) != 0 ||
        jw__mkdir_p(*states_dir, 0755) != 0) {
        free(*system_dir);
        free(*saves_dir);
        free(*states_dir);
        *system_dir = NULL;
        *saves_dir = NULL;
        *states_dir = NULL;
        jw__set_error(error, error_size, "could not create RetroArch state folders");
        return -1;
    }

    return 0;
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
    const char *platform_root = jw__env_value("UMRK_PLATFORM_PATH");
    if (platform_root && snprintf(path, sizeof(path), "%s/defaults/%s",
                                  platform_root, filename) < (int)sizeof(path) &&
        jw__path_exists(path)) {
        return jw__dup_realpath_or_literal(path);
    }

    const char *system_root = jw__env_value("SYSTEM_PATH");
    if (system_root && snprintf(path, sizeof(path), "%s/defaults/%s",
                                system_root, filename) < (int)sizeof(path) &&
        jw__path_exists(path)) {
        return jw__dup_realpath_or_literal(path);
    }

    char child[PATH_MAX];
    if (!jw__format_string(child, sizeof(child), "defaults/%s", filename) ||
        !jw__format_default_system_child(path, sizeof(path), sdcard_root, child)) {
        return NULL;
    }

    return jw__path_exists(path) ? jw__dup_realpath_or_literal(path) : NULL;
}

static bool jw__retroarch_cfg_line_has_key(const char *line, size_t len, const char *key) {
    if (!line || !key) {
        return false;
    }

    while (len > 0 && (*line == ' ' || *line == '\t')) {
        line++;
        len--;
    }
    if (len == 0 || *line == '#') {
        return false;
    }

    size_t key_len = strlen(key);
    if (len < key_len || strncmp(line, key, key_len) != 0) {
        return false;
    }

    line += key_len;
    len -= key_len;
    while (len > 0 && (*line == ' ' || *line == '\t')) {
        line++;
        len--;
    }

    return len > 0 && *line == '=';
}

static bool jw__retroarch_cfg_line_key(const char *line, size_t len,
                                       char *out, size_t out_size) {
    if (!line || !out || out_size == 0) {
        return false;
    }

    while (len > 0 && (*line == ' ' || *line == '\t')) {
        line++;
        len--;
    }
    if (len == 0 || *line == '#') {
        return false;
    }

    const char *start = line;
    size_t key_len = 0;
    while (key_len < len && start[key_len] != '=' &&
           start[key_len] != ' ' && start[key_len] != '\t') {
        key_len++;
    }
    if (key_len == 0) {
        return false;
    }

    const char *cursor = start + key_len;
    size_t remain = len - key_len;
    while (remain > 0 && (*cursor == ' ' || *cursor == '\t')) {
        cursor++;
        remain--;
    }
    if (remain == 0 || *cursor != '=') {
        return false;
    }

    if (key_len >= out_size) {
        key_len = out_size - 1u;
    }
    memcpy(out, start, key_len);
    out[key_len] = '\0';
    return true;
}

static bool jw__retroarch_cfg_key_is_protected(const char *key) {
    static const char *protected_keys[] = {
        "system_directory",
        "savefile_directory",
        "savestate_directory",
        "sort_savefiles_enable",
        "sort_savestates_enable",
        "libretro_directory",
        "libretro_info_path",
        "config_save_on_exit",
        "network_cmd_enable",
        "network_cmd_port",
        "pause_nonactive",
        "screenshot_directory",
        "savestate_thumbnail_enable",
        "joypad_autoconfig_dir",
#ifdef PLATFORM_MLP1
        "audio_device",
        "audio_driver",
        "audio_block_frames",
        "audio_latency",
        "builtin_imageviewer_enable",
        "builtin_mediaplayer_enable",
        "check_firmware_before_loading",
        "load_dummy_on_core_shutdown",
        "menu_driver",
        "menu_scale_factor",
        "ozone_menu_color_theme",
        "user_language",
        "menu_show_load_content_animation",
        "menu_swap_ok_cancel_buttons",
        "notification_show_autoconfig",
        "notification_show_autoconfig_fails",
        "notification_show_config_override_load",
        "input_max_users",
        "savestate_file_compression",
        "video_threaded",
        "video_refresh_rate",
        "video_black_frame_insertion",
        "video_driver",
        "video_context_driver",
        "aspect_ratio_index",
        "video_force_aspect",
        "video_scale_integer",
        /* Recording. RetroArch owns these once it has run, and save-on-exit will
           persist whatever it had -- "wav" on any device predating this feature.
           Stripping them from the merge keeps the daemon the single source of
           truth, the same way the BFI and refresh-rate toggles work. */
        "record_driver",
        "video_record_config",
        "video_record_quality",
        "recording_output_directory",
#endif
        "input_player1_joypad_index",
        "input_player2_joypad_index",
        "input_player3_joypad_index",
        "input_player4_joypad_index",
        "cheevos_enable",
        "cheevos_username",
        "cheevos_password",
        /* RetroArch derives this from the baked-in account password; it is
           not a user-owned setting and must never merge into the shared
           FAT-card config as plaintext. The proxy's own cached token lives in
           its retained userdata instead. */
        "cheevos_token",
    };

    if (!key || !key[0]) {
        return false;
    }
    for (size_t i = 0; i < sizeof(protected_keys) / sizeof(protected_keys[0]); i++) {
        if (strcmp(key, protected_keys[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool jw__retroarch_cfg_text_has_key(const char *text, const char *key) {
    if (!text || !key || !key[0]) {
        return false;
    }

    const char *line = text;
    while (*line) {
        const char *next = strchr(line, '\n');
        size_t len = next ? (size_t)(next - line) : strlen(line);
        if (jw__retroarch_cfg_line_has_key(line, len, key)) {
            return true;
        }
        if (!next) {
            break;
        }
        line = next + 1;
    }
    return false;
}

/* first_match selects which duplicate wins when the text carries a key more
 * than once. RetroArch keeps the FIRST occurrence and discards the rest
 * (libretro-common/file/config_file.c), so any decision that must agree with
 * what RetroArch will actually do has to pass true. Passing false keeps the
 * historical last-wins behaviour for callers that predate this distinction. */
static char *jw__retroarch_cfg_text_string_value_ex(const char *text,
                                                    const char *wanted_key,
                                                    bool first_match) {
    if (!text || !wanted_key || !wanted_key[0]) {
        return NULL;
    }

    char *result = NULL;
    const char *line = text;
    while (*line) {
        const char *next = strchr(line, '\n');
        size_t len = next ? (size_t)(next - line) : strlen(line);
        char key[128];
        if (jw__retroarch_cfg_line_key(line, len, key, sizeof(key)) &&
            strcmp(key, wanted_key) == 0) {
            const char *equals = memchr(line, '=', len);
            if (equals) {
                const char *start = equals + 1;
                const char *end = line + len;
                while (start < end && (*start == ' ' || *start == '\t')) {
                    start++;
                }
                while (end > start &&
                       (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) {
                    end--;
                }
                if (end > start && *start == '"') {
                    start++;
                    const char *quote = memchr(start, '"', (size_t)(end - start));
                    if (quote) {
                        end = quote;
                    }
                }
                size_t value_len = (size_t)(end - start);
                char *value = malloc(value_len + 1u);
                if (!value) {
                    free(result);
                    return NULL;
                }
                memcpy(value, start, value_len);
                value[value_len] = '\0';
                free(result);
                result = value;
                if (first_match) {
                    return result;
                }
            }
        }
        if (!next) {
            break;
        }
        line = next + 1;
    }
    return result;
}

static char *jw__retroarch_cfg_text_string_value(const char *text,
                                                 const char *wanted_key) {
    return jw__retroarch_cfg_text_string_value_ex(text, wanted_key, false);
}

static bool jw__release_managed_shader_dir(const char *path) {
    static const char marker[] = "/.system/leaf/platforms/";
    if (!path || !path[0]) {
        return false;
    }
    const char *platform = strstr(path, marker);
    if (!platform) {
        return false;
    }
    platform += sizeof(marker) - 1u;
    const char *suffix = strchr(platform, '/');
    return suffix && strcmp(suffix, "/shaders") == 0;
}

static int jw__write_retroarch_cfg_filtered_skipping(
    FILE *fp, const char *text, const char *override_text, bool dedupe,
    const char *const *skip_keys, size_t skip_key_count) {
    if (!fp || !text) {
        return -1;
    }

    const char *line = text;
    while (*line) {
        const char *next = strchr(line, '\n');
        size_t len = next ? (size_t)(next - line) : strlen(line);
        char key[128];
        bool has_key = jw__retroarch_cfg_line_key(line, len, key, sizeof(key));
        bool skip = false;

        if (has_key) {
            for (size_t i = 0; i < skip_key_count && !skip; i++) {
                skip = skip_keys[i] && strcmp(key, skip_keys[i]) == 0;
            }
            skip = skip ||
                   jw__retroarch_cfg_key_is_protected(key) ||
                   (override_text && jw__retroarch_cfg_text_has_key(override_text, key)) ||
                   (dedupe && next && jw__retroarch_cfg_text_has_key(next + 1, key));
        } else if (len == strlen("# Jawaka protected runtime settings") &&
                   strncmp(line, "# Jawaka protected runtime settings", len) == 0) {
            skip = true;
        }

        if (!skip) {
            fwrite(line, 1, len, fp);
            fputc('\n', fp);
        }
        if (!next) {
            break;
        }
        line = next + 1;
    }

    return ferror(fp) ? -1 : 0;
}

static int jw__write_retroarch_cfg_filtered(FILE *fp, const char *text,
                                            const char *override_text,
                                            bool dedupe) {
    return jw__write_retroarch_cfg_filtered_skipping(
        fp, text, override_text, dedupe, NULL, 0);
}

static int jw__mkdir_child(const char *root, const char *name) {
    char path[PATH_MAX];
    if (!root || !name || snprintf(path, sizeof(path), "%s/%s", root, name) >= (int)sizeof(path)) {
        return -1;
    }
    return jw__mkdir_if_needed(path, 0755);
}

static int jw__copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) {
        return -1;
    }

    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }

    char buf[8192];
    int failed = 0;
    while (!feof(in)) {
        size_t n = fread(buf, 1, sizeof(buf), in);
        if (n > 0 && fwrite(buf, 1, n, out) != n) {
            failed = 1;
            break;
        }
        if (ferror(in)) {
            failed = 1;
            break;
        }
    }

    if (fclose(out) != 0) {
        failed = 1;
    }
    fclose(in);
    return failed ? -1 : 0;
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
    const char *env = jw__env_value("UMRK_RUNTIME_PATH");
    if (!env) {
        env = jw__env_value("JAWAKA_RUNTIME_DIR");
    }
    char *path = NULL;

    if (env && env[0]) {
        path = jw__dup_printf("%s", env);
    } else {
        const jw_path_layout *layout = jw__path_layout();
        if (layout->runtime_dir_absolute) {
            path = jw__dup_printf("%s", layout->runtime_dir_absolute);
        } else {
            path = jw__dup_printf(JW_DESKTOP_RUNTIME_DIR_FMT, jw__username());
        }
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
    const char *env = jw__env_value("SDCARD_PATH");
    if (!env) {
        env = jw__env_value("JAWAKA_SDCARD_ROOT");
    }
    if (env && env[0]) {
        return jw__dup_printf("%s", env);
    }
    return jw__dup_printf("./mock-sdcard");
}

char *jw_state_dir(void) {
    const char *env = jw__env_value("UMRK_INTERNAL_DATA_PATH");
    if (env && env[0]) {
        char *state_dir = jw__dup_realpath_or_literal(env);
        if (!state_dir) {
            return NULL;
        }
        if (jw__mkdir_if_needed(state_dir, 0755) != 0) {
            free(state_dir);
            return NULL;
        }
        return state_dir;
    }

    char *sdcard_root = jw_sdcard_root();
    if (!sdcard_root) {
        return NULL;
    }

    char state_path[PATH_MAX];
    if (!jw__format_default_internal_data(state_path, sizeof(state_path),
                                          sdcard_root)) {
        free(sdcard_root);
        return NULL;
    }

    char *state_dir = jw__dup_printf("%s", state_path);
    if (!state_dir) {
        free(sdcard_root);
        return NULL;
    }

    if (jw__mkdir_if_needed(state_dir, 0755) != 0) {
        free(state_dir);
        state_dir = NULL;
    }

    free(sdcard_root);
    return state_dir;
}

char *jw_socket_path(void) {
    const char *env = getenv("UMRK_DAEMON_SOCKET");
    if (!env || !env[0]) env = getenv("JAWAKA_SOCKET_PATH");
    if (env && env[0]) return jw__dup_printf("%s", env);

    char *runtime_dir = jw_runtime_dir();
    if (!runtime_dir) {
        return NULL;
    }

    char *path = jw__dup_printf("%s/jawakad.sock", runtime_dir);
    free(runtime_dir);
    return path;
}

char *jw_osd_socket_path(void) {
    const char *env = getenv("JAWAKA_OSD_SOCKET");
    if (env && env[0]) {
        return jw__dup_printf("%s", env);
    }

    char *runtime_dir = jw_runtime_dir();
    if (!runtime_dir) {
        return NULL;
    }

    char *path = jw__dup_printf("%s/jawaka-osd.sock", runtime_dir);
    free(runtime_dir);
    return path;
}

char *jw_ingame_ui_mode_path(void) {
    char *runtime_dir = jw_runtime_dir();
    if (!runtime_dir) {
        return NULL;
    }

    char *path = jw__dup_printf("%s/ingame-ui-mode", runtime_dir);
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

char *jw_retroarch_state_dir(const char *sdcard_root) {
    if (!sdcard_root || !sdcard_root[0]) {
        return NULL;
    }

    char sdroot_abs[PATH_MAX];
    if (!realpath(sdcard_root, sdroot_abs)) {
        if (!jw__format_string(sdroot_abs, sizeof(sdroot_abs), "%s", sdcard_root)) {
            return NULL;
        }
    }

    const char *internal = jw__env_value("UMRK_INTERNAL_DATA_PATH");
    char state_root[PATH_MAX];
    if (internal && internal[0]) {
        if (!jw__format_string(state_root, sizeof(state_root), "%s", internal)) {
            return NULL;
        }
    } else if (!jw__format_default_internal_data(state_root, sizeof(state_root),
                                                 sdroot_abs)) {
        return NULL;
    }
    if (jw__mkdir_if_needed(state_root, 0755) != 0) {
        return NULL;
    }

    if (jw__mkdir_child(state_root, "retroarch") != 0) {
        return NULL;
    }

    return jw__dup_printf("%s/retroarch", state_root);
}

/* ---- Emulated controller type ------------------------------------------
 *
 * Some cores default to a pad that lacks hardware the user expects. PS1 is the
 * live case: pcsx_rearmed's standard digital pad has no vibration motors, so
 * rumble is not weak on PlayStation games, it is structurally impossible.
 *
 * This cannot be pinned in retroarch.cfg, which is the surprise. RetroArch reads
 * input_libretro_device_p1 into its own settings quite happily -- the value reads
 * back correctly -- but never pushes it to the core at boot, so the core keeps
 * its default and the menu shows "standard" while the config says otherwise.
 * Remap files are applied *after* the core declares its controller list, and
 * RetroArch's remap loader calls retro_set_controller_port_device directly, which
 * is the same path the in-game menu uses. So the pin has to live in the remap.
 *
 * Verified on Puff 2026-07-27: Tekken 3 is silent through the config route and
 * rumbles through this one.
 *
 * The device ids are subclass-encoded and not published anywhere readable, so
 * they were taken out of the core binary itself -- the retro_controller_description
 * array, reached via the RELATIVE relocations that point at its name strings:
 *
 *     standard 1   dualshock 517   analog 261   negcon 773   guncon 260
 *
 * "analog" is the flight stick and has no motors; picking it would look like a
 * fix and do nothing. Re-derive the same way if a core bump ever moves them. */
/* RETRO_DEVICE_JOYPAD: the plain digital pad every core gets unless told
   otherwise, and therefore the only value we treat as "unchosen". */
#define JW_RA_DEVICE_JOYPAD 1u

typedef struct {
    const char *core_id;
    unsigned    device;
    const char *why;
} jw_core_device_pin;

static const jw_core_device_pin k_core_device_pins[] = {
    { "pcsx_rearmed", 517u, "DualShock, for rumble" },
};

/* Rewrite the core's remap, keeping every line the user (or RetroArch) put there
   except the one key we own. Same shape as the protected block in retroarch.cfg:
   Leaf owns this key, the rest is not ours to touch. Seeding only when the file
   is absent would miss anyone whose remap already exists with the wrong pad --
   which is exactly the upgrade case pinning exists for. */
static int jw__write_core_device_remap(const char *remap_path, unsigned device) {
    struct stat st;
    bool present = stat(remap_path, &st) == 0;
    char *existing = jw__read_text_file(remap_path, 256u * 1024u);
    /* A NULL read means "absent" or "there but unreadable", and those must not be
       treated alike: rewriting an unreadable file would replace the user's button
       remaps with a single line and never say so. Absent is fine, unreadable is a
       reason to leave it exactly where it is and go without the pin. */
    if (!existing && present) {
        jw_log_warn("controller pin: %s exists but could not be read; "
                    "leaving it untouched", remap_path);
        return -1;
    }
    /* An interior NUL is the other way a read "succeeds" on content we cannot
       safely rewrite: everything past it is invisible to the string walk below,
       so we would silently truncate the user's remaps and then commit the
       truncation. A half-flushed cluster after a power cut looks exactly like
       this on FAT32. */
    if (existing && present && strlen(existing) != (size_t)st.st_size) {
        jw_log_warn("controller pin: %s contains embedded NULs (%zu of %lld bytes "
                    "readable); leaving it untouched",
                    remap_path, strlen(existing), (long long)st.st_size);
        free(existing);
        return -1;
    }

    /* Build the result in memory first, so the common case -- the file already
       says what we want -- costs a read and a compare instead of a write, an
       fsync and a rename on every single launch. */
    char *wanted = NULL;
    size_t wanted_len = 0;
    FILE *mem = open_memstream(&wanted, &wanted_len);
    if (!mem) {
        free(existing);
        return -1;
    }

    /* Only correct a default, never a decision. RetroArch's own menu lets a user
       pick neGcon or a lightgun and save it to this very file, and silently
       reverting that on every launch would be us owning a key that is theirs.
       An absent value, or the standard digital pad, is what "nobody chose"
       looks like -- and it is also exactly what every install predating this
       has, so the upgrade case is untouched. */
    if (existing) {
        const char *line = existing;
        while (*line) {
            const char *eol = strchr(line, '\n');
            size_t len = eol ? (size_t)(eol - line) : strlen(line);
            if (jw__retroarch_cfg_line_has_key(line, len,
                                               "input_libretro_device_p1")) {
                unsigned current = 0;
                const char *eq = memchr(line, '=', len);
                if (eq && sscanf(eq + 1, " \"%u\"", &current) == 1 &&
                    current != JW_RA_DEVICE_JOYPAD && current != device) {
                    jw_log_info("controller pin: %s already selects device=%u; "
                                "leaving the user's choice alone",
                                remap_path, current);
                    fclose(mem);
                    free(wanted);
                    free(existing);
                    return 1;   /* deliberately not pinned; caller must not claim it was */
                }
                break;
            }
            if (!eol) {
                break;
            }
            line = eol + 1;
        }
    }

    bool replaced = false;
    if (existing) {
        const char *line = existing;
        while (*line) {
            const char *eol = strchr(line, '\n');
            size_t len = eol ? (size_t)(eol - line) : strlen(line);
            /* Match on the whole key, not a prefix. A bare strncmp would also
               swallow input_libretro_device_p10 through p16, which are real
               RetroArch keys, and deleting one of those is silent data loss. */
            if (jw__retroarch_cfg_line_has_key(line, len,
                                               "input_libretro_device_p1")) {
                /* Substituted where it stands rather than moved to the end.
                   RetroArch rewrites this file itself on exit in its own order,
                   so appending would make our output differ from its every time
                   and defeat the unchanged check below -- an fsync per launch
                   for a file that already says the right thing. Keeping the
                   position means the two agree and we stop writing. */
                if (!replaced) {
                    fprintf(mem, "input_libretro_device_p1 = \"%u\"\n", device);
                    replaced = true;
                }
            } else {
                fwrite(line, 1u, len, mem);
                fputc('\n', mem);
            }
            if (!eol) {
                break;
            }
            line = eol + 1;
        }
    }
    if (!replaced) {
        fprintf(mem, "input_libretro_device_p1 = \"%u\"\n", device);
    }

    int failed = ferror(mem) != 0;
    if (fclose(mem) != 0) {
        failed = 1;
    }
    if (failed || !wanted) {
        free(wanted);
        free(existing);
        return -1;
    }

    bool unchanged = existing && strcmp(existing, wanted) == 0;
    free(existing);
    if (unchanged) {
        free(wanted);
        return 0;   /* already correct: touch nothing */
    }

    char tmp_path[PATH_MAX];
    if (!jw__format_string(tmp_path, sizeof(tmp_path), "%s.tmp", remap_path)) {
        free(wanted);
        return -1;
    }

    FILE *fp = fopen(tmp_path, "wb");
    if (!fp) {
        free(wanted);
        return -1;
    }
    fwrite(wanted, 1u, wanted_len, fp);
    free(wanted);

    /* Get the bytes onto the card before the rename publishes them. fclose only
       reaches the page cache, and vfat has no journal, so a hard power-off in
       between leaves the directory entry pointing at a file whose contents were
       never written -- with the user's original already unlinked. This device
       gets hard-powered-off routinely. */
    failed = ferror(fp) != 0;
    if (!failed && (fflush(fp) != 0 || fsync(fileno(fp)) != 0)) {
        failed = 1;
    }
    if (fclose(fp) != 0) {
        failed = 1;
    }
    if (failed || rename(tmp_path, remap_path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

/* RetroArch loads the first remap it finds -- game, then content directory, then
   core -- and stops. A game remap therefore beats ours silently, and the user
   sees a log line saying the pin succeeded while their pad stays standard. We do
   not write game remaps (that is per-title and unbounded), but we can at least
   say the pin will not apply, which is the difference between a one-line
   diagnosis and an afternoon. */
/* RetroArch loads the FIRST remap it finds -- game, then content directory,
   then core -- and stops. Writing the core file while a higher-priority one
   exists produces a pin that is never read, and a log line claiming it worked.
   So resolve the file RetroArch will actually load. When none exists the core
   remap is the answer, because that is where RetroArch will look last and what
   we are entitled to create. */
static bool jw__resolve_winning_remap(char *out, size_t out_size,
                                      const char *remap_dir,
                                      const char *core_folder,
                                      const char *rom_path) {
    char candidate[PATH_MAX];

    if (rom_path && rom_path[0]) {
        const char *base = strrchr(rom_path, '/');
        base = base ? base + 1 : rom_path;

        /* Game remap: the ROM's own name without its extension. */
        char stem[256];
        if (jw__format_string(stem, sizeof(stem), "%s", base)) {
            char *dot = strrchr(stem, '.');
            if (dot && dot != stem) *dot = '\0';
            if (jw__format_string(candidate, sizeof(candidate), "%s/%s.rmp",
                                  remap_dir, stem) &&
                jw__path_exists(candidate)) {
                return jw__format_string(out, out_size, "%s", candidate);
            }
        }

        /* Content-directory remap: the name of the folder holding the ROM. */
        const char *slash = strrchr(rom_path, '/');
        if (slash && slash != rom_path) {
            const char *dir_end = slash;
            const char *dir_start = dir_end;
            while (dir_start > rom_path && dir_start[-1] != '/') dir_start--;
            size_t dir_len = (size_t)(dir_end - dir_start);
            char dir_name[256];
            if (dir_len > 0 && dir_len < sizeof(dir_name)) {
                memcpy(dir_name, dir_start, dir_len);
                dir_name[dir_len] = '\0';
                if (jw__format_string(candidate, sizeof(candidate), "%s/%s.rmp",
                                      remap_dir, dir_name) &&
                    jw__path_exists(candidate)) {
                    return jw__format_string(out, out_size, "%s", candidate);
                }
            }
        }
    }

    return jw__format_string(out, out_size, "%s/%s.rmp", remap_dir, core_folder);
}

/* One-time migration marker. The pin exists to move installs off RetroArch's
   default pad, and a migration is by definition something that happens once --
   re-running it every launch turns it into permanent ownership of a key the
   user can edit in RetroArch's own menu, and makes "I want Standard" an opinion
   the daemon overwrites forever. After this marker exists the file is theirs. */
static bool jw__controller_pin_done(const char *ra_home, const char *core_id) {
    char stamp[PATH_MAX];
    if (!jw__format_string(stamp, sizeof(stamp), "%s/.leaf-controller-pin-%s-v1",
                           ra_home, core_id)) {
        return true;   /* cannot name it: treat as done rather than loop forever */
    }
    return jw__path_exists(stamp);
}

static void jw__controller_pin_mark_done(const char *ra_home, const char *core_id) {
    char stamp[PATH_MAX];
    if (!jw__format_string(stamp, sizeof(stamp), "%s/.leaf-controller-pin-%s-v1",
                           ra_home, core_id)) {
        return;
    }
    FILE *fp = fopen(stamp, "wb");
    if (fp) {
        fclose(fp);
    }
}

void jw_retroarch_pin_core_device(const char *ra_home, const char *core_id,
                                  const char *core_config_folder,
                                  const char *rom_path) {
    if (!ra_home || !ra_home[0] || !core_id || !core_id[0] ||
        !core_config_folder || !core_config_folder[0]) {
        return;
    }
    /* The folder is a name, not a path: it comes from the catalog and is used
       verbatim. Refuse anything that could climb out of the remap directory. */
    if (strchr(core_config_folder, '/') || strcmp(core_config_folder, "..") == 0 ||
        strcmp(core_config_folder, ".") == 0) {
        return;
    }

    const jw_core_device_pin *pin = NULL;
    for (size_t i = 0; i < sizeof(k_core_device_pins) / sizeof(k_core_device_pins[0]); i++) {
        if (strcmp(core_id, k_core_device_pins[i].core_id) == 0) {
            pin = &k_core_device_pins[i];
            break;
        }
    }
    if (!pin) {
        return;   /* the overwhelmingly common path */
    }
    if (jw__controller_pin_done(ra_home, core_id)) {
        return;   /* migrated already; the file belongs to the user now */
    }

    char remap_dir[PATH_MAX];
    if (!jw__format_string(remap_dir, sizeof(remap_dir),
                           "%s/.config/retroarch/config/remaps/%s",
                           ra_home, core_config_folder)) {
        jw_log_warn("rumble: remap path too long for %s", core_id);
        return;
    }
    if (jw__mkdir_p(remap_dir, 0755) != 0) {
        jw_log_warn("rumble: could not create remap dir %s: %s",
                    remap_dir, strerror(errno));
        return;
    }

    /* Migrate the file RetroArch will actually read, not the one we would
       prefer to write. A game or content-directory remap outranks the core one,
       so pinning the core file there would be a no-op wearing a success log. */
    char remap_path[PATH_MAX];
    if (!jw__resolve_winning_remap(remap_path, sizeof(remap_path), remap_dir,
                                   core_config_folder, rom_path)) {
        return;
    }

    /* Three outcomes, and they must not be conflated in the log: pinned,
       deliberately left alone, and failed. The callee reports the specific
       reason for the latter two (errno is meaningless for most of them, so
       don't dress them up as write errors here). */
    int rc = jw__write_core_device_remap(remap_path, pin->device);
    if (rc < 0) {
        jw_log_warn("controller pin: %s not pinned", remap_path);
        return;   /* no stamp: a failed migration should be retried */
    }
    if (rc == 0) {
        jw_log_info("controller pin: %s port 1 device=%u (%s) in %s",
                    core_id, pin->device, pin->why, remap_path);
    }
    /* Stamp on "pinned" AND on "left the user's choice alone". Both are a
       finished migration -- the second is a decision, not a failure, and
       re-deciding it every launch is exactly what F4 was about. */
    jw__controller_pin_mark_done(ra_home, core_id);
}

char *jw_retroarch_bin_path(void) {
    const jw_path_layout *layout = jw__path_layout();
    char *path = jw__dup_env_value("UMRK_RETROARCH_BIN");
    if (!path) {
        path = jw__dup_env_value("JAWAKA_RETROARCH_BIN");
    }
    if (path) {
        return path;
    }

    const char *system_path = jw__env_value("SYSTEM_PATH");
    if (system_path) {
        char candidate[PATH_MAX];
        if (snprintf(candidate, sizeof(candidate), "%s/bin/retroarch", system_path) <
            (int)sizeof(candidate)) {
            return jw__env_or_probe("UMRK_RETROARCH_BIN", NULL, candidate);
        }
    }

    char *sdcard_root = jw_sdcard_root();
    if (sdcard_root) {
        char candidate[PATH_MAX];
        bool ok = jw__format_default_system_child(candidate, sizeof(candidate), sdcard_root,
                                                  "bin/retroarch");
        free(sdcard_root);
        if (ok) {
            path = jw__env_or_probe("UMRK_RETROARCH_BIN", layout->retroarch_bin_relative,
                                    candidate);
            if (path) {
                return path;
            }
        }
    }

    return jw__env_or_probe("UMRK_RETROARCH_BIN", layout->retroarch_bin_relative, NULL);
}

static const char *jw__core_name_for_system(const char *system) {
    if (!system) return NULL;
    const jw_path_layout *layout = jw__path_layout();
    const char *base = NULL;
    if (strcmp(system, "FC") == 0 || strcmp(system, "NES") == 0) base = "fceumm";
    else if (strcmp(system, "GB") == 0 || strcmp(system, "GBC") == 0) base = "gambatte";
    else if (strcmp(system, "GBA") == 0) base = "mgba";
    else if (strcmp(system, "MD") == 0 || strcmp(system, "GEN") == 0 ||
             strcmp(system, "GENESIS") == 0 || strcmp(system, "GG") == 0 ||
             strcmp(system, "MS") == 0) base = "genesis_plus_gx";
    else if (strcmp(system, "SFC") == 0 || strcmp(system, "SNES") == 0) base = "snes9x";
    else if (strcmp(system, "PS") == 0 || strcmp(system, "PSX") == 0) return layout->ps_core_name;
    else return NULL;

    static char core_name[96];
    snprintf(core_name, sizeof(core_name), "%s%s", base, layout->core_library_suffix);
    return core_name;
}

static char *jw__core_name_from_defaults(const char *system) {
    if (!system || !system[0]) {
        return NULL;
    }

    char *sdcard_root = jw_sdcard_root();
    if (!sdcard_root) {
        return NULL;
    }

    char *defaults_path = jw__platform_defaults_path(sdcard_root, "cores.json");
    free(sdcard_root);
    if (!defaults_path) {
        return NULL;
    }

    char *json = jw__read_text_file(defaults_path, 64u * 1024u);
    free(defaults_path);
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
    const char *system_path = jw__env_value("SYSTEM_PATH");
    if (system_path && system_path[0]) {
        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/cores", system_path) < (int)sizeof(path) &&
            jw__is_directory(path)) {
            return jw__dup_realpath_or_literal(path);
        }
    }

    char *sdcard_root = jw_sdcard_root();
    if (!sdcard_root) {
        return NULL;
    }

    char path[PATH_MAX];
    bool ok = jw__format_default_system_child(path, sizeof(path), sdcard_root, "cores");
    free(sdcard_root);
    if (!ok || !jw__is_directory(path)) {
        return NULL;
    }

    return jw__dup_realpath_or_literal(path);
}

static char *jw__default_cores_dir(void) {
    char *env_dir = jw__dup_env_value("CORES_PATH");
    if (!env_dir) {
        env_dir = jw__dup_env_value("JAWAKA_RETROARCH_CORES_DIR");
    }
    if (env_dir) {
        return env_dir;
    }

    char *packaged_dir = jw__packaged_core_dir();
    if (packaged_dir) {
        return packaged_dir;
    }

    const jw_path_layout *layout = jw__path_layout();
    return jw__env_or_probe("CORES_PATH", layout->cores_relative, NULL);
}

char *jw_retroarch_core_path_for_system_choice(const char *system,
                                               const char *preferred_core_id,
                                               char *out_core_id,
                                               size_t out_core_id_size,
                                               char *out_config_folder,
                                               size_t out_config_folder_size,
                                               char *diagnostic,
                                               size_t diagnostic_size) {
    if (!system || !system[0]) {
        return NULL;
    }
    if (out_core_id && out_core_id_size > 0) {
        out_core_id[0] = '\0';
    }
    if (out_config_folder && out_config_folder_size > 0) {
        out_config_folder[0] = '\0';
    }
    if (diagnostic && diagnostic_size > 0) {
        diagnostic[0] = '\0';
    }

    const char *disable_v2 = getenv("JAWAKA_DISABLE_RETROARCH_V2");
    if (!disable_v2 || strcmp(disable_v2, "1") != 0) {
        char *sdcard_root = jw_sdcard_root();
        char *cores_dir = jw__default_cores_dir();
        char error[256];
        const jw_ra_catalog *catalog = jw_ra_catalog_get(sdcard_root, error, sizeof(error));
        if (catalog && cores_dir) {
            char core_file[PATH_MAX];
            char core_id[128];
            char local_diagnostic[256];
            if (jw_ra_catalog_resolve_core_file_for_choice(catalog, system,
                                                           preferred_core_id,
                                                           cores_dir,
                                                           core_file, sizeof(core_file),
                                                           core_id, sizeof(core_id),
                                                           local_diagnostic,
                                                           sizeof(local_diagnostic)) == 0) {
                if (local_diagnostic[0]) {
                    jw_log_warn("RetroArch metadata fallback for %s: %s", system, local_diagnostic);
                } else {
                    jw_log_info("RetroArch metadata resolved %s -> %s", system, core_id);
                }
                if (out_core_id && out_core_id_size > 0) {
                    snprintf(out_core_id, out_core_id_size, "%s", core_id);
                }
                const jw_ra_core *resolved_core =
                    jw_ra_catalog_find_core(catalog, core_id);
                if (out_config_folder && out_config_folder_size > 0 &&
                    resolved_core &&
                    jw_ra_core_folder_is_safe(resolved_core->config_folder) &&
                    strlen(resolved_core->config_folder) < out_config_folder_size) {
                    snprintf(out_config_folder, out_config_folder_size, "%s",
                             resolved_core->config_folder);
                }
                if (diagnostic && diagnostic_size > 0 && local_diagnostic[0]) {
                    snprintf(diagnostic, diagnostic_size, "%s", local_diagnostic);
                }
                char *path = jw__dup_printf("%s/%s", cores_dir, core_file);
                free(sdcard_root);
                free(cores_dir);
                return path;
            }
            if (local_diagnostic[0]) {
                jw_log_warn("RetroArch metadata could not resolve %s: %s", system, local_diagnostic);
                if (diagnostic && diagnostic_size > 0) {
                    snprintf(diagnostic, diagnostic_size, "%s", local_diagnostic);
                }
            } else {
                jw_log_warn("RetroArch metadata could not resolve %s", system);
            }
        } else if (error[0]) {
            jw_log_warn("RetroArch metadata unavailable: %s", error);
            if (diagnostic && diagnostic_size > 0) {
                snprintf(diagnostic, diagnostic_size, "%s", error);
            }
        }
        free(sdcard_root);
        free(cores_dir);
    }

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
    if (path && out_core_id && out_core_id_size > 0) {
        snprintf(out_core_id, out_core_id_size, "%s", core_name);
        char *suffix = strstr(out_core_id, "_libretro.so");
        if (suffix) {
            *suffix = '\0';
        }
    }
    free(cores_dir);
    free(core_name);
    return path;
}

char *jw_retroarch_core_path_for_system(const char *system) {
    return jw_retroarch_core_path_for_system_choice(system, NULL, NULL, 0,
                                                    NULL, 0, NULL, 0);
}

bool jw_sdcard_exec_available_for_path(const char *path, char *error, size_t error_size) {
    const jw_path_layout *layout = jw__path_layout();
    jw__set_error(error, error_size, "");
    if (!layout->requires_sdcard_exec_check) {
        (void)path;
        return true;
    }

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

    char mountpoint[4096];
    char opts[2048];
    char line[8192];
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "%*s %4095s %*s %2047s", mountpoint, opts) != 2) {
            continue;
        }
        if (strcmp(mountpoint, sdcard_abs) == 0) {
            found = true;
            break;
        }
    }
    fclose(fp);

    if (!found) {
        jw__set_error(error, error_size,
                      "SD-card exec check failed: no SD-card mount entry");
        return false;
    }

    if (jw__option_list_has(opts, "noexec")) {
        jw__set_error(error, error_size,
                      "SD-card is mounted noexec; switcher remount failed or regressed");
        return false;
    }

    return true;
}

#ifdef PLATFORM_MLP1
/* Number of players the launch roster actually filled (indices are assigned
   in roster order, so this is just the count of non-negative entries). */
static int jw__roster_user_count(const int player_joypad_indices[4]) {
    int count = 0;
    for (int user = 0; user < 4; user++) {
        if (player_joypad_indices[user] >= 0) {
            count++;
        }
    }
    return count > 0 ? count : 1;
}
#endif

/* Write input_max_users plus one joypad index per roster member.
   RetroArch keeps these indices in an unsigned array and uses them to index
   its pad state, so writing a "-1" sentinel for an unused player wraps to a
   huge index and segfaults on the first input poll. Bound input_max_users by
   the roster instead and emit indices only for players that have a pad: the
   child's private /dev/input already guarantees no other joystick exists, so
   there is nothing left for a phantom player to bind to. */
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

#ifdef PLATFORM_MLP1
static void jw__retroarch_cfg_max_users(FILE *fp,
                                        const int player_joypad_indices[4]) {
    char value[16];
    if (!player_joypad_indices) {
        jw__retroarch_cfg_string(fp, "input_max_users", "1");
        return;
    }
    snprintf(value, sizeof(value), "%d",
             jw__roster_user_count(player_joypad_indices));
    jw__retroarch_cfg_string(fp, "input_max_users", value);
}
#endif

static void jw__retroarch_cfg_player_indices(
        FILE *fp, const int player_joypad_indices[4]) {
    if (!player_joypad_indices) {
        return;
    }
    for (int user = 0; user < 4; user++) {
        if (player_joypad_indices[user] < 0) {
            continue;
        }
        char key[40];
        char value[16];
        snprintf(key, sizeof(key), "input_player%d_joypad_index", user + 1);
        snprintf(value, sizeof(value), "%d", player_joypad_indices[user]);
        jw__retroarch_cfg_string(fp, key, value);
    }
}

#ifdef PLATFORM_MLP1
static void jw__mlp1_retroarch_audio_cfg(FILE *fp) {
    const char *output = jw__env_value("JAWAKA_AUDIO_OUTPUT");
    if (!output || !output[0]) {
        output = jw__env_value("UMRK_AUDIO_OUTPUT");
    }
    const char *device = jw__env_value("JAWAKA_AUDIO_DEVICE");
    if (!device || !device[0]) {
        device = jw__env_value("UMRK_AUDIO_DEVICE");
    }

    if (output && strcmp(output, "BLUETOOTH") == 0) {
        /* Bluetooth is served by BlueALSA (a2dp-source), which exposes a raw
           ALSA `bluealsa` pcm and is *not* a PulseAudio sink. Keep the ALSA
           driver for this path so BT playback keeps working. */
        jw__retroarch_cfg_string(fp, "audio_driver", "alsa");
        jw__retroarch_cfg_string(fp, "audio_device", "bluealsa");
        return;
    }

    /* Speaker/HDMI: play through PulseAudio natively. The rk817 codec is owned
       by the PulseAudio daemon; routing through the ALSA `default` pcm (the
       libasound pulse plugin) gave RetroArch bogus buffer-fill readings, so its
       audio sync / dynamic-rate-control mis-corrected after every savestate
       load and left the game running with laggy, desynced audio. The native
       pulse driver queries real PA latency and survives state loads. */
    jw__retroarch_cfg_string(fp, "audio_driver", "pulse");

    /* With the pulse driver, audio_device names a PulseAudio *sink*, not an
       ALSA pcm. Honor an explicit sink name from the env, but ignore the
       "default" sentinel (an ALSA pcm name that PA only tolerates by luck) and
       fall through to PA's real default sink — what the launcher's volume
       buttons (pactl set-sink-volume) operate on. */
    if (device && device[0] && strcmp(device, "default") != 0) {
        jw__retroarch_cfg_string(fp, "audio_device", device);
    } else {
        jw__retroarch_cfg_string(fp, "audio_device", "");
    }
}
#endif

static char *jw__core_info_dir(const char *core_path) {
    if (!core_path) {
        return NULL;
    }

    char core_dir[PATH_MAX];
    if (!jw__format_string(core_dir, sizeof(core_dir), "%s", core_path)) {
        return NULL;
    }
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

static char *jw__default_info_dir(const char *sdcard_root, const char *core_dir) {
    char *env_dir = jw__dup_env_value("INFO_PATH");
    if (env_dir) {
        return env_dir;
    }

    const char *system_path = jw__env_value("SYSTEM_PATH");
    if (system_path && system_path[0]) {
        char path[PATH_MAX];
        if (jw__format_string(path, sizeof(path), "%s/info", system_path) &&
            jw__is_directory(path)) {
            return jw__dup_realpath_or_literal(path);
        }
    }

    if (sdcard_root && sdcard_root[0]) {
        char path[PATH_MAX];
        if (jw__format_default_system_child(path, sizeof(path), sdcard_root, "info") &&
            jw__is_directory(path)) {
            return jw__dup_realpath_or_literal(path);
        }
    }

    if (core_dir && core_dir[0]) {
        char path[PATH_MAX];
        if (jw__format_string(path, sizeof(path), "%s/../info", core_dir) &&
            jw__is_directory(path)) {
            return jw__dup_realpath_or_literal(path);
        }
    }

    return NULL;
}

static char *jw__default_autoconfig_dir(const char *sdcard_root) {
    const char *platform_path = jw__env_value("UMRK_PLATFORM_PATH");
    char path[PATH_MAX];
    if (platform_path && jw__format_string(path, sizeof(path), "%s/autoconfig",
                                           platform_path) &&
        jw__is_directory(path)) {
        return jw__dup_realpath_or_literal(path);
    }

    const char *system_path = jw__env_value("SYSTEM_PATH");
    if (system_path && jw__format_string(path, sizeof(path), "%s/autoconfig",
                                         system_path) &&
        jw__is_directory(path)) {
        return jw__dup_realpath_or_literal(path);
    }

    if (sdcard_root &&
        jw__format_default_system_child(path, sizeof(path), sdcard_root, "autoconfig") &&
        jw__is_directory(path)) {
        return jw__dup_realpath_or_literal(path);
    }

    return NULL;
}

static char *jw__default_retroarch_user_shaders_dir(const char *sdcard_root) {
    char *env_dir = jw__dup_env_value("UMRK_RETROARCH_USER_SHADERS_DIR");
    if (env_dir) {
        if (jw__mkdir_p(env_dir, 0755) == 0) {
            return env_dir;
        }
        free(env_dir);
    }

    const char *internal_data = jw__env_value("UMRK_INTERNAL_DATA_PATH");
    char internal_path[PATH_MAX];
    if (internal_data) {
        if (!jw__format_string(internal_path, sizeof(internal_path), "%s",
                               internal_data)) {
            return NULL;
        }
    } else if (!jw__format_default_internal_data(
                   internal_path, sizeof(internal_path), sdcard_root)) {
        return NULL;
    }

    char path[PATH_MAX];
    if (!jw__format_string(path, sizeof(path),
                           "%s/retroarch/.config/retroarch/shaders",
                           internal_path) ||
        jw__mkdir_p(path, 0755) != 0) {
        return NULL;
    }
    return jw__dup_realpath_or_literal(path);
}

static char *jw__default_retroarch_shaders_dir(const char *sdcard_root) {
    char *user_dir = jw__default_retroarch_user_shaders_dir(sdcard_root);
    if (user_dir) {
        return user_dir;
    }

    /* Last-resort compatibility fallback. New Leaf installs use the durable
       user shader root above so RetroArch's online updater cannot mutate the
       release-managed, manifest-validated bundle. */
    char *env_dir = jw__dup_env_value("UMRK_RETROARCH_SHADERS_DIR");
    if (env_dir) {
        if (jw__is_directory(env_dir)) {
            return env_dir;
        }
        free(env_dir);
    }

    const char *platform_path = jw__env_value("UMRK_PLATFORM_PATH");
    char path[PATH_MAX];
    if (platform_path && jw__format_string(path, sizeof(path), "%s/shaders",
                                           platform_path) &&
        jw__is_directory(path)) {
        return jw__dup_realpath_or_literal(path);
    }

    const char *system_path = jw__env_value("SYSTEM_PATH");
    if (system_path && jw__format_string(path, sizeof(path), "%s/shaders",
                                         system_path) &&
        jw__is_directory(path)) {
        return jw__dup_realpath_or_literal(path);
    }

    if (sdcard_root &&
        jw__format_default_system_child(path, sizeof(path), sdcard_root, "shaders") &&
        jw__is_directory(path)) {
        return jw__dup_realpath_or_literal(path);
    }

    return NULL;
}

static char *jw__retroarch_shared_config_path(const char *sdcard_root) {
    char *state_dir = jw_retroarch_state_dir(sdcard_root);
    if (!state_dir) {
        return NULL;
    }

    char *path = jw__dup_printf("%s/retroarch.cfg", state_dir);
    free(state_dir);
    return path;
}

/* sdroot_abs is the CONTENT root -- the card the ROM came from, so saves and states
   land beside the game. config_sdroot_abs is the primary card, where the release
   payload and the launcher's own state live. They differ whenever a game is played
   off the second SD slot, and conflating them is how recording ended up resolving
   its preset on a card that never had one. */
static int jw__write_retroarch_protected_config(FILE *fp, const char *sdroot_abs,
                                                 const char *config_sdroot_abs,
                                                 const char *core_path,
                                                 const char *screenshot_dir,
                                                 const int player_joypad_indices[4],
                                                 bool persist_changes,
                                                 char *error, size_t error_size) {
    if (!fp || !sdroot_abs || !sdroot_abs[0]) {
        jw__set_error(error, error_size, "missing RetroArch config inputs");
        return -1;
    }

    char *system_dir = NULL;
    char *saves_dir = NULL;
    char *states_dir = NULL;
    if (jw__retroarch_storage_dirs(sdroot_abs, &system_dir, &saves_dir, &states_dir,
                                   error, error_size) != 0) {
        return -1;
    }

    char core_dir_buf[PATH_MAX];
    char *core_dir_alloc = NULL;
    const char *core_dir = NULL;
    if (core_path && core_path[0]) {
        if (!jw__format_string(core_dir_buf, sizeof(core_dir_buf), "%s", core_path)) {
            jw__set_error(error, error_size, "RetroArch core path too long");
            free(system_dir);
            free(saves_dir);
            free(states_dir);
            return -1;
        }
        char *slash = strrchr(core_dir_buf, '/');
        if (slash) {
            *slash = '\0';
        }
        core_dir = core_dir_buf;
    } else {
        core_dir_alloc = jw__default_cores_dir();
        core_dir = core_dir_alloc;
    }

    char *info_dir = NULL;
    if (core_path && core_path[0]) {
        info_dir = jw__core_info_dir(core_path);
    } else {
        info_dir = jw__default_info_dir(sdroot_abs, core_dir);
    }
    char *autoconfig_dir = jw__default_autoconfig_dir(sdroot_abs);

    jw__retroarch_cfg_string(fp, "system_directory", system_dir);
    jw__retroarch_cfg_string(fp, "savefile_directory", saves_dir);
    jw__retroarch_cfg_string(fp, "savestate_directory", states_dir);
    /* Core overrides make cross-core state loads unsafe. Keep RetroArch's
       per-core namespaces protected even if a persisted user config disabled
       the upstream defaults. Battery saves remain isolated until device
       round-trip testing proves that two cores may safely share them. */
    jw__retroarch_cfg_string(fp, "sort_savefiles_enable", "true");
    jw__retroarch_cfg_string(fp, "sort_savestates_enable", "true");
    /* Emit a PNG next to each savestate so the in-game menu can preview slots. */
    jw__retroarch_cfg_string(fp, "savestate_thumbnail_enable", "true");
    /* Steer manual/command screenshots to an ephemeral tmpfs dir the in-game
       menu reads to show the paused game behind itself. */
    if (screenshot_dir && screenshot_dir[0]) {
        jw__retroarch_cfg_string(fp, "screenshot_directory", screenshot_dir);
    }
    if (core_dir && core_dir[0]) {
        jw__retroarch_cfg_string(fp, "libretro_directory", core_dir);
    }
    if (info_dir && jw__is_directory(info_dir)) {
        jw__retroarch_cfg_string(fp, "libretro_info_path", info_dir);
    }
    if (autoconfig_dir && jw__is_directory(autoconfig_dir)) {
        jw__retroarch_cfg_string(fp, "joypad_autoconfig_dir", autoconfig_dir);
    }
    jw__retroarch_cfg_string(fp, "config_save_on_exit", persist_changes ? "true" : "false");
    jw__retroarch_cfg_string(fp, "network_cmd_enable", "true");
    char command_port[16];
    snprintf(command_port, sizeof(command_port), "%u", JW_RA_DEFAULT_PORT);
    jw__retroarch_cfg_string(fp, "network_cmd_port", command_port);
    jw__retroarch_cfg_string(fp, "pause_nonactive", "false");
#ifdef PLATFORM_MLP1
    jw__mlp1_retroarch_audio_cfg(fp);
    jw__retroarch_cfg_string(fp, "audio_latency", "128");
    jw__retroarch_cfg_string(fp, "audio_block_frames", "256");
    jw__retroarch_cfg_string(fp, "video_threaded", "false");
    /* Ozone draws its icons and text from assets_directory, which Leaf ships and
       syncs into the durable user tree. Without this RetroArch falls back to
       RGUI, which renders from a built-in bitmap font, reads no assets at all,
       and cannot draw CJK -- leaving the shipped bundle, Chinese fallback font
       included, inert. Written here so save-on-exit cannot revert it. */
    jw__retroarch_cfg_string(fp, "menu_driver", "ozone");
    /* Ozone's own default targets a 1080p desktop and is small on a 960x720
       panel. Pinned rather than seeded because a seed only reaches a fresh
       install, and most devices arrive here by update with "1.000000" already
       persisted. RetroArch clamps this against the panel, so the stored value
       is an upper bound rather than a literal multiplier. */
    jw__retroarch_cfg_string(fp, "menu_scale_factor", "5.000000");
    /* Selenium. Ozone stores its theme as an enum index, and that enum has
       grown across RetroArch releases, so 13 was read back off the device after
       picking the theme by name rather than guessed from the string table. */
    jw__retroarch_cfg_string(fp, "ozone_menu_color_theme", "13");
    /* Follow Leaf's UI language. RetroArch ships the translations already, so
       this one integer is what makes its menus Chinese; the CJK font it needs to
       draw them arrives in the same asset bundle Ozone reads. Values are
       RETRO_LANGUAGE_* from libretro.h, not a guess: English 0, Simplified
       Chinese 12. Anything else falls back to English rather than shipping a
       half-translated menu in a language nobody selected. */
    {
        const char *ui_lang = jw__env_value("JAWAKA_LANGUAGE");
        const char *retro_lang = "0";
        if (ui_lang && strcmp(ui_lang, "zh_CN") == 0) {
            retro_lang = "12";
        }
        jw__retroarch_cfg_string(fp, "user_language", retro_lang);
    }
    /* Pin RA's reported refresh to the live panel rate the daemon read from the
       active DRM mode. RA's frame pacing — and Black Frame Insertion's
       refresh-aware cadence — misfire if it believes 60Hz while the panel runs
       100/120. Written in the protected section so it's correct from the first
       frame of every launch, independent of save-on-exit; absent env (unknown
       rate) leaves whatever the merged config had. */
    {
        const char *refresh_hz = jw__env_value("JAWAKA_REFRESH_RATE_HZ");
        if (refresh_hz && refresh_hz[0]) {
            int hz = atoi(refresh_hz);
            if (hz >= 50 && hz <= 240) {
                char refresh_val[24];
                snprintf(refresh_val, sizeof(refresh_val), "%d.000000", hz);
                jw__retroarch_cfg_string(fp, "video_refresh_rate", refresh_val);
            }
        }
    }
    /* Black Frame Insertion. The daemon sets JAWAKA_BFI=1 only when the user
       enabled it AND the panel runs at twice a content rate (120Hz for 60fps,
       100Hz for 50fps PAL) - one black frame per content frame mimics a CRT's
       impulse, cutting motion blur. Written here and
       stripped from the merge so the Leaf toggle is the single source of truth,
       independent of RA's own menu / save-on-exit. */
    {
        const char *bfi = jw__env_value("JAWAKA_BFI");
        jw__retroarch_cfg_string(fp, "video_black_frame_insertion",
                                 (bfi && bfi[0] == '1') ? "1" : "0");
    }
    jw__retroarch_cfg_string(fp, "menu_show_load_content_animation", "false");
    /* The MLP1 is a Nintendo-style layout (A=East confirms) that reports
       Xbox-style SDL button indices, so RetroArch's OK/Cancel default lands on
       the wrong face button when swapped. RA's default (false) puts menu OK on
       the A/East button — matching the OS. Pin it here so RA menus are correct
       from the first launch on fresh installs AND upgrades, overriding any stale
       user value persisted by save-on-exit. */
    jw__retroarch_cfg_string(fp, "menu_swap_ok_cancel_buttons", "false");
    jw__retroarch_cfg_string(fp, "check_firmware_before_loading", "false");
    jw__retroarch_cfg_string(fp, "builtin_mediaplayer_enable", "false");
    jw__retroarch_cfg_string(fp, "builtin_imageviewer_enable", "false");
    jw__retroarch_cfg_string(fp, "load_dummy_on_core_shutdown", "true");
    jw__retroarch_cfg_string(fp, "notification_show_autoconfig", "false");
    jw__retroarch_cfg_string(fp, "notification_show_autoconfig_fails", "false");
    jw__retroarch_cfg_string(fp, "notification_show_config_override_load", "false");
    jw__retroarch_cfg_max_users(fp, player_joypad_indices);
    jw__retroarch_cfg_string(fp, "savestate_file_compression", "false");
    jw__retroarch_cfg_string(fp, "video_driver", "gl");
    jw__retroarch_cfg_string(fp, "video_context_driver", "sdl_gl");
    jw__retroarch_cfg_string(fp, "aspect_ratio_index", "22");
    jw__retroarch_cfg_string(fp, "video_force_aspect", "true");
    jw__retroarch_cfg_string(fp, "video_scale_integer", "false");
    /* Game recording. Written here rather than left to the merged config because
       RetroArch rewrites every setting it owns on exit, so an upgrading device
       would keep whatever record_driver it already had -- "wav" on anything
       predating this feature -- and go on producing useless files forever.
       video_record_quality 0 is RECORD_CONFIG_TYPE_RECORDING_CUSTOM: "use the
       preset I am pointing at".
       The preset lives under .system because it is release content, not user
       state -- tuning it is our job, so it should travel with an update rather
       than sit in .umrk at whatever value some past release seeded.
       Gated on the preset being present: without the FFmpeg payload there is no
       h264_rkmpp to encode with, and a device that does not ship it must behave
       exactly as before rather than half-enable recording. */
    {
        /* Resolved against the PRIMARY card and through the same helper the other
           release defaults use, so UMRK_PLATFORM_PATH / SYSTEM_PATH are honoured.
           Using the content root here meant a game launched from the second SD slot
           looked for the preset on that card, never found it, and fell through --
           see below for why falling through was the dangerous part. */
        char *record_preset = jw__platform_defaults_path(
            config_sdroot_abs && config_sdroot_abs[0] ? config_sdroot_abs : sdroot_abs,
            "retroarch-record.cfg");
        char recordings_dir[PATH_MAX];
        const char *rec_root =
            (config_sdroot_abs && config_sdroot_abs[0]) ? config_sdroot_abs : sdroot_abs;

        if (record_preset &&
            jw_primary_recordings_path(recordings_dir, sizeof(recordings_dir), rec_root)) {
            /* The same primary helper feeds the daemon child environment and
               conversion pass. A secondary-card game still records somewhere
               VFH can discover after the conversion completes. */
            (void)jw__mkdir_p(recordings_dir, 0755);
            jw__retroarch_cfg_string(fp, "record_driver", "ffmpeg");
            jw__retroarch_cfg_string(fp, "video_record_config", record_preset);
            jw__retroarch_cfg_string(fp, "video_record_quality", "0");
            jw__retroarch_cfg_string(fp, "recording_output_directory",
                                     recordings_dir);
        } else {
            /* No payload, so recording must be OFF -- and saying so explicitly is
               required, not tidiness. These keys are stripped from the merge
               unconditionally, so simply not writing them leaves them absent, and
               RetroArch's own defaults for absent are record_driver = "ffmpeg"
               (this build has HAVE_FFMPEG) at MED_QUALITY, which ignores our preset
               and hardcodes SOFTWARE libx264 plus real-time AAC. That is a CPU
               encode underneath a running emulator on a Cortex-A55 -- far worse
               than the "wav" fallback an earlier comment here claimed. "null" is
               the record driver that genuinely does nothing. */
            jw__retroarch_cfg_string(fp, "record_driver", "null");
        }
        free(record_preset);
    }
#endif
    jw__retroarch_cfg_player_indices(fp, player_joypad_indices);

    /* RetroAchievements: jawakad exports the credentials stored under
       Settings > Accounts; RetroArch validates them with the service at
       launch. Absent env = don't touch whatever the user set up inside
       RetroArch itself. The session config lives in the runtime dir, so the
       password never lands in the persistent shared config. */
    const char *cheevos_user = jw__env_value("JAWAKA_CHEEVOS_USERNAME");
    const char *cheevos_pass = jw__env_value("JAWAKA_CHEEVOS_PASSWORD");
    if (cheevos_user && cheevos_user[0] && cheevos_pass && cheevos_pass[0]) {
        jw__retroarch_cfg_string(fp, "cheevos_enable", "true");
        jw__retroarch_cfg_string(fp, "cheevos_username", cheevos_user);
        jw__retroarch_cfg_string(fp, "cheevos_password", cheevos_pass);
    }

    free(core_dir_alloc);
    free(info_dir);
    free(autoconfig_dir);
    free(system_dir);
    free(saves_dir);
    free(states_dir);
    return ferror(fp) ? -1 : 0;
}

char *jw_prepare_retroarch_config(const char *runtime_dir, const char *sdcard_root,
                                  const char *core_path,
                                  const int player_joypad_indices[4],
                                  bool persist_changes,
                                  bool proxied_cheevos,
                                  char *error, size_t error_size) {
    jw__set_error(error, error_size, "");
    if (!runtime_dir || !runtime_dir[0] || !sdcard_root || !sdcard_root[0]) {
        jw__set_error(error, error_size, "missing RetroArch config inputs");
        return NULL;
    }

    char content_sdroot_abs[PATH_MAX];
    if (!realpath(sdcard_root, content_sdroot_abs)) {
        if (!jw__format_string(content_sdroot_abs, sizeof(content_sdroot_abs), "%s", sdcard_root)) {
            jw__set_error(error, error_size, "SD-card root path too long");
            return NULL;
        }
    }

    char *config_root = jw_sdcard_root();
    char config_sdroot_abs[PATH_MAX];
    if (!config_root) {
        jw__set_error(error, error_size, "could not resolve primary SD-card root");
        return NULL;
    }
    if (!realpath(config_root, config_sdroot_abs)) {
        if (!jw__format_string(config_sdroot_abs, sizeof(config_sdroot_abs), "%s", config_root)) {
            free(config_root);
            jw__set_error(error, error_size, "primary SD-card root path too long");
            return NULL;
        }
    }

    char *defaults_path = jw__platform_defaults_path(config_sdroot_abs, "retroarch.cfg");
    char *shared_path = jw__retroarch_shared_config_path(config_sdroot_abs);
    char *runtime_path = jw__dup_printf("%s/retroarch-current-%ld.cfg",
                                        runtime_dir, (long)getpid());
    char *defaults_text = NULL;
    char *shared_text = NULL;
    if (!shared_path || !runtime_path) {
        free(defaults_path);
        free(shared_path);
        free(runtime_path);
        free(config_root);
        jw__set_error(error, error_size, "could not resolve RetroArch config paths");
        return NULL;
    }

    if (!jw__path_exists(shared_path) && defaults_path) {
        if (jw__copy_file(defaults_path, shared_path) != 0) {
            free(defaults_path);
            free(shared_path);
            free(runtime_path);
            free(config_root);
            jw__set_error(error, error_size, "could not initialize shared RetroArch config");
            return NULL;
        }
    }

    if (defaults_path) {
        defaults_text = jw__read_text_file(defaults_path, 256u * 1024u);
    }
    if (jw__path_exists(shared_path)) {
        shared_text = jw__read_text_file(shared_path, 256u * 1024u);
    }

    FILE *fp = fopen(runtime_path, "wb");
    if (!fp) {
        free(defaults_path);
        free(shared_path);
        free(runtime_path);
        free(defaults_text);
        free(shared_text);
        free(config_root);
        jw__set_error(error, error_size, "could not write RetroArch runtime config");
        return NULL;
    }
    /* The runtime cfg can hold the RetroAchievements password in cleartext, so
       keep it readable only by the owner. */
    (void)fchmod(fileno(fp), S_IRUSR | S_IWUSR);

    bool defaults_have_shader_dir =
        jw__retroarch_cfg_text_has_key(defaults_text, "video_shader_dir");
    bool shared_has_shader_dir =
        jw__retroarch_cfg_text_has_key(shared_text, "video_shader_dir");
    bool migrate_release_shader_dir = false;
    char *shaders_dir = NULL;
    if (!defaults_have_shader_dir) {
        shaders_dir = jw__default_retroarch_shaders_dir(config_sdroot_abs);
        if (shaders_dir && shared_has_shader_dir) {
            char *persisted_shader_dir =
                jw__retroarch_cfg_text_string_value(shared_text, "video_shader_dir");
            migrate_release_shader_dir =
                persisted_shader_dir &&
                strcmp(persisted_shader_dir, shaders_dir) != 0 &&
                jw__release_managed_shader_dir(persisted_shader_dir);
            if (migrate_release_shader_dir) {
                jw_log_info("Migrating release-managed RetroArch shader directory "
                            "from %s to %s",
                            persisted_shader_dir, shaders_dir);
            }
            free(persisted_shader_dir);
        }
    }

    /* A proxied launch owns both cheevos keys for this session only, so they
       must not also arrive from defaults or shared: RetroArch keeps the FIRST
       occurrence of a key, so a merged-in value would win over the override
       written below and the launch would bypass the proxy without saying so. */
    const char *skip_keys[3];
    size_t skip_key_count = 0;
    if (migrate_release_shader_dir) {
        skip_keys[skip_key_count++] = "video_shader_dir";
    }
    if (proxied_cheevos) {
        skip_keys[skip_key_count++] = "cheevos_custom_host";
        skip_keys[skip_key_count++] = "cheevos_hardcore_mode_enable";
    }
    /* The shader skip is scoped to the shared text only (a packaged default
       is never a stale release-managed path), the cheevos skips to both. */
    size_t defaults_skip_first = migrate_release_shader_dir ? 1u : 0u;

    if (defaults_text) {
        jw__write_retroarch_cfg_filtered_skipping(
            fp, defaults_text, shared_text, false,
            skip_keys + defaults_skip_first,
            skip_key_count - defaults_skip_first);
    }
    if (shared_text) {
        /* Preserve RetroArch's order and duplicate-resolution semantics.
           Deduplicating here rescans the remaining text for every line; a
           normal 3,300-line persisted config added about 500 ms to each MLP1
           game launch. Note that RetroArch resolves duplicates by keeping the
           FIRST occurrence and discarding the rest (libretro-common/file/
           config_file.c: "Only add entry to the map if an entry with the
           specified value does not already exist") -- passing the text through
           unchanged is what preserves that, and it is also why a key Jawaka
           needs to own for one launch has to be skipped here rather than
           appended later. */
        jw__write_retroarch_cfg_filtered_skipping(
            fp, shared_text, NULL, false, skip_keys, skip_key_count);
    }
    /* Make the release-managed bundle browsable on first launch, but keep this
       key outside the protected section. A packaged default or a value already
       persisted by RetroArch always wins, except for a release-managed path
       whose SD mount point changed. Save-on-exit may replace this fallback with
       any genuine custom directory the user chooses. */
    if (shaders_dir &&
        (!shared_has_shader_dir || migrate_release_shader_dir)) {
        jw__retroarch_cfg_string(fp, "video_shader_dir", shaders_dir);
    }
    free(shaders_dir);
    /* Ephemeral tmpfs dir for paused-frame screenshots the in-game menu reads.
       Best-effort: if it can't be made, screenshots simply won't be steered. */
    char shots_dir[PATH_MAX];
    shots_dir[0] = '\0';
    if (jw__mkdir_child(runtime_dir, "shots") == 0) {
        jw__format_string(shots_dir, sizeof(shots_dir), "%s/shots", runtime_dir);
    }

    fputs("\n# Jawaka protected runtime settings\n", fp);
    int protected_rc = jw__write_retroarch_protected_config(fp, content_sdroot_abs, config_sdroot_abs, core_path,
                                                            shots_dir,
                                                            player_joypad_indices,
                                                            persist_changes,
                                                           error, error_size);
    /* The only occurrence of either key in this file, because both were
       skipped above. No scheme on the host: rcheevos prefixes "http://"
       itself and the value must fit its 64-byte field. Hardcore is forced
       off rather than derived -- proxied launches are casual-only and never
       touch the durable user setting, which the exit-time backup restores. */
    if (proxied_cheevos) {
        jw__retroarch_cfg_string(fp, "cheevos_custom_host", "127.0.0.1:8080");
        jw__retroarch_cfg_string(fp, "cheevos_hardcore_mode_enable", "false");
    }

    int failed = protected_rc != 0 || ferror(fp);
    if (fclose(fp) != 0) {
        failed = 1;
    }

    free(defaults_path);
    free(shared_path);
    free(defaults_text);
    free(shared_text);
    free(config_root);
    if (failed) {
        unlink(runtime_path);
        free(runtime_path);
        if (error && error_size > 0 && !error[0]) {
            jw__set_error(error, error_size, "could not finish RetroArch runtime config");
        }
        return NULL;
    }

    return runtime_path;
}

/* Exact line (without the trailing newline or CR) carrying key, for the
 * launch snapshot. Returns false when absent or when the line won't fit. */
static bool jw__retroarch_cfg_text_line_for_key(const char *text,
                                                const char *wanted_key,
                                                char *out, size_t out_size) {
    if (!text || !wanted_key || !wanted_key[0] || !out || out_size == 0) {
        return false;
    }
    const char *line = text;
    while (*line) {
        const char *next = strchr(line, '\n');
        size_t len = next ? (size_t)(next - line) : strlen(line);
        while (len > 0 && line[len - 1] == '\r') {
            len--;
        }
        char key[128];
        if (jw__retroarch_cfg_line_key(line, len, key, sizeof(key)) &&
            strcmp(key, wanted_key) == 0) {
            if (len >= out_size) {
                return false;
            }
            memcpy(out, line, len);
            out[len] = '\0';
            return true;
        }
        if (!next) {
            break;
        }
        line = next + 1;
    }
    return false;
}

void jw_retroarch_launch_snapshot_init(jw_retroarch_launch_snapshot *snapshot) {
    if (!snapshot) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
}

void jw_retroarch_launch_snapshot_capture(jw_retroarch_launch_snapshot *snapshot,
                                           const char *shared_text) {
    if (!snapshot || !shared_text) {
        return;
    }
    snapshot->custom_host_present = jw__retroarch_cfg_text_line_for_key(
        shared_text, "cheevos_custom_host",
        snapshot->custom_host_line, sizeof(snapshot->custom_host_line));
    snapshot->hardcore_present = jw__retroarch_cfg_text_line_for_key(
        shared_text, "cheevos_hardcore_mode_enable",
        snapshot->hardcore_line, sizeof(snapshot->hardcore_line));
}

char *jw_retroarch_shared_config_read(const char *sdcard_root) {
    if (!sdcard_root || !sdcard_root[0]) {
        return NULL;
    }
    char *shared_path = jw__retroarch_shared_config_path(sdcard_root);
    if (!shared_path) {
        return NULL;
    }
    char *text = jw__read_text_file(shared_path, 256u * 1024u);
    free(shared_path);
    return text;
}

bool jw_retroarch_shared_hardcore_enabled(const char *sdcard_root) {
    char *shared_text = jw_retroarch_shared_config_read(sdcard_root);
    if (!shared_text) {
        return false;
    }
    /* First match, not last: this decision must agree with what RetroArch will
       actually do, and RetroArch keeps the first occurrence of a key. With
       "true" first and "false" later, a last-wins read would report Hardcore
       off and route a hardcore session through the casual-only proxy. Reading
       the shared config for duplicates is not hypothetical -- the shared text
       is passed through undeduplicated, so save-on-exit churn can leave two. */
    char *value = jw__retroarch_cfg_text_string_value_ex(
        shared_text, "cheevos_hardcore_mode_enable", true);
    free(shared_text);
    bool enabled = value && strcmp(value, "true") == 0;
    free(value);
    return enabled;
}

int jw_backup_retroarch_config(const char *runtime_config_path, const char *sdcard_root,
                               const jw_retroarch_launch_snapshot *snapshot,
                               char *error, size_t error_size) {
    jw__set_error(error, error_size, "");
    if (!runtime_config_path || !runtime_config_path[0] ||
        !sdcard_root || !sdcard_root[0]) {
        jw__set_error(error, error_size, "missing RetroArch config backup inputs");
        return -1;
    }

    char *shared_path = jw__retroarch_shared_config_path(sdcard_root);
    if (!shared_path) {
        jw__set_error(error, error_size, "could not resolve shared RetroArch config");
        return -1;
    }

    char *runtime_text = jw__read_text_file(runtime_config_path, 256u * 1024u);
    if (!runtime_text) {
        free(shared_path);
        jw__set_error(error, error_size, "could not read RetroArch runtime config");
        return -1;
    }

    FILE *fp = fopen(shared_path, "wb");
    if (!fp) {
        free(runtime_text);
        free(shared_path);
        jw__set_error(error, error_size, "could not write shared RetroArch config");
        return -1;
    }

    /* Proxied launches: the injected runtime values are discarded and the
       exact prior shared lines (including absence) restored. This is scoped
       to the proxied launch only, so direct sessions keep persisting these
       settings as ordinary user changes. */
    bool restore_transient = snapshot && snapshot->proxied;
    static const char *transient_keys[] = {
        "cheevos_custom_host",
        "cheevos_hardcore_mode_enable",
    };
    int rc;
    if (restore_transient) {
        rc = jw__write_retroarch_cfg_filtered_skipping(
            fp, runtime_text, NULL, true, transient_keys, 2u);
        if (rc == 0 && snapshot->custom_host_present) {
            rc = fprintf(fp, "%s\n", snapshot->custom_host_line) < 0 ? -1 : 0;
        }
        if (rc == 0 && snapshot->hardcore_present) {
            rc = fprintf(fp, "%s\n", snapshot->hardcore_line) < 0 ? -1 : 0;
        }
    } else {
        rc = jw__write_retroarch_cfg_filtered(fp, runtime_text, NULL, true);
    }
    if (fclose(fp) != 0) {
        rc = -1;
    }
    free(runtime_text);
    free(shared_path);
    if (rc != 0) {
        jw__set_error(error, error_size, "could not save shared RetroArch config");
        return -1;
    }

    return 0;
}

int jw_reset_retroarch_shared_config(const char *sdcard_root,
                                     char *status, size_t status_size) {
    jw__set_error(status, status_size, "");
    if (!sdcard_root || !sdcard_root[0]) {
        jw__set_error(status, status_size, "missing SD-card root");
        return -1;
    }

    char sdroot_abs[PATH_MAX];
    if (!realpath(sdcard_root, sdroot_abs)) {
        if (!jw__format_string(sdroot_abs, sizeof(sdroot_abs), "%s", sdcard_root)) {
            jw__set_error(status, status_size, "SD-card root path too long");
            return -1;
        }
    }

    char *defaults_path = jw__platform_defaults_path(sdroot_abs, "retroarch.cfg");
    char *shared_path = jw__retroarch_shared_config_path(sdroot_abs);
    if (!defaults_path || !shared_path) {
        free(defaults_path);
        free(shared_path);
        jw__set_error(status, status_size, "packaged RetroArch defaults missing");
        return -1;
    }

    int rc = jw__copy_file(defaults_path, shared_path);
    free(defaults_path);
    free(shared_path);
    if (rc != 0) {
        jw__set_error(status, status_size, "could not reset RetroArch config");
        return -1;
    }

    jw__set_error(status, status_size, "RetroArch config reset");
    return 0;
}

char *jw_write_retroarch_append_config(const char *runtime_dir, const char *sdcard_root,
                                       const char *core_path,
                                   const int player_joypad_indices[4]) {
    if (!runtime_dir || !sdcard_root || !core_path) {
        return NULL;
    }

    char sdroot_abs[PATH_MAX];
    if (!realpath(sdcard_root, sdroot_abs)) {
        if (!jw__format_string(sdroot_abs, sizeof(sdroot_abs), "%s", sdcard_root)) {
            return NULL;
        }
    }

    char *system_dir = NULL;
    char *saves_dir = NULL;
    char *states_dir = NULL;
    if (jw__retroarch_storage_dirs(sdroot_abs, &system_dir, &saves_dir, &states_dir,
                                   NULL, 0) != 0) {
        return NULL;
    }

    char *path = jw__dup_printf("%s/retroarch-launch-%ld.cfg", runtime_dir, (long)getpid());
    if (!path) {
        free(system_dir);
        free(saves_dir);
        free(states_dir);
        return NULL;
    }

    char core_dir[PATH_MAX];
    if (!jw__format_string(core_dir, sizeof(core_dir), "%s", core_path)) {
        free(system_dir);
        free(saves_dir);
        free(states_dir);
        free(path);
        return NULL;
    }
    char *slash = strrchr(core_dir, '/');
    if (slash) {
        *slash = '\0';
    }

    char *info_dir = jw__core_info_dir(core_path);
    char *autoconfig_dir = jw__default_autoconfig_dir(sdroot_abs);
    char *defaults_path = jw__platform_defaults_path(sdroot_abs, "retroarch.cfg");

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        free(defaults_path);
        free(info_dir);
        free(autoconfig_dir);
        free(system_dir);
        free(saves_dir);
        free(states_dir);
        free(path);
        return NULL;
    }

    if (defaults_path) {
        char *defaults_text = jw__read_text_file(defaults_path, 256u * 1024u);
        if (defaults_text) {
            /* The append-config compatibility path obeys the same protected
               key contract as the normal merged session config. */
            jw__write_retroarch_cfg_filtered(fp, defaults_text, NULL, true);
            free(defaults_text);
        }
    }
    jw__retroarch_cfg_string(fp, "system_directory", system_dir);
    jw__retroarch_cfg_string(fp, "savefile_directory", saves_dir);
    jw__retroarch_cfg_string(fp, "savestate_directory", states_dir);
    jw__retroarch_cfg_string(fp, "sort_savefiles_enable", "true");
    jw__retroarch_cfg_string(fp, "sort_savestates_enable", "true");
    jw__retroarch_cfg_string(fp, "libretro_directory", core_dir);
    if (info_dir && jw__is_directory(info_dir)) {
        jw__retroarch_cfg_string(fp, "libretro_info_path", info_dir);
    }
    if (autoconfig_dir && jw__is_directory(autoconfig_dir)) {
        jw__retroarch_cfg_string(fp, "joypad_autoconfig_dir", autoconfig_dir);
    }
    jw__retroarch_cfg_string(fp, "config_save_on_exit", "false");
    jw__retroarch_cfg_string(fp, "network_cmd_enable", "true");
    char command_port[16];
    snprintf(command_port, sizeof(command_port), "%u", JW_RA_DEFAULT_PORT);
    jw__retroarch_cfg_string(fp, "network_cmd_port", command_port);
    jw__retroarch_cfg_string(fp, "pause_nonactive", "false");
#ifdef PLATFORM_MLP1
    jw__mlp1_retroarch_audio_cfg(fp);
    jw__retroarch_cfg_string(fp, "audio_latency", "128");
    jw__retroarch_cfg_string(fp, "audio_block_frames", "256");
    jw__retroarch_cfg_string(fp, "video_threaded", "false");
    /* Ozone draws its icons and text from assets_directory, which Leaf ships and
       syncs into the durable user tree. Without this RetroArch falls back to
       RGUI, which renders from a built-in bitmap font, reads no assets at all,
       and cannot draw CJK -- leaving the shipped bundle, Chinese fallback font
       included, inert. Written here so save-on-exit cannot revert it. */
    jw__retroarch_cfg_string(fp, "menu_driver", "ozone");
    /* Ozone's own default targets a 1080p desktop and is small on a 960x720
       panel. Pinned rather than seeded because a seed only reaches a fresh
       install, and most devices arrive here by update with "1.000000" already
       persisted. RetroArch clamps this against the panel, so the stored value
       is an upper bound rather than a literal multiplier. */
    jw__retroarch_cfg_string(fp, "menu_scale_factor", "5.000000");
    /* Selenium. Ozone stores its theme as an enum index, and that enum has
       grown across RetroArch releases, so 13 was read back off the device after
       picking the theme by name rather than guessed from the string table. */
    jw__retroarch_cfg_string(fp, "ozone_menu_color_theme", "13");
    /* Follow Leaf's UI language. RetroArch ships the translations already, so
       this one integer is what makes its menus Chinese; the CJK font it needs to
       draw them arrives in the same asset bundle Ozone reads. Values are
       RETRO_LANGUAGE_* from libretro.h, not a guess: English 0, Simplified
       Chinese 12. Anything else falls back to English rather than shipping a
       half-translated menu in a language nobody selected. */
    {
        const char *ui_lang = jw__env_value("JAWAKA_LANGUAGE");
        const char *retro_lang = "0";
        if (ui_lang && strcmp(ui_lang, "zh_CN") == 0) {
            retro_lang = "12";
        }
        jw__retroarch_cfg_string(fp, "user_language", retro_lang);
    }
    /* Pin RA's reported refresh to the live panel rate the daemon read from the
       active DRM mode. RA's frame pacing — and Black Frame Insertion's
       refresh-aware cadence — misfire if it believes 60Hz while the panel runs
       100/120. Written in the protected section so it's correct from the first
       frame of every launch, independent of save-on-exit; absent env (unknown
       rate) leaves whatever the merged config had. */
    {
        const char *refresh_hz = jw__env_value("JAWAKA_REFRESH_RATE_HZ");
        if (refresh_hz && refresh_hz[0]) {
            int hz = atoi(refresh_hz);
            if (hz >= 50 && hz <= 240) {
                char refresh_val[24];
                snprintf(refresh_val, sizeof(refresh_val), "%d.000000", hz);
                jw__retroarch_cfg_string(fp, "video_refresh_rate", refresh_val);
            }
        }
    }
    /* Black Frame Insertion. The daemon sets JAWAKA_BFI=1 only when the user
       enabled it AND the panel runs at twice a content rate (120Hz for 60fps,
       100Hz for 50fps PAL) - one black frame per content frame mimics a CRT's
       impulse, cutting motion blur. Written here and
       stripped from the merge so the Leaf toggle is the single source of truth,
       independent of RA's own menu / save-on-exit. */
    {
        const char *bfi = jw__env_value("JAWAKA_BFI");
        jw__retroarch_cfg_string(fp, "video_black_frame_insertion",
                                 (bfi && bfi[0] == '1') ? "1" : "0");
    }
    jw__retroarch_cfg_string(fp, "menu_show_load_content_animation", "false");
    /* The MLP1 is a Nintendo-style layout (A=East confirms) that reports
       Xbox-style SDL button indices, so RetroArch's OK/Cancel default lands on
       the wrong face button when swapped. RA's default (false) puts menu OK on
       the A/East button — matching the OS. Pin it here so RA menus are correct
       from the first launch on fresh installs AND upgrades, overriding any stale
       user value persisted by save-on-exit. */
    jw__retroarch_cfg_string(fp, "menu_swap_ok_cancel_buttons", "false");
    jw__retroarch_cfg_string(fp, "check_firmware_before_loading", "false");
    jw__retroarch_cfg_string(fp, "builtin_mediaplayer_enable", "false");
    jw__retroarch_cfg_string(fp, "builtin_imageviewer_enable", "false");
    jw__retroarch_cfg_string(fp, "load_dummy_on_core_shutdown", "true");
    jw__retroarch_cfg_string(fp, "notification_show_autoconfig", "false");
    jw__retroarch_cfg_string(fp, "notification_show_autoconfig_fails", "false");
    jw__retroarch_cfg_string(fp, "notification_show_config_override_load", "false");
    jw__retroarch_cfg_max_users(fp, player_joypad_indices);
    jw__retroarch_cfg_string(fp, "savestate_file_compression", "false");
    jw__retroarch_cfg_string(fp, "video_driver", "gl");
    jw__retroarch_cfg_string(fp, "video_context_driver", "sdl_gl");
    jw__retroarch_cfg_string(fp, "aspect_ratio_index", "22");
    jw__retroarch_cfg_string(fp, "video_force_aspect", "true");
    jw__retroarch_cfg_string(fp, "video_scale_integer", "false");
#endif
    jw__retroarch_cfg_player_indices(fp, player_joypad_indices);

    int failed = ferror(fp);
    if (fclose(fp) != 0) {
        failed = 1;
    }

    free(defaults_path);
    free(info_dir);
    free(autoconfig_dir);
    free(system_dir);
    free(saves_dir);
    free(states_dir);
    if (failed) {
        unlink(path);
        free(path);
        return NULL;
    }

    return path;
}
