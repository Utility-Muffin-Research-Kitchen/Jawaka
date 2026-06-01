#include "cmd/jawaka-osd/osd_backend.h"

#include "xdg-shell-client-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#define JW_OSD_HIDE_AFTER_MS 1200u

typedef struct {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;

    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_buffer *buffer;
    void *pixels;
    size_t buffer_size;
    int width;
    int height;
    int percent;
    int mode;  /* 0 = brightness, 1 = volume */
    uint64_t hide_at;
    bool visible;
    bool configured;
} jw_wayland_osd;

static jw_wayland_osd s_osd;

static int jw__env_int(const char *name, int fallback) {
    const char *value = getenv(name);
    if (!value || !value[0]) {
        return fallback;
    }
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || (end && *end != '\0') || parsed <= 0 || parsed > 4096) {
        return fallback;
    }
    return (int)parsed;
}

#define JW_OSD_SHM_TEMPLATE "/tmp/jawaka-osd-shm-XXXXXX"

static int jw__create_shm_file(size_t size) {
    char template[] = JW_OSD_SHM_TEMPLATE;
    int fd = mkstemp(template);
    if (fd < 0) {
        return -1;
    }
    unlink(template);
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static uint32_t jw__argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void jw__fill_rect(uint32_t *pixels, int width, int height,
                          int x, int y, int w, int h, uint32_t color) {
    if (!pixels || w <= 0 || h <= 0) {
        return;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > width) w = width - x;
    if (y + h > height) h = height - y;
    if (w <= 0 || h <= 0) {
        return;
    }

    for (int row = y; row < y + h; row++) {
        uint32_t *dst = pixels + row * width + x;
        for (int col = 0; col < w; col++) {
            dst[col] = color;
        }
    }
}

static void jw__draw_segment(uint32_t *pixels, int width, int height,
                             int x, int y, int scale, int segment, uint32_t color) {
    int t = scale;
    int l = scale * 5;
    switch (segment) {
        case 0: jw__fill_rect(pixels, width, height, x + t, y, l, t, color); break;
        case 1: jw__fill_rect(pixels, width, height, x + l + t, y + t, t, l, color); break;
        case 2: jw__fill_rect(pixels, width, height, x + l + t, y + l + 2 * t, t, l, color); break;
        case 3: jw__fill_rect(pixels, width, height, x + t, y + 2 * l + 2 * t, l, t, color); break;
        case 4: jw__fill_rect(pixels, width, height, x, y + l + 2 * t, t, l, color); break;
        case 5: jw__fill_rect(pixels, width, height, x, y + t, t, l, color); break;
        case 6: jw__fill_rect(pixels, width, height, x + t, y + l + t, l, t, color); break;
    }
}

static void jw__draw_digit(uint32_t *pixels, int width, int height,
                           int x, int y, int scale, int digit, uint32_t color) {
    static const uint8_t digits[10] = {
        0x3f, 0x06, 0x5b, 0x4f, 0x66,
        0x6d, 0x7d, 0x07, 0x7f, 0x6f
    };
    if (digit < 0 || digit > 9) {
        return;
    }
    uint8_t mask = digits[digit];
    for (int segment = 0; segment < 7; segment++) {
        if (mask & (1u << segment)) {
            jw__draw_segment(pixels, width, height, x, y, scale, segment, color);
        }
    }
}

static void jw__draw_percent(uint32_t *pixels, int width, int height,
                             int x, int y, int percent, uint32_t color) {
    int scale = 3;
    int digit_w = scale * 7;
    if (percent >= 100) {
        jw__draw_digit(pixels, width, height, x, y, scale, 1, color);
        x += digit_w + scale * 2;
        jw__draw_digit(pixels, width, height, x, y, scale, 0, color);
        x += digit_w + scale * 2;
        jw__draw_digit(pixels, width, height, x, y, scale, 0, color);
    } else {
        int tens = percent / 10;
        int ones = percent % 10;
        if (tens > 0) {
            jw__draw_digit(pixels, width, height, x, y, scale, tens, color);
            x += digit_w + scale * 2;
        }
        jw__draw_digit(pixels, width, height, x, y, scale, ones, color);
    }
}

static void jw__draw_sun(uint32_t *pixels, int width, int height,
                         int cx, int cy, uint32_t color) {
    jw__fill_rect(pixels, width, height, cx - 8, cy - 8, 16, 16, color);
    jw__fill_rect(pixels, width, height, cx - 2, cy - 20, 4, 8, color);
    jw__fill_rect(pixels, width, height, cx - 2, cy + 12, 4, 8, color);
    jw__fill_rect(pixels, width, height, cx - 20, cy - 2, 8, 4, color);
    jw__fill_rect(pixels, width, height, cx + 12, cy - 2, 8, 4, color);
}

static void jw__draw_speaker(uint32_t *pixels, int width, int height,
                             int cx, int cy, uint32_t color) {
    /* Speaker body */
    jw__fill_rect(pixels, width, height, cx - 12, cy - 6, 8, 12, color);
    /* Speaker cone */
    jw__fill_rect(pixels, width, height, cx - 4, cy - 12, 4, 24, color);
    jw__fill_rect(pixels, width, height, cx,     cy - 16, 4, 32, color);
    /* Sound waves */
    jw__fill_rect(pixels, width, height, cx + 8,  cy - 4, 3, 8, color);
    jw__fill_rect(pixels, width, height, cx + 14, cy - 8, 3, 16, color);
}

static void jw__draw_osd(void) {
    uint32_t *pixels = (uint32_t *)s_osd.pixels;
    int toast_w = 520;
    int toast_h = 96;
    if (toast_w > s_osd.width - 48) {
        toast_w = s_osd.width - 48;
    }
    int x = (s_osd.width - toast_w) / 2;
    int y = s_osd.height - toast_h - 48;

    uint32_t bg = jw__argb(220, 18, 20, 24);
    uint32_t track = jw__argb(255, 72, 76, 84);
    uint32_t fill = jw__argb(255, 250, 210, 92);
    uint32_t knob = jw__argb(255, 255, 240, 150);

    jw__fill_rect(pixels, s_osd.width, s_osd.height, x, y, toast_w, toast_h, bg);
    if (s_osd.mode == 1) {
        jw__draw_speaker(pixels, s_osd.width, s_osd.height, x + 44, y + 48, fill);
    } else {
        jw__draw_sun(pixels, s_osd.width, s_osd.height, x + 44, y + 48, fill);
    }

    int track_x = x + 88;
    int track_y = y + 44;
    int track_w = toast_w - 190;
    jw__fill_rect(pixels, s_osd.width, s_osd.height, track_x, track_y, track_w, 12, track);
    jw__fill_rect(pixels, s_osd.width, s_osd.height, track_x, track_y,
                  (track_w * s_osd.percent) / 100, 12, fill);
    int knob_x = track_x + (track_w * s_osd.percent) / 100 - 7;
    jw__fill_rect(pixels, s_osd.width, s_osd.height, knob_x, track_y - 8, 14, 28, knob);
    jw__draw_percent(pixels, s_osd.width, s_osd.height,
                     x + toast_w - 82, y + 34, s_osd.percent, knob);
}

static void jw__toast_rect(int *out_x, int *out_y, int *out_w, int *out_h) {
    int toast_w = 520;
    int toast_h = 96;
    if (toast_w > s_osd.width - 48) {
        toast_w = s_osd.width - 48;
    }

    if (out_x) *out_x = (s_osd.width - toast_w) / 2;
    if (out_y) *out_y = s_osd.height - toast_h - 48;
    if (out_w) *out_w = toast_w;
    if (out_h) *out_h = toast_h;
}

static void jw__destroy_surface(void) {
    if (s_osd.buffer) {
        wl_buffer_destroy(s_osd.buffer);
        s_osd.buffer = NULL;
    }
    if (s_osd.pixels) {
        munmap(s_osd.pixels, s_osd.buffer_size);
        s_osd.pixels = NULL;
        s_osd.buffer_size = 0;
    }
    if (s_osd.toplevel) {
        xdg_toplevel_destroy(s_osd.toplevel);
        s_osd.toplevel = NULL;
    }
    if (s_osd.xdg_surface) {
        xdg_surface_destroy(s_osd.xdg_surface);
        s_osd.xdg_surface = NULL;
    }
    if (s_osd.surface) {
        wl_surface_destroy(s_osd.surface);
        s_osd.surface = NULL;
    }
    s_osd.visible = false;
    s_osd.configured = false;
}

static void jw__hide_surface(void) {
    if (!s_osd.surface) {
        return;
    }

    jw__destroy_surface();
    if (s_osd.display) {
        wl_display_flush(s_osd.display);
    }
}

static void jw__xdg_wm_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener s_wm_base_listener = {
    .ping = jw__xdg_wm_ping,
};

static void jw__xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
    (void)surface;
    jw_wayland_osd *osd = (jw_wayland_osd *)data;
    xdg_surface_ack_configure(osd->xdg_surface, serial);
    osd->configured = true;
}

static const struct xdg_surface_listener s_xdg_surface_listener = {
    .configure = jw__xdg_surface_configure,
};

static void jw__toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                                   int32_t width, int32_t height,
                                   struct wl_array *states) {
    (void)data;
    (void)toplevel;
    (void)width;
    (void)height;
    (void)states;
}

static void jw__toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    (void)data;
    (void)toplevel;
    jw__destroy_surface();
}

static const struct xdg_toplevel_listener s_toplevel_listener = {
    .configure = jw__toplevel_configure,
    .close = jw__toplevel_close,
};

static void jw__registry_global(void *data, struct wl_registry *registry,
                                uint32_t name, const char *interface,
                                uint32_t version) {
    jw_wayland_osd *osd = (jw_wayland_osd *)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        osd->compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
                                           version < 4 ? version : 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        osd->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        osd->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(osd->wm_base, &s_wm_base_listener, osd);
    }
}

static void jw__registry_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener s_registry_listener = {
    .global = jw__registry_global,
    .global_remove = jw__registry_remove,
};

static int jw__create_buffer(void) {
    s_osd.buffer_size = (size_t)s_osd.width * (size_t)s_osd.height * 4u;
    int fd = jw__create_shm_file(s_osd.buffer_size);
    if (fd < 0) {
        return -1;
    }

    s_osd.pixels = mmap(NULL, s_osd.buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (s_osd.pixels == MAP_FAILED) {
        s_osd.pixels = NULL;
        close(fd);
        return -1;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(s_osd.shm, fd, (int32_t)s_osd.buffer_size);
    close(fd);
    if (!pool) {
        munmap(s_osd.pixels, s_osd.buffer_size);
        s_osd.pixels = NULL;
        return -1;
    }

    s_osd.buffer = wl_shm_pool_create_buffer(pool, 0, s_osd.width, s_osd.height,
                                            s_osd.width * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    if (s_osd.buffer) {
        memset(s_osd.pixels, 0, s_osd.buffer_size);
    }
    return s_osd.buffer ? 0 : -1;
}

static int jw__ensure_surface(void) {
    if (s_osd.surface && s_osd.buffer && s_osd.pixels) {
        return 0;
    }

    s_osd.surface = wl_compositor_create_surface(s_osd.compositor);
    if (!s_osd.surface) {
        return -1;
    }

    struct wl_region *empty = wl_compositor_create_region(s_osd.compositor);
    if (empty) {
        wl_surface_set_input_region(s_osd.surface, empty);
        wl_region_destroy(empty);
    }

    s_osd.xdg_surface = xdg_wm_base_get_xdg_surface(s_osd.wm_base, s_osd.surface);
    s_osd.toplevel = xdg_surface_get_toplevel(s_osd.xdg_surface);
    xdg_surface_add_listener(s_osd.xdg_surface, &s_xdg_surface_listener, &s_osd);
    xdg_toplevel_add_listener(s_osd.toplevel, &s_toplevel_listener, &s_osd);
    xdg_toplevel_set_title(s_osd.toplevel, "Jawaka OSD");
    xdg_toplevel_set_fullscreen(s_osd.toplevel, NULL);
    wl_surface_commit(s_osd.surface);
    wl_display_roundtrip(s_osd.display);

    if (!s_osd.configured) {
        jw__destroy_surface();
        return -1;
    }

    if (jw__create_buffer() != 0) {
        jw__destroy_surface();
        return -1;
    }

    return 0;
}

static int jw__show_surface(void) {
    if (jw__ensure_surface() != 0) {
        return -1;
    }

    jw__draw_osd();
    wl_surface_attach(s_osd.surface, s_osd.buffer, 0, 0);
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    jw__toast_rect(&x, &y, &w, &h);
    wl_surface_damage_buffer(s_osd.surface, x, y, w, h);
    wl_surface_commit(s_osd.surface);
    wl_display_flush(s_osd.display);
    s_osd.visible = true;
    return 0;
}

int jw_osd_backend_init(void) {
    if (!getenv("XDG_RUNTIME_DIR")) {
        setenv("XDG_RUNTIME_DIR", "/var/run", 0);
    }
    if (!getenv("WAYLAND_DISPLAY")) {
        setenv("WAYLAND_DISPLAY", "wayland-0", 0);
    }

    memset(&s_osd, 0, sizeof(s_osd));
    s_osd.width = jw__env_int("CAT_WINDOW_WIDTH", 960);
    s_osd.height = jw__env_int("CAT_WINDOW_HEIGHT", 720);
    s_osd.percent = 50;

    s_osd.display = wl_display_connect(NULL);
    if (!s_osd.display) {
        return -1;
    }
    s_osd.registry = wl_display_get_registry(s_osd.display);
    wl_registry_add_listener(s_osd.registry, &s_registry_listener, &s_osd);
    wl_display_roundtrip(s_osd.display);

    if (!s_osd.compositor || !s_osd.shm || !s_osd.wm_base) {
        jw_osd_backend_shutdown();
        return -1;
    }
    return 0;
}

void jw_osd_backend_show_brightness(int percent, uint64_t now_ms) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_osd.percent = percent;
    s_osd.mode = 0;
    s_osd.hide_at = now_ms + JW_OSD_HIDE_AFTER_MS;
    jw__show_surface();
}

void jw_osd_backend_show_volume(int percent, uint64_t now_ms) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_osd.percent = percent;
    s_osd.mode = 1;
    s_osd.hide_at = now_ms + JW_OSD_HIDE_AFTER_MS;
    jw__show_surface();
}

void jw_osd_backend_tick(uint64_t now_ms) {
    if (s_osd.display) {
        wl_display_dispatch_pending(s_osd.display);
        wl_display_flush(s_osd.display);
    }
    if (s_osd.visible && now_ms >= s_osd.hide_at) {
        jw__hide_surface();
    }
}

void jw_osd_backend_shutdown(void) {
    jw__destroy_surface();
    if (s_osd.wm_base) {
        xdg_wm_base_destroy(s_osd.wm_base);
        s_osd.wm_base = NULL;
    }
    if (s_osd.shm) {
        wl_shm_destroy(s_osd.shm);
        s_osd.shm = NULL;
    }
    if (s_osd.compositor) {
        wl_compositor_destroy(s_osd.compositor);
        s_osd.compositor = NULL;
    }
    if (s_osd.registry) {
        wl_registry_destroy(s_osd.registry);
        s_osd.registry = NULL;
    }
    if (s_osd.display) {
        wl_display_disconnect(s_osd.display);
        s_osd.display = NULL;
    }
}
