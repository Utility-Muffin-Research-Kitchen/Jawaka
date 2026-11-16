#include "cmd/jawakad/osd_respawn.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* A long-running OSD that lost Weston comes back as soon as Weston does. */
static void test_steady_exit_respawns_at_once(void) {
    jw_osd_respawn r = {0};
    jw_osd_respawn_after_exit(&r, true, 7LL * 3600 * 1000, 1000);
    assert(r.pending);
    assert(jw_osd_respawn_due(&r, 1000));
    assert(jw_osd_respawn_deadline_ms(&r) == 1000);
}

/* Weston still down: the next look is one poll interval away, not every pass. */
static void test_display_down_polls(void) {
    jw_osd_respawn r = {0};
    jw_osd_respawn_after_exit(&r, true, 60000, 1000);
    assert(jw_osd_respawn_due(&r, 1000));
    jw_osd_respawn_display_down(&r, 1000);
    assert(!jw_osd_respawn_due(&r, 1000 + JW_OSD_RESPAWN_DISPLAY_POLL_MS - 1));
    assert(jw_osd_respawn_due(&r, 1000 + JW_OSD_RESPAWN_DISPLAY_POLL_MS));
    assert(jw_osd_respawn_deadline_ms(&r) == 1000 + JW_OSD_RESPAWN_DISPLAY_POLL_MS);
    /* Still pending however long Weston stays away. */
    for (int i = 0; i < 100; i++) {
        long long now = 2000 + i * JW_OSD_RESPAWN_DISPLAY_POLL_MS;
        jw_osd_respawn_display_down(&r, now);
        assert(r.pending);
    }
}

/* An OSD that fails at start (no compositor, no font) backs off, doubling up
   to the cap, so the reaper never turns into a fork loop. */
static void test_quick_exits_back_off(void) {
    jw_osd_respawn r = {0};
    long long now = 0;
    long long expected = JW_OSD_RESPAWN_FIRST_DELAY_MS;
    for (int i = 0; i < 12; i++) {
        jw_osd_respawn_after_exit(&r, false, 20, now);
        assert(r.pending);
        assert(!jw_osd_respawn_due(&r, now + expected - 1));
        assert(jw_osd_respawn_due(&r, now + expected));
        now += expected;
        jw_osd_respawn_clear(&r);   /* the spawn */
        assert(!r.pending);
        assert(jw_osd_respawn_deadline_ms(&r) == -1);
        expected *= 2;
        if (expected > JW_OSD_RESPAWN_MAX_DELAY_MS) expected = JW_OSD_RESPAWN_MAX_DELAY_MS;
    }
    assert(r.not_before_ms == 0);

    /* Ready but short-lived counts as quick too. */
    jw_osd_respawn_after_exit(&r, true, JW_OSD_RESPAWN_STEADY_MS - 1, now);
    assert(!jw_osd_respawn_due(&r, now + JW_OSD_RESPAWN_MAX_DELAY_MS - 1));

    /* A steady run clears the count. */
    jw_osd_respawn_clear(&r);
    jw_osd_respawn_after_exit(&r, true, JW_OSD_RESPAWN_STEADY_MS, now);
    assert(jw_osd_respawn_due(&r, now));
    jw_osd_respawn_clear(&r);
    jw_osd_respawn_after_exit(&r, false, 0, now);
    assert(jw_osd_respawn_due(&r, now + JW_OSD_RESPAWN_FIRST_DELAY_MS));
    assert(!jw_osd_respawn_due(&r, now + JW_OSD_RESPAWN_FIRST_DELAY_MS - 1));
}

static void test_socket_path(void) {
    char path[256];
    unsetenv("WAYLAND_DISPLAY");
    unsetenv("XDG_RUNTIME_DIR");
    assert(jw_osd_wayland_socket_path(path, sizeof(path)) == 0);
    assert(strcmp(path, "/var/run/wayland-0") == 0);

    setenv("XDG_RUNTIME_DIR", "/run/user/0", 1);
    setenv("WAYLAND_DISPLAY", "wayland-1", 1);
    assert(jw_osd_wayland_socket_path(path, sizeof(path)) == 0);
    assert(strcmp(path, "/run/user/0/wayland-1") == 0);

    setenv("WAYLAND_DISPLAY", "/tmp/w", 1);
    assert(jw_osd_wayland_socket_path(path, sizeof(path)) == 0);
    assert(strcmp(path, "/tmp/w") == 0);

    char tiny[4];
    assert(jw_osd_wayland_socket_path(tiny, sizeof(tiny)) == -1);
    unsetenv("WAYLAND_DISPLAY");
    unsetenv("XDG_RUNTIME_DIR");
}

static int listen_on(const char *path) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    assert(strlen(path) < sizeof(addr.sun_path));
    strcpy(addr.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(fd >= 0);
    assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(fd, 4) == 0);
    return fd;
}

/* Up only while something listens: a missing socket and the stale file a
   killed Weston leaves behind both read as down. */
static void test_probe(void) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0]) tmp = "/tmp";
    char dir[200];
    snprintf(dir, sizeof(dir), "%s/osd-respawn-XXXXXX", tmp);
    assert(mkdtemp(dir));
    char path[256];
    snprintf(path, sizeof(path), "%s/wayland-0", dir);

    assert(!jw_osd_wayland_accepts(path));
    assert(!jw_osd_wayland_accepts(NULL));

    int fd = listen_on(path);
    assert(jw_osd_wayland_accepts(path));
    assert(jw_osd_wayland_accepts(path));   /* repeated looks stay harmless */

    close(fd);                               /* the file stays: stale socket */
    assert(access(path, F_OK) == 0);
    assert(!jw_osd_wayland_accepts(path));

    unlink(path);
    fd = listen_on(path);                    /* Weston back on the same name */
    assert(jw_osd_wayland_accepts(path));
    close(fd);
    unlink(path);
    rmdir(dir);

    char longpath[300];
    memset(longpath, 'a', sizeof(longpath) - 1);
    longpath[0] = '/';
    longpath[sizeof(longpath) - 1] = '\0';
    assert(!jw_osd_wayland_accepts(longpath));
}

int main(void) {
    test_steady_exit_respawns_at_once();
    test_display_down_polls();
    test_quick_exits_back_off();
    test_socket_path();
    test_probe();
    puts("osd-respawn-test: ok");
    return 0;
}
