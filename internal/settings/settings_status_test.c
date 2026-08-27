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

    /* System Icons cycles the three packs and rides the theme-changed flag --
       that flag is what makes the launcher clear its memoized icon paths, so a
       pack change with the flag down would keep drawing the old artwork. */
    memset(&ui, 0, sizeof(ui));
    ui.open = true;
    ui.screen = JW_SETTINGS_LAYOUT;
    ui.layout_list.cursor = JW_LAYOUT_SYSTEM_ICONS;
    ui.system_icon_pack_index = JW_SYSTEM_ICON_PACK_AUTO;
    for (int i = 1; i <= JW_SYSTEM_ICON_PACK_COUNT; i++) {
        bool theme_changed = false;
        jw_settings_ui_handle_button(&ui, CAT_BTN_RIGHT, status, sizeof(status),
                                     &theme_changed);
        if (ui.system_icon_pack_index != i % JW_SYSTEM_ICON_PACK_COUNT)
            return fail("System Icons did not cycle to the next pack");
        if (!theme_changed)
            return fail("System Icons change did not raise the rebuild flag");
    }

    bool theme_changed = false;
    jw_settings_ui_handle_button(&ui, CAT_BTN_LEFT, status, sizeof(status),
                                 &theme_changed);
    if (ui.system_icon_pack_index != JW_SYSTEM_ICON_PACK_COUNT - 1 || !theme_changed)
        return fail("System Icons did not cycle backwards");

    puts("PASS settings-status-test");
    return 0;
}
