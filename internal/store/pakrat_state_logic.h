#ifndef JW_STORE_PAKRAT_STATE_LOGIC_H
#define JW_STORE_PAKRAT_STATE_LOGIC_H

#include "internal/store/pakrat_state.h"

/* Resolve an owned install without ever treating an older catalog selection as
   an update. Invalid versions fail closed.

   `app_present` is whether the install is listed in Apps. `content_only` is
   whether the INSTALLED pak declares `provides` and ships no executable
   launch.sh -- in which case its absence from Apps is the contract working,
   not damage, and reporting it as Stale would offer the user a repair for a
   pak that is perfectly installed. Both come from the installed pak, never
   from which storefront lane the package was listed in. */
jw_pakrat_app_status jw_pakrat_resolve_owned_state(
    const char *selected_version,
    const char *installed_version,
    int app_present,
    int content_only,
    int *out_primary_action_allowed);

#endif
