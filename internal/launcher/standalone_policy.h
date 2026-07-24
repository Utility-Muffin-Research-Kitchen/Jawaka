#ifndef JW_STANDALONE_POLICY_H
#define JW_STANDALONE_POLICY_H

#include <stdbool.h>

bool jw_standalone_policy_is_mupen64plus(const char *core_id,
                                         const char *launcher_path);
bool jw_standalone_policy_is_flycast(const char *core_id,
                                     const char *launcher_path);
bool jw_standalone_policy_is_ppsspp(const char *core_id,
                                    const char *launcher_path);
bool jw_standalone_policy_is_ports(const char *core_id,
                                   const char *launcher_path);

bool jw_standalone_policy_requires_direct_drm(const char *core_id,
                                              const char *launcher_path,
                                              bool metadata_requires_direct_drm);
bool jw_standalone_policy_uses_calibrated_virtual_input(
    const char *core_id,
    const char *launcher_path);

#endif
