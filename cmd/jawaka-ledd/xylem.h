/* jawaka-ledd's xylem side (OS=xylem, -DJW_OS_XYLEM): the ring as the AW20036
 * itself over I2C, and the battery found by type.
 *
 * Stock writes 8 ARGB words to the vendor driver's mmrgball node and freezes
 * loong_light while it does. xylem has neither, and binds no kernel driver to
 * the chip: the helper owns it on /dev/i2c-1 (internal/platform/aw20036_i2c.h).
 *
 * A frame's 0xAARRGGBB for one LED becomes what stock's driver made of an
 * mmrgball word: each channel's FADE is its colour byte and the LED's DIM is
 * alpha >> 2. A frame is one 25-byte write of the FADE page, plus one of the
 * DIM page when alpha changed, and nothing at all when the frame is the one
 * already shown. Breath runs on the chip's own breathing engine instead
 * (main.c), with no frames.
 *
 * The chip's state is checked every JW_LEDD_RING_CHECK_MS of CLOCK_BOOTTIME,
 * and at once after a resume: jawakad puts the chip to sleep before suspending
 * (the GPIO hog keeps it powered, so it would stay lit), and a reset or a
 * failed write leaves it asleep too. Then the chip is set up again and the
 * whole frame rewritten. Exiting puts it to sleep: nothing else lights the
 * ring on xylem.
 *
 * Included by main.c only; static state, one translation unit. */
#ifndef JW_LEDD_XYLEM_H
#define JW_LEDD_XYLEM_H

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "internal/platform/aw20036_i2c.h"
#include "internal/platform/power_supply_sysfs.h"

#define JW_LEDD_RING_CHECK_MS 5000LL

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

static int g_ledd_ring_fd = -1;
static bool g_ledd_ring_ready;          /* set up and awake, as far as we know */
static bool g_ledd_ring_reported;       /* the "unavailable" line is logged once */
static bool g_ledd_ring_have_frame;     /* dim/fade below are what the chip shows */
static uint8_t g_ledd_ring_dim[JW_AW20036_CHANNELS];
static uint8_t g_ledd_ring_fade[JW_AW20036_CHANNELS];
static long long g_ledd_ring_checked_ms = -1;
static long long g_ledd_ring_gap_ms = -1;   /* CLOCK_BOOTTIME - CLOCK_MONOTONIC */

static long long jw_ledd_ring_clock_ms(clockid_t clock) {
    struct timespec ts;
    clock_gettime(clock, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* True once after a suspend: BOOTTIME pulls ahead of MONOTONIC only then. */
static bool jw_ledd_ring_resumed(void) {
    long long gap = jw_ledd_ring_clock_ms(CLOCK_BOOTTIME) -
                    jw_ledd_ring_clock_ms(CLOCK_MONOTONIC);
    bool resumed = g_ledd_ring_gap_ms >= 0 && gap - g_ledd_ring_gap_ms > 1000;
    g_ledd_ring_gap_ms = gap;
    return resumed;
}

/* Open the chip if needed and set it up if it isn't (or `check` finds it
   asleep). Leaves the ring dark and manual after a set-up. */
static bool jw_ledd_ring_ensure(bool check) {
    if (g_ledd_ring_fd < 0) {
        g_ledd_ring_fd = jw_aw20036_open();
        if (g_ledd_ring_fd < 0) {
            if (!g_ledd_ring_reported) {
                fprintf(stderr, "jawaka-ledd: LED ring chip %s 0x%02x unavailable: %s%s\n",
                        JW_AW20036_I2C_DEV, JW_AW20036_ADDR, strerror(errno),
                        errno == EBUSY ? " (a kernel driver holds it)" : "");
                g_ledd_ring_reported = true;
            }
            return false;
        }
        g_ledd_ring_ready = false;
    }
    if (g_ledd_ring_ready && check &&
        jw_aw20036_get(g_ledd_ring_fd, JW_AW20036_PAGE_CONFIG, JW_AW20036_SLPCR) != 0x00) {
        g_ledd_ring_ready = false;
    }
    if (!g_ledd_ring_ready) {
        g_ledd_ring_ready = jw_aw20036_setup(g_ledd_ring_fd);
        g_ledd_ring_have_frame = false;
        if (!g_ledd_ring_ready) {
            fprintf(stderr, "jawaka-ledd: LED ring set-up failed: %s\n", strerror(errno));
        }
    }
    return g_ledd_ring_ready;
}

/* Whether this call is due a state check: the first, after a resume, or
   every JW_LEDD_RING_CHECK_MS. */
static bool jw_ledd_ring_check_due(void) {
    long long now = jw_ledd_ring_clock_ms(CLOCK_BOOTTIME);
    bool due = jw_ledd_ring_resumed() || g_ledd_ring_checked_ms < 0 ||
               now - g_ledd_ring_checked_ms >= JW_LEDD_RING_CHECK_MS;
    if (due) g_ledd_ring_checked_ms = now;
    return due;
}

static void jw_ledd_ring_open(void) {
    (void)jw_ledd_ring_resumed();   /* start the suspend clock */
    (void)jw_ledd_ring_ensure(false);
}

static void jw_ledd_ring_write(const uint32_t *argb, int leds) {
    if (!jw_ledd_ring_ensure(jw_ledd_ring_check_due())) {
        return;
    }
    uint8_t dim[JW_AW20036_CHANNELS];
    uint8_t fade[JW_AW20036_CHANNELS];
    jw_aw20036_frame(argb, leds, dim, fade);
    bool all = !g_ledd_ring_have_frame;
    bool ok = true;
    if (all || memcmp(dim, g_ledd_ring_dim, sizeof(dim)) != 0) {
        ok = jw_aw20036_write(g_ledd_ring_fd, JW_AW20036_PAGE_DIM, 0, dim, sizeof(dim));
        if (ok) memcpy(g_ledd_ring_dim, dim, sizeof(dim));
    }
    if (ok && (all || memcmp(fade, g_ledd_ring_fade, sizeof(fade)) != 0)) {
        ok = jw_aw20036_write(g_ledd_ring_fd, JW_AW20036_PAGE_FADE, 0, fade, sizeof(fade));
        if (ok) memcpy(g_ledd_ring_fade, fade, sizeof(fade));
    }
    g_ledd_ring_have_frame = ok;
    if (!ok) {
        g_ledd_ring_ready = false;   /* set up again on the next frame */
    }
}

/* Breath on the chip's engine: every LED the colour, between dark and
   `alpha` (0 to 255), rising and falling over the nearest time steps to
   `rise_ms` and `fall_ms`. */
static bool jw_ledd_ring_breathe(int r, int g, int b, int alpha, int rise_ms, int fall_ms) {
    if (!jw_ledd_ring_ensure(true)) {
        return false;
    }
    uint32_t colour = 0xff000000u | ((uint32_t)(r & 0xff) << 16) |
                      ((uint32_t)(g & 0xff) << 8) | (uint32_t)(b & 0xff);
    uint32_t argb[JW_AW20036_LEDS];
    for (int i = 0; i < JW_AW20036_LEDS; i++) argb[i] = colour;
    uint8_t dim[JW_AW20036_CHANNELS];
    uint8_t fade[JW_AW20036_CHANNELS];
    jw_aw20036_frame(argb, JW_AW20036_LEDS, dim, fade);
    for (int ch = 0; ch < JW_AW20036_CHANNELS; ch++) {
        dim[ch] = (uint8_t)(fade[ch] >> 2);   /* the colour as current, 0 to 63 */
    }
    g_ledd_ring_have_frame = false;
    bool ok = jw_aw20036_breathe(g_ledd_ring_fd, dim, (uint8_t)(alpha & 0xff),
                                 jw_aw20036_step(rise_ms), jw_aw20036_step(fall_ms));
    if (!ok) {
        fprintf(stderr, "jawaka-ledd: LED ring breathing failed: %s\n", strerror(errno));
        g_ledd_ring_ready = false;
    }
    return ok;
}

/* Whether the engine is still breathing; false after a resume (jawakad put
   the chip to sleep) or a reset, and then main.c starts it again. */
static bool jw_ledd_ring_breathing(void) {
    if (g_ledd_ring_fd < 0) return false;
    bool resumed = jw_ledd_ring_resumed();
    return !resumed && jw_aw20036_breathing(g_ledd_ring_fd);
}

/* Dark and asleep, once. Async-signal-safe enough for the fatal handler:
   ioctl and nothing else. */
static void jw_ledd_ring_off(void) {
    static volatile sig_atomic_t done;
    if (done || g_ledd_ring_fd < 0) return;
    done = 1;
    (void)jw_aw20036_stop_patterns(g_ledd_ring_fd);
    (void)jw_aw20036_sleep(g_ledd_ring_fd);
}

#endif /* JW_LEDD_XYLEM_H */
