#include "internal/store/pakrat_state_logic.h"

#include "internal/platform/leaf_version.h"

jw_pakrat_app_status jw_pakrat_resolve_owned_state(
    const char *selected_version,
    const char *installed_version,
    int app_present,
    int *out_primary_action_allowed) {
    int selected[3];
    int installed[3];
    int valid = selected_version && installed_version &&
                jw_pak_version_parse(selected_version, selected) == 0 &&
                jw_pak_version_parse(installed_version, installed) == 0;
    int cmp = valid ? jw_version_cmp(selected, installed) : 0;

    if (out_primary_action_allowed) {
        *out_primary_action_allowed = 0;
    }
    if (!app_present) {
        if (out_primary_action_allowed) {
            *out_primary_action_allowed = valid && cmp == 0;
        }
        return JW_PAKRAT_APP_STALE;
    }
    if (valid && cmp > 0) {
        if (out_primary_action_allowed) {
            *out_primary_action_allowed = 1;
        }
        return JW_PAKRAT_APP_UPDATE_AVAILABLE;
    }
    if (out_primary_action_allowed) {
        *out_primary_action_allowed = valid && cmp == 0;
    }
    return JW_PAKRAT_APP_INSTALLED;
}
