#define _POSIX_C_SOURCE 200809L

#include "internal/platform/paths.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int fail(const char *message) {
    fprintf(stderr, "retroarch-config-test: %s\n", message);
    return 1;
}

static int mkdir_one(const char *path) {
    return mkdir(path, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static int write_text(const char *path, const char *text) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    size_t len = strlen(text);
    return fwrite(text, 1, len, fp) == len && fclose(fp) == 0 ? 0 : -1;
}

static char *read_text(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp || fseek(fp, 0, SEEK_END) != 0) return NULL;
    long size = ftell(fp);
    if (size < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    char *text = calloc((size_t)size + 1u, 1u);
    if (!text || fread(text, 1, (size_t)size, fp) != (size_t)size) {
        free(text);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    return text;
}

static int key_count(const char *text, const char *key, const char *value) {
    int count = 0;
    size_t key_len = strlen(key);
    for (const char *line = text; line && *line;) {
        const char *next = strchr(line, '\n');
        size_t len = next ? (size_t)(next - line) : strlen(line);
        if (len >= key_len && strncmp(line, key, key_len) == 0 &&
            (!value || strstr(line, value))) {
            count++;
        }
        line = next ? next + 1 : NULL;
    }
    return count;
}

static int verify_runtime(const char *path, const char *shader_dir) {
    char *text = read_text(path);
    if (!text) return -1;
    int ok = key_count(text, "sort_savefiles_enable", NULL) == 1 &&
             key_count(text, "sort_savefiles_enable", "= \"true\"") == 1 &&
             key_count(text, "sort_savestates_enable", NULL) == 1 &&
             key_count(text, "sort_savestates_enable", "= \"true\"") == 1 &&
             (!shader_dir ||
              key_count(text, "video_shader_dir", shader_dir) == 1);
    free(text);
    return ok ? 0 : -1;
}

#ifdef PLATFORM_MLP1
static int verify_recordings(const char *path, const char *recordings_dir) {
    char *text = read_text(path);
    if (!text) return -1;
    int ok = key_count(text, "recording_output_directory", recordings_dir) == 1;
    free(text);
    return ok ? 0 : -1;
}
#endif

int main(void) {
    char root[] = "/tmp/jw-retroarch-config-XXXXXX";
    int fd = mkstemp(root);
    if (fd < 0) return fail("mkstemp failed");
    close(fd);
    if (unlink(root) != 0 || mkdir_one(root) != 0) return fail("root mkdir failed");

    char platform[PATH_MAX], defaults[PATH_MAX], internal[PATH_MAX];
    char runtime[PATH_MAX], cores[PATH_MAX], shaders[PATH_MAX];
    char user_shaders[PATH_MAX], custom_shaders[PATH_MAX];
    char default_cfg[PATH_MAX], record_cfg[PATH_MAX], recordings_dir[PATH_MAX];
    char retroarch_dir[PATH_MAX], shared_cfg[PATH_MAX];
    snprintf(platform, sizeof(platform), "%s/platform", root);
    snprintf(defaults, sizeof(defaults), "%s/defaults", platform);
    snprintf(internal, sizeof(internal), "%s/internal", root);
    snprintf(runtime, sizeof(runtime), "%s/runtime", root);
    snprintf(cores, sizeof(cores), "%s/cores", platform);
    snprintf(shaders, sizeof(shaders), "%s/shaders", platform);
    snprintf(user_shaders, sizeof(user_shaders),
             "%s/retroarch/.config/retroarch/shaders", internal);
    snprintf(custom_shaders, sizeof(custom_shaders), "%s/custom-shaders", root);
    snprintf(retroarch_dir, sizeof(retroarch_dir), "%s/retroarch", internal);
    snprintf(shared_cfg, sizeof(shared_cfg), "%s/retroarch.cfg", retroarch_dir);
    if (mkdir_one(platform) || mkdir_one(defaults) || mkdir_one(internal) ||
        mkdir_one(runtime) || mkdir_one(cores) || mkdir_one(shaders) ||
        mkdir_one(custom_shaders) || mkdir_one(retroarch_dir)) {
        return fail("fixture mkdir failed");
    }
    snprintf(default_cfg, sizeof(default_cfg), "%s/retroarch.cfg", defaults);
    snprintf(record_cfg, sizeof(record_cfg), "%s/retroarch-record.cfg", defaults);
    if (write_text(default_cfg,
                   "video_vsync = \"true\"\n"
                   "sort_savefiles_enable = \"false\"\n"
                   "sort_savestates_enable = \"false\"\n"
                   "sort_savefiles_enable = \"false\"\n") != 0) {
        return fail("defaults write failed");
    }
    if (write_text(record_cfg, "# recording preset fixture\n") != 0) {
        return fail("recording preset write failed");
    }
    if (write_text(shared_cfg,
                   "menu_driver = \"rgui\"\n"
                   "sort_savefiles_enable = \"false\"\n"
                   "sort_savestates_enable = \"false\"\n"
                   "sort_savestates_enable = \"false\"\n") != 0) {
        return fail("persisted config write failed");
    }

    setenv("SDCARD_PATH", root, 1);
    setenv("UMRK_PLATFORM_PATH", platform, 1);
    setenv("UMRK_INTERNAL_DATA_PATH", internal, 1);
    setenv("UMRK_RETROARCH_SHADERS_DIR", shaders, 1);
    setenv("UMRK_RETROARCH_USER_SHADERS_DIR", user_shaders, 1);
    if (!jw_primary_recordings_path(recordings_dir, sizeof(recordings_dir), root)) {
        return fail("could not resolve primary recordings path");
    }

    char core[PATH_MAX];
    snprintf(core, sizeof(core), "%s/mgba_libretro.so", cores);
    char *append_cfg = jw_write_retroarch_append_config(runtime, root, core, -1);
    if (!append_cfg || verify_runtime(append_cfg, NULL) != 0) {
        return fail("append config did not normalize protected sort keys");
    }
    unlink(append_cfg);
    free(append_cfg);

    char error[256];
    char *runtime_cfg = jw_prepare_retroarch_config(runtime, root, core, -1,
                                                    true, error, sizeof(error));
    if (!runtime_cfg || verify_runtime(runtime_cfg, user_shaders) != 0) {
        return fail(error[0] ? error : "protected keys not normalized");
    }
#ifdef PLATFORM_MLP1
    if (verify_recordings(runtime_cfg, recordings_dir) != 0) {
        return fail("RetroArch recording directory disagrees with the primary path");
    }
#endif
    struct stat user_shader_stat;
    if (stat(user_shaders, &user_shader_stat) != 0 ||
        !S_ISDIR(user_shader_stat.st_mode)) {
        return fail("durable user shader directory was not created");
    }
    if (jw_backup_retroarch_config(runtime_cfg, root, error, sizeof(error)) != 0) {
        return fail(error[0] ? error : "backup failed");
    }

    char *shared = read_text(shared_cfg);
    if (!shared || key_count(shared, "sort_savefiles_enable", NULL) != 0 ||
        key_count(shared, "sort_savestates_enable", NULL) != 0) {
        free(shared);
        return fail("protected keys persisted to shared config");
    }
    free(shared);
    unlink(runtime_cfg);
    free(runtime_cfg);

    if (write_text(
            shared_cfg,
            "menu_driver = \"rgui\"\n"
            "video_shader_dir = "
            "\"/mnt/sdcard/.system/leaf/platforms/mlp1/shaders\"\n") != 0) {
        return fail("stale release shader config write failed");
    }
    runtime_cfg = jw_prepare_retroarch_config(runtime, root, core, -1,
                                              true, error, sizeof(error));
    if (!runtime_cfg || verify_runtime(runtime_cfg, user_shaders) != 0) {
        return fail("stale release shader directory did not migrate to user state");
    }
    char *migrated_runtime = read_text(runtime_cfg);
    if (!migrated_runtime ||
        key_count(migrated_runtime, "video_shader_dir",
                  "/mnt/sdcard/.system/leaf/platforms/mlp1/shaders") != 0) {
        free(migrated_runtime);
        return fail("stale release shader directory survived migration");
    }
    free(migrated_runtime);
    unlink(runtime_cfg);
    free(runtime_cfg);

    char custom_cfg[PATH_MAX + 64];
    snprintf(custom_cfg, sizeof(custom_cfg),
             "menu_driver = \"rgui\"\nvideo_shader_dir = \"%s\"\n",
             custom_shaders);
    if (write_text(shared_cfg, custom_cfg) != 0) {
        return fail("custom shader config write failed");
    }
    runtime_cfg = jw_prepare_retroarch_config(runtime, root, core, -1,
                                              true, error, sizeof(error));
    if (!runtime_cfg || verify_runtime(runtime_cfg, custom_shaders) != 0) {
        return fail("custom shader directory did not win over bundle fallback");
    }
    char *custom_runtime = read_text(runtime_cfg);
    if (!custom_runtime ||
        key_count(custom_runtime, "video_shader_dir", user_shaders) != 0) {
        free(custom_runtime);
        return fail("durable shader fallback overrode the custom shader directory");
    }
    free(custom_runtime);
    unlink(runtime_cfg);
    free(runtime_cfg);

    if (rmdir(shaders) != 0 ||
        write_text(shared_cfg, "menu_driver = \"rgui\"\n") != 0) {
        return fail("missing shader bundle fixture setup failed");
    }
    runtime_cfg = jw_prepare_retroarch_config(runtime, root, core, -1,
                                              true, error, sizeof(error));
    if (!runtime_cfg || verify_runtime(runtime_cfg, user_shaders) != 0) {
        return fail("missing shader bundle prevented config generation");
    }
    char *missing_runtime = read_text(runtime_cfg);
    if (!missing_runtime ||
        key_count(missing_runtime, "video_shader_dir", user_shaders) != 1) {
        free(missing_runtime);
        return fail("missing bundle did not retain the durable shader root");
    }
    free(missing_runtime);
    unlink(runtime_cfg);
    free(runtime_cfg);
    printf("retroarch-config-test: ok\n");
    return 0;
}
