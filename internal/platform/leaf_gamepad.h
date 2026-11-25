/* The Leaf Gamepad: the virtual pad jawakad's input proxy presents on xylem
 * (OS=xylem, -DJW_OS_XYLEM). Stock keeps cloning "Loong Gamepad".
 *
 * xylem is not tied to one device, so neither is its pad. Every board shows
 * the same name, ids and layout, built from that board's own input devices
 * by the source table below. Catastrophe, SDL mappings (PortMaster) and
 * RetroArch autoconfig key on the identity here and need nothing per board.
 *
 * Layout. Buttons in code order, which is also SDL's and RetroArch's raw
 * button order:
 *    0 BTN_SOUTH   1 BTN_EAST    2 BTN_NORTH   3 BTN_WEST
 *    4 BTN_TL      5 BTN_TR      6 BTN_TL2     7 BTN_TR2
 *    8 BTN_SELECT  9 BTN_START  10 BTN_MODE   11 BTN_THUMBL
 *   12 BTN_THUMBR, only on boards with an R3.
 * All of 0 to 11 are always declared, so no index moves from board to board.
 * Face buttons keep the codes the board reports: positions, not labels (the
 * MLP1's A is BTN_EAST, as on stock).
 *
 * Axes: the left stick as ABS_X/ABS_Y over JW_LEAF_PAD_STICK_MIN..MAX after
 * calibration (raw axes 0 and 1); a right stick as ABS_RX/ABS_RY only on
 * boards with one (axes 2 and 3); the d-pad always as ABS_HAT0X/ABS_HAT0Y,
 * -1..1 (hat 0), however the board reports it. Sticks carry no fuzz or flat:
 * the proxy has already applied the deadzone, and a second fuzz filter here
 * would latch a resting stick off zero. EV_FF/FF_RUMBLE is added by the proxy.
 * No device-specific codes: the volume keys are jawakad's and never reach the
 * pad.
 *
 * Only built for PLATFORM=mlp1 OS=xylem (and the Linux host tests). */
#ifndef JW_PLATFORM_LEAF_GAMEPAD_H
#define JW_PLATFORM_LEAF_GAMEPAD_H

#include "internal/platform/calibration.h"

#include <linux/input.h>
#include <linux/uinput.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Identity. SDL builds its GUID from these, name included:
   060088f3464c00000100000000010000 (name CRC 0xf388). RetroArch autoconfig
   matches input_vendor_id 19526, input_product_id 1. */
#define JW_LEAF_PAD_NAME     "Leaf Gamepad"
#define JW_LEAF_PAD_BUS      0x0006   /* BUS_VIRTUAL */
#define JW_LEAF_PAD_VENDOR   0x4c46   /* "LF" */
#define JW_LEAF_PAD_PRODUCT  0x0001
#define JW_LEAF_PAD_VERSION  0x0100
#define JW_LEAF_PAD_SDL_GUID "060088f3464c00000100000000010000"

/* Stick range after calibration: symmetric, so centre is exactly 0. */
#define JW_LEAF_PAD_STICK_MIN (-32767)
#define JW_LEAF_PAD_STICK_MAX 32767

/* How a board's d-pad arrives. */
typedef enum {
    JW_LEAF_DPAD_BUTTONS,   /* BTN_DPAD_UP/DOWN/LEFT/RIGHT, turned into the hat */
    JW_LEAF_DPAD_HAT,       /* its own ABS_HAT0X/ABS_HAT0Y, passed through */
} jw_leaf_dpad_source;

/* One board: the evdev devices (by name) behind the pad, and how to read
   them. Add an entry for a new board; nothing else here is per board. */
typedef struct {
    const char *id;                /* for the log */
    const char *pad_name;          /* the gamepad that feeds the pad */
    const char *volume_keys_name;  /* the volume keys' own device; NULL when the pad has them */
    const char *power_key_name;
    jw_leaf_dpad_source dpad;
    bool thumbr;                   /* R3 is a real button */
    /* ABS_X/ABS_Y in the board's raw units: the default when there is no
       profile. deadzone is the driver's flat, which is also the floor for a
       profile's; out_min/out_max are the pad's stick range. */
    jw_stick_calibration left_stick;
    /* ABS_RX/ABS_RY the same way; loaded=false when there is no right stick,
       and then the axes are dropped. */
    jw_stick_calibration right_stick;
} jw_leaf_pad_board;

extern const jw_leaf_pad_board jw_leaf_pad_boards[];
extern const size_t jw_leaf_pad_board_count;

/* The board whose pad has this evdev name, or NULL. A non-empty override
   (JAWAKA_XYLEM_PAD_NAME) stands in for every entry's pad name, so a pad by
   that name is read with the first entry's layout. Two boards sharing a pad
   name would need a device-tree match here instead. */
const jw_leaf_pad_board *jw_leaf_pad_board_for_name(const char *evdev_name,
                                                    const char *override_name);

/* The pad as uinput is to create it: identity (ff_effects_max left 0) and
   every key and axis, axis values 0. */
#define JW_LEAF_PAD_MAX_KEYS 13
#define JW_LEAF_PAD_MAX_ABS  6
typedef struct {
    struct uinput_setup     setup;
    int                     key_count;
    uint16_t                keys[JW_LEAF_PAD_MAX_KEYS];
    int                     abs_count;
    struct uinput_abs_setup abs[JW_LEAF_PAD_MAX_ABS];
} jw_leaf_pad_descriptor;

void jw_leaf_pad_describe(const jw_leaf_pad_board *board, jw_leaf_pad_descriptor *out);

/* Whether the pad declares this key or axis on this board. */
bool jw_leaf_pad_has_key(const jw_leaf_pad_board *board, uint16_t code);
bool jw_leaf_pad_has_abs(const jw_leaf_pad_board *board, uint16_t code);

/* What the pad's hat reads, kept from the board's d-pad buttons. */
typedef struct {
    const jw_leaf_pad_board *board;
    bool   held[4];   /* up, down, left, right */
    int8_t last[2];   /* x, y: the direction pressed last, -1 or 1; 0 for none */
    int8_t hat[2];    /* x, y: the hat's value */
} jw_leaf_pad_state;

void jw_leaf_pad_state_init(jw_leaf_pad_state *st, const jw_leaf_pad_board *board);

/* One event from the board's pad, in the Leaf Gamepad's shape. Returns true
   with *out filled (the input's time kept) when there is something to
   handle; false when the event has no counterpart on the pad, or is a d-pad
   edge that leaves the hat where it was.
     - Buttons the pad declares pass through, and so do the volume keys,
       which jawakad consumes; any other key is dropped.
     - BTN_DPAD_* become ABS_HAT0X/ABS_HAT0Y. With both directions of an
       axis down, the later press wins, and releasing it hands the hat back
       to the one still held.
     - ABS_X/ABS_Y pass raw (the proxy calibrates them); ABS_RX/ABS_RY only
       with a right stick; a board hat only when the d-pad is one.
     - EV_SYN passes; other event types are dropped. */
bool jw_leaf_pad_translate(jw_leaf_pad_state *st, const struct input_event *in,
                           struct input_event *out);

/* Resync from a key snapshot of the board's pad (EVIOCGKEY, `len` bytes):
   the d-pad's state and the hat follow it, and codes the pad does not carry
   are cleared from `keys` (the volume keys stay). */
void jw_leaf_pad_resync(jw_leaf_pad_state *st, unsigned char *keys, size_t len);

/* The hat's value for ABS_HAT0X or ABS_HAT0Y; 0 for any other code. */
int jw_leaf_pad_hat(const jw_leaf_pad_state *st, uint16_t code);

#endif /* JW_PLATFORM_LEAF_GAMEPAD_H */
