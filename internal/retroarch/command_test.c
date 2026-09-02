#include "internal/retroarch/command.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(void) {
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
