/* jawaka-ledd — Miniloong Pocket 1 LED effects engine.
 *
 * The stock loong_light daemon owns the AW20036 ring and re-asserts its config
 * on a refresh loop, so to run custom per-LED animations we FREEZE it
 * (SIGSTOP), drive /sys/.../mmrgball directly at a fixed frame rate, and THAW
 * it (SIGCONT) on exit. On SIGCONT the daemon resumes and re-applies its cfg,
 * so the ring returns to the cooperative (Path A) state automatically.
 *
 * Crash-safety: the freeze is a hostage situation — if this process dies while
 * loong_light is stopped, the ring would be stuck. So we thaw from atexit() AND
 * from handlers for every fatal/term signal. Killing this process is therefore
 * the clean "stop the effect" operation: death itself thaws the daemon.
 *
 * Usage: jawaka-ledd <effect> <r> <g> <b> <brightness 0-10> <speed 0-10>
 *   effects: off static breath rainbow comet sweep fountain hiccup
 *
 * Standalone + MLP1-specific (like device_mlp1.c): pure libc + sysfs, no deps.
 */
#include <dirent.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define JW_LED_COUNT      8
#define JW_LED_MMRGBALL   "/sys/class/leds/aw20036_led/mmrgball"
#define JW_LED_DAEMON     "loong_light"

/* Sub-LED resolution: the moving dot's position is tracked in 1/256 of an LED
   so it can sit *between* physical LEDs, splitting its glow across the two it
   straddles. That sub-pixel positioning at a high frame rate is what makes the
   motion glide instead of hopping LED-to-LED. */
#define JW_LED_SUB   256
#define JW_LED_RING  (JW_LED_COUNT * JW_LED_SUB)
#define JW_LED_FPS_INTERVAL_MS 25   /* ~40fps for smooth motion */

/* Physical ring geometry (verified): index 0 ≈ 4 o'clock, winding CCW ~45°.
 * For the Fountain effect we fill from the bottom up each side to the top.
 * Bottom sits between LED7 (~5:30) and LED6 (~7:00). */
static const int kRightChain[4] = { 7, 0, 1, 2 };  /* bottom → top, right side */
static const int kLeftChain[4]  = { 6, 5, 4, 3 };  /* bottom → top, left side  */

static volatile sig_atomic_t g_running = 1;
static pid_t g_daemon_pid = -1;

static pid_t jw__find_daemon(const char *name) {
    DIR *proc = opendir("/proc");
    if (!proc) return -1;
    struct dirent *entry;
    char path[300];
    char comm[128];
    pid_t found = -1;
    while ((entry = readdir(proc)) != NULL) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        if (fgets(comm, sizeof(comm), fp)) {
            comm[strcspn(comm, "\n")] = '\0';
            if (strcmp(comm, name) == 0) { found = (pid_t)atoi(entry->d_name); }
        }
        fclose(fp);
        if (found > 0) break;
    }
    closedir(proc);
    return found;
}

static void jw__thaw(void) {
    if (g_daemon_pid > 0) kill(g_daemon_pid, SIGCONT);
}

static void jw__on_term(int sig) {
    (void)sig;
    g_running = 0;   /* loop exits, atexit thaws */
}

static void jw__on_fatal(int sig) {
    jw__thaw();
    signal(sig, SIG_DFL);
    raise(sig);      /* re-raise so the real fault is still reported */
}

static void jw__write_frame(const uint32_t argb[JW_LED_COUNT]) {
    FILE *fp = fopen(JW_LED_MMRGBALL, "w");
    if (!fp) return;
    for (int i = 0; i < JW_LED_COUNT; i++)
        fprintf(fp, "%s0x%08x", i ? " " : "", argb[i]);
    fclose(fp);
}

static int jw__clamp8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

static uint32_t jw__argb(int a, int r, int g, int b) {
    return ((uint32_t)jw__clamp8(a) << 24) | ((uint32_t)jw__clamp8(r) << 16) |
           ((uint32_t)jw__clamp8(g) << 8) | (uint32_t)jw__clamp8(b);
}

/* ─── Effects: fill argb[] for frame counter `t`. ──────────────────────── */

typedef struct {
    int r, g, b;        /* chosen color */
    int alpha_max;      /* 0..255 from brightness */
    int tail_sub;       /* comet/sweep tail length, in sub-LED units (0 = bare dot) */
} jw_effect_params;

static void jw__fx_fill(uint32_t out[JW_LED_COUNT], int a, int r, int g, int b) {
    uint32_t c = jw__argb(a, r, g, b);
    for (int i = 0; i < JW_LED_COUNT; i++) out[i] = c;
}

static uint32_t jw__wheel(int pos, int alpha) {
    pos &= 255;
    if (pos < 85) {
        return jw__argb(alpha, 255 - pos * 3, pos * 3, 0);
    }
    if (pos < 170) {
        pos -= 85;
        return jw__argb(alpha, 0, 255 - pos * 3, pos * 3);
    }
    pos -= 170;
    return jw__argb(alpha, pos * 3, 0, 255 - pos * 3);
}

static void jw__fx_breath(uint32_t out[JW_LED_COUNT], int t,
                          const jw_effect_params *p, int speed) {
    int period = 96 - speed * 7;
    if (period < 24) period = 24;
    int phase = t % period;
    int half = period / 2;
    int pulse;
    if (phase < half) {
        pulse = half > 0 ? phase * 255 / half : 255;
    } else {
        int down = period - phase;
        int span = period - half;
        pulse = span > 0 ? down * 255 / span : 0;
    }
    jw__fx_fill(out, p->alpha_max * pulse / 255, p->r, p->g, p->b);
}

static void jw__fx_rainbow(uint32_t out[JW_LED_COUNT], int t,
                           const jw_effect_params *p, int speed) {
    int advance = 1 + speed * 2;
    int offset = (t * advance) & 255;
    for (int i = 0; i < JW_LED_COUNT; i++) {
        out[i] = jw__wheel(offset + i * 256 / JW_LED_COUNT, p->alpha_max);
    }
}

/* Comet (bg white) / Sweep (bg off): a smoothly gliding color dot with an
   optional fading tail. `head` is a sub-LED position so the dot's glow splits
   across the two LEDs it sits between — that interpolation is the smoothness.
   Per LED we compute a 0..256 weight, then render it differently per background:
     - white bg (comet): blend color OVER white at FULL alpha, so edges are
       pale-color (still bright) instead of dim — no dark notches stepping along.
     - dark bg (sweep): fade the color's alpha, off where weight is 0. */
static void jw__fx_comet(uint32_t out[JW_LED_COUNT], long head,
                         const jw_effect_params *p, int bg_white) {
    const long head_spread = JW_LED_SUB;   /* symmetric glow radius: ±1 LED */
    for (int i = 0; i < JW_LED_COUNT; i++) {
        long center = (long)i * JW_LED_SUB;
        /* Symmetric (anti-aliased) glow at the head — glides between LEDs. */
        long dsym = (center - head + JW_LED_RING) % JW_LED_RING;
        if (dsym > JW_LED_RING - dsym) dsym = JW_LED_RING - dsym;
        long head_w = (dsym < head_spread) ? 256 * (head_spread - dsym) / head_spread : 0;
        /* One-sided fading tail behind the head (in travel direction). */
        long lag = (head - center + JW_LED_RING) % JW_LED_RING;
        long tail_w = (p->tail_sub > 0 && lag < p->tail_sub)
                    ? 256 * (p->tail_sub - lag) / p->tail_sub : 0;
        long w = head_w > tail_w ? head_w : tail_w;   /* 0..256 */

        if (bg_white) {
            int r = 255 + (int)((p->r - 255) * w / 256);
            int g = 255 + (int)((p->g - 255) * w / 256);
            int b = 255 + (int)((p->b - 255) * w / 256);
            out[i] = jw__argb(p->alpha_max, r, g, b);
        } else {
            out[i] = (w > 0) ? jw__argb((int)(p->alpha_max * w / 256), p->r, p->g, p->b)
                             : jw__argb(0, 0, 0, 0);
        }
    }
}

/* Fountain: fill from the bottom up both sides to the top, then drain back. */
static void jw__fx_fountain(uint32_t out[JW_LED_COUNT], int t,
                            const jw_effect_params *p, int speed) {
    for (int i = 0; i < JW_LED_COUNT; i++) out[i] = jw__argb(0, 0, 0, 0);
    int hold = 3 + (10 - speed) * 4;   /* frames held per fill level */
    if (hold < 1) hold = 1;
    static const int kLevels[8] = { 0, 1, 2, 3, 4, 3, 2, 1 };
    int level = kLevels[(t / hold) % 8];
    uint32_t lit = jw__argb(p->alpha_max, p->r, p->g, p->b);
    for (int n = 0; n < level; n++) {
        out[kRightChain[n]] = lit;
        out[kLeftChain[n]]  = lit;
    }
}

/* Hiccup: a sharp flash to full, then a smooth dim to off, brief pause, repeat. */
static void jw__fx_hiccup(uint32_t out[JW_LED_COUNT], int t,
                          const jw_effect_params *p, int speed) {
    int decay = 6 + (10 - speed);      /* frames to fade from full to off */
    int pause = 4 + (10 - speed) / 2;  /* dark frames before the next flash */
    int phase = t % (decay + pause);
    int a = (phase < decay) ? p->alpha_max * (decay - phase) / decay : 0;
    uint32_t c = jw__argb(a, p->r, p->g, p->b);
    for (int i = 0; i < JW_LED_COUNT; i++) out[i] = c;
}

int main(int argc, char **argv) {
    if (argc < 7) {
        fprintf(stderr, "usage: %s <effect> <r> <g> <b> <brightness 0-10> <speed 0-10>\n", argv[0]);
        return 2;
    }
    const char *effect = argv[1];
    jw_effect_params p;
    p.r = atoi(argv[2]);
    p.g = atoi(argv[3]);
    p.b = atoi(argv[4]);
    int brightness = atoi(argv[5]);
    int speed = atoi(argv[6]);
    if (brightness < 0) brightness = 0;
    if (brightness > 10) brightness = 10;
    if (speed < 0) speed = 0;
    if (speed > 10) speed = 10;
    p.alpha_max = brightness * 255 / 10;
    /* Sweep is a bare gliding dot (color-on-dark, darkest); comet keeps a
       fading tail on its white background. */
    p.tail_sub = (strcmp(effect, "sweep") == 0) ? 0 : 3 * JW_LED_SUB;

    /* Fixed high frame rate for smooth motion; speed drives how fast the dot
       advances per frame (sub-LED units), not the frame interval. */
    /* "off" and "static" are constant frames: the AW20036 latches its registers,
       so re-driving them at the animation frame rate just churns the shared i2c
       bus (codec, brightness) ~40x/second for a frame that never changes. Hold
       them at a slow refresh; animated effects keep the smooth rate. */
    int constant_frame = (strcmp(effect, "off") == 0 ||
                          strcmp(effect, "static") == 0);
    long interval_ms = constant_frame ? 1000 : JW_LED_FPS_INTERVAL_MS;
    long advance = 6 + (long)speed * 6;   /* sub-LED units per frame */

    g_daemon_pid = jw__find_daemon(JW_LED_DAEMON);
    if (g_daemon_pid > 0) kill(g_daemon_pid, SIGSTOP);

    atexit(jw__thaw);
    signal(SIGTERM, jw__on_term);
    signal(SIGINT,  jw__on_term);
    signal(SIGHUP,  jw__on_term);
    signal(SIGSEGV, jw__on_fatal);
    signal(SIGABRT, jw__on_fatal);
    signal(SIGFPE,  jw__on_fatal);
    signal(SIGBUS,  jw__on_fatal);

    struct timespec sleep_for = {
        .tv_sec  = interval_ms / 1000,
        .tv_nsec = (interval_ms % 1000) * 1000000L,
    };

    uint32_t frame[JW_LED_COUNT];
    int t = 0;
    long head = 0;
    while (g_running) {
        if (strcmp(effect, "off") == 0)           jw__fx_fill(frame, 0, 0, 0, 0);
        else if (strcmp(effect, "static") == 0)   jw__fx_fill(frame, p.alpha_max, p.r, p.g, p.b);
        else if (strcmp(effect, "breath") == 0)   jw__fx_breath(frame, t, &p, speed);
        else if (strcmp(effect, "rainbow") == 0)  jw__fx_rainbow(frame, t, &p, speed);
        else if (strcmp(effect, "comet") == 0)    jw__fx_comet(frame, head, &p, 1);
        else if (strcmp(effect, "sweep") == 0)    jw__fx_comet(frame, head, &p, 0);
        else if (strcmp(effect, "fountain") == 0) jw__fx_fountain(frame, t, &p, speed);
        else if (strcmp(effect, "hiccup") == 0)   jw__fx_hiccup(frame, t, &p, speed);
        else { fprintf(stderr, "unknown effect: %s\n", effect); return 2; }

        jw__write_frame(frame);
        nanosleep(&sleep_for, NULL);
        head = (head + advance) % JW_LED_RING;
        t++;
    }
    jw__thaw();
    return 0;
}
