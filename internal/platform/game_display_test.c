/* The standalone-game display decisions, and the S49weston commands run for
   real against a fake init script with the HDMI config file redirected, so the
   shell quoting and the printf escapes are exercised too. */
#include "internal/platform/game_display.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "game-display-test: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

#define OVERRIDE_INI "/userdata/root/.config/weston.ini"

static char g_root[] = "/tmp/jawaka-game-display-XXXXXX";
static char g_script[PATH_MAX];
static char g_conf[PATH_MAX];
static char g_out[PATH_MAX];

/* Copy `in` to `out` with every `from` replaced by `to`. */
static void replace_all(const char *in, const char *from, const char *to,
                        char *out, size_t size) {
    size_t used = 0;
    size_t from_len = strlen(from);
    for (const char *p = in; *p;) {
        const char *piece = p;
        size_t len = 1;
        if (strncmp(p, from, from_len) == 0) {
            piece = to;
            len = strlen(to);
            p += from_len;
        } else {
            p++;
        }
        CHECK(used + len < size);
        memcpy(out + used, piece, len);
        used += len;
    }
    out[used] = '\0';
}

static char *slurp(const char *path) {
    static char text[16384];
    FILE *f = fopen(path, "r");
    CHECK(f);
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

/* Run a builder command with the stock paths swapped for the test's. */
static void run(const char *command) {
    char a[2048];
    char b[2048];
    replace_all(command, JW_WESTON_INITD_SCRIPT, g_script, a, sizeof(a));
    replace_all(a, JW_GAME_DISPLAY_WESTON_CONF, g_conf, b, sizeof(b));
    unlink(g_out);
    CHECK(jw_weston_initd_run(b) == 0);
    CHECK(access(g_out, F_OK) == 0);
}

static void build(int mode, bool use120, const char *override, const char *verb,
                  char *buf, size_t size) {
    CHECK(jw_game_display_weston_command(mode, use120, override, verb, buf, size) == 0);
}

static void check_decisions(void) {
    /* Only a connected TV that is not the live output is hidden. */
    CHECK(jw_game_display_hide_external(true, false));
    CHECK(!jw_game_display_hide_external(true, true));
    CHECK(!jw_game_display_hide_external(false, false));
    CHECK(!jw_game_display_hide_external(false, true));

    /* The TV comes back only with a cable and a TV setting. */
    CHECK(jw_game_display_restore_mode(true, 1) == JW_GAME_DISPLAY_TV_4_3);
    CHECK(jw_game_display_restore_mode(true, 2) == JW_GAME_DISPLAY_TV_STRETCH);
    CHECK(jw_game_display_restore_mode(true, 0) == JW_GAME_DISPLAY_PANEL);
    CHECK(jw_game_display_restore_mode(true, 7) == JW_GAME_DISPLAY_PANEL);
    CHECK(jw_game_display_restore_mode(true, -1) == JW_GAME_DISPLAY_PANEL);
    CHECK(jw_game_display_restore_mode(false, 1) == JW_GAME_DISPLAY_PANEL);
    CHECK(jw_game_display_restore_mode(false, 2) == JW_GAME_DISPLAY_PANEL);

    /* After the handoff Weston is down: always restart, even on the same
       output (the old plain start came back mirrored and squeezed). */
    CHECK(jw_game_display_needs_restart(true, 1, 1, 1280, 720, 0, 0));
    CHECK(jw_game_display_needs_restart(true, 0, 0, 720, 960, 720, 960));
    /* The output changes: cable pulled or plugged during the game. */
    CHECK(jw_game_display_needs_restart(false, 1, 0, 1280, 720, 1280, 720));
    CHECK(jw_game_display_needs_restart(false, 0, 2, 720, 960, 720, 960));
    /* An unknown output before the game is rebuilt. */
    CHECK(jw_game_display_needs_restart(false, -1, 1, 1280, 720, 1280, 720));
    /* A game beside Weston that left the panel alone: nothing to do. */
    CHECK(!jw_game_display_needs_restart(false, 0, 0, 720, 960, 720, 960));
    CHECK(!jw_game_display_needs_restart(false, 1, 1, 1280, 720, 1280, 720));
    /* mupen64plus left 1024x768 on the TV. */
    CHECK(jw_game_display_needs_restart(false, 1, 1, 1280, 720, 1024, 768));
    /* ... or no mode at all. */
    CHECK(jw_game_display_needs_restart(false, 1, 1, 1280, 720, 0, 0));
    /* Nothing known from before: nothing to compare. */
    CHECK(!jw_game_display_needs_restart(false, 0, 0, 0, 0, 1024, 768));
}

/* The HDMI Output switch must keep producing exactly the commands it always
   has; the builder replaced its inline snprintf calls. */
static void check_switch_commands_unchanged(void) {
    char buf[1024];
    build(0, false, OVERRIDE_INI, "restart", buf, sizeof(buf));
    CHECK(strcmp(buf,
        "rm -f /tmp/.weston_drm.conf " OVERRIDE_INI "; "
        JW_WESTON_ROOTFS_ENV " WESTON_DRM_SINGLE_HEAD=1 WESTON_DRM_PRIMARY=DSI-1 "
        JW_WESTON_INITD_SCRIPT " restart") == 0);

    build(1, false, OVERRIDE_INI, "restart", buf, sizeof(buf));
    CHECK(strcmp(buf,
        "rm -f " OVERRIDE_INI "; "
        "printf 'output:HDMI-A-1:mode=1280x720\\noutput:HDMI-A-1:rect=<160,0,1120,720>\\n"
        "output:HDMI-A-1:primary\\n' > /tmp/.weston_drm.conf; "
        JW_WESTON_ROOTFS_ENV " WESTON_DRM_SINGLE_HEAD=1 WESTON_DRM_PRIMARY=HDMI-A-1 "
        "WESTON_DRM_VIRTUAL_SIZE=960x720 WESTON_DRM_CONFIG=/tmp/.weston_drm.conf "
        JW_WESTON_INITD_SCRIPT " restart") == 0);

    build(2, true, OVERRIDE_INI, "restart", buf, sizeof(buf));
    CHECK(strcmp(buf,
        "rm -f " OVERRIDE_INI "; "
        "printf 'output:HDMI-A-1:mode=1920x1080@120\\noutput:HDMI-A-1:primary\\n' > "
        "/tmp/.weston_drm.conf; "
        JW_WESTON_ROOTFS_ENV " WESTON_DRM_SINGLE_HEAD=1 WESTON_DRM_PRIMARY=HDMI-A-1 "
        "WESTON_DRM_VIRTUAL_SIZE=960x720 WESTON_DRM_CONFIG=/tmp/.weston_drm.conf "
        JW_WESTON_INITD_SCRIPT " restart") == 0);

    /* Too small a buffer is an error, not a truncated command. */
    char tiny[32];
    CHECK(jw_game_display_weston_command(1, false, OVERRIDE_INI, "restart",
                                         tiny, sizeof(tiny)) == -1);
}

static void check_commands_run(void) {
    char buf[1024];

    /* TV 4:3 at 720p: the conf file Weston reads, and its settings. */
    build(1, false, OVERRIDE_INI, "restart", buf, sizeof(buf));
    run(buf);
    CHECK(strcmp(slurp(g_conf),
                 "output:HDMI-A-1:mode=1280x720\n"
                 "output:HDMI-A-1:rect=<160,0,1120,720>\n"
                 "output:HDMI-A-1:primary\n") == 0);
    char *seen = slurp(g_out);
    CHECK(has_line(seen, "verb=restart"));
    CHECK(has_line(seen, "cwd=/"));
    CHECK(has_line(seen, "WESTON_DRM_SINGLE_HEAD=1"));
    CHECK(has_line(seen, "WESTON_DRM_PRIMARY=HDMI-A-1"));
    CHECK(has_line(seen, "WESTON_DRM_VIRTUAL_SIZE=960x720"));
    char config_line[PATH_MAX + 32];
    snprintf(config_line, sizeof(config_line), "WESTON_DRM_CONFIG=%s", g_conf);
    CHECK(has_line(seen, config_line));

    /* TV 4:3 at 1080p120 uses the wider rect. */
    build(1, true, OVERRIDE_INI, "restart", buf, sizeof(buf));
    run(buf);
    CHECK(strstr(slurp(g_conf), "output:HDMI-A-1:mode=1920x1080@120\n"
                                "output:HDMI-A-1:rect=<240,0,1680,1080>\n"));

    /* Stretch has no rect. */
    build(2, false, OVERRIDE_INI, "restart", buf, sizeof(buf));
    run(buf);
    CHECK(strcmp(slurp(g_conf),
                 "output:HDMI-A-1:mode=1280x720\noutput:HDMI-A-1:primary\n") == 0);

    /* The panel after a game: the HDMI conf goes, single-head on DSI-1, and
       the panel's refresh-rate override is not named at all. */
    build(0, false, NULL, "restart", buf, sizeof(buf));
    CHECK(!strstr(buf, OVERRIDE_INI));
    run(buf);
    CHECK(access(g_conf, F_OK) != 0);
    seen = slurp(g_out);
    CHECK(has_line(seen, "WESTON_DRM_SINGLE_HEAD=1"));
    CHECK(has_line(seen, "WESTON_DRM_PRIMARY=DSI-1"));
    CHECK(!has_line(seen, config_line));

    /* The verb passes through. */
    build(0, false, NULL, "start", buf, sizeof(buf));
    run(buf);
    CHECK(has_line(slurp(g_out), "verb=start"));
}

int main(void) {
    check_decisions();
    check_switch_commands_unchanged();

    CHECK(mkdtemp(g_root));
    snprintf(g_script, sizeof(g_script), "%s/S49weston", g_root);
    snprintf(g_conf, sizeof(g_conf), "%s/weston_drm.conf", g_root);
    snprintf(g_out, sizeof(g_out), "%s/seen", g_root);
    FILE *f = fopen(g_script, "w");
    CHECK(f);
    fprintf(f,
            "#!/bin/sh\n"
            "{ echo \"verb=$1\"; echo \"cwd=$(pwd)\"; env; } >'%s.tmp' && mv '%s.tmp' '%s'\n",
            g_out, g_out, g_out);
    fclose(f);
    CHECK(chmod(g_script, 0755) == 0);
    CHECK(chdir(g_root) == 0);

    check_commands_run();

    unlink(g_out);
    unlink(g_conf);
    unlink(g_script);
    rmdir(g_root);
    printf("game-display-test: ok\n");
    return 0;
}
