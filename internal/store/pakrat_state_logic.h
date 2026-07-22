#ifndef JW_STORE_PAKRAT_STATE_LOGIC_H
#define JW_STORE_PAKRAT_STATE_LOGIC_H

#include "internal/store/pakrat_state.h"

/* Resolve an owned install without ever treating an older catalog selection as
   an update. Invalid versions fail closed. */
jw_pakrat_app_status jw_pakrat_resolve_owned_state(
    const char *selected_version,
    const char *installed_version,
    int app_present,
    int *out_primary_action_allowed);

#endif
