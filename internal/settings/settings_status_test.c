#define CAT_IMPLEMENTATION
#include "catastrophe.h"
#define CAT_WIDGETS_IMPLEMENTATION
#include "catastrophe_widgets.h"

#include "internal/settings/settings.h"

#include <stdio.h>
#include <string.h>

static int fail(const char *message) {
    fprintf(stderr, "settings-status-test: %s\n", message);
    return 1;
}

int main(void) {
    jw_settings_ui ui = {0};
    char status[64] = "Saved scrape order";

    ui.open = true;
    ui.screen = JW_SETTINGS_SCRAPE_PRIORITY;
    ui.scrape_edit_grabbed = true;
    jw_settings_ui_handle_button(&ui, CAT_BTN_B, status, sizeof(status), NULL);
    if (ui.screen != JW_SETTINGS_SCRAPE_PRIORITY || ui.scrape_edit_grabbed ||
        strcmp(status, "Saved scrape order") != 0)
        return fail("canceling a scrape grab cleared status or changed page");

    jw_settings_ui_handle_button(&ui, CAT_BTN_B, status, sizeof(status), NULL);
    if (ui.screen != JW_SETTINGS_SCRAPING || status[0] != '\0')
        return fail("leaving scrape priority did not clear status");

    memset(&ui, 0, sizeof(ui));
    snprintf(status, sizeof(status), "%s", "Saved home tabs");
    ui.open = true;
    ui.screen = JW_SETTINGS_HOME_TABS;
    ui.home_tabs_grabbed = true;
    jw_settings_ui_handle_button(&ui, CAT_BTN_B, status, sizeof(status), NULL);
    if (ui.screen != JW_SETTINGS_HOME_TABS || ui.home_tabs_grabbed ||
        strcmp(status, "Saved home tabs") != 0)
        return fail("canceling a home-tab grab cleared status or changed page");

    jw_settings_ui_handle_button(&ui, CAT_BTN_B, status, sizeof(status), NULL);
    if (ui.screen != JW_SETTINGS_LAYOUT || status[0] != '\0')
        return fail("leaving home tabs did not clear status");

    puts("PASS settings-status-test");
    return 0;
}
