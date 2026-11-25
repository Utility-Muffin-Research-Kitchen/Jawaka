/* jawaka-ledd's xylem side (OS=xylem, -DJW_OS_XYLEM): the ring as LED-class
 * devices, and the battery found by type.
 *
 * Stock writes 8 ARGB words to the vendor driver's mmrgball node and freezes
 * loong_light while it does. xylem has neither. Mainline drives the AW20036
 * with leds-aw200xx, one LED-class device per channel: 8 ring LEDs times red,
 * green and blue, 24 devices under /sys/class/leds. Their names come from the
 * device tree, so they are read, in ring order and red, green, blue per LED,
 * from:
 *   - JAWAKA_LEDD_LEDS: the 24 names separated by commas;
 *   - else the file named by JAWAKA_LEDD_LEDS_FILE: the same names separated
 *     by commas, spaces or newlines, with '#' starting a comment;
 *   - else ring<N>:red, ring<N>:green, ring<N>:blue for N = 0..7.
 *
 * A frame's 0xAARRGGBB for one LED becomes, per channel, colour * alpha / 255
 * scaled to that device's max_brightness. Only channels whose value changed
 * are written, plus every channel once every JW_LEDD_CLASS_FULL_MS of
 * CLOCK_BOOTTIME: that rewrite also follows a resume, when the chip may have
 * lost its registers, and reopens any device that was missing.
 *
 * Included by main.c only; static state, one translation unit. */
#ifndef JW_LEDD_XYLEM_H
#define JW_LEDD_XYLEM_H

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "internal/platform/power_supply_sysfs.h"

#define JW_LEDD_CLASS_LEDS      8
#define JW_LEDD_CLASS_CHANNELS  (JW_LEDD_CLASS_LEDS * 3)
#ifndef JW_LEDD_CLASS_DIR   /* overridable so a test can point at a fixture */
#define JW_LEDD_CLASS_DIR       "/sys/class/leds"
#endif
#define JW_LEDD_CLASS_NAME_MAX  64
#define JW_LEDD_CLASS_FULL_MS   5000LL

/* Battery Level reads the gauge the type lookup finds (rk817-battery on the
   MLP1) instead of stock's power_supply/battery. */
static char g_ledd_battery_dir[JW_POWER_SUPPLY_PATH_MAX];
static char g_ledd_capacity_path[JW_POWER_SUPPLY_PATH_MAX + 16];

static const char *jw_ledd_xylem_capacity_path(void) {
    const char *dir = jw_power_supply_battery_dir(g_ledd_battery_dir);
    snprintf(g_ledd_capacity_path, sizeof(g_ledd_capacity_path), "%s/capacity",
             dir ? dir : JW_POWER_SUPPLY_SYSFS_DIR "/rk817-battery");
    return g_ledd_capacity_path;
}
#define JW_LEDD_CAPACITY_PATH jw_ledd_xylem_capacity_path()

typedef struct {
    char name[JW_LEDD_CLASS_NAME_MAX];
    int fd;     /* the brightness attribute, -1 while not open */
    int max;    /* max_brightness */
    int last;   /* value last written, -1 unknown */
} jw_ledd_class_channel;

static jw_ledd_class_channel g_ledd_class[JW_LEDD_CLASS_CHANNELS];
static long long g_ledd_class_full_ms = -1;

static long long jw_ledd_class_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Split `text` into up to JW_LEDD_CLASS_CHANNELS names at commas and white
   space; with `comments`, '#' runs to the end of its line. Returns the count. */
static int jw_ledd_class_parse(const char *text, bool comments) {
    int count = 0;
    const char *p = text;
    while (p && *p && count < JW_LEDD_CLASS_CHANNELS) {
        if (comments && *p == '#') {
            while (*p && *p != '\n') p++;
            continue;
        }
        if (*p == ',' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            p++;
            continue;
        }
        const char *start = p;
        while (*p && *p != ',' && *p != ' ' && *p != '\t' && *p != '\r' &&
               *p != '\n' && !(comments && *p == '#')) {
            p++;
        }
        size_t len = (size_t)(p - start);
        if (len >= JW_LEDD_CLASS_NAME_MAX) {
            fprintf(stderr, "jawaka-ledd: LED name too long: %.*s\n", (int)len, start);
            len = JW_LEDD_CLASS_NAME_MAX - 1;
        }
        memcpy(g_ledd_class[count].name, start, len);
        g_ledd_class[count].name[len] = '\0';
        count++;
    }
    return count;
}

static char *jw_ledd_class_read_file(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;
    char *buf = calloc(1, 4096);
    if (buf) {
        size_t n = fread(buf, 1, 4095, fp);
        buf[n] = '\0';
    }
    fclose(fp);
    return buf;
}

static void jw_ledd_class_try_open(jw_ledd_class_channel *ch) {
    if (ch->fd >= 0 || !ch->name[0]) return;
    char path[sizeof(JW_LEDD_CLASS_DIR) + JW_LEDD_CLASS_NAME_MAX + 32];
    snprintf(path, sizeof(path), "%s/%s/brightness", JW_LEDD_CLASS_DIR, ch->name);
    ch->fd = open(path, O_WRONLY | O_CLOEXEC);
    if (ch->fd < 0) return;
    char dir[sizeof(JW_LEDD_CLASS_DIR) + JW_LEDD_CLASS_NAME_MAX + 8];
    snprintf(dir, sizeof(dir), "%s/%s", JW_LEDD_CLASS_DIR, ch->name);
    long max = 0;
    ch->max = jw_power_supply_read_long(dir, "max_brightness", &max) && max > 0
                  ? (int)max : 255;
    ch->last = -1;
}

/* Resolve the 24 names and open what exists. Missing devices are reported
   once here and looked for again on every full rewrite. */
static void jw_ledd_class_open(void) {
    for (int i = 0; i < JW_LEDD_CLASS_CHANNELS; i++) {
        g_ledd_class[i].name[0] = '\0';
        g_ledd_class[i].fd = -1;
        g_ledd_class[i].max = 255;
        g_ledd_class[i].last = -1;
    }

    const char *source = NULL;
    int count = 0;
    const char *list = getenv("JAWAKA_LEDD_LEDS");
    const char *file = getenv("JAWAKA_LEDD_LEDS_FILE");
    if (list && list[0]) {
        count = jw_ledd_class_parse(list, false);
        source = "JAWAKA_LEDD_LEDS";
    } else if (file && file[0]) {
        char *text = jw_ledd_class_read_file(file);
        if (text) {
            count = jw_ledd_class_parse(text, true);
            free(text);
            source = file;
        } else {
            fprintf(stderr, "jawaka-ledd: cannot read JAWAKA_LEDD_LEDS_FILE %s: %s\n",
                    file, strerror(errno));
        }
    }
    if (count == 0) {
        static const char *const colors[3] = { "red", "green", "blue" };
        for (int i = 0; i < JW_LEDD_CLASS_CHANNELS; i++) {
            snprintf(g_ledd_class[i].name, sizeof(g_ledd_class[i].name), "ring%d:%s",
                     i / 3, colors[i % 3]);
        }
        count = JW_LEDD_CLASS_CHANNELS;
        source = "default ring<N>:<color> names";
    } else if (count < JW_LEDD_CLASS_CHANNELS) {
        fprintf(stderr, "jawaka-ledd: %s names %d of %d ring channels; "
                "the rest stay dark\n", source, count, JW_LEDD_CLASS_CHANNELS);
    }

    int opened = 0;
    for (int i = 0; i < count; i++) {
        jw_ledd_class_try_open(&g_ledd_class[i]);
        if (g_ledd_class[i].fd >= 0) {
            opened++;
        } else {
            fprintf(stderr, "jawaka-ledd: %s/%s/brightness: %s\n",
                    JW_LEDD_CLASS_DIR, g_ledd_class[i].name, strerror(errno));
        }
    }
    if (opened < count) {
        fprintf(stderr, "jawaka-ledd: %d of %d ring LEDs open (%s)\n",
                opened, count, source);
    }
}

static void jw_ledd_class_write(const uint32_t *argb, int leds) {
    if (leds > JW_LEDD_CLASS_LEDS) leds = JW_LEDD_CLASS_LEDS;
    long long now = jw_ledd_class_now_ms();
    bool full = g_ledd_class_full_ms < 0 || now - g_ledd_class_full_ms >= JW_LEDD_CLASS_FULL_MS;
    if (full) g_ledd_class_full_ms = now;
    for (int led = 0; led < leds; led++) {
        uint32_t a = (argb[led] >> 24) & 0xff;
        uint32_t c[3] = { (argb[led] >> 16) & 0xff, (argb[led] >> 8) & 0xff, argb[led] & 0xff };
        for (int k = 0; k < 3; k++) {
            jw_ledd_class_channel *ch = &g_ledd_class[led * 3 + k];
            if (full) jw_ledd_class_try_open(ch);
            if (ch->fd < 0) continue;
            int value = (int)((c[k] * a * (uint32_t)ch->max + 255u * 255u / 2u) / (255u * 255u));
            if (!full && value == ch->last) continue;
            char buf[16];
            int len = snprintf(buf, sizeof(buf), "%d\n", value);
            if (pwrite(ch->fd, buf, (size_t)len, 0) == (ssize_t)len) {
                ch->last = value;
            } else {
                ch->last = -1;   /* try again on the next frame */
            }
        }
    }
}

#endif /* JW_LEDD_XYLEM_H */
