#include "internal/platform/paths.h"

#include <errno.h>
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
    char *sdcard_root = jw_sdcard_root();
    if (!sdcard_root) {
        return NULL;
    }

    char *jawaka_dir = jw__dup_printf("%s/.jawaka", sdcard_root);
    if (!jawaka_dir) {
        free(sdcard_root);
        return NULL;
    }

    if (jw__mkdir_if_needed(jawaka_dir, 0755) != 0) {
        free(jawaka_dir);
        free(sdcard_root);
        return NULL;
    }

    char *db_path = jw__dup_printf("%s/library.db", jawaka_dir);
    free(jawaka_dir);
    free(sdcard_root);
    return db_path;
}
