#include "internal/launcher/standalone_policy.h"

#include <string.h>

static bool jw__string_equals(const char *value, const char *expected) {
    return value && expected && strcmp(value, expected) == 0;
}

static bool jw__path_contains(const char *path, const char *component) {
    return path && component && strstr(path, component) != NULL;
}

bool jw_standalone_policy_is_mupen64plus(const char *core_id,
                                         const char *launcher_path) {
    return jw__string_equals(core_id, "mupen64plus_standalone") ||
           jw__string_equals(core_id, "mupen64plus") ||
           jw__path_contains(launcher_path, "/mupen64plus/") ||
           jw__path_contains(launcher_path, "/Mupen64Plus");
}

bool jw_standalone_policy_is_flycast(const char *core_id,
                                     const char *launcher_path) {
    return jw__string_equals(core_id, "flycast_standalone") ||
           jw__string_equals(core_id, "flycast") ||
           jw__path_contains(launcher_path, "/flycast/") ||
           jw__path_contains(launcher_path, "/Flycast/");
}

bool jw_standalone_policy_is_ports(const char *core_id,
                                   const char *launcher_path) {
    return jw__string_equals(core_id, "ports") ||
           jw__path_contains(launcher_path, "/emulators/ports/") ||
           jw__path_contains(launcher_path, "/Roms/PORTS");
}

bool jw_standalone_policy_requires_direct_drm(
        const char *core_id,
        const char *launcher_path,
        bool metadata_requires_direct_drm) {
    return metadata_requires_direct_drm ||
           jw_standalone_policy_is_flycast(core_id, launcher_path);
}

bool jw_standalone_policy_uses_calibrated_virtual_input(
        const char *core_id,
        const char *launcher_path) {
    return jw_standalone_policy_is_mupen64plus(core_id, launcher_path) ||
           jw_standalone_policy_is_flycast(core_id, launcher_path) ||
           jw_standalone_policy_is_ports(core_id, launcher_path);
}
