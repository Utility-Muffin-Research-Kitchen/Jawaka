#define CAT_IMPLEMENTATION
#include "catastrophe.h"
#define CAT_WIDGETS_IMPLEMENTATION
#include "catastrophe_widgets.h"

#include "internal/settings/settings.h"
#include "internal/launcher/system_activity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail(const char *message) {
    fprintf(stderr, "settings-status-test: %s\n", message);
    return 1;
}

static int check_activity(void) {
    jw_system_activity activity = {0};
    if (jw_system_activity_text(&activity, 0, "Scanning")[0])
        return fail("idle System has a status line");
    activity.scan_running = true;
    snprintf(activity.scrape, sizeof(activity.scrape), "Scraping Genesis · 42/175");
    if (strcmp(jw_system_activity_text(&activity, 100, "Scanning"), activity.scrape))
        return fail("library scan hid scrape progress");
    jw_system_notice_set(&activity.feedback, "Saved", 100);
    if (strcmp(jw_system_activity_text(&activity, 6099, "Scanning"), "Saved"))
        return fail("feedback expired before six seconds");
    if (strcmp(jw_system_activity_text(&activity, 6100, "Scanning"), activity.scrape))
        return fail("feedback did not restore progress at six seconds");
    jw_system_notice_set(&activity.feedback, "Saved", 6200);
    if (strcmp(jw_system_activity_text(&activity, 12199, "Scanning"), "Saved"))
        return fail("repeated feedback did not restart its timer");
    activity.feedback.text[0] = '\0';  /* leave the originating page */
    snprintf(activity.scrape, sizeof(activity.scrape), "Scraping paused: quota");
    if (strcmp(jw_system_activity_text(&activity, 90000, "Scanning"), activity.scrape))
        return fail("paused activity expired or disappeared with page feedback");
    activity.scrape[0] = '\0';
    jw_system_notice_set(&activity.completion, "Scrape finished", 90000);
    if (strcmp(jw_system_activity_text(&activity, 90001, "Scanning"), "Scrape finished") ||
        strcmp(jw_system_activity_text(&activity, 96000, "Scanning"), "Scanning"))
        return fail("completion summary did not expire back to scan activity");
    activity.scan_running = false;
    if (jw_system_activity_text(&activity, 96000, "Scanning")[0])
        return fail("idle strip did not reclaim its space");
    jw_system_notice_set(&activity.feedback, "Saved", UINT32_MAX - 100);
    if (!jw_system_notice_remaining(&activity.feedback, 100) ||
        jw_system_notice_remaining(&activity.feedback, 6000))
        return fail("feedback expiry failed across clock wrap");
    return 0;
}

static int write_theme(const char *root, const char *dir, const char *name) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/Themes", root);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/Themes/%s", root, dir);
    if (mkdir(path, 0755) != 0) return -1;
    snprintf(path, sizeof(path), "%s/Themes/%s/theme.json", root, dir);
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "{ \"name\": \"%s\" }\n", name);
    return fclose(fp);
}

/* The Layout > Theme row and Pak Rat's Apply share one selection path, so a
   store Apply is exactly a Settings pick: same persisted key, same rebuild. */
static int check_theme_selection(void) {
    char root[] = "/tmp/settings-status-test.XXXXXX";
    if (!mkdtemp(root)) return fail("could not create a themes root");
    if (write_theme(root, "aurora", "Aurora") || write_theme(root, "neon-nights", "Neon Nights"))
        return fail("could not write test themes");
    jw_settings_ui ui = {0};
    ui.user_theme_index = -1;
    jw_settings_ui_set_themes_root(&ui, root);
    if (ui.user_themes.count != 2) return fail("test themes were not scanned");

    int neon = jw_user_themes_find(&ui.user_themes, "neon-nights");
    char status[128] = "stale";
    if (!jw_settings_ui_select_user_theme(&ui, neon, status, sizeof(status)) ||
        strcmp(ui.user_theme_dir, "neon-nights") != 0 ||
        jw_settings_user_theme_index(&ui) != neon)
        return fail("selecting a theme did not select it");
    if (jw_settings_ui_select_user_theme(&ui, neon, status, sizeof(status)))
        return fail("reselecting the current theme reported a change");
    if (jw_settings_ui_select_user_theme(&ui, 2, NULL, 0) ||
        jw_settings_ui_select_user_theme(&ui, -2, NULL, 0) ||
        strcmp(ui.user_theme_dir, "neon-nights") != 0)
        return fail("an out-of-range selection changed the theme");

    /* The Appearance > Theme row cycles through the same function and raises
       the flag. */
    ui.open = true;
    ui.screen = JW_SETTINGS_APPEARANCE;
    ui.appearance_list.cursor = JW_APPEAR_THEME;
    bool theme_changed = false;
    jw_settings_ui_handle_button(&ui, CAT_BTN_RIGHT, status, sizeof(status), &theme_changed);
    if (!theme_changed || ui.user_theme_dir[0] || jw_settings_user_theme_index(&ui) != -1)
        return fail("cycling past the last theme did not select None");
    if (status[0]) return fail("selecting None kept a theme's status");

    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0) return fail("could not remove the themes root");
    return 0;
}

/* Render at device size with a software renderer. Sentinel pixels catch text,
   sliders, swatches or highlights escaping the allocated page rectangle. */
static int check_layout_viewport(void) {
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    SDL_setenv("CAT_WINDOW_WIDTH", "960", 1);
    SDL_setenv("CAT_WINDOW_HEIGHT", "720", 1);
    char font[4096];
    const char *fonts = getenv("CAT_FONTS_DIR");
    snprintf(font, sizeof(font), "%s/fonts/Nunito/Nunito-Bold.ttf", fonts ? fonts : "res");
    cat_config config = { .start_hidden = true, .defer_input_init = true,
                          .disable_background = true, .disable_font_bump = true,
                          .font_path = font };
    if (cat_init(&config) != CAT_OK) return fail("could not initialize render check");
    SDL_Renderer *renderer = cat_get_renderer();
    uint32_t *pixels = malloc(960 * 720 * sizeof(*pixels));
    if (!pixels) return fail("pixel allocation failed");
    jw_settings_ui ui = {0};
    ui.open = true;
    ui.screen = JW_SETTINGS_HOME_SCREEN;
    ui.user_theme_index = -1;
    cat_list_state_init(&ui.home_screen_list, JW_HOMESCREEN_ROW_COUNT);
    ui.home_screen_list.cursor = JW_HOMESCREEN_TABS;
    for (int bump = 2; bump <= 5; bump += 3) {
        if (cat_set_font_bump(bump) != CAT_OK) return fail("font bump failed");
        /* Expanded/shrunk/restored: hints and activity taking or releasing space. */
        const int heights[] = { 620, 500, 450, 620 };
        for (unsigned i = 0; i < sizeof(heights) / sizeof(heights[0]); ++i) {
            SDL_SetRenderDrawColor(renderer, 13, 29, 47, 255);
            SDL_RenderClear(renderer);
            jw_settings_ui_render(&ui, 12, 60, 936, heights[i]);
            if (ui.home_screen_list.cursor != JW_HOMESCREEN_TABS ||
                ui.home_screen_list.cursor < ui.home_screen_list.scroll_offset ||
                ui.home_screen_list.cursor >= ui.home_screen_list.scroll_offset + ui.home_screen_list.visible_rows)
                return fail("viewport resize lost the selected Home Tabs row");
            int row_h = TTF_FontHeight(cat_get_font(CAT_FONT_MEDIUM)) + cat_scale(12);
            int header_h = TTF_FontHeight(cat_get_font(CAT_FONT_LARGE)) + cat_scale(10);
            if (ui.home_screen_list.visible_rows * row_h > heights[i] - header_h)
                return fail("visible rows exceed the available content height");
            if (SDL_RenderIsClipEnabled(renderer)) return fail("page leaked its clip");
            if (SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, pixels,
                                     960 * sizeof(*pixels)) != 0)
                return fail("could not inspect rendered pixels");
            ap_color text = cat_get_theme()->highlighted_text;
            uint32_t ink = 0xff000000u | (uint32_t)text.r << 16 |
                           (uint32_t)text.g << 8 | text.b;
            int selected_y = 60 + header_h +
                (ui.home_screen_list.cursor - ui.home_screen_list.scroll_offset) * row_h;
            bool label_visible = false;
            for (int y = selected_y + 6; y < selected_y + row_h - 6; ++y)
                for (int x = 48; x < 240; ++x)
                    if (pixels[y * 960 + x] == ink) label_visible = true;
            if (!label_visible) return fail("selected row label did not scroll with its highlight");
            for (int y = 0; y < 720; ++y)
                for (int x = 0; x < 960; ++x)
                    if ((x < 12 || x >= 948 || y < 60 || y >= 60 + heights[i]) &&
                        pixels[y * 960 + x] != 0xff0d1d2f)
                        return fail("settings drew outside its viewport");
            cat_list_state_move(&ui.home_screen_list, -1, JW_HOMESCREEN_ROW_COUNT);
            cat_list_state_move(&ui.home_screen_list, 1, JW_HOMESCREEN_ROW_COUNT);
        }
    }
    free(pixels);
    cat_quit();
    return 0;
}

/* Every child page hands B back to the page that opens it. This is the check
   that reorganizing Settings needs: moving a page under a new parent means
   retargeting its B, and a stale target strands the user on an unrelated screen
   with no sign anything is wrong. (Bluetooth did exactly that, landing on Wi-Fi
   after it stopped being a row there.) */
static int check_back_targets(void) {
    static const struct {
        jw_settings_screen screen;
        jw_settings_screen parent;
    } kBack[] = {
        { JW_SETTINGS_COLORS,              JW_SETTINGS_APPEARANCE  },
        { JW_SETTINGS_STATUS_BAR,          JW_SETTINGS_APPEARANCE  },
        { JW_SETTINGS_APPEARANCE,          JW_SETTINGS_HOME        },
        { JW_SETTINGS_HOME_SCREEN,         JW_SETTINGS_HOME        },
        { JW_SETTINGS_HOME_TABS,           JW_SETTINGS_HOME_SCREEN },
        { JW_SETTINGS_DISPLAY,             JW_SETTINGS_HOME        },
        { JW_SETTINGS_LIGHTING,            JW_SETTINGS_HOME        },
        { JW_SETTINGS_WIFI,                JW_SETTINGS_HOME        },
        { JW_SETTINGS_BLUETOOTH,           JW_SETTINGS_HOME        },
        { JW_SETTINGS_GAMES,               JW_SETTINGS_HOME        },
        { JW_SETTINGS_ACCOUNTS,            JW_SETTINGS_GAMES       },
        { JW_SETTINGS_SCRAPE_PRIORITY,     JW_SETTINGS_GAMES       },
        { JW_SETTINGS_SCRAPE_QUEUE,        JW_SETTINGS_GAMES       },
        { JW_SETTINGS_SCRAPE_DOWNLOAD,     JW_SETTINGS_GAMES       },
        { JW_SETTINGS_SCRAPE_QUEUE_DETAIL, JW_SETTINGS_SCRAPE_QUEUE },
        { JW_SETTINGS_CONTROLS,            JW_SETTINGS_HOME        },
        { JW_SETTINGS_SYSTEM,              JW_SETTINGS_HOME        },
        { JW_SETTINGS_TIMEZONE_PICKER,     JW_SETTINGS_SYSTEM      },
        { JW_SETTINGS_SERVICES,            JW_SETTINGS_SYSTEM      },
        { JW_SETTINGS_UPDATE,              JW_SETTINGS_HOME        },
        { JW_SETTINGS_UPDATE_PICKER,       JW_SETTINGS_UPDATE      },
    };
    for (unsigned i = 0; i < sizeof(kBack) / sizeof(kBack[0]); ++i) {
        jw_settings_ui ui = {0};
        char status[64] = "";
        ui.open = true;
        ui.screen = kBack[i].screen;
        jw_settings_ui_handle_button(&ui, CAT_BTN_B, status, sizeof(status), NULL);
        if (ui.screen != kBack[i].parent) {
            fprintf(stderr, "settings-status-test: screen %d went back to %d, "
                            "expected %d\n", (int)kBack[i].screen,
                    (int)ui.screen, (int)kBack[i].parent);
            return 1;
        }
    }
    return 0;
}

int main(void) {
    jw_settings_ui ui = {0};
    char status[64] = "Saved scrape order";

    if (check_back_targets()) return 1;

    ui.open = true;
    ui.screen = JW_SETTINGS_SCRAPE_PRIORITY;
    ui.scrape_edit_grabbed = true;
    jw_settings_ui_handle_button(&ui, CAT_BTN_B, status, sizeof(status), NULL);
    if (ui.screen != JW_SETTINGS_SCRAPE_PRIORITY || ui.scrape_edit_grabbed ||
        strcmp(status, "Saved scrape order") != 0)
        return fail("canceling a scrape grab cleared status or changed page");

    jw_settings_ui_handle_button(&ui, CAT_BTN_B, status, sizeof(status), NULL);
    if (ui.screen != JW_SETTINGS_GAMES || status[0] != '\0')
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
    if (ui.screen != JW_SETTINGS_HOME_SCREEN || status[0] != '\0')
        return fail("leaving home tabs did not clear status");

    /* System Icons cycles the three packs and rides the theme-changed flag --
       that flag is what makes the launcher clear its memoized icon paths, so a
       pack change with the flag down would keep drawing the old artwork. */
    memset(&ui, 0, sizeof(ui));
    ui.open = true;
    ui.screen = JW_SETTINGS_HOME_SCREEN;
    ui.home_screen_list.cursor = JW_HOMESCREEN_SYSTEM_ICONS;
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

    /* Hotkeys & Rumble keeps all seven rows off MLP1: the capture toggles
       stay on the parent page because there is no Hotkeys child to
       move them into. On MLP1 this is four, and the shortcut page's own logic
       is covered by input-shortcuts-test -- the UI cannot be built in MLP1
       shape on this host, because Catastrophe's MLP1 paths need <linux/input.h>
       and poll(). */
#ifndef PLATFORM_MLP1
    if (JW_CONTROLS_ROW_COUNT != 7)
        return fail("non-MLP1 Controls page changed row count");
    if (JW_CONTROLS_REC_KEEP != JW_CONTROLS_ROW_COUNT - 1)
        return fail("non-MLP1 Controls rows are no longer contiguous");

    memset(&ui, 0, sizeof(ui));
    ui.open = true;
    ui.screen = JW_SETTINGS_CONTROLS;
    ui.controls_list.cursor = JW_CONTROLS_REC_KEEP;
    status[0] = '\0';
    /* cat_list_state_move wraps rather than clamps, so the last row leads back
       to the first. The point of the check is that the page's row count is the
       seven-row one, not that the cursor stops. */
    jw_settings_ui_handle_button(&ui, CAT_BTN_DOWN, status, sizeof(status), NULL);
    if (ui.controls_list.cursor != JW_CONTROLS_UI_RUMBLE)
        return fail("Controls cursor did not wrap from the last row to the first");
    jw_settings_ui_handle_button(&ui, CAT_BTN_UP, status, sizeof(status), NULL);
    if (ui.controls_list.cursor != JW_CONTROLS_REC_KEEP)
        return fail("Controls cursor did not wrap back to the last row");
    jw_settings_ui_handle_button(&ui, CAT_BTN_B, status, sizeof(status), NULL);
    if (ui.screen != JW_SETTINGS_HOME)
        return fail("B on Controls did not return to the settings home");
#endif


    /* Home Tabs: switching a tab off leaves it where it sits. The list used to
       be partitioned into shown-then-hidden, so hiding a row moved it under the
       cursor; only X plus Up/Down reorders now, hidden rows included. */
    {
        jw_settings_ui tabs = {0};
        tabs.open = true;
        tabs.screen = JW_SETTINGS_HOME_TABS;
        for (int i = 0; i < JW_HOME_TABS_COUNT; i++) tabs.home_tab_order[i] = i;
        tabs.home_tab_visible = JW_HOME_TABS_COUNT;
        tabs.home_tabs_list.cursor = 1;

        jw_settings_ui_handle_button(&tabs, CAT_BTN_A, status, sizeof(status), NULL);
        if (!tabs.home_tab_hidden[1] || tabs.home_tab_visible != JW_HOME_TABS_COUNT - 1)
            return fail("A did not hide the tab under the cursor");
        if (tabs.home_tabs_list.cursor != 1)
            return fail("hiding a tab moved the cursor");
        for (int i = 0; i < JW_HOME_TABS_COUNT; i++)
            if (tabs.home_tab_order[i] != i)
                return fail("hiding a tab reordered the list");

        jw_settings_ui_handle_button(&tabs, CAT_BTN_A, status, sizeof(status), NULL);
        if (tabs.home_tab_hidden[1] || tabs.home_tab_visible != JW_HOME_TABS_COUNT)
            return fail("A did not show the tab again");

        /* A hidden row moves like any other: where it sits is where it returns. */
        tabs.home_tabs_list.cursor = 0;
        jw_settings_ui_handle_button(&tabs, CAT_BTN_A, status, sizeof(status), NULL);
        jw_settings_ui_handle_button(&tabs, CAT_BTN_X, status, sizeof(status), NULL);
        if (!tabs.home_tabs_grabbed)
            return fail("X did not grab a hidden row");
        jw_settings_ui_handle_button(&tabs, CAT_BTN_DOWN, status, sizeof(status), NULL);
        if (tabs.home_tab_order[0] != 1 || tabs.home_tab_order[1] != 0 ||
            tabs.home_tabs_list.cursor != 1)
            return fail("a grabbed hidden row did not move down");
        jw_settings_ui_handle_button(&tabs, CAT_BTN_X, status, sizeof(status), NULL);

        /* The last visible tab cannot be switched off. */
        for (int i = 0; i < JW_HOME_TABS_COUNT; i++) {
            tabs.home_tab_hidden[i] = i != 2;
        }
        tabs.home_tab_visible = 1;
        for (int i = 0; i < JW_HOME_TABS_COUNT; i++) {
            if (tabs.home_tab_order[i] != 2) continue;
            tabs.home_tabs_list.cursor = i;
            break;
        }
        jw_settings_ui_handle_button(&tabs, CAT_BTN_A, status, sizeof(status), NULL);
        if (tabs.home_tab_hidden[2] || tabs.home_tab_visible != 1)
            return fail("the last visible tab was switched off");
    }

    if (check_activity() || check_theme_selection() || check_layout_viewport()) return 1;
    puts("PASS settings-status-test");
    return 0;
}
