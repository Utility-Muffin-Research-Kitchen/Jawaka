/* Runs the real S49weston command strings against a fake init script from a
   jawakad-like environment: card cwd, card LD_LIBRARY_PATH, session settings. */
#include "internal/platform/weston_initd.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_root[] = "/tmp/jawaka-weston-initd-XXXXXX";
static char g_script[PATH_MAX];
static char g_out[PATH_MAX];

/* Swap the stock script path for the fake one, keeping the rest verbatim. */
static void run(const char *command) {
    char buf[1024];
    const char *at = strstr(command, JW_WESTON_INITD_SCRIPT);
    assert(at);
    snprintf(buf, sizeof(buf), "%.*s%s%s", (int)(at - command), command, g_script,
             at + strlen(JW_WESTON_INITD_SCRIPT));
    unlink(g_out);
    assert(system(buf) == 0);
}

static char *slurp(void) {
    static char text[16384];
    FILE *f = fopen(g_out, "r");
    assert(f);
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    return text;
}

static int has_line(const char *text, const char *line) {
    size_t len = strlen(line);
    for (const char *p = text; (p = strstr(p, line)); p += len) {
        if ((p == text || p[-1] == '\n') && (p[len] == '\n' || p[len] == '\0')) return 1;
    }
    return 0;
}

static int has_name(const char *text, const char *name) {
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "%s=", name);
    for (const char *p = text; (p = strstr(p, prefix)); p++) {
        if (p == text || p[-1] == '\n') return 1;
    }
    return 0;
}

int main(void) {
    assert(mkdtemp(g_root));
    snprintf(g_script, sizeof(g_script), "%s/S49weston", g_root);
    snprintf(g_out, sizeof(g_out), "%s/seen", g_root);
    FILE *f = fopen(g_script, "w");
    assert(f);
    fprintf(f, "#!/bin/sh\n{ echo \"verb=$1\"; echo \"cwd=$(pwd)\"; env; } >'%s'\n", g_out);
    fclose(f);
    assert(chmod(g_script, 0755) == 0);

    /* What jawakad carries on the device. */
    assert(chdir(g_root) == 0);
    assert(setenv("LD_LIBRARY_PATH", "/media/sdcard1/.system/leaf/platforms/mlp1/launcher/lib:", 1) == 0);
    assert(setenv("LD_PRELOAD", "/media/sdcard1/shim.so", 1) == 0);
    assert(setenv("WESTON_DRM_PRIMARY", "DSI-1", 1) == 0);

    const char *verbs[] = { "start", "stop", "restart" };
    const char *commands[] = { JW_WESTON_INITD("start"), JW_WESTON_INITD("stop"),
                               JW_WESTON_INITD("restart") };
    for (size_t i = 0; i < 3; i++) {
        run(commands[i]);
        char *seen = slurp();
        char verb[32];
        snprintf(verb, sizeof(verb), "verb=%s", verbs[i]);
        assert(has_line(seen, verb));
        assert(has_line(seen, "cwd=/"));
        assert(!has_name(seen, "LD_LIBRARY_PATH"));
        assert(!has_name(seen, "LD_PRELOAD"));
        /* The boot-time panel-first choice still reaches Weston. */
        assert(has_line(seen, "WESTON_DRM_PRIMARY=DSI-1"));
    }

    /* The HDMI switch passes its Weston settings after the prefix. */
    run(JW_WESTON_ROOTFS_ENV " WESTON_DRM_SINGLE_HEAD=1 WESTON_DRM_PRIMARY=HDMI-A-1 "
        JW_WESTON_INITD_SCRIPT " restart");
    char *seen = slurp();
    assert(has_line(seen, "WESTON_DRM_SINGLE_HEAD=1"));
    assert(has_line(seen, "WESTON_DRM_PRIMARY=HDMI-A-1"));
    assert(has_line(seen, "cwd=/"));
    assert(!has_name(seen, "LD_LIBRARY_PATH"));

    unlink(g_out);
    unlink(g_script);
    rmdir(g_root);
    printf("weston-initd-test: ok\n");
    return 0;
}
