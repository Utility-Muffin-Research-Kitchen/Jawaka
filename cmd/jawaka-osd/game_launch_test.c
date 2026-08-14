#include "cmd/jawaka-osd/game_launch.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

static cJSON *parse(const char *json) {
    cJSON *root = cJSON_Parse(json);
    assert(root);
    return root;
}

static void expect_valid(const char *json, jw_osd_game_stage want_stage,
                         int want_pending, const char *want_title,
                         const char *want_action) {
    cJSON *root = parse(json);
    jw_osd_game_stage stage;
    int pending = -1;
    assert(jw_osd_game_launch_parse(root, &stage, &pending));
    assert(stage == want_stage);
    assert(pending == want_pending);
    char title[64];
    char action[64];
    jw_osd_game_launch_text(stage, pending, title, sizeof(title),
                            action, sizeof(action));
    assert(strcmp(title, want_title) == 0);
    assert(strcmp(action, want_action) == 0);
    assert(strcmp(jw_osd_game_stage_name(stage),
                  cJSON_GetObjectItemCaseSensitive(root, "stage")->valuestring) == 0);
    cJSON_Delete(root);
}

static void expect_invalid(const char *json) {
    cJSON *root = parse(json);
    jw_osd_game_stage stage;
    int pending = 0;
    assert(!jw_osd_game_launch_parse(root, &stage, &pending));
    cJSON_Delete(root);
}

int main(void) {
    expect_valid("{\"type\":\"show-game-launch\",\"stage\":\"checking\"}",
                 JW_OSD_GAME_CHECKING, 0, "SYNCTHING: CHECKING SAVES", "");
    expect_valid("{\"type\":\"show-game-launch\",\"stage\":\"syncing\",\"pending_items\":1}",
                 JW_OSD_GAME_SYNCING, 1, "SYNCTHING: SYNCING 1 ITEM", "MENU: START NOW");
    expect_valid("{\"type\":\"show-game-launch\",\"stage\":\"syncing\",\"pending_items\":12}",
                 JW_OSD_GAME_SYNCING, 12, "SYNCTHING: SYNCING 12 ITEMS", "MENU: START NOW");
    expect_valid("{\"type\":\"show-game-launch\",\"stage\":\"stopping\"}",
                 JW_OSD_GAME_STOPPING, 0, "SYNCTHING: STOPPING", "");

    expect_invalid("{\"type\":\"show-game-launch\",\"stage\":\"unknown\"}");
    expect_invalid("{\"type\":\"show-game-launch\",\"stage\":\"starting\"}");
    expect_invalid("{\"type\":\"show-game-launch\",\"stage\":\"syncing\"}");
    expect_invalid("{\"type\":\"show-game-launch\",\"stage\":\"syncing\",\"pending_items\":-1}");
    expect_invalid("{\"type\":\"show-game-launch\",\"stage\":\"syncing\",\"pending_items\":1.5}");
    expect_invalid("{\"type\":\"show-game-launch\",\"stage\":\"checking\",\"pending_items\":0}");
    expect_invalid("{\"type\":\"show-game-launch\",\"stage\":\"checking\",\"extra\":true}");
    expect_invalid("{\"type\":\"show-game-waiting\",\"stage\":\"syncing\",\"pending_items\":1}");
    puts("PASS osd-game-launch-test");
    return 0;
}
