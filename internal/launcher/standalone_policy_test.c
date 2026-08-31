#include "internal/launcher/standalone_policy.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static void expect(const char *label, bool actual, bool expected) {
    if (actual == expected) {
        return;
    }
    fprintf(stderr, "%s: got %s, expected %s\n", label,
            actual ? "true" : "false", expected ? "true" : "false");
    exit(1);
}

int main(void) {
    const char *flycast_path =
        "/sd/.system/leaf/platforms/mlp1/emulators/flycast/launch.sh";

    expect("Flycast identity",
           jw_standalone_policy_is_flycast("flycast_standalone", flycast_path),
           true);
    expect("Flycast direct DRM by identity",
           jw_standalone_policy_requires_direct_drm(
               "flycast_standalone", flycast_path, false),
           true);
    expect("Flycast calibrated input",
           jw_standalone_policy_uses_calibrated_virtual_input(
               "flycast_standalone", flycast_path),
           true);
    expect("Flycast path compatibility",
           jw_standalone_policy_is_flycast("legacy_path_core", flycast_path),
           true);

    expect("metadata direct DRM",
           jw_standalone_policy_requires_direct_drm(
               "ppsspp", "/sd/emulators/ppsspp/launch.sh", true),
           true);
    expect("PPSSPP calibrated input",
           jw_standalone_policy_uses_calibrated_virtual_input(
               "ppsspp", "/sd/emulators/ppsspp/launch.sh"),
           true);
    expect("PPSSPP GLES calibrated input by path",
           jw_standalone_policy_uses_calibrated_virtual_input(
               "ppsspp_gles", "/sd/emulators/ppsspp/launch-gles.sh"),
           true);

    expect("Mupen calibrated input",
           jw_standalone_policy_uses_calibrated_virtual_input(
               "mupen64plus_standalone",
               "/sd/emulators/mupen64plus/launch.sh"),
           true);
    expect("Mupen no implicit direct DRM",
           jw_standalone_policy_requires_direct_drm(
               "mupen64plus_standalone",
               "/sd/emulators/mupen64plus/launch.sh", false),
           false);

    expect("Ports calibrated input",
           jw_standalone_policy_uses_calibrated_virtual_input(
               "ports", "/sd/emulators/ports/launch.sh"),
           true);
    expect("Ports no blanket direct DRM",
           jw_standalone_policy_requires_direct_drm(
               "ports", "/sd/emulators/ports/launch.sh", false),
           false);

    expect("DraStic no implicit direct DRM",
           jw_standalone_policy_requires_direct_drm(
               "drastic", "/sd/emulators/drastic/launch.sh", false),
           false);
    expect("DraStic calibrated input (paired wireless controllers)",
           jw_standalone_policy_uses_calibrated_virtual_input(
               "drastic", "/sd/emulators/drastic/launch.sh"),
           true);
    expect("DraStic calibrated input by path only",
           jw_standalone_policy_uses_calibrated_virtual_input(
               "unrelated_core", "/sd/emulators/DraStic/launch.sh"),
           true);

    expect("YabaSanshiro standalone identity",
           jw_standalone_policy_is_yabasanshiro(
               "yabasanshiro_standalone", NULL),
           true);
    expect("YabaSanshiro standalone path compatibility",
           jw_standalone_policy_is_yabasanshiro(
               "legacy_path_core",
               "/sd/.system/leaf/platforms/mlp1/emulators/yabasanshiro/launch.sh"),
           true);
    expect("YabaSanshiro exact standalone identity",
           jw_standalone_policy_is_yabasanshiro(
               "yabasanshiro_standalone_preview", NULL),
           false);
    expect("YabaSanshiro exact package path",
           jw_standalone_policy_is_yabasanshiro(
               "legacy_path_core",
               "/sd/emulators/yabasanshiro-preview/launch.sh"),
           false);
    expect("RetroArch YabaSanshiro is not standalone",
           jw_standalone_policy_is_yabasanshiro(
               "yabasanshiro", "/sd/cores/yabasanshiro_libretro.so"),
           false);
    expect("RetroArch YabaSanshiro keeps normal input",
           jw_standalone_policy_uses_calibrated_virtual_input(
               "yabasanshiro", "/sd/cores/yabasanshiro_libretro.so"),
           false);
    expect("YabaSanshiro standalone calibrated input",
           jw_standalone_policy_uses_calibrated_virtual_input(
               "yabasanshiro_standalone", NULL),
           true);
    expect("YabaSanshiro standalone path calibrated input",
           jw_standalone_policy_uses_calibrated_virtual_input(
               "legacy_path_core",
               "/sd/emulators/yabasanshiro/launch.sh"),
           true);
    expect("YabaSanshiro direct DRM remains metadata-driven",
           jw_standalone_policy_requires_direct_drm(
               "yabasanshiro_standalone",
               "/sd/emulators/yabasanshiro/launch.sh", false),
           false);
    expect("YabaSanshiro CHD supported",
           jw_standalone_policy_supports_content(
               "yabasanshiro_standalone", NULL,
               "/sd/Roms/SATURN/Shining Force III.chd"),
           true);
    expect("YabaSanshiro ZIP rejected",
           jw_standalone_policy_supports_content(
               "yabasanshiro_standalone", NULL,
               "/sd/Roms/SATURN/Rampage.ZIP"),
           false);
    expect("YabaSanshiro M3U rejected by package path",
           jw_standalone_policy_supports_content(
               "legacy_path_core", "/sd/emulators/yabasanshiro/launch.sh",
               "Roms/SATURN/Enemy Zero.m3u"),
           false);
    expect("RetroArch YabaSanshiro keeps ZIP support",
           jw_standalone_policy_supports_content(
               "yabasanshiro", "/sd/cores/yabasanshiro_libretro.so",
               "/sd/Roms/SATURN/Rampage.zip"),
           true);
    expect("Other standalone content unaffected",
           jw_standalone_policy_supports_content(
               "flycast_standalone", "/sd/emulators/flycast/launch.sh",
               "/sd/Roms/DC/Game.zip"),
           true);
    expect("missing identity calibrated input",
           jw_standalone_policy_uses_calibrated_virtual_input(NULL, NULL),
           false);

    puts("Standalone launch policy checks passed");
    return 0;
}
