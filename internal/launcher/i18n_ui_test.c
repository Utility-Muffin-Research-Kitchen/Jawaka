/* Exercise real UI/IPC composition with the shipped Chinese table, so tests
 * catch a caller replacing numeric message translation with exact lookup. */
#define main jw_launcher_main
#include "cmd/jawaka-launcher/main.c"
#undef main
#include <assert.h>
#include <sys/wait.h>

static void expect_text(const char *got, const char *want) {
    if (strcmp(got, want)) {
        fprintf(stderr, "i18n-ui-test: got [%s], want [%s]\n", got, want);
        abort();
    }
}

static void serve_reply(jw_ipc_server *server, const char *reply) {
    jw_ipc_client *client = NULL;
    assert(jw_ipc_server_accept(server, &client, 5000) == 0);
    char *request = NULL;
    size_t size = 0;
    assert(jw_ipc_client_recv(client, &request, &size) == 0);
    free(request);
    assert(jw_ipc_client_send(client, reply, strlen(reply)) == 0);
    jw_ipc_client_close(client);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    char root[] = "/tmp/jw-i18n-ui-XXXXXX";
    assert(mkdtemp(root));
    setenv("UMRK_PLATFORM_PATH", argv[1], 1);
    setenv("UMRK_INTERNAL_DATA_PATH", root, 1);
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    char font[PATH_MAX];
    snprintf(font, sizeof(font), "%s/fonts/Nunito/Nunito-Bold.ttf", getenv("CAT_FONTS_DIR"));
    cat_config config = {.start_hidden = true, .defer_input_init = true,
                         .disable_background = true, .font_path = font};
    assert(cat_init(&config) == CAT_OK);
    assert(jw_i18n_load("zh_CN"));
    jw_launcher_state *state = calloc(1, sizeof(*state));
    assert(state);

    jw_status_poller *poller = &jw__status_poller;
    pthread_mutex_init(&poller->scrape_mu, NULL);
    poller->started = true;
    snprintf(poller->scrape.state, sizeof(poller->scrape.state), "paused-quota");
    snprintf(poller->scrape.message, sizeof(poller->scrape.message),
             "Daily quota exhausted (20000/20000)");
    atomic_store(&poller->scrape_seq, 1);
    jw__poll_scrape_status(state);
    char expected[512];
    snprintf(expected, sizeof(expected), T("Scraping paused: %.200s"),
             "已达每日配额（20000/20000）");
    expect_text(state->status, expected);
    expect_text(state->system_activity.scrape, expected);
    poller->started = false;
    pthread_mutex_destroy(&poller->scrape_mu);

    /* Count text is composed by the actual store handler. No English 's' may
     * survive in Chinese for zero, one, or multiple apps. */
    state->pakrat_apps = calloc(3, sizeof(*state->pakrat_apps));
    assert(state->pakrat_apps);
    for (int count = 0; count <= 3; ++count) {
        state->pakrat_app_count = count;
        jw__pakrat_build_view(state);
        snprintf(expected, sizeof(expected), "%d 个 Pak Rat 应用", count);
        expect_text(state->pakrat_message, expected);
    }

    /* A title matching a UI key is rendered/measured as the original title. */
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    assert(strcmp(T("Settings"), "Settings"));
    int title_width = 200;
    assert(jw__delete_line("Settings", 0, 0, title_width, false) ==
           cat_measure_wrapped_text_height(small, "Settings", title_width) + CAT_S(10));
    SDL_Renderer *renderer = cat_get_renderer();
    SDL_Rect region = {0, 0, 200, 100};
    unsigned char actual[200 * 100 * 4], original[sizeof(actual)];
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    jw__delete_line("Settings", 0, 0, title_width, true);
    assert(SDL_RenderReadPixels(renderer, &region, SDL_PIXELFORMAT_RGBA32,
                                actual, 200 * 4) == 0);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    cat_draw_text_wrapped(small, "Settings", 0, 0, title_width,
                          cat_get_theme()->text, CAT_ALIGN_LEFT);
    assert(SDL_RenderReadPixels(renderer, &region, SDL_PIXELFORMAT_RGBA32,
                                original, 200 * 4) == 0);
    assert(memcmp(actual, original, sizeof(actual)) == 0);

    char socket_path[PATH_MAX];
    snprintf(socket_path, sizeof(socket_path), "%s/ipc.sock", root);
    jw_ipc_server *server = NULL;
    assert(jw_ipc_server_listen(socket_path, &server) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        serve_reply(server, "{\"type\":\"error\",\"message\":\"game no longer exists\"}");
        serve_reply(server, "{\"type\":\"ok\",\"game_count\":3,\"app_count\":2}");
        _exit(0);
    }
    char status[512];
    assert(jw_ipc_launch_game(socket_path, "GBA", "test.gba", status, sizeof(status)) == -1);
    expect_text(status, "启动失败：游戏已不存在");
    assert(jw_ipc_scan_library(socket_path, status, sizeof(status)) == 0);
    snprintf(expected, sizeof(expected), T("scan complete: %d games, %d apps"), 3, 2);
    expect_text(status, expected);
    assert(strcmp(status, "scan complete: 3 games, 2 apps"));
    int child_status;
    assert(waitpid(child, &child_status, 0) == child);
    assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    jw_ipc_server_close(server);
    free(state->pakrat_apps);
    free(state);
    jw_i18n_shutdown();
    cat_quit();
    rmdir(root);
    puts("i18n-ui-test: scrape, counts, titles and IPC translation passed");
    return 0;
}
