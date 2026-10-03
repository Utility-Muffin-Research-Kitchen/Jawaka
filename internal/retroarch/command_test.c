#include "internal/retroarch/command.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static void expect_supported(const char *command) {
    if (!jw_ra_raw_command_supported(command)) {
        fprintf(stderr, "retroarch-command-test: rejected supported command: %s\n",
                command);
        exit(1);
    }
}

static void expect_rejected(const char *command) {
    if (jw_ra_raw_command_supported(command)) {
        fprintf(stderr, "retroarch-command-test: accepted unsafe command: %s\n",
                command ? command : "(null)");
        exit(1);
    }
}

/* --------------------------------------------------------- shader commands */

static void expect_parse(const char *reply, const char *id, const char *op,
                         jw_ra_result want_result,
                         jw_ra_shader_outcome want_outcome) {
    jw_ra_shader_outcome outcome = (jw_ra_shader_outcome)-1;
    char path[512];
    jw_ra_result got = jw_ra_parse_shader_reply(reply, id, op, &outcome,
                                                path, sizeof(path));
    if (got != want_result) {
        fprintf(stderr, "retroarch-command-test: %s -> result %d, want %d\n",
                reply, (int)got, (int)want_result);
        exit(1);
    }
    if (want_result == JW_RA_OK && outcome != want_outcome) {
        fprintf(stderr, "retroarch-command-test: %s -> outcome %d, want %d\n",
                reply, (int)outcome, (int)want_outcome);
        exit(1);
    }
}

static void shader_reply_tests(void) {
    char path[512];
    jw_ra_shader_outcome outcome;

    /* Documented forms. */
    expect_parse("JAWAKA_SHADER a1 GET NONE", "a1", "GET",
                 JW_RA_OK, JW_RA_SHADER_NONE);
    expect_parse("JAWAKA_SHADER a1 SET OK", "a1", "SET",
                 JW_RA_OK, JW_RA_SHADER_OK);
    expect_parse("JAWAKA_SHADER a1 SET ERROR missing", "a1", "SET",
                 JW_RA_OK, JW_RA_SHADER_ERR_MISSING);
    expect_parse("JAWAKA_SHADER a1 SET ERROR unsupported", "a1", "SET",
                 JW_RA_OK, JW_RA_SHADER_ERR_UNSUPPORTED);
    expect_parse("JAWAKA_SHADER a1 SET ERROR apply", "a1", "SET",
                 JW_RA_OK, JW_RA_SHADER_ERR_APPLY);
    expect_parse("JAWAKA_SHADER a1 CLEAR OK", "a1", "CLEAR",
                 JW_RA_OK, JW_RA_SHADER_OK);
    expect_parse("JAWAKA_SHADER a1 SAVE GAME OK", "a1", "SAVE",
                 JW_RA_OK, JW_RA_SHADER_OK);
    expect_parse("JAWAKA_SHADER a1 SAVE GLOBAL ERROR", "a1", "SAVE",
                 JW_RA_OK, JW_RA_SHADER_ERR);
    expect_parse("JAWAKA_SHADER a1 REMOVE CORE ABSENT", "a1", "REMOVE",
                 JW_RA_OK, JW_RA_SHADER_ABSENT);

    /* A reply for somebody else's request is not an answer: it must be
     * ignorable, not a parse error and never an outcome. */
    expect_parse("JAWAKA_SHADER b2 SET OK", "a1", "SET", JW_RA_TIMEOUT, 0);
    /* A stale reply describing a different operation is malformed for this
     * exchange. */
    expect_parse("JAWAKA_SHADER a1 CLEAR OK", "a1", "SET", JW_RA_PARSE_ERROR, 0);

    /* Malformed replies are never partially believed. */
    expect_parse("", "a1", "SET", JW_RA_PARSE_ERROR, 0);
    expect_parse("SET OK", "a1", "SET", JW_RA_PARSE_ERROR, 0);
    expect_parse("JAWAKA_SHADER a1 SET MAYBE", "a1", "SET",
                 JW_RA_PARSE_ERROR, 0);
    expect_parse("JAWAKA_SHADER a1 SET ERROR wat", "a1", "SET",
                 JW_RA_PARSE_ERROR, 0);
    /* Our ID with nothing after it is a truncated reply to us. */
    expect_parse("JAWAKA_SHADER a1", "a1", "SET", JW_RA_PARSE_ERROR, 0);
    /* A longer ID that merely starts with ours is a different request, so it
     * must be ignored rather than treated as a malformed answer to ours. */
    expect_parse("JAWAKA_SHADER a12 SET OK", "a1", "SET", JW_RA_TIMEOUT, 0);

    /* GET OK carries a path, verbatim, spaces included. */
    outcome = (jw_ra_shader_outcome)-1;
    if (jw_ra_parse_shader_reply("JAWAKA_SHADER a1 GET OK /tmp/a b.glslp",
                                 "a1", "GET", &outcome, path,
                                 sizeof(path)) != JW_RA_OK ||
        strcmp(path, "/tmp/a b.glslp") != 0) {
        fprintf(stderr, "retroarch-command-test: GET path not parsed verbatim\n");
        exit(1);
    }

    /* A path at the buffer boundary is rejected rather than truncated. */
    {
        char small[8];
        outcome = (jw_ra_shader_outcome)-1;
        if (jw_ra_parse_shader_reply("JAWAKA_SHADER a1 GET OK /tmp/toolong.glslp",
                                     "a1", "GET", &outcome, small,
                                     sizeof(small)) != JW_RA_PARSE_ERROR) {
            fprintf(stderr, "retroarch-command-test: over-long path not rejected\n");
            exit(1);
        }
    }
}

static void shader_scope_tests(void) {
    if (strcmp(jw_ra_shader_scope_token(JW_RA_SHADER_SCOPE_GAME), "GAME") != 0 ||
        strcmp(jw_ra_shader_scope_token(JW_RA_SHADER_SCOPE_PARENT), "PARENT") != 0 ||
        strcmp(jw_ra_shader_scope_token(JW_RA_SHADER_SCOPE_CORE), "CORE") != 0 ||
        strcmp(jw_ra_shader_scope_token(JW_RA_SHADER_SCOPE_GLOBAL), "GLOBAL") != 0) {
        fprintf(stderr, "retroarch-command-test: scope token mismatch\n");
        exit(1);
    }
    if (jw_ra_shader_scope_token((jw_ra_shader_scope)99) != NULL) {
        fprintf(stderr, "retroarch-command-test: unknown scope accepted\n");
        exit(1);
    }
}

static void menu_status_tests(void) {
    jw_ra_status status;

    if (jw_ra_parse_status_reply(
            "GET_STATUS MENU mGBA,example.zip,crc32=0", &status) != JW_RA_OK ||
        status.state != JW_RA_STATE_MENU || strcmp(status.system, "mGBA") != 0 ||
        strcmp(status.content, "example.zip") != 0) {
        fprintf(stderr, "retroarch-command-test: menu status not parsed\n");
        exit(1);
    }
}

/* ------------------------------------------------ power-hold sync save */

static void check(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "retroarch-command-test: %s\n", what);
        exit(1);
    }
}

static void test_state_save_info_parse(void) {
    jw_ra_state_save_info info;
    check(jw_ra_parse_state_save_info_reply("GET_STATE_SAVE_INFO 1 4456472 0\n", &info) ==
              JW_RA_OK && info.supported && !info.compressed && info.bytes == 4456472ull,
          "info: plain reply");
    check(jw_ra_parse_state_save_info_reply("GET_STATE_SAVE_INFO 1 9068872 1", &info) ==
              JW_RA_OK && info.supported && info.compressed,
          "info: compressed flag");
    check(jw_ra_parse_state_save_info_reply("GET_STATE_SAVE_INFO 1 NO", &info) == JW_RA_OK &&
              !info.supported,
          "info: NO");
    check(jw_ra_parse_state_save_info_reply("GET_STATE_SAVE_INFO 2 100 0", &info) ==
              JW_RA_UNSUPPORTED && !info.supported,
          "info: unknown version must be unsupported");
    check(jw_ra_parse_state_save_info_reply("GET_STATE_SAVE_INFO 1 0 0", &info) ==
              JW_RA_PARSE_ERROR, "info: zero bytes");
    check(jw_ra_parse_state_save_info_reply("GET_STATE_SAVE_INFO 1 -5 0", &info) ==
              JW_RA_PARSE_ERROR, "info: negative bytes");
    check(jw_ra_parse_state_save_info_reply("GET_STATE_SAVE_INFO 1 5 2", &info) ==
              JW_RA_PARSE_ERROR, "info: bad compressed flag");
    check(jw_ra_parse_state_save_info_reply("GET_STATE_SAVE_INFO 1 5 0 x", &info) ==
              JW_RA_PARSE_ERROR, "info: trailing field");
    check(jw_ra_parse_state_save_info_reply("GET_INFO 0 0 0", &info) == JW_RA_PARSE_ERROR,
          "info: other command");
}

static void test_sync_save_parse(void) {
    jw_ra_sync_save_reply r;
    check(jw_ra_parse_sync_save_reply(
              "SAVE_STATE_SYNC ab12 TMP_READY 4456472 /c/States/PCSX-ReARMed/Spyro (USA).state99.tmp-ab12\n",
              "ab12", &r) == JW_RA_OK && r.ready && r.bytes == 4456472ull &&
              strcmp(r.tmp_path, "/c/States/PCSX-ReARMed/Spyro (USA).state99.tmp-ab12") == 0,
          "sync: TMP_READY with spaces in path");
    check(jw_ra_parse_sync_save_reply("SAVE_STATE_SYNC ab12 ERROR LATE", "ab12", &r) ==
              JW_RA_OK && !r.ready && strcmp(r.error, "LATE") == 0,
          "sync: ERROR code");
    check(jw_ra_parse_sync_save_reply("SAVE_STATE_SYNC ab123 ERROR LATE", "ab12", &r) ==
              JW_RA_TIMEOUT, "sync: longer id is someone else's");
    check(jw_ra_parse_sync_save_reply("SAVE_STATE_SYNC zz ERROR LATE", "ab12", &r) ==
              JW_RA_TIMEOUT, "sync: other id ignored");
    check(jw_ra_parse_sync_save_reply("SAVE_STATE_SYNC - ERROR BAD_ARGS", "ab12", &r) ==
              JW_RA_TIMEOUT, "sync: id-less BAD_ARGS is not ours");
    check(jw_ra_parse_sync_save_reply("GET_STATUS PLAYING x", "ab12", &r) == JW_RA_TIMEOUT,
          "sync: unrelated reply ignored");
    check(jw_ra_parse_sync_save_reply("SAVE_STATE_SYNC ab12 TMP_READY 0 /x", "ab12", &r) ==
              JW_RA_PARSE_ERROR, "sync: zero bytes");
    check(jw_ra_parse_sync_save_reply("SAVE_STATE_SYNC ab12 TMP_READY 12 rel/path", "ab12", &r) ==
              JW_RA_PARSE_ERROR && !r.ready, "sync: relative path");
    check(jw_ra_parse_sync_save_reply("SAVE_STATE_SYNC ab12 TMP_READY 12", "ab12", &r) ==
              JW_RA_PARSE_ERROR, "sync: missing path");
    check(jw_ra_parse_sync_save_reply("SAVE_STATE_SYNC ab12 ERROR late; rm", "ab12", &r) ==
              JW_RA_PARSE_ERROR, "sync: malformed error code");
    check(jw_ra_parse_sync_save_reply("SAVE_STATE_SYNC ab12 DONE", "ab12", &r) ==
              JW_RA_PARSE_ERROR, "sync: unknown verb");
}

/* A fake RetroArch on loopback: the real send/poll path, including discarding
   another request's reply and a datagram from a different source port. */
static void test_sync_save_loopback(void) {
    int server = socket(AF_INET, SOCK_DGRAM, 0);
    check(server >= 0, "loopback: server socket");
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check(bind(server, (struct sockaddr *)&addr, sizeof(addr)) == 0, "loopback: bind");
    socklen_t len = sizeof(addr);
    check(getsockname(server, (struct sockaddr *)&addr, &len) == 0, "loopback: name");
    jw_ra_client client = { "127.0.0.1", ntohs(addr.sin_port), 500 };

    jw_ra_sync_save save;
    jw_ra_sync_save_init(&save);
    jw_ra_sync_save_reply reply;
    check(jw_ra_sync_save_poll(&save, &reply) == JW_RA_SOCKET_ERROR, "loopback: poll closed");
    check(jw_ra_sync_save_send(&client, &save, 99, 5242880ull, 123456) == JW_RA_OK,
          "loopback: send");
    check(strlen(save.request_id) > 0, "loopback: request id");

    char buf[256];
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    ssize_t n = recvfrom(server, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&peer, &peer_len);
    check(n > 0, "loopback: server received");
    buf[n] = '\0';
    char want[128];
    snprintf(want, sizeof(want), "SAVE_STATE_SYNC %s 99 5242880 123456", save.request_id);
    check(strcmp(buf, want) == 0, "loopback: exact command line");

    check(jw_ra_sync_save_poll(&save, &reply) == JW_RA_TIMEOUT, "loopback: nothing yet");

    /* Same source, other id: discarded. */
    const char *other = "SAVE_STATE_SYNC ffff ERROR LATE";
    sendto(server, other, strlen(other), 0, (struct sockaddr *)&peer, peer_len);
    /* Different source port, right id: the connected socket never sees it. */
    int spoof = socket(AF_INET, SOCK_DGRAM, 0);
    char spoofed[128];
    snprintf(spoofed, sizeof(spoofed), "SAVE_STATE_SYNC %s TMP_READY 1 /evil", save.request_id);
    sendto(spoof, spoofed, strlen(spoofed), 0, (struct sockaddr *)&peer, peer_len);
    close(spoof);
    usleep(20000);
    check(jw_ra_sync_save_poll(&save, &reply) == JW_RA_TIMEOUT,
          "loopback: other id and other source ignored");

    char good[256];
    snprintf(good, sizeof(good),
             "SAVE_STATE_SYNC %s TMP_READY 4456472 /c/States/Core/Game.state99.tmp-%s",
             save.request_id, save.request_id);
    sendto(server, good, strlen(good), 0, (struct sockaddr *)&peer, peer_len);
    usleep(20000);
    check(jw_ra_sync_save_poll(&save, &reply) == JW_RA_OK && reply.ready &&
              reply.bytes == 4456472ull && strstr(reply.tmp_path, ".state99.tmp-"),
          "loopback: matching TMP_READY");
    jw_ra_sync_save_close(&save);
    check(save.fd == -1, "loopback: closed");

    /* Probe: answered, then an older RetroArch that stays silent. */
    jw_ra_state_save_info info;
    int pid = fork();
    check(pid >= 0, "loopback: fork");
    if (pid == 0) {
        peer_len = sizeof(peer);
        n = recvfrom(server, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&peer, &peer_len);
        const char *answer = "GET_STATE_SAVE_INFO 1 823432 0";
        if (n > 0) sendto(server, answer, strlen(answer), 0, (struct sockaddr *)&peer, peer_len);
        _exit(0);
    }
    check(jw_ra_get_state_save_info(&client, &info) == JW_RA_OK && info.supported &&
              info.bytes == 823432ull, "loopback: probe answered");
    waitpid(pid, NULL, 0);
    client.timeout_ms = 100;
    check(jw_ra_get_state_save_info(&client, &info) == JW_RA_TIMEOUT && !info.supported,
          "loopback: silent RetroArch times out unsupported");
    close(server);
}

int main(void) {
    test_state_save_info_parse();
    test_sync_save_parse();
    test_sync_save_loopback();
    expect_supported("SET_SHADER /tmp/example.glslp");
    expect_supported("SET_SHADER\t/tmp/example.glslp");
    expect_supported("GET_CONFIG_PARAM video_shader");
    expect_supported("GET_PERF_INFO");
    expect_supported("OPEN_MENU SHADERS");
    expect_supported("SCREENSHOT");

    expect_rejected(NULL);
    expect_rejected("");
    expect_rejected("SET_SHADER_PATH /tmp/example.glslp");
    expect_rejected("SET_SHADER\nQUIT");

    expect_supported("JAWAKA_GET_SHADER a1");
    expect_supported("JAWAKA_SET_SHADER a1 /tmp/example.glslp");
    expect_supported("JAWAKA_CLEAR_SHADER a1");
    expect_supported("JAWAKA_SAVE_SHADER_PRESET a1 GAME");
    expect_supported("JAWAKA_REMOVE_SHADER_PRESET a1 GLOBAL");
    expect_rejected("JAWAKA_SET_SHADER a1 /tmp/a.glslp\nQUIT");

    shader_reply_tests();
    shader_scope_tests();
    menu_status_tests();

    puts("PASS retroarch-command-test");
    return 0;
}
