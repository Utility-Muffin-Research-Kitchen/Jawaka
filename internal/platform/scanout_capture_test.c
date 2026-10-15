/* Screenshot and menu-still capture: the size line, turning the panel's
   portrait buffer upright, leaving a TV's landscape buffer alone, and a real
   grab through a fake kmsgrab. */
#include "internal/platform/scanout_capture.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "scanout-capture-test: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

/* A B,G,R,X frame whose pixel (x, y) is B=x, G=y, R=7, X=0x5a; `pad` extra
   bytes per row. */
static uint8_t *make_frame(int w, int h, int pad, jw_scanout_format *f) {
    f->width = w;
    f->height = h;
    f->pitch = w * 4 + pad;
    uint8_t *raw = calloc((size_t)f->pitch * (size_t)h, 1);
    CHECK(raw);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t *px = raw + (size_t)y * f->pitch + (size_t)x * 4;
            px[0] = (uint8_t)x;
            px[1] = (uint8_t)y;
            px[2] = 7;
            px[3] = 0x5a;
        }
    }
    return raw;
}

static void check_header(void) {
    jw_scanout_format f;
    CHECK(jw_scanout_parse_header("fb handle=1 size=960x720(3840)\nfirst pixel: ff1e331e\n", &f));
    CHECK(f.width == 960 && f.height == 720 && f.pitch == 3840);
    CHECK(!jw_scanout_is_portrait(&f));
    CHECK(jw_scanout_parse_header("fb handle=2 size=720x960(2880)", &f));
    CHECK(jw_scanout_is_portrait(&f));
    int w = 0, h = 0;
    jw_scanout_upright_size(&f, &w, &h);
    CHECK(w == 960 && h == 720);
    CHECK(jw_scanout_parse_header("size=1280x720(5120)", &f) && f.pitch == 5120);

    CHECK(!jw_scanout_parse_header(NULL, &f));
    CHECK(!jw_scanout_parse_header("", &f));
    CHECK(!jw_scanout_parse_header("Valid crtcs: 85", &f));
    CHECK(!jw_scanout_parse_header("size=960x720", &f));
    CHECK(!jw_scanout_parse_header("size=960x720(100)", &f));     /* pitch below w*4 */
    CHECK(!jw_scanout_parse_header("size=0x720(3840)", &f));
    CHECK(!jw_scanout_parse_header("size=99999x720(400000)", &f));
}

static void check_convert(void) {
    /* The panel: a 2x3 portrait frame becomes 3x2, turned 90° clockwise, the
       mapping the panel captures have always used: out(h-1-y, x) = in(x, y). */
    jw_scanout_format f;
    uint8_t *raw = make_frame(2, 3, 8, &f);
    uint8_t rgb[3 * 2 * 3];
    jw_scanout_to_upright(raw, &f, JW_SCANOUT_RGB24, rgb);
    for (int y = 0; y < 3; y++) {
        for (int x = 0; x < 2; x++) {
            const uint8_t *o = rgb + ((size_t)x * 3 + (size_t)(3 - 1 - y)) * 3;
            CHECK(o[0] == 7 && o[1] == y && o[2] == x);   /* R, G, B */
        }
    }
    uint8_t bgra[3 * 2 * 4];
    jw_scanout_to_upright(raw, &f, JW_SCANOUT_BGRA32, bgra);
    const uint8_t *corner = bgra + ((size_t)0 * 3 + 2) * 4;   /* in(0, 0) */
    CHECK(corner[0] == 0 && corner[1] == 0 && corner[2] == 7 && corner[3] == 255);
    free(raw);

    /* A TV: a 3x2 landscape frame stays as it is, padding skipped. */
    raw = make_frame(3, 2, 4, &f);
    uint8_t rgb2[3 * 2 * 3];
    jw_scanout_to_upright(raw, &f, JW_SCANOUT_RGB24, rgb2);
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 3; x++) {
            const uint8_t *o = rgb2 + ((size_t)y * 3 + (size_t)x) * 3;
            CHECK(o[0] == 7 && o[1] == y && o[2] == x);
        }
    }
    free(raw);
}

/* A fake kmsgrab: --crtc N, a frame of W*H*4 bytes on stdout (pitch = W*4),
   and the size line on stderr. */
static void check_grab(void) {
    char dir[] = "/tmp/jawaka-scanout-XXXXXX";
    CHECK(mkdtemp(dir));
    char tool[PATH_MAX];
    snprintf(tool, sizeof(tool), "%s/kmsgrab", dir);
    FILE *f = fopen(tool, "w");
    CHECK(f);
    fprintf(f,
            "#!/bin/sh\n"
            "[ \"$1\" = --crtc ] && [ \"$2\" = 85 ] || exit 1\n"
            "echo \"fb handle=1 size=${FAKE_W}x${FAKE_H}($((FAKE_W * 4)))\" >&2\n"
            "echo 'first pixel: ffffffff' >&2\n"
            "head -c $((FAKE_W * FAKE_H * 4)) /dev/zero | tr '\\000' '\\001'\n");
    fclose(f);
    CHECK(chmod(tool, 0755) == 0);

    jw_scanout_format fmt;
    CHECK(setenv("FAKE_W", "960", 1) == 0 && setenv("FAKE_H", "720", 1) == 0);
    uint8_t *raw = jw_scanout_grab(tool, 85, &fmt);
    CHECK(raw);
    CHECK(fmt.width == 960 && fmt.height == 720 && fmt.pitch == 3840);
    CHECK(raw[0] == 1 && raw[(size_t)3840 * 720 - 1] == 1);
    free(raw);

    /* Bigger than the first 4 MB buffer: 1920x1080 grows it. */
    CHECK(setenv("FAKE_W", "1920", 1) == 0 && setenv("FAKE_H", "1080", 1) == 0);
    raw = jw_scanout_grab(tool, 85, &fmt);
    CHECK(raw && fmt.width == 1920 && fmt.height == 1080);
    free(raw);

    /* No size line: refused rather than guessed. */
    CHECK(jw_scanout_grab(tool, 7, &fmt) == NULL);

    unlink(tool);
    rmdir(dir);
}

int main(void) {
    check_header();
    check_convert();
    check_grab();
    printf("scanout-capture-test: ok\n");
    return 0;
}
