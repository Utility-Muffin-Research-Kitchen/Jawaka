#include "internal/store/pakrat_state_logic.h"

#include <assert.h>
#include <stdio.h>

static void expect(const char *selected, const char *installed, int present,
                   jw_pakrat_app_status expected_status,
                   int expected_action) {
    int action = -1;
    assert(jw_pakrat_resolve_owned_state(
               selected, installed, present, &action) == expected_status);
    assert(action == expected_action);
}

int main(void) {
    expect("0.2.0", "0.2.0", 1, JW_PAKRAT_APP_INSTALLED, 1);
    expect("0.3.0", "0.2.0", 1, JW_PAKRAT_APP_UPDATE_AVAILABLE, 1);
    expect("0.1.2", "0.2.0", 1, JW_PAKRAT_APP_INSTALLED, 0);
    expect("0.2.0", "v0.2.0", 1, JW_PAKRAT_APP_INSTALLED, 0);
    expect("0.2.0", "0.2.0", 0, JW_PAKRAT_APP_STALE, 1);
    expect("0.1.2", "0.2.0", 0, JW_PAKRAT_APP_STALE, 0);

    puts("PASS pakrat-state-logic-test");
    return 0;
}
