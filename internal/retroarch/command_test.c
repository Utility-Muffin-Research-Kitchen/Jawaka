#include "internal/retroarch/command.h"

#include <stdio.h>
#include <stdlib.h>

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

int main(void) {
    expect_supported("SET_SHADER /tmp/example.glslp");
    expect_supported("SET_SHADER\t/tmp/example.glslp");
    expect_supported("GET_CONFIG_PARAM video_shader");
    expect_supported("GET_PERF_INFO");
    expect_supported("SCREENSHOT");

    expect_rejected(NULL);
    expect_rejected("");
    expect_rejected("SET_SHADER_PATH /tmp/example.glslp");
    expect_rejected("SET_SHADER\nQUIT");

    puts("PASS retroarch-command-test");
    return 0;
}
