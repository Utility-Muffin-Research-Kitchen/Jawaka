#ifndef JW_STANDALONE_POLICY_H
#define JW_STANDALONE_POLICY_H

#include <stdbool.h>

bool jw_standalone_policy_is_mupen64plus(const char *core_id,
                                         const char *launcher_path);
bool jw_standalone_policy_is_flycast(const char *core_id,
                                     const char *launcher_path);
bool jw_standalone_policy_is_ppsspp(const char *core_id,
                                    const char *launcher_path);
/* DraStic is one of the supported roster standalones: it runs under the full
   calibrated proxy (paired wireless controllers plan) even though its bundled
   SDL backend reads a raw event path instead of standard joystick enums. */
bool jw_standalone_policy_is_drastic(const char *core_id,
                                     const char *launcher_path);
bool jw_standalone_policy_is_yabasanshiro(const char *core_id,
                                          const char *launcher_path);
/* Fun DraStic is a second Nintendo DS standalone: tenlevels' frontend over the
   same closed-source drastic64 binary the DraStic package ships. It gets its
   own predicate rather than widening is_drastic(), the way YabaSanshiro did:
   the ids collide on substring, every session would otherwise log the wrong
   emulator's name, and the two packages must be able to diverge on policy. */
bool jw_standalone_policy_is_fun_drastic(const char *core_id,
                                         const char *launcher_path);
bool jw_standalone_policy_is_ports(const char *core_id,
                                   const char *launcher_path);

/* False only for content formats proven unsupported by a recognized
   standalone. Other cores and untested formats remain eligible. */
bool jw_standalone_policy_supports_content(const char *core_id,
                                           const char *launcher_path,
                                           const char *content_path);

bool jw_standalone_policy_requires_direct_drm(const char *core_id,
                                              const char *launcher_path,
                                              bool metadata_requires_direct_drm);
bool jw_standalone_policy_uses_calibrated_virtual_input(
    const char *core_id,
    const char *launcher_path);

#endif
