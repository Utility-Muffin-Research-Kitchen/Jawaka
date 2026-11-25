/* The MLP1's LED ring chip, an Awinic AW20036, driven from userspace over I2C
 * on xylem (OS=xylem, -DJW_OS_XYLEM).
 *
 * xylem binds no kernel driver to it: mainline's leds-aw200xx writes one
 * register per transaction, two per channel plus a work item per LED, which
 * cost 5.9% of a core for a 30 fps rainbow against 0.17% as one burst per
 * frame, and it has no way to start the chip's own breathing engines. The
 * device tree holds the chip's enable line with a GPIO hog, so the chip
 * powers up at its reset state, asleep, on /dev/i2c-1 at 0x3a. The register
 * recipe was measured on the Green Genes MLP1
 * (umrk-workspace plans/xylem/utilities.md, "What jawaka-ledd writes").
 *
 * Every access is one adapter transfer that selects its page first, so two
 * writers (jawaka-ledd's frames, jawakad putting the chip to sleep before a
 * suspend) never land between each other's page select and data.
 *
 * Header-only: jawaka-ledd and the MLP1 backend both include it. The pure
 * parts (channel map, frame conversion, time steps) are tested on the host. */
#ifndef JW_PLATFORM_AW20036_I2C_H
#define JW_PLATFORM_AW20036_I2C_H

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef JW_AW20036_I2C_DEV      /* overridable for a test */
#define JW_AW20036_I2C_DEV      "/dev/i2c-1"
#endif
#define JW_AW20036_ADDR         0x3a
#define JW_AW20036_LEDS         8
#define JW_AW20036_CHANNELS     (JW_AW20036_LEDS * 3)
#define JW_AW20036_REG_PAGE     0xf0   /* written as 0xC0 | page, as stock's driver does */

/* Pages: 0 configuration, then one byte per channel, registers 0 to 23. */
#define JW_AW20036_PAGE_CONFIG  0
#define JW_AW20036_PAGE_DIM     1      /* current, 0 to 63 */
#define JW_AW20036_PAGE_FADE    2      /* PWM, 0 to 255 */
#define JW_AW20036_PAGE_PATTERN 3      /* 0 manual, n + 1 = pattern controller n */

/* Page 0 registers. */
#define JW_AW20036_SLPCR        0x01   /* 0x80 asleep, 0x00 awake */
#define JW_AW20036_RSTR         0x02   /* 0x01 resets every register */
#define JW_AW20036_GCCR         0x03   /* 0x10: global current 20 mA, ALLON off */
#define JW_AW20036_PATCR        0x43   /* pattern controllers enabled, bits 0 to 2 */
#define JW_AW20036_PAT0_MAX     0x44
#define JW_AW20036_PAT0_MIN     0x47
#define JW_AW20036_PAT0_T0      0x4a   /* rise << 4 | on, time steps */
#define JW_AW20036_PAT0_T1      0x4b   /* fall << 4 | off */
#define JW_AW20036_PAT0_T2      0x4c   /* 0x30: loop from step 3, endless */
#define JW_AW20036_PAT0_T3      0x4d   /* repeat count, 0 = endless */
#define JW_AW20036_PAT0_CFG     0x56   /* 0x03: auto breathing with ramp */
#define JW_AW20036_PATGO        0x59   /* bit n starts controller n; bits 4-6 read back running */
#define JW_AW20036_LEDON        0x31   /* 0x31 to 0x36: LED-on bits */
#define JW_AW20036_DSIZE        0x80   /* 0x01: two rows, channels 0 to 23 */

/* The channel of ring LED `led` (Jawaka's index: 0 at about four o'clock,
   then counter-clockwise) for colour 0 red, 1 green, 2 blue. Each LED is a
   group of three channels, blue first. */
static inline int jw_aw20036_channel(int led, int colour) {
    static const uint8_t group[JW_AW20036_LEDS] = { 4, 0, 1, 2, 3, 7, 6, 5 };
    return 3 * group[led] + (2 - colour);
}

/* A frame of 0xAARRGGBB words as stock's driver made of an mmrgball write:
   each channel's FADE is its colour byte and its DIM is the LED's alpha >> 2.
   LEDs past `leds` stay dark. */
static inline void jw_aw20036_frame(const uint32_t *argb, int leds,
                                    uint8_t dim[JW_AW20036_CHANNELS],
                                    uint8_t fade[JW_AW20036_CHANNELS]) {
    memset(dim, 0, JW_AW20036_CHANNELS);
    memset(fade, 0, JW_AW20036_CHANNELS);
    if (leds > JW_AW20036_LEDS) leds = JW_AW20036_LEDS;
    for (int led = 0; led < leds; led++) {
        uint8_t a = (uint8_t)(argb[led] >> 24);
        for (int c = 0; c < 3; c++) {
            int ch = jw_aw20036_channel(led, c);
            dim[ch] = (uint8_t)(a >> 2);
            fade[ch] = (uint8_t)(argb[led] >> (16 - 8 * c));
        }
    }
}

/* A pattern controller time step's length in ms (0 to 15). */
static inline int jw_aw20036_step_ms(int step) {
    static const int ms[16] = {
        0, 130, 260, 380, 510, 770, 1040, 1600,
        2100, 2600, 3100, 4200, 5200, 6200, 7300, 8300,
    };
    return ms[step & 0xf];
}

/* The step nearest `ms` (the shorter one on a tie). */
static inline int jw_aw20036_step(int ms) {
    int best = 0;
    for (int i = 1; i < 16; i++) {
        if (abs(jw_aw20036_step_ms(i) - ms) < abs(jw_aw20036_step_ms(best) - ms)) {
            best = i;
        }
    }
    return best;
}

/* The chip's bus with its address set, or -1. I2C_SLAVE (not _FORCE) fails
   with EBUSY when a kernel driver is bound to the chip, and then the ring is
   the driver's, not ours. */
static inline int jw_aw20036_open(void) {
    int fd = open(JW_AW20036_I2C_DEV, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    if (ioctl(fd, I2C_SLAVE, JW_AW20036_ADDR) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

/* `len` bytes from register `reg` on `page` (the register auto-increments),
   page select included, in one transfer. */
static inline bool jw_aw20036_write(int fd, int page, uint8_t reg,
                                    const uint8_t *data, size_t len) {
    uint8_t sel[2] = { JW_AW20036_REG_PAGE, (uint8_t)(0xc0 | page) };
    uint8_t buf[1 + 2 * JW_AW20036_CHANNELS];
    if (fd < 0 || len == 0 || len > sizeof(buf) - 1) return false;
    buf[0] = reg;
    memcpy(buf + 1, data, len);
    struct i2c_msg msgs[2] = {
        { .addr = JW_AW20036_ADDR, .flags = 0, .len = 2, .buf = sel },
        { .addr = JW_AW20036_ADDR, .flags = 0, .len = (uint16_t)(len + 1), .buf = buf },
    };
    struct i2c_rdwr_ioctl_data xfer = { .msgs = msgs, .nmsgs = 2 };
    return ioctl(fd, I2C_RDWR, &xfer) == 2;
}

static inline bool jw_aw20036_set(int fd, int page, uint8_t reg, uint8_t value) {
    return jw_aw20036_write(fd, page, reg, &value, 1);
}

/* One register's value, or -1. */
static inline int jw_aw20036_get(int fd, int page, uint8_t reg) {
    uint8_t sel[2] = { JW_AW20036_REG_PAGE, (uint8_t)(0xc0 | page) };
    uint8_t value = 0;
    struct i2c_msg msgs[3] = {
        { .addr = JW_AW20036_ADDR, .flags = 0, .len = 2, .buf = sel },
        { .addr = JW_AW20036_ADDR, .flags = 0, .len = 1, .buf = &reg },
        { .addr = JW_AW20036_ADDR, .flags = I2C_M_RD, .len = 1, .buf = &value },
    };
    struct i2c_rdwr_ioctl_data xfer = { .msgs = msgs, .nmsgs = 3 };
    return ioctl(fd, I2C_RDWR, &xfer) == 3 ? value : -1;
}

/* Reset, then 20 mA, every LED on, two rows, awake. Every channel is dark
   and manual afterwards (DIM, FADE and pattern select reset to 0). */
static inline bool jw_aw20036_setup(int fd) {
    static const uint8_t on[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    if (!jw_aw20036_set(fd, JW_AW20036_PAGE_CONFIG, JW_AW20036_RSTR, 0x01)) return false;
    usleep(2000);   /* at least 1 ms after a reset */
    return jw_aw20036_set(fd, JW_AW20036_PAGE_CONFIG, JW_AW20036_GCCR, 0x10) &&
           jw_aw20036_write(fd, JW_AW20036_PAGE_CONFIG, JW_AW20036_LEDON, on, sizeof(on)) &&
           jw_aw20036_set(fd, JW_AW20036_PAGE_CONFIG, JW_AW20036_DSIZE, 0x01) &&
           jw_aw20036_set(fd, JW_AW20036_PAGE_CONFIG, JW_AW20036_SLPCR, 0x00);
}

/* Asleep: the ring goes dark and the chip keeps its registers' power. */
static inline bool jw_aw20036_sleep(int fd) {
    return jw_aw20036_set(fd, JW_AW20036_PAGE_CONFIG, JW_AW20036_SLPCR, 0x80);
}

/* Every pattern controller stopped and every channel back to manual. */
static inline bool jw_aw20036_stop_patterns(int fd) {
    static const uint8_t manual[JW_AW20036_CHANNELS] = { 0 };
    return jw_aw20036_set(fd, JW_AW20036_PAGE_CONFIG, JW_AW20036_PATGO, 0x00) &&
           jw_aw20036_set(fd, JW_AW20036_PAGE_CONFIG, JW_AW20036_PATCR, 0x00) &&
           jw_aw20036_write(fd, JW_AW20036_PAGE_PATTERN, 0, manual, sizeof(manual));
}

/* Every channel breathing on pattern controller 0, endlessly, between level
   0 and `max_level` (0 to 255, standing in for FADE): each channel's DIM is
   its `dim` byte (0 to 63), rise and fall are time steps, with no hold at
   the top or the bottom. Needs no I2C and no CPU once started. */
static inline bool jw_aw20036_breathe(int fd, const uint8_t dim[JW_AW20036_CHANNELS],
                                      uint8_t max_level, int rise_step, int fall_step) {
    uint8_t full[JW_AW20036_CHANNELS];
    uint8_t select[JW_AW20036_CHANNELS];
    memset(full, 0xff, sizeof(full));
    memset(select, 1, sizeof(select));
    return jw_aw20036_stop_patterns(fd) &&
           jw_aw20036_write(fd, JW_AW20036_PAGE_DIM, 0, dim, JW_AW20036_CHANNELS) &&
           jw_aw20036_write(fd, JW_AW20036_PAGE_FADE, 0, full, sizeof(full)) &&
           jw_aw20036_write(fd, JW_AW20036_PAGE_PATTERN, 0, select, sizeof(select)) &&
           jw_aw20036_set(fd, JW_AW20036_PAGE_CONFIG, JW_AW20036_PAT0_MAX, max_level) &&
           jw_aw20036_set(fd, JW_AW20036_PAGE_CONFIG, JW_AW20036_PAT0_MIN, 0) &&
           jw_aw20036_set(fd, JW_AW20036_PAGE_CONFIG, JW_AW20036_PAT0_T0,
                          (uint8_t)((rise_step & 0xf) << 4)) &&
           jw_aw20036_set(fd, JW_AW20036_PAGE_CONFIG, JW_AW20036_PAT0_T1,
                          (uint8_t)((fall_step & 0xf) << 4)) &&
           jw_aw20036_set(fd, JW_AW20036_PAGE_CONFIG, JW_AW20036_PAT0_T2, 0x30) &&
           jw_aw20036_set(fd, JW_AW20036_PAGE_CONFIG, JW_AW20036_PAT0_T3, 0x00) &&
           jw_aw20036_set(fd, JW_AW20036_PAGE_CONFIG, JW_AW20036_PAT0_CFG, 0x03) &&
           jw_aw20036_set(fd, JW_AW20036_PAGE_CONFIG, JW_AW20036_PATCR, 0x07) &&
           jw_aw20036_set(fd, JW_AW20036_PAGE_CONFIG, JW_AW20036_PATGO, 0x01) &&
           jw_aw20036_set(fd, JW_AW20036_PAGE_CONFIG, JW_AW20036_PATGO, 0x00);
}

/* Awake with pattern controller 0 running. */
static inline bool jw_aw20036_breathing(int fd) {
    if (jw_aw20036_get(fd, JW_AW20036_PAGE_CONFIG, JW_AW20036_SLPCR) != 0x00) return false;
    int go = jw_aw20036_get(fd, JW_AW20036_PAGE_CONFIG, JW_AW20036_PATGO);
    return go >= 0 && (go & 0x10) != 0;
}

#endif /* JW_PLATFORM_AW20036_I2C_H */
