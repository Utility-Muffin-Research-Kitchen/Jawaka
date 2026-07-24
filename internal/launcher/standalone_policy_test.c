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

    expect("unrelated path core direct DRM",
           jw_standalone_policy_requires_direct_drm(
               "drastic", "/sd/emulators/drastic/launch.sh", false),
           false);
    expect("missing identity calibrated input",
           jw_standalone_policy_uses_calibrated_virtual_input(NULL, NULL),
           false);

    puts("Standalone launch policy checks passed");
    return 0;
}
