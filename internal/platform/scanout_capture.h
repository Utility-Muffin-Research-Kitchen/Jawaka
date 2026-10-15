#ifndef JW_PLATFORM_SCANOUT_CAPTURE_H
#define JW_PLATFORM_SCANOUT_CAPTURE_H

/* Reading what the display shows with the on-device `kmsgrab` tool, for the
   screenshot hotkey (jawakad) and the in-game menu's paused-frame still
   (jawaka-menu).

   kmsgrab writes the CRTC's framebuffer raw (bytes B,G,R,X) to stdout and
   describes it on stderr as "fb handle=N size=WxH(PITCH)". On the MLP1 panel
   that is the portrait 720x960 buffer, mounted sideways, so it is turned 90°
   clockwise to be upright. With a TV it is Weston's landscape 960x720 output,
   the same number of bytes, or the mode a game set up; those are already
   upright. Reading the size line instead of assuming the panel's is what keeps
   TV captures from coming out scrambled. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    int width;
    int height;
    int pitch;   /* bytes per row */
} jw_scanout_format;

/* Parse kmsgrab's "size=WxH(PITCH)". False when absent or implausible. */
static inline bool jw_scanout_parse_header(const char *text, jw_scanout_format *out) {
    const char *p = text ? strstr(text, "size=") : NULL;
    int w = 0, h = 0, pitch = 0;
    if (!p || sscanf(p, "size=%dx%d(%d)", &w, &h, &pitch) != 3) {
        return false;
    }
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || pitch < w * 4 || pitch > w * 16) {
        return false;
    }
    out->width = w;
    out->height = h;
    out->pitch = pitch;
    return true;
}

/* A portrait buffer is the sideways-mounted panel's. */
static inline bool jw_scanout_is_portrait(const jw_scanout_format *f) {
    return f->height > f->width;
}

/* The size of the upright image. */
static inline void jw_scanout_upright_size(const jw_scanout_format *f, int *w, int *h) {
    if (jw_scanout_is_portrait(f)) {
        *w = f->height;
        *h = f->width;
    } else {
        *w = f->width;
        *h = f->height;
    }
}

enum {
    JW_SCANOUT_RGB24 = 3,   /* R,G,B: the screenshot PNG */
    JW_SCANOUT_BGRA32 = 4,  /* B,G,R,255: SDL ARGB8888 on little-endian */
};

/* Convert a raw B,G,R,X frame to an upright image in `dst`, which holds
   upright width * height * out_bpp bytes. */
static inline void jw_scanout_to_upright(const uint8_t *raw, const jw_scanout_format *f,
                                         int out_bpp, uint8_t *dst) {
    int ow = 0, oh = 0;
    jw_scanout_upright_size(f, &ow, &oh);
    bool rotate = jw_scanout_is_portrait(f);
    for (int y = 0; y < f->height; y++) {
        const uint8_t *row = raw + (size_t)y * (size_t)f->pitch;
        for (int x = 0; x < f->width; x++) {
            const uint8_t *px = row + (size_t)x * 4u;
            int nx = rotate ? f->height - 1 - y : x;   /* 90° clockwise */
            int ny = rotate ? x : y;
            uint8_t *o = dst + ((size_t)ny * (size_t)ow + (size_t)nx) * (size_t)out_bpp;
            if (out_bpp == JW_SCANOUT_RGB24) {
                o[0] = px[2];
                o[1] = px[1];
                o[2] = px[0];
            } else {
                o[0] = px[0];
                o[1] = px[1];
                o[2] = px[2];
                o[3] = 255;
            }
        }
    }
}

/* Largest frame accepted: 4K at 4 bytes per pixel, with room for a wide pitch. */
#define JW_SCANOUT_MAX_BYTES ((size_t)64 * 1024 * 1024)

/* Run `kmsgrab --crtc <crtc>` (`tool` is its path) and return the raw frame,
   malloc'd, with its format; NULL on any failure. The size line goes through a
   temp file in /tmp, since popen gives only stdout. */
static inline uint8_t *jw_scanout_grab(const char *tool, int crtc, jw_scanout_format *fmt) {
    char err_path[] = "/tmp/jw-kmsgrab-XXXXXX";
    int fd = mkstemp(err_path);
    if (fd < 0) {
        return NULL;
    }
    close(fd);
    char cmd[160];
    int n = snprintf(cmd, sizeof(cmd), "%s --crtc %d 2>'%s'", tool, crtc, err_path);
    FILE *cap = (n > 0 && (size_t)n < sizeof(cmd)) ? popen(cmd, "r") : NULL;
    if (!cap) {
        unlink(err_path);
        return NULL;
    }
    uint8_t *buf = NULL;
    size_t cap_size = 0, got = 0;
    bool ok = true;
    for (;;) {
        if (got == cap_size) {
            size_t want = cap_size ? cap_size * 2 : (size_t)4 * 1024 * 1024;
            uint8_t *grown = want <= JW_SCANOUT_MAX_BYTES ? (uint8_t *)realloc(buf, want) : NULL;
            if (!grown) {
                ok = false;
                break;
            }
            buf = grown;
            cap_size = want;
        }
        size_t r = fread(buf + got, 1, cap_size - got, cap);
        if (r == 0) {
            break;
        }
        got += r;
    }
    pclose(cap);

    char text[512] = "";
    FILE *ef = fopen(err_path, "r");
    if (ef) {
        size_t t = fread(text, 1, sizeof(text) - 1, ef);
        text[t] = '\0';
        fclose(ef);
    }
    unlink(err_path);

    if (!ok || !jw_scanout_parse_header(text, fmt) ||
        got < (size_t)fmt->pitch * (size_t)fmt->height) {
        free(buf);
        return NULL;
    }
    return buf;
}

#endif
