/* Chord state-machine test for the MLP1 input proxy.
 *
 * Runs on the device (see `make mlp1-adb-chord-test`), because the proxy needs
 * <linux/input.h> and uinput and cannot be built on the Mac host.
 *
 * It includes the implementation directly to reach the static state machine.
 * The alternative -- widening input_proxy.h until the internals are public --
 * would make the API worse to make it testable, and the chord logic is exactly
 * the part that has to be right.
 *
 * No real device is touched: the physical fd stays -1 and the "virtual pad" is
 * a temp file, so every emitted event is readable back and asserted. That is
 * what lets this drive cases the hardware cannot produce -- notably autorepeat.
 * This pad emits no repeats (measured 2026-08-30: no EV_REP, and zero value==2
 * over six buttons held to 8s), but a repeat must not strand a button if a
 * future unit or an external pad sends one, and Jawaka@0b0edd6 reports exactly
 * that failure from a device that apparently did. */

#define _GNU_SOURCE

#include "internal/platform/input_proxy_mlp1.c"

#include <stdio.h>

/* Deliberately no <assert.h>. The device build is a release profile with
   -DNDEBUG, so assert() compiles to nothing and an assert-based test would
   pass without executing a single check. Everything here reports through
   expect() instead, and no call with a side effect is ever wrapped. */

/* ── harness ─────────────────────────────────────────────────────────── */

static jw_input_proxy g_proxy;
static jw_mlp1_input_proxy_data g_data;
static char g_sink_path[64];

/* Which buttons the dispatcher claims, and what it was asked about. */
static bool g_claim[JW_INPUT_SHORTCUT_BUTTON_COUNT];
static jw_input_shortcut_button g_last_button;
static int g_dispatch_calls;

static bool test_dispatch(void *userdata, jw_input_shortcut_button button) {
    (void)userdata;
    g_last_button = button;
    g_dispatch_calls++;
    return button < JW_INPUT_SHORTCUT_BUTTON_COUNT && g_claim[button];
}

static bool g_have_sink;

static void reset_proxy(bool watch_only) {
    /* Guarded by a flag, not by uinput_fd >= 0: on the first call g_data is
       still zeroed, and fd 0 would close stdin. */
    if (g_have_sink && g_data.uinput_fd >= 0) close(g_data.uinput_fd);
    g_have_sink = false;
    memset(&g_proxy, 0, sizeof(g_proxy));
    memset(&g_data, 0, sizeof(g_data));
    memset(g_claim, 0, sizeof(g_claim));
    g_dispatch_calls = 0;
    g_last_button = JW_INPUT_SHORTCUT_BUTTON_NONE;

    snprintf(g_sink_path, sizeof(g_sink_path), "/tmp/jw-chord-XXXXXX");
    int fd = mkstemp(g_sink_path);
    if (fd < 0) {
        fprintf(stderr, "input-proxy-chord-test: mkstemp failed\n");
        exit(2);
    }
    unlink(g_sink_path);   /* the fd is all we need */
    /* Watch-only is "no virtual pad", which is exactly uinput_fd < 0. */
    g_data.uinput_fd = watch_only ? -1 : fd;
    if (watch_only) close(fd); else g_have_sink = true;
    g_data.input_fd = -1;
    g_data.power_fd = -1;
    g_proxy.backend_data = &g_data;
    g_proxy.enabled = true;
    g_proxy.shortcut = test_dispatch;
}

/* Feed one evdev event through the state machine. */
static void feed(uint16_t code, int32_t value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_KEY;
    ev.code = code;
    ev.value = value;
    jw__handle_key(&g_proxy, &ev);
}

/* Everything written to the virtual pad since the last drain, EV_KEY only.
   Returns the count; SYN reports are skipped as framing. */
typedef struct { uint16_t code; int32_t value; } emitted;

static int drain(emitted *out, int max) {
    if (g_data.uinput_fd < 0) return 0;
    int fd = g_data.uinput_fd;
    if (lseek(fd, 0, SEEK_SET) != 0) {
        fprintf(stderr, "input-proxy-chord-test: rewind failed\n");
        exit(2);
    }
    int n = 0;
    struct input_event ev;
    while (n < max && read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type != EV_KEY) continue;   /* SYN is framing */
        out[n].code = ev.code;
        out[n].value = ev.value;
        n++;
    }
    /* Start the next window clean. */
    if (ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) != 0) {
        fprintf(stderr, "input-proxy-chord-test: reset failed\n");
        exit(2);
    }
    return n;
}

static int g_failures;

static void expect(const char *what, bool ok) {
    if (!ok) {
        fprintf(stderr, "input-proxy-chord-test: FAIL %s\n", what);
        g_failures++;
    }
}

static void expect_seq(const char *what, const emitted *got, int n,
                       const emitted *want, int want_n) {
    bool ok = (n == want_n);
    for (int i = 0; ok && i < n; i++) {
        ok = got[i].code == want[i].code && got[i].value == want[i].value;
    }
    if (!ok) {
        fprintf(stderr, "input-proxy-chord-test: FAIL %s\n  got: ", what);
        for (int i = 0; i < n; i++)
            fprintf(stderr, "%u=%d ", got[i].code, got[i].value);
        fprintf(stderr, "\n  want: ");
        for (int i = 0; i < want_n; i++)
            fprintf(stderr, "%u=%d ", want[i].code, want[i].value);
        fprintf(stderr, "\n");
        g_failures++;
    }
}

/* ── cases ───────────────────────────────────────────────────────────── */

int main(void) {
    emitted got[16];
    int n;

    /* A claimed chord reaches the game as nothing at all. */
    reset_proxy(false);
    g_claim[JW_INPUT_SHORTCUT_BUTTON_SELECT] = true;
    feed(BTN_MODE, 1);
    feed(BTN_SELECT, 1);
    feed(BTN_SELECT, 0);
    feed(BTN_MODE, 0);
    n = drain(got, 16);
    expect("claimed chord emits nothing", n == 0);
    expect("dispatch saw Select", g_last_button == JW_INPUT_SHORTCUT_BUTTON_SELECT);
    expect("dispatch called once", g_dispatch_calls == 1);

    /* Autorepeat inside a claimed chord. The hardware does not produce this;
       the code must survive it anyway. Every repeat is swallowed, the action
       fires once, and the release does not strand the button down. */
    reset_proxy(false);
    g_claim[JW_INPUT_SHORTCUT_BUTTON_R1] = true;
    feed(BTN_MODE, 1);
    feed(BTN_TR, 1);
    feed(BTN_TR, 2);
    feed(BTN_TR, 2);
    feed(BTN_TR, 2);
    feed(BTN_TR, 0);
    feed(BTN_MODE, 0);
    n = drain(got, 16);
    expect("repeats in a claimed chord emit nothing", n == 0);
    expect("repeats do not re-dispatch", g_dispatch_calls == 1);
    expect("no key left held", !jw__bits_any(g_data.held_keys,
                                             sizeof(g_data.held_keys)));

    /* Same, with Select. The pre-refactor Select block dispatched on value>0
       and would have fired the switcher again on every repeat. */
    reset_proxy(false);
    g_claim[JW_INPUT_SHORTCUT_BUTTON_SELECT] = true;
    feed(BTN_MODE, 1);
    feed(BTN_SELECT, 1);
    feed(BTN_SELECT, 2);
    feed(BTN_SELECT, 2);
    feed(BTN_SELECT, 0);
    feed(BTN_MODE, 0);
    n = drain(got, 16);
    expect("Select repeats emit nothing", n == 0);
    expect("Select dispatches exactly once", g_dispatch_calls == 1);

    /* A declined chord forwards Menu first, then the button: RetroArch needs
       its modifier down before the action to read it as a hotkey. */
    reset_proxy(false);
    feed(BTN_MODE, 1);
    feed(BTN_NORTH, 1);
    n = drain(got, 16);
    {
        const emitted want[] = {{BTN_MODE, 1}, {BTN_NORTH, 1}};
        expect_seq("declined chord forwards Menu then button", got, n, want, 2);
    }
    expect("dispatch was asked", g_dispatch_calls == 1);
    expect("dispatch saw X", g_last_button == JW_INPUT_SHORTCUT_BUTTON_X);

    /* ...and releasing Menu first holds the modifier up until the action is
       released, so the core never sees a live press after the modifier goes.
       This is the Slice A regression, re-asserted here now that the chord
       path has been rewritten around it. */
    feed(BTN_MODE, 0);
    n = drain(got, 16);
    expect("Menu-up deferred while action held", n == 0);
    feed(BTN_NORTH, 0);
    n = drain(got, 16);
    {
        const emitted want[] = {{BTN_NORTH, 0}, {BTN_MODE, 0}};
        expect_seq("action-up precedes deferred Menu-up", got, n, want, 2);
    }

    /* Ordinary release order needs no deferral. */
    reset_proxy(false);
    feed(BTN_MODE, 1);
    feed(BTN_NORTH, 1);
    (void)drain(got, 16);
    feed(BTN_NORTH, 0);
    feed(BTN_MODE, 0);
    n = drain(got, 16);
    {
        const emitted want[] = {{BTN_NORTH, 0}, {BTN_MODE, 0}};
        expect_seq("ordinary order forwards both releases", got, n, want, 2);
    }

    /* A repeat on a DECLINED chord must forward, not be swallowed: the game
       is holding that button and the proxy is only a wire here. */
    reset_proxy(false);
    feed(BTN_MODE, 1);
    feed(BTN_NORTH, 1);
    (void)drain(got, 16);
    feed(BTN_NORTH, 2);
    n = drain(got, 16);
    {
        const emitted want[] = {{BTN_NORTH, 2}};
        expect_seq("declined chord forwards repeats", got, n, want, 1);
    }
    /* And must not ask again. This is the only place the dispatch guard's
       value==1 is observable: once a chord is claimed the consumed bitset
       returns early, so a value>0 guard looks identical there. Re-asking
       mid-hold would let a handler that has since changed its mind consume a
       button the game is already holding down. */
    expect("declined chord does not re-dispatch on a repeat",
           g_dispatch_calls == 1);

    /* An unbindable button (d-pad arrives as EV_KEY on some pads) is never
       offered to the dispatcher, and forwards as an ordinary chord. */
    reset_proxy(false);
    feed(BTN_MODE, 1);
    feed(KEY_UP, 1);
    n = drain(got, 16);
    expect("unbindable code is not dispatched", g_dispatch_calls == 0);
    {
        const emitted want[] = {{BTN_MODE, 1}, {KEY_UP, 1}};
        expect_seq("unbindable code forwards as a chord", got, n, want, 2);
    }

    /* Two claimed buttons held at once: the bitset must track both, and
       neither release may leak. */
    reset_proxy(false);
    g_claim[JW_INPUT_SHORTCUT_BUTTON_L1] = true;
    g_claim[JW_INPUT_SHORTCUT_BUTTON_R1] = true;
    feed(BTN_MODE, 1);
    feed(BTN_TL, 1);
    feed(BTN_TR, 1);
    feed(BTN_TL, 0);
    feed(BTN_TR, 0);
    feed(BTN_MODE, 0);
    n = drain(got, 16);
    expect("two claimed buttons emit nothing", n == 0);
    expect("both dispatched", g_dispatch_calls == 2);

    /* Screen-off must not strand a deferred Menu. */
    reset_proxy(false);
    feed(BTN_MODE, 1);
    feed(BTN_NORTH, 1);
    (void)drain(got, 16);
    feed(BTN_MODE, 0);          /* deferred: action still held */
    (void)drain(got, 16);
    jw_input_proxy_set_swallow(&g_proxy, true);
    n = drain(got, 16);
    {
        const emitted want[] = {{BTN_MODE, 0}};
        expect_seq("screen-off releases the pending Menu", got, n, want, 1);
    }
    expect("no pending Menu after screen-off", !g_data.menu_up_pending);

    /* A bare press with no Menu held is never a chord. */
    reset_proxy(false);
    g_claim[JW_INPUT_SHORTCUT_BUTTON_SELECT] = true;
    feed(BTN_SELECT, 1);
    feed(BTN_SELECT, 0);
    n = drain(got, 16);
    expect("bare press is not dispatched", g_dispatch_calls == 0);
    {
        const emitted want[] = {{BTN_SELECT, 1}, {BTN_SELECT, 0}};
        expect_seq("bare press forwards unchanged", got, n, want, 2);
    }

    /* Watch-only never dispatches: nothing is grabbed, so a claimed chord
       would reach the emulator anyway. */
    reset_proxy(true);
    g_claim[JW_INPUT_SHORTCUT_BUTTON_SELECT] = true;
    feed(BTN_MODE, 1);
    feed(BTN_SELECT, 1);
    feed(BTN_SELECT, 0);
    feed(BTN_MODE, 0);
    expect("watch-only does not dispatch", g_dispatch_calls == 0);

    if (g_failures) {
        fprintf(stderr, "input-proxy-chord-test: %d failure(s)\n", g_failures);
        return 1;
    }
    puts("input proxy chord tests passed");
    return 0;
}
