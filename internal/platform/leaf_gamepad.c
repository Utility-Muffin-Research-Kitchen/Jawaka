#include "internal/platform/leaf_gamepad.h"

#include <stdio.h>
#include <string.h>

/* The source table. Measured on the units; see
   umrk-workspace/plans/xylem/utilities.md for the MLP1's devices. */
const jw_leaf_pad_board jw_leaf_pad_boards[] = {
    {
        /* Miniloong Pocket 1: ROCKNIX's rocknix-joypad, named by the device
           tree's joypad-name. The tree also lists BTN_THUMBR (GPIO4 A7), but
           the MLP1 has no R3, and its ABS_RX/ABS_RY read SARADC channels 0
           and 1 with no stick behind them. Axes are -1800..1800 with fuzz and
           flat 54, centred at 0, and every unit reaches the full range. */
        .id = "mlp1",
        .pad_name = "retrogame_joypad",
        .volume_keys_name = "gpio-keys-vol",
        .power_key_name = "rk805 pwrkey",
        .dpad = JW_LEAF_DPAD_BUTTONS,
        .thumbr = false,
        .left_stick = {
            .loaded = true,
            .x_min = -1800, .x_max = 1800,
            .y_min = -1800, .y_max = 1800,
            .x_zero = 0, .y_zero = 0,
            .deadzone = 54,
            .out_min = JW_LEAF_PAD_STICK_MIN, .out_max = JW_LEAF_PAD_STICK_MAX,
            .radial_clamp = false,
        },
        .right_stick = { .loaded = false },
    },
};
const size_t jw_leaf_pad_board_count =
    sizeof(jw_leaf_pad_boards) / sizeof(jw_leaf_pad_boards[0]);

/* Buttons 0 to 11, always declared, in code order. */
static const uint16_t jw__leaf_keys[] = {
    BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST,
    BTN_TL, BTN_TR, BTN_TL2, BTN_TR2,
    BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL,
};
#define JW__LEAF_KEY_COUNT ((int)(sizeof(jw__leaf_keys) / sizeof(jw__leaf_keys[0])))

const jw_leaf_pad_board *jw_leaf_pad_board_for_name(const char *evdev_name,
                                                    const char *override_name) {
    if (!evdev_name || !evdev_name[0]) {
        return NULL;
    }
    if (override_name && override_name[0]) {
        return jw_leaf_pad_board_count > 0 && strcmp(evdev_name, override_name) == 0
            ? &jw_leaf_pad_boards[0] : NULL;
    }
    for (size_t i = 0; i < jw_leaf_pad_board_count; i++) {
        if (strcmp(evdev_name, jw_leaf_pad_boards[i].pad_name) == 0) {
            return &jw_leaf_pad_boards[i];
        }
    }
    return NULL;
}

bool jw_leaf_pad_has_key(const jw_leaf_pad_board *board, uint16_t code) {
    for (int i = 0; i < JW__LEAF_KEY_COUNT; i++) {
        if (jw__leaf_keys[i] == code) {
            return true;
        }
    }
    return code == BTN_THUMBR && board && board->thumbr;
}

bool jw_leaf_pad_has_abs(const jw_leaf_pad_board *board, uint16_t code) {
    switch (code) {
    case ABS_X: case ABS_Y: case ABS_HAT0X: case ABS_HAT0Y:
        return true;
    case ABS_RX: case ABS_RY:
        return board && board->right_stick.loaded;
    default:
        return false;
    }
}

static void jw__leaf_add_abs(jw_leaf_pad_descriptor *out, uint16_t code,
                             int32_t min, int32_t max) {
    struct uinput_abs_setup *abs = &out->abs[out->abs_count++];
    memset(abs, 0, sizeof(*abs));
    abs->code = code;
    abs->absinfo.minimum = min;
    abs->absinfo.maximum = max;
}

void jw_leaf_pad_describe(const jw_leaf_pad_board *board, jw_leaf_pad_descriptor *out) {
    memset(out, 0, sizeof(*out));
    snprintf(out->setup.name, sizeof(out->setup.name), "%s", JW_LEAF_PAD_NAME);
    out->setup.id.bustype = JW_LEAF_PAD_BUS;
    out->setup.id.vendor  = JW_LEAF_PAD_VENDOR;
    out->setup.id.product = JW_LEAF_PAD_PRODUCT;
    out->setup.id.version = JW_LEAF_PAD_VERSION;

    for (int i = 0; i < JW__LEAF_KEY_COUNT; i++) {
        out->keys[out->key_count++] = jw__leaf_keys[i];
    }
    if (board && board->thumbr) {
        out->keys[out->key_count++] = BTN_THUMBR;
    }

    jw__leaf_add_abs(out, ABS_X, JW_LEAF_PAD_STICK_MIN, JW_LEAF_PAD_STICK_MAX);
    jw__leaf_add_abs(out, ABS_Y, JW_LEAF_PAD_STICK_MIN, JW_LEAF_PAD_STICK_MAX);
    if (board && board->right_stick.loaded) {
        jw__leaf_add_abs(out, ABS_RX, JW_LEAF_PAD_STICK_MIN, JW_LEAF_PAD_STICK_MAX);
        jw__leaf_add_abs(out, ABS_RY, JW_LEAF_PAD_STICK_MIN, JW_LEAF_PAD_STICK_MAX);
    }
    jw__leaf_add_abs(out, ABS_HAT0X, -1, 1);
    jw__leaf_add_abs(out, ABS_HAT0Y, -1, 1);
}

void jw_leaf_pad_state_init(jw_leaf_pad_state *st, const jw_leaf_pad_board *board) {
    memset(st, 0, sizeof(*st));
    st->board = board;
}

/* Index into held[]: up, down, left, right; -1 for any other code. */
static int jw__leaf_dpad_index(uint16_t code) {
    switch (code) {
    case BTN_DPAD_UP:    return 0;
    case BTN_DPAD_DOWN:  return 1;
    case BTN_DPAD_LEFT:  return 2;
    case BTN_DPAD_RIGHT: return 3;
    default:             return -1;
    }
}

/* One hat axis from its two buttons: up/left is -1, down/right is 1. */
static int8_t jw__leaf_axis_value(bool neg, bool pos, int8_t last) {
    if (neg && pos) return last;
    if (neg) return -1;
    if (pos) return 1;
    return 0;
}

static bool jw__leaf_buttons_dpad(const jw_leaf_pad_state *st) {
    return st->board && st->board->dpad == JW_LEAF_DPAD_BUTTONS;
}

bool jw_leaf_pad_translate(jw_leaf_pad_state *st, const struct input_event *in,
                           struct input_event *out) {
    *out = *in;
    switch (in->type) {
    case EV_SYN:
        return true;

    case EV_KEY: {
        int dir = jw__leaf_dpad_index(in->code);
        if (dir >= 0) {
            if (!jw__leaf_buttons_dpad(st)) {
                return false;
            }
            int axis = dir / 2;          /* 0 = y (up/down), 1 = x (left/right) */
            int hat = axis == 0 ? 1 : 0; /* hat[] is x, y */
            bool down = in->value != 0;
            if (down && !st->held[dir]) {   /* a press, not a repeat */
                st->last[hat] = (dir % 2) ? 1 : -1;
            }
            st->held[dir] = down;
            int8_t value = jw__leaf_axis_value(st->held[axis * 2], st->held[axis * 2 + 1],
                                               st->last[hat]);
            if (value == st->hat[hat]) {
                return false;
            }
            st->hat[hat] = value;
            out->type = EV_ABS;
            out->code = hat == 0 ? ABS_HAT0X : ABS_HAT0Y;
            out->value = value;
            return true;
        }
        return jw_leaf_pad_has_key(st->board, in->code) ||
               in->code == KEY_VOLUMEUP || in->code == KEY_VOLUMEDOWN;
    }

    case EV_ABS:
        if (in->code == ABS_HAT0X || in->code == ABS_HAT0Y) {
            return st->board && st->board->dpad == JW_LEAF_DPAD_HAT;
        }
        return jw_leaf_pad_has_abs(st->board, in->code);

    default:
        return false;
    }
}

void jw_leaf_pad_resync(jw_leaf_pad_state *st, unsigned char *keys, size_t len) {
    if (jw__leaf_buttons_dpad(st)) {
        static const uint16_t codes[4] = {
            BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT,
        };
        for (int i = 0; i < 4; i++) {
            size_t byte = codes[i] / 8u;
            st->held[i] = byte < len && (keys[byte] & (1u << (codes[i] % 8u))) != 0;
        }
        /* With both directions of an axis down, the last press seen still
           wins; when none was seen (both went down unread), the hat reads
           centred until the next press. */
        for (int hat = 0; hat < 2; hat++) {
            int axis = hat == 0 ? 1 : 0;
            bool neg = st->held[axis * 2], pos = st->held[axis * 2 + 1];
            if (!neg && !pos) {
                st->last[hat] = 0;
            }
            st->hat[hat] = jw__leaf_axis_value(neg, pos, st->last[hat]);
        }
    }
    for (size_t byte = 0; byte < len; byte++) {
        if (!keys[byte]) continue;
        for (unsigned bit = 0; bit < 8; bit++) {
            unsigned code = (unsigned)byte * 8u + bit;
            if ((keys[byte] & (1u << bit)) &&
                !jw_leaf_pad_has_key(st->board, (uint16_t)code) &&
                code != KEY_VOLUMEUP && code != KEY_VOLUMEDOWN) {
                keys[byte] &= (unsigned char)~(1u << bit);
            }
        }
    }
}

int jw_leaf_pad_hat(const jw_leaf_pad_state *st, uint16_t code) {
    if (code == ABS_HAT0X) return st->hat[0];
    if (code == ABS_HAT0Y) return st->hat[1];
    return 0;
}
