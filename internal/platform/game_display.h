#ifndef JW_PLATFORM_GAME_DISPLAY_H
#define JW_PLATFORM_GAME_DISPLAY_H

/* Handing the display to a standalone game and taking it back (MLP1).

   Some standalone emulators drive the display themselves (SDL kmsdrm,
   VK_KHR_display), either after jawakad stops Weston (the direct-DRM handoff) or
   beside a running Weston (mupen64plus). They draw on the first connected
   connector, and HDMI-A-1 is listed before DSI-1, so with a cable in they take
   the TV even when HDMI Output is Off, and leave their own mode on the shared
   CRTC when they exit. Measured 2026-10-15, see umrk-workspace
   plans/hdmi-standalone-evidence/2026-10-15-phase0.md.

   The decisions and the S49weston command live here, free of I/O, so the host
   test can cover them; device_mlp1.c does the sysfs and DRM work. */

#include "internal/platform/weston_initd.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* Output values, matching the hdmi_output_mode setting. */
enum {
    JW_GAME_DISPLAY_PANEL = 0,
    JW_GAME_DISPLAY_TV_4_3 = 1,
    JW_GAME_DISPLAY_TV_STRETCH = 2,
};

/* The vendor Weston's live HDMI configuration file (WESTON_DRM_CONFIG). */
#define JW_GAME_DISPLAY_WESTON_CONF "/tmp/.weston_drm.conf"

/* Hide a connected external display that is not the live output, so a game
   that picks the first connected connector stays on the panel. */
static inline bool jw_game_display_hide_external(bool connected, bool live) {
    return connected && !live;
}

/* The output to put back after a game: the TV in the saved HDMI Output mode
   while a cable is in, otherwise the panel. Mirrors the HDMI hotplug poll. */
static inline int jw_game_display_restore_mode(bool connected, int setting) {
    if (!connected ||
        (setting != JW_GAME_DISPLAY_TV_4_3 && setting != JW_GAME_DISPLAY_TV_STRETCH)) {
        return JW_GAME_DISPLAY_PANEL;
    }
    return setting;
}

/* Whether Weston has to be (re)started after a game: it was stopped for the
   handoff, the output changes, or a game that drew beside it left a different
   scanout size (or none) on the CRTC. Sizes <= 0 are unknown; with no size
   from before the game there is nothing to compare. */
static inline bool jw_game_display_needs_restart(bool compositor_stopped,
                                                 int before_mode, int want_mode,
                                                 int before_w, int before_h,
                                                 int now_w, int now_h) {
    if (compositor_stopped || before_mode != want_mode) {
        return true;
    }
    if (before_w <= 0 || before_h <= 0) {
        return false;
    }
    return now_w != before_w || now_h != before_h;
}

/* The screen a standalone game draws on, for every standalone child:

     LEAF_DISPLAY_OUTPUT     panel | hdmi
     LEAF_DISPLAY_CONNECTOR  the DRM connector, e.g. DSI-1 or HDMI-A-1
     LEAF_DISPLAY_MODE       the scanout the launcher was using, WxH@Hz
     LEAF_DISPLAY_ASPECT     4:3 | stretch (HDMI Output's choice on a TV)

   The MLP1 panel is a portrait part mounted sideways, so emulators that drive
   the display themselves rotate for it; a TV needs no rotation. An unset
   LEAF_DISPLAY_OUTPUT means the panel, which is how Leaf before these variables
   behaved, so a launch script reading them works on either. A TV whose mode
   Leaf did not set up (mode -1) still counts as hdmi. */
typedef struct {
    char output[8];
    char connector[16];
    char mode[32];
    char aspect[8];
} jw_game_display_env;

/* Values for the variables above; an empty string means "leave it unset". */
static inline void jw_game_display_env_values(int mode, const char *connector,
                                              int width, int height, int hz,
                                              jw_game_display_env *out) {
    snprintf(out->output, sizeof(out->output), "%s",
             mode == JW_GAME_DISPLAY_PANEL ? "panel" : "hdmi");
    snprintf(out->connector, sizeof(out->connector), "%s", connector ? connector : "");
    out->mode[0] = '\0';
    if (width > 0 && height > 0) {
        if (hz > 0) {
            snprintf(out->mode, sizeof(out->mode), "%dx%d@%d", width, height, hz);
        } else {
            snprintf(out->mode, sizeof(out->mode), "%dx%d", width, height);
        }
    }
    snprintf(out->aspect, sizeof(out->aspect), "%s",
             mode == JW_GAME_DISPLAY_TV_STRETCH ? "stretch" : "4:3");
}

/* The JW_WESTON_INITD command that brings Weston up on one output, single-head:

   - the panel (DSI-1). `panel_override`, when not NULL, is removed first: the
     HDMI Output switch drops the panel's refresh-rate override, while
     restoring the panel after a game keeps it.
   - the TV (HDMI-A-1) at 1280x720, or 1920x1080@120 with `use120`, as a 960x720
     virtual output, pillarboxed by a centered rect in 4:3 and stretched
     otherwise. `panel_override` is always removed here (when given) so it
     cannot pin the mode.

   `verb` is "start" or "restart". Returns 0, or -1 when the buffer is too
   small. */
static inline int jw_game_display_weston_command(int mode, bool use120,
                                                 const char *panel_override,
                                                 const char *verb,
                                                 char *buf, size_t size) {
    int n;
    if (mode == JW_GAME_DISPLAY_TV_4_3 || mode == JW_GAME_DISPLAY_TV_STRETCH) {
        char rectline[64] = "";
        if (mode == JW_GAME_DISPLAY_TV_4_3) {
            snprintf(rectline, sizeof(rectline), "output:HDMI-A-1:rect=<%s>\\n",
                     use120 ? "240,0,1680,1080" : "160,0,1120,720");
        }
        n = snprintf(buf, size,
            "rm -f %s; "
            "printf 'output:HDMI-A-1:mode=%s\\n%soutput:HDMI-A-1:primary\\n' > "
            JW_GAME_DISPLAY_WESTON_CONF "; "
            JW_WESTON_ROOTFS_ENV " WESTON_DRM_SINGLE_HEAD=1 WESTON_DRM_PRIMARY=HDMI-A-1 "
            "WESTON_DRM_VIRTUAL_SIZE=960x720 WESTON_DRM_CONFIG=" JW_GAME_DISPLAY_WESTON_CONF " "
            JW_WESTON_INITD_SCRIPT " %s",
            panel_override ? panel_override : "",
            use120 ? "1920x1080@120" : "1280x720", rectline, verb);
    } else {
        n = snprintf(buf, size,
            "rm -f " JW_GAME_DISPLAY_WESTON_CONF "%s%s; "
            JW_WESTON_ROOTFS_ENV " WESTON_DRM_SINGLE_HEAD=1 WESTON_DRM_PRIMARY=DSI-1 "
            JW_WESTON_INITD_SCRIPT " %s",
            panel_override ? " " : "", panel_override ? panel_override : "", verb);
    }
    return n > 0 && (size_t)n < size ? 0 : -1;
}

#endif
