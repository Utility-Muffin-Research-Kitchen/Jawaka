/* The Leaf Gamepad on xylem: the board table, the pad's descriptor, the
 * translation from the board's pad, and the xylem stick calibration.
 *
 * Linux hosts only (<linux/input.h>), built with -DJW_OS_XYLEM by
 * `make xylem-input-tests`. No device is opened: everything here is pure,
 * apart from the profile files, which live in a temp directory. */

#define _GNU_SOURCE

#include "internal/platform/leaf_gamepad.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* No <assert.h>: a release-profile build defines NDEBUG. */
static int g_failures;

static void expect(const char *what, bool ok) {
    if (!ok) {
        fprintf(stderr, "leaf-gamepad-test: FAIL %s\n", what);
        g_failures++;
    }
}

static const jw_leaf_pad_board *mlp1(void) { return &jw_leaf_pad_boards[0]; }

/* Translate one event; returns whether it produced one, *out set if so. */
static bool tr(jw_leaf_pad_state *st, uint16_t type, uint16_t code, int32_t value,
               struct input_event *out) {
    struct input_event in;
    memset(&in, 0, sizeof(in));
    in.type = type;
    in.code = code;
    in.value = value;
    in.input_event_sec = 12;
    in.input_event_usec = 345;
    return jw_leaf_pad_translate(st, &in, out);
}

static bool hat_is(jw_leaf_pad_state *st, uint16_t code, int32_t value,
                   uint16_t want_code, int32_t want_value) {
    struct input_event out;
    return tr(st, EV_KEY, code, value, &out) && out.type == EV_ABS &&
           out.code == want_code && out.value == want_value &&
           out.input_event_sec == 12 && out.input_event_usec == 345;
}

static bool silent(jw_leaf_pad_state *st, uint16_t type, uint16_t code, int32_t value) {
    struct input_event out;
    return !tr(st, type, code, value, &out);
}

static char g_dir[64];

static void write_file(const char *path, const char *text) {
    FILE *fp = fopen(path, "w");
    if (!fp) exit(2);
    fputs(text, fp);
    fclose(fp);
}

static void write_profile(const char *path, int x_min, int x_max, int y_min,
                          int y_max, int center_noise) {
    char json[512];
    snprintf(json, sizeof(json),
             "{ \"version\": 1, \"left\": {"
             " \"x_min\": %d, \"x_max\": %d, \"y_min\": %d, \"y_max\": %d,"
             " \"x_zero\": 0, \"y_zero\": 0, \"center_noise\": %d },"
             " \"derived\": { \"normalized_abs_min\": -32768,"
             " \"normalized_abs_max\": 32767 } }",
             x_min, x_max, y_min, y_max, center_noise);
    write_file(path, json);
}

static void describe_print(const jw_leaf_pad_descriptor *d) {
    printf("descriptor: \"%s\" bus 0x%04x vendor 0x%04x product 0x%04x version 0x%04x\n",
           d->setup.name, d->setup.id.bustype, d->setup.id.vendor,
           d->setup.id.product, d->setup.id.version);
    printf("  keys:");
    for (int i = 0; i < d->key_count; i++) printf(" %u", d->keys[i]);
    printf("\n");
    for (int i = 0; i < d->abs_count; i++) {
        printf("  abs %u: %d..%d fuzz %d flat %d\n", d->abs[i].code,
               d->abs[i].absinfo.minimum, d->abs[i].absinfo.maximum,
               d->abs[i].absinfo.fuzz, d->abs[i].absinfo.flat);
    }
}

int main(void) {
    struct input_event out;

    /* ── board table ── */
    expect("mlp1 is found by its pad's name",
           jw_leaf_pad_board_for_name("retrogame_joypad", NULL) == mlp1());
    expect("the Leaf Gamepad is never a board's pad",
           jw_leaf_pad_board_for_name(JW_LEAF_PAD_NAME, NULL) == NULL);
    expect("stock's pad is not a xylem board",
           jw_leaf_pad_board_for_name("Loong Gamepad", NULL) == NULL);
    expect("an override names the first board's pad",
           jw_leaf_pad_board_for_name("rocknix-joypad", "rocknix-joypad") == mlp1() &&
           jw_leaf_pad_board_for_name("retrogame_joypad", "rocknix-joypad") == NULL);
    expect("an empty override is no override",
           jw_leaf_pad_board_for_name("retrogame_joypad", "") == mlp1());
    expect("mlp1 devices", strcmp(mlp1()->volume_keys_name, "gpio-keys-vol") == 0 &&
           strcmp(mlp1()->power_key_name, "rk805 pwrkey") == 0 &&
           mlp1()->dpad == JW_LEAF_DPAD_BUTTONS && !mlp1()->thumbr &&
           !mlp1()->right_stick.loaded);

    /* ── descriptor ── */
    jw_leaf_pad_descriptor d;
    jw_leaf_pad_describe(mlp1(), &d);
    describe_print(&d);
    expect("identity", strcmp(d.setup.name, "Leaf Gamepad") == 0 &&
           d.setup.id.bustype == BUS_VIRTUAL && d.setup.id.vendor == 0x4c46 &&
           d.setup.id.product == 0x0001 && d.setup.id.version == 0x0100 &&
           d.setup.ff_effects_max == 0);
    {
        static const uint16_t want[] = {
            BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR,
            BTN_TL2, BTN_TR2, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL,
        };
        bool ok = d.key_count == 12;
        for (int i = 0; ok && i < 12; i++) ok = d.keys[i] == want[i];
        expect("mlp1: the twelve standard buttons in code order", ok);
    }
    expect("mlp1: sticks then hats",
           d.abs_count == 4 &&
           d.abs[0].code == ABS_X && d.abs[1].code == ABS_Y &&
           d.abs[2].code == ABS_HAT0X && d.abs[3].code == ABS_HAT0Y);
    expect("stick range -32767..32767, no fuzz or flat",
           d.abs[0].absinfo.minimum == -32767 && d.abs[0].absinfo.maximum == 32767 &&
           d.abs[1].absinfo.minimum == -32767 && d.abs[1].absinfo.maximum == 32767 &&
           d.abs[0].absinfo.fuzz == 0 && d.abs[0].absinfo.flat == 0 &&
           d.abs[1].absinfo.fuzz == 0 && d.abs[1].absinfo.flat == 0);
    expect("hat -1..1", d.abs[2].absinfo.minimum == -1 && d.abs[2].absinfo.maximum == 1 &&
           d.abs[3].absinfo.minimum == -1 && d.abs[3].absinfo.maximum == 1);
    expect("mlp1: no R3, right stick or board codes",
           !jw_leaf_pad_has_key(mlp1(), BTN_THUMBR) &&
           !jw_leaf_pad_has_key(mlp1(), BTN_DPAD_UP) &&
           !jw_leaf_pad_has_key(mlp1(), KEY_VOLUMEUP) &&
           !jw_leaf_pad_has_abs(mlp1(), ABS_RX) && !jw_leaf_pad_has_abs(mlp1(), ABS_RY));

    /* A board with R3 and a right stick: both appear, after everything
       else, so no standard index moves. */
    jw_leaf_pad_board full = *mlp1();
    full.thumbr = true;
    full.right_stick = full.left_stick;
    jw_leaf_pad_describe(&full, &d);
    expect("R3 is button 12", d.key_count == 13 && d.keys[11] == BTN_THUMBL &&
           d.keys[12] == BTN_THUMBR);
    expect("right stick is axes 2 and 3",
           d.abs_count == 6 && d.abs[2].code == ABS_RX && d.abs[3].code == ABS_RY &&
           d.abs[4].code == ABS_HAT0X && d.abs[5].code == ABS_HAT0Y);

    /* ── translation ── */
    jw_leaf_pad_state st;
    jw_leaf_pad_state_init(&st, mlp1());
    {
        static const uint16_t pass[] = {
            BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR,
            BTN_TL2, BTN_TR2, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL,
            KEY_VOLUMEDOWN, KEY_VOLUMEUP,
        };
        bool ok = true;
        for (size_t i = 0; i < sizeof(pass) / sizeof(pass[0]); i++) {
            for (int v = 0; v <= 2; v++) {
                ok = ok && tr(&st, EV_KEY, pass[i], v, &out) && out.type == EV_KEY &&
                     out.code == pass[i] && out.value == v &&
                     out.input_event_sec == 12 && out.input_event_usec == 345;
            }
        }
        expect("buttons and volume keys pass with their codes", ok);
    }
    expect("R3 is dropped on the MLP1", silent(&st, EV_KEY, BTN_THUMBR, 1) &&
           silent(&st, EV_KEY, BTN_THUMBR, 0));
    expect("other keys are dropped", silent(&st, EV_KEY, KEY_UP, 1) &&
           silent(&st, EV_KEY, BTN_TRIGGER_HAPPY1, 1));
    expect("the stick passes raw",
           tr(&st, EV_ABS, ABS_X, -1234, &out) && out.code == ABS_X && out.value == -1234 &&
           tr(&st, EV_ABS, ABS_Y, 1800, &out) && out.code == ABS_Y && out.value == 1800);
    expect("RX/RY are dropped", silent(&st, EV_ABS, ABS_RX, 900) &&
           silent(&st, EV_ABS, ABS_RY, -900));
    expect("a board hat is dropped when the d-pad is buttons",
           silent(&st, EV_ABS, ABS_HAT0X, 1));
    expect("SYN passes", tr(&st, EV_SYN, SYN_REPORT, 0, &out) && out.type == EV_SYN &&
           tr(&st, EV_SYN, SYN_DROPPED, 0, &out) && out.code == SYN_DROPPED);
    expect("other types are dropped", silent(&st, EV_MSC, MSC_SCAN, 7) &&
           silent(&st, EV_FF, 0, 1));

    /* D-pad: one direction at a time. */
    expect("up", hat_is(&st, BTN_DPAD_UP, 1, ABS_HAT0Y, -1));
    expect("up released", hat_is(&st, BTN_DPAD_UP, 0, ABS_HAT0Y, 0));
    expect("down", hat_is(&st, BTN_DPAD_DOWN, 1, ABS_HAT0Y, 1));
    expect("down released", hat_is(&st, BTN_DPAD_DOWN, 0, ABS_HAT0Y, 0));
    expect("left", hat_is(&st, BTN_DPAD_LEFT, 1, ABS_HAT0X, -1));
    expect("left released", hat_is(&st, BTN_DPAD_LEFT, 0, ABS_HAT0X, 0));
    expect("right", hat_is(&st, BTN_DPAD_RIGHT, 1, ABS_HAT0X, 1));
    expect("right released", hat_is(&st, BTN_DPAD_RIGHT, 0, ABS_HAT0X, 0));

    /* Opposite directions overlapping, released in both orders. */
    expect("up, then down wins", hat_is(&st, BTN_DPAD_UP, 1, ABS_HAT0Y, -1) &&
           hat_is(&st, BTN_DPAD_DOWN, 1, ABS_HAT0Y, 1));
    expect("releasing the loser keeps the winner", silent(&st, EV_KEY, BTN_DPAD_UP, 0));
    expect("releasing the winner centres", hat_is(&st, BTN_DPAD_DOWN, 0, ABS_HAT0Y, 0));
    expect("left, then right wins", hat_is(&st, BTN_DPAD_LEFT, 1, ABS_HAT0X, -1) &&
           hat_is(&st, BTN_DPAD_RIGHT, 1, ABS_HAT0X, 1));
    expect("releasing the winner hands back to the one still held",
           hat_is(&st, BTN_DPAD_RIGHT, 0, ABS_HAT0X, -1));
    expect("and then centres", hat_is(&st, BTN_DPAD_LEFT, 0, ABS_HAT0X, 0));

    /* A repeat is not a press: it must not take the hat back. */
    expect("down, then up wins", hat_is(&st, BTN_DPAD_DOWN, 1, ABS_HAT0Y, 1) &&
           hat_is(&st, BTN_DPAD_UP, 1, ABS_HAT0Y, -1));
    expect("a repeat of the loser changes nothing", silent(&st, EV_KEY, BTN_DPAD_DOWN, 2));
    expect("the winner still holds", hat_is(&st, BTN_DPAD_UP, 0, ABS_HAT0Y, 1) &&
           hat_is(&st, BTN_DPAD_DOWN, 0, ABS_HAT0Y, 0));
    expect("a stray release changes nothing", silent(&st, EV_KEY, BTN_DPAD_DOWN, 0));

    /* Diagonals: the two axes are independent. */
    expect("up-right", hat_is(&st, BTN_DPAD_UP, 1, ABS_HAT0Y, -1) &&
           hat_is(&st, BTN_DPAD_RIGHT, 1, ABS_HAT0X, 1) &&
           hat_is(&st, BTN_DPAD_UP, 0, ABS_HAT0Y, 0) &&
           hat_is(&st, BTN_DPAD_RIGHT, 0, ABS_HAT0X, 0));

    /* ── resync ── */
    {
        unsigned char keys[(KEY_MAX + 8) / 8];
        memset(keys, 0, sizeof(keys));
        int set[] = { BTN_DPAD_UP, BTN_DPAD_LEFT, BTN_THUMBR, BTN_EAST, KEY_VOLUMEUP, KEY_UP };
        for (size_t i = 0; i < sizeof(set) / sizeof(set[0]); i++)
            keys[set[i] / 8] |= (unsigned char)(1u << (set[i] % 8));
        jw_leaf_pad_state_init(&st, mlp1());
        jw_leaf_pad_resync(&st, keys, sizeof(keys));
#define KEY_SET(c) ((keys[(c) / 8] & (1u << ((c) % 8))) != 0)
        expect("resync reads the hat", jw_leaf_pad_hat(&st, ABS_HAT0X) == -1 &&
               jw_leaf_pad_hat(&st, ABS_HAT0Y) == -1 && jw_leaf_pad_hat(&st, ABS_X) == 0);
        expect("resync keeps the pad's keys and the volume keys",
               KEY_SET(BTN_EAST) && KEY_SET(KEY_VOLUMEUP));
        expect("resync drops the board's own codes",
               !KEY_SET(BTN_DPAD_UP) && !KEY_SET(BTN_DPAD_LEFT) &&
               !KEY_SET(BTN_THUMBR) && !KEY_SET(KEY_UP));
        expect("after a resync, releases centre the hat",
               hat_is(&st, BTN_DPAD_UP, 0, ABS_HAT0Y, 0) &&
               hat_is(&st, BTN_DPAD_LEFT, 0, ABS_HAT0X, 0));

        /* Both directions down: the last press seen still wins; with none
           seen the hat reads centred. */
        expect("down seen", hat_is(&st, BTN_DPAD_DOWN, 1, ABS_HAT0Y, 1));
        memset(keys, 0, sizeof(keys));
        keys[BTN_DPAD_UP / 8] |= (unsigned char)(1u << (BTN_DPAD_UP % 8));
        keys[BTN_DPAD_DOWN / 8] |= (unsigned char)(1u << (BTN_DPAD_DOWN % 8));
        jw_leaf_pad_resync(&st, keys, sizeof(keys));
        expect("both held, down last", jw_leaf_pad_hat(&st, ABS_HAT0Y) == 1);
        jw_leaf_pad_state_init(&st, mlp1());
        keys[BTN_DPAD_UP / 8] |= (unsigned char)(1u << (BTN_DPAD_UP % 8));
        keys[BTN_DPAD_DOWN / 8] |= (unsigned char)(1u << (BTN_DPAD_DOWN % 8));
        jw_leaf_pad_resync(&st, keys, sizeof(keys));
        expect("both held, order unknown", jw_leaf_pad_hat(&st, ABS_HAT0Y) == 0);
        expect("then a release resolves it", hat_is(&st, BTN_DPAD_UP, 0, ABS_HAT0Y, 1));
#undef KEY_SET
    }

    /* A board whose d-pad is already a hat passes it through. */
    {
        jw_leaf_pad_board hat_board = *mlp1();
        hat_board.dpad = JW_LEAF_DPAD_HAT;
        jw_leaf_pad_state_init(&st, &hat_board);
        expect("a board hat passes", tr(&st, EV_ABS, ABS_HAT0X, -1, &out) &&
               out.code == ABS_HAT0X && out.value == -1);
        expect("d-pad buttons are dropped on a hat board",
               silent(&st, EV_KEY, BTN_DPAD_UP, 1));
    }

    /* ── calibration ── */
    const jw_stick_calibration *board = &mlp1()->left_stick;
    jw_stick_calibration cal = *board;
    expect("default: full throw at the driver's range",
           jw_calibration_axis(&cal, true, 1800) == 32767 &&
           jw_calibration_axis(&cal, true, -1800) == -32767 &&
           jw_calibration_axis(&cal, false, 1800) == 32767 &&
           jw_calibration_axis(&cal, false, -1800) == -32767);
    {
        /* The input core's fuzz (54) can leave a released stick anywhere
           strictly inside +/-54. */
        bool rest = true;
        for (int v = -54; v <= 54; v++)
            rest = rest && jw_calibration_axis(&cal, true, v) == 0 &&
                   jw_calibration_axis(&cal, false, v) == 0;
        expect("default: the driver's flat reads 0", rest);
    }
    expect("default: the ramp starts just past the flat",
           jw_calibration_axis(&cal, true, 55) > 0 &&
           jw_calibration_axis(&cal, true, 55) < 100 &&
           jw_calibration_axis(&cal, true, -55) < 0);
    expect("default: half throw is about half",
           jw_calibration_axis(&cal, true, 927) > 16000 &&
           jw_calibration_axis(&cal, true, 927) < 16800);

    snprintf(g_dir, sizeof(g_dir), "/tmp/leaf-gamepad-test-XXXXXX");
    if (!mkdtemp(g_dir)) exit(2);
    char input_dir[128], stock_path[192], leaf_path[192];
    snprintf(input_dir, sizeof(input_dir), "%s/input", g_dir);
    snprintf(stock_path, sizeof(stock_path), "%s/loong-gamepad-calibration.json", input_dir);
    snprintf(leaf_path, sizeof(leaf_path), "%s/leaf-gamepad-calibration.json", input_dir);
    if (mkdir(input_dir, 0700) != 0) exit(2);
    unsetenv("JAWAKA_STICK_CAL_PROFILE");
    setenv("USERDATA_PATH", g_dir, 1);

    expect("no profile", !jw_calibration_load_board(&cal, board) && !cal.loaded);

    /* Stock's profile in the same userdata is never read on xylem. */
    write_profile(stock_path, -21180, 23256, -21765, 25056, 1800);
    expect("stock's profile file is not xylem's", !jw_calibration_load_board(&cal, board));

    /* Nor is a stock profile handed in by path: it is off this stick's scale. */
    setenv("JAWAKA_STICK_CAL_PROFILE", stock_path, 1);
    expect("a stock profile is ignored", !jw_calibration_load_board(&cal, board) &&
           !cal.loaded);
    unsetenv("JAWAKA_STICK_CAL_PROFILE");

    /* A xylem profile: the deadzone floor is the board's 54, not stock's
       1800, and the output range is the pad's whatever the profile says. */
    write_profile(leaf_path, -1700, 1750, -1720, 1790, 0);
    expect("a xylem profile loads", jw_calibration_load_board(&cal, board) && cal.loaded);
    expect("its deadzone floor is the driver's flat", cal.deadzone == 54);
    expect("its output range is the pad's", cal.out_min == -32767 && cal.out_max == 32767);
    expect("its extents are used", cal.x_min == -1700 && cal.x_max == 1750 &&
           jw_calibration_axis(&cal, true, 1750) == 32767 &&
           jw_calibration_axis(&cal, true, -1700) == -32767 &&
           jw_calibration_axis(&cal, true, 54) == 0);

    write_profile(leaf_path, -1700, 1750, -1720, 1790, 120);
    expect("a larger measured noise is kept", jw_calibration_load_board(&cal, board) &&
           cal.deadzone == 120);

    write_profile(leaf_path, -2400, 2600, -2000, 2100, 0);
    expect("more reach than the default still loads",
           jw_calibration_load_board(&cal, board));
    write_profile(leaf_path, -1700, 1750, -1720, 3700, 0);
    expect("beyond twice the board's extents is ignored",
           !jw_calibration_load_board(&cal, board));

    unlink(stock_path);
    unlink(leaf_path);
    rmdir(input_dir);
    rmdir(g_dir);

    if (g_failures) {
        fprintf(stderr, "leaf-gamepad-test: %d failure(s)\n", g_failures);
        return 1;
    }
    puts("leaf gamepad tests passed");
    return 0;
}
