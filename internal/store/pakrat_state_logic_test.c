#include "internal/store/pakrat_state_logic.h"

#include <assert.h>
#include <stdio.h>

static void expect_content(const char *selected, const char *installed,
                           int present, int content_only,
                           jw_pakrat_app_status expected_status,
                           int expected_action) {
    int action = -1;
    assert(jw_pakrat_resolve_owned_state(selected, installed, present,
                                         content_only, &action)
           == expected_status);
    assert(action == expected_action);
}

static void expect(const char *selected, const char *installed, int present,
                   jw_pakrat_app_status expected_status,
                   int expected_action) {
    expect_content(selected, installed, present, 0, expected_status,
                   expected_action);
}

int main(void) {
    jw_pakrat_app_state app = {
        .status = JW_PAKRAT_APP_INSTALLED,
        .installed_owned = 1,
        .open_allowed = 1,
        .primary_action_allowed = 1,
    };
    assert(jw_pakrat_primary_action_opens(&app));
    app.status = JW_PAKRAT_APP_UPDATE_AVAILABLE;
    assert(!jw_pakrat_primary_action_opens(&app));
    app.status = JW_PAKRAT_APP_INSTALLED;
    app.primary_action_allowed = 0; /* Catalog gates must not block Open. */
    assert(jw_pakrat_primary_action_opens(&app));
    app.open_allowed = 0; /* Content-only paks have no app to open. */
    assert(!jw_pakrat_primary_action_opens(&app));
    app.open_allowed = 1;
    app.managed = 1;
    assert(!jw_pakrat_primary_action_opens(&app));
    app.managed = 0;
    app.installed_owned = 0;
    assert(!jw_pakrat_primary_action_opens(&app));
    assert(!jw_pakrat_primary_action_opens(NULL));

    expect("0.2.0", "0.2.0", 1, JW_PAKRAT_APP_INSTALLED, 1);
    expect("0.3.0", "0.2.0", 1, JW_PAKRAT_APP_UPDATE_AVAILABLE, 1);
    expect("0.1.2", "0.2.0", 1, JW_PAKRAT_APP_INSTALLED, 0);
    expect("0.2.0", "v0.2.0", 1, JW_PAKRAT_APP_INSTALLED, 0);
    expect("0.2.0", "0.2.0", 0, JW_PAKRAT_APP_STALE, 1);
    expect("0.1.2", "0.2.0", 0, JW_PAKRAT_APP_STALE, 0);

    /* CONTENT-1: a pure content pak is absent from Apps BY DESIGN. Reporting
       it as Stale would offer a repair for a pak that is installed correctly,
       so absence only means damage when the pak was supposed to be listed. */
    expect_content("0.2.0", "0.2.0", 0, 1, JW_PAKRAT_APP_INSTALLED, 1);
    expect_content("0.3.0", "0.2.0", 0, 1, JW_PAKRAT_APP_UPDATE_AVAILABLE, 1);
    expect_content("0.1.2", "0.2.0", 0, 1, JW_PAKRAT_APP_INSTALLED, 0);

    /* A hybrid is listed in Apps, so it behaves exactly like an app: if it
       goes missing from Apps, that IS damage. */
    expect_content("0.2.0", "0.2.0", 1, 0, JW_PAKRAT_APP_INSTALLED, 1);
    expect_content("0.2.0", "0.2.0", 0, 0, JW_PAKRAT_APP_STALE, 1);

    puts("PASS pakrat-state-logic-test");
    return 0;
}
