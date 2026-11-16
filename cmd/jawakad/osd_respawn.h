#ifndef JW_OSD_RESPAWN_H
#define JW_OSD_RESPAWN_H

/* When jawaka-osd exits on its own, jawakad starts a new one. The OSD exits
   when its compositor goes away (a port's exit trap or a hand restart of
   Weston), because a Wayland client cannot carry its objects over to a new
   compositor. Starting the replacement at once would only work if Weston were
   already back, and an OSD started without a compositor fails at once, which
   would turn the reaper into a loop of forks. So the restart is decided here:
     - an OSD that was ready and ran at least JW_OSD_RESPAWN_STEADY_MS may come
       back at once;
     - any other exit waits, from JW_OSD_RESPAWN_FIRST_DELAY_MS doubling to
       JW_OSD_RESPAWN_MAX_DELAY_MS, and a steady run clears the count;
     - either way it waits for a compositor that accepts connections, looked
       for every JW_OSD_RESPAWN_DISPLAY_POLL_MS.
   Header-only so the policy and the probe are tested on the host. */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define JW_OSD_RESPAWN_STEADY_MS        5000LL
#define JW_OSD_RESPAWN_FIRST_DELAY_MS    250LL
#define JW_OSD_RESPAWN_MAX_DELAY_MS    30000LL
#define JW_OSD_RESPAWN_DISPLAY_POLL_MS   500LL

typedef struct {
    bool pending;           /* an OSD exited and none has been started since */
    int quick_exits;        /* exits since the last steady run */
    long long not_before_ms;
} jw_osd_respawn;

static inline void jw_osd_respawn_clear(jw_osd_respawn *r) {
    r->pending = false;
    r->not_before_ms = 0;
}

static inline void jw_osd_respawn_after_exit(jw_osd_respawn *r, bool was_ready,
                                             long long ran_ms, long long now_ms) {
    r->pending = true;
    if (was_ready && ran_ms >= JW_OSD_RESPAWN_STEADY_MS) {
        r->quick_exits = 0;
        r->not_before_ms = now_ms;
        return;
    }
    long long delay = JW_OSD_RESPAWN_FIRST_DELAY_MS;
    for (int i = 0; i < r->quick_exits && delay < JW_OSD_RESPAWN_MAX_DELAY_MS; i++) {
        delay *= 2;
    }
    if (delay > JW_OSD_RESPAWN_MAX_DELAY_MS) delay = JW_OSD_RESPAWN_MAX_DELAY_MS;
    if (r->quick_exits < 1000) r->quick_exits++;
    r->not_before_ms = now_ms + delay;
}

/* True once a respawn is pending and its wait has run out; the caller then
   looks for the compositor. */
static inline bool jw_osd_respawn_due(const jw_osd_respawn *r, long long now_ms) {
    return r->pending && now_ms >= r->not_before_ms;
}

/* The compositor is not there yet: look again after the poll interval. */
static inline void jw_osd_respawn_display_down(jw_osd_respawn *r, long long now_ms) {
    r->not_before_ms = now_ms + JW_OSD_RESPAWN_DISPLAY_POLL_MS;
}

/* When the loop must wake to act on a pending respawn, or -1 for none. */
static inline long long jw_osd_respawn_deadline_ms(const jw_osd_respawn *r) {
    return r->pending ? r->not_before_ms : -1;
}

/* The socket the OSD's wl_display_connect(NULL) opens, with the same
   defaults the OSD sets when the session exports neither variable. */
static inline int jw_osd_wayland_socket_path(char *out, size_t out_size) {
    const char *display = getenv("WAYLAND_DISPLAY");
    if (!display || !display[0]) display = "wayland-0";
    int n;
    if (display[0] == '/') {
        n = snprintf(out, out_size, "%s", display);
    } else {
        const char *runtime = getenv("XDG_RUNTIME_DIR");
        if (!runtime || !runtime[0]) runtime = "/var/run";
        n = snprintf(out, out_size, "%s/%s", runtime, display);
    }
    return n > 0 && (size_t)n < out_size ? 0 : -1;
}

/* True when something listens on the socket. A file alone is not enough:
   Weston killed outright leaves its socket behind, and connecting to that
   fails with ECONNREFUSED. A compositor that is up sees one client come and
   go. Never blocks. */
static inline bool jw_osd_wayland_accepts(const char *path) {
    struct sockaddr_un addr;
    if (!path || strlen(path) >= sizeof(addr.sun_path)) return false;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, strlen(path) + 1);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    (void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    int rc;
    do {
        rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    } while (rc != 0 && errno == EINTR);
    /* A full backlog still means a listener. */
    bool up = rc == 0 || errno == EAGAIN || errno == EINPROGRESS;
    close(fd);
    return up;
}

#endif /* JW_OSD_RESPAWN_H */
