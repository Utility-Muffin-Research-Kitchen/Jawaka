#include "internal/services/log_redact.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void jw__test_line_with_no_secret_passes_through(void) {
    char out[128];
    bool redacted = jw_svc_log_redact_line("level=info msg=hello world", out, sizeof(out));
    assert(!redacted);
    assert(strcmp(out, "level=info msg=hello world") == 0);

    puts("PASS log-redact-test a line with no recognized secret passes through unchanged");
}

static void jw__test_logfmt_key_value_is_redacted(void) {
    char out[128];
    bool redacted = jw_svc_log_redact_line("level=info token=abc123 msg=ok", out, sizeof(out));
    assert(redacted);
    assert(strcmp(out, "level=info token=[REDACTED] msg=ok") == 0);

    puts("PASS log-redact-test a logfmt-style token= pair is redacted, siblings are untouched");
}

static void jw__test_header_style_redacts_to_end_of_line(void) {
    char out[128];
    bool redacted = jw_svc_log_redact_line("Authorization: Bearer xyz.abc.def", out, sizeof(out));
    assert(redacted);
    assert(strcmp(out, "Authorization: [REDACTED]") == 0);

    puts("PASS log-redact-test a header-style Authorization value is redacted to end of line");
}

static void jw__test_cookie_header_hides_embedded_pairs(void) {
    char out[128];
    bool redacted = jw_svc_log_redact_line("Cookie: session=abc123; other=def456", out, sizeof(out));
    assert(redacted);
    assert(strcmp(out, "Cookie: [REDACTED]") == 0);

    puts("PASS log-redact-test a Cookie header redacts its embedded key=value pairs too");
}

static void jw__test_quoted_value_is_redacted_whole(void) {
    char out[128];
    bool redacted = jw_svc_log_redact_line("password=\"hello world\" next=1", out, sizeof(out));
    assert(redacted);
    assert(strcmp(out, "password=[REDACTED] next=1") == 0);

    puts("PASS log-redact-test a quoted value with embedded spaces is redacted as one unit");
}

static void jw__test_unterminated_quote_redacts_to_end_of_line(void) {
    char out[128];
    bool redacted = jw_svc_log_redact_line("secret=\"never closes", out, sizeof(out));
    assert(redacted);
    assert(strcmp(out, "secret=[REDACTED]") == 0);

    puts("PASS log-redact-test an unterminated quoted value redacts to end of line");
}

static void jw__test_key_match_is_case_insensitive(void) {
    char out[128];
    bool redacted = jw_svc_log_redact_line("PASSWORD=hunter2", out, sizeof(out));
    assert(redacted);
    assert(strcmp(out, "PASSWORD=[REDACTED]") == 0);

    puts("PASS log-redact-test the sensitive-key match is case-insensitive");
}

static void jw__test_pin_and_secret_and_apikey(void) {
    char out[128];
    assert(jw_svc_log_redact_line("pin=4321", out, sizeof(out)));
    assert(strcmp(out, "pin=[REDACTED]") == 0);

    assert(jw_svc_log_redact_line("secret=topsecret", out, sizeof(out)));
    assert(strcmp(out, "secret=[REDACTED]") == 0);

    assert(jw_svc_log_redact_line("api_key=sk_live_abcdef", out, sizeof(out)));
    assert(strcmp(out, "api_key=[REDACTED]") == 0);

    puts("PASS log-redact-test pin, secret, and api_key keys are all recognized");
}

static void jw__test_multiple_sensitive_pairs_all_redacted(void) {
    char out[128];
    bool redacted = jw_svc_log_redact_line("token=aaa user=bob pwd=bbb", out, sizeof(out));
    assert(redacted);
    assert(strcmp(out, "token=[REDACTED] user=bob pwd=[REDACTED]") == 0);

    puts("PASS log-redact-test multiple sensitive pairs on one line are all redacted");
}

static void jw__test_crypt_hash_marker_redacts_whole_line(void) {
    char out[128];
    bool redacted = jw_svc_log_redact_line(
        "user root hash $6$abcxyz$therealhashvaluehere==", out, sizeof(out));
    assert(redacted);
    assert(strcmp(out, "[REDACTED LINE]") == 0);

    puts("PASS log-redact-test a crypt hash marker redacts the entire line");
}

static void jw__test_shadow_entry_redacts_whole_line(void) {
    char out[128];
    bool redacted = jw_svc_log_redact_line(
        "root:$6$abcd$longhashvalue:19000:0:99999:7:::", out, sizeof(out));
    assert(redacted);
    assert(strcmp(out, "[REDACTED LINE]") == 0);

    puts("PASS log-redact-test a shadow-file-shaped entry redacts via its crypt hash marker");
}

static void jw__test_private_key_marker_redacts_whole_line(void) {
    char out[128];
    bool redacted = jw_svc_log_redact_line(
        "-----BEGIN RSA PRIVATE KEY-----", out, sizeof(out));
    assert(redacted);
    assert(strcmp(out, "[REDACTED LINE]") == 0);

    puts("PASS log-redact-test a PEM private-key marker line redacts the entire line");
}

static void jw__test_truncation_still_reports_redacted(void) {
    char out[8];
    /* The secret's placeholder cannot possibly fit in 8 bytes, but the
     * boolean must still reflect that the full input line contained
     * one. */
    bool redacted = jw_svc_log_redact_line("token=abcdefghijklmnop", out, sizeof(out));
    assert(redacted);
    assert(strlen(out) < sizeof(out));

    puts("PASS log-redact-test the redacted flag is correct even when output is truncated");
}

static void jw__test_invalid_arguments(void) {
    char out[16];
    assert(!jw_svc_log_redact_line(NULL, out, sizeof(out)));
    assert(!jw_svc_log_redact_line("token=abc", NULL, sizeof(out)));
    assert(!jw_svc_log_redact_line("token=abc", out, 0));

    puts("PASS log-redact-test NULL line/out or a zero out_size is handled safely");
}

static void jw__test_empty_line_and_bare_delimiters(void) {
    char out[32];
    assert(!jw_svc_log_redact_line("", out, sizeof(out)));
    assert(strcmp(out, "") == 0);

    assert(!jw_svc_log_redact_line("=", out, sizeof(out)));
    assert(strcmp(out, "=") == 0);

    assert(!jw_svc_log_redact_line(":", out, sizeof(out)));
    assert(strcmp(out, ":") == 0);

    puts("PASS log-redact-test an empty line and bare delimiters with no key are untouched");
}

int main(void) {
    jw__test_line_with_no_secret_passes_through();
    jw__test_logfmt_key_value_is_redacted();
    jw__test_header_style_redacts_to_end_of_line();
    jw__test_cookie_header_hides_embedded_pairs();
    jw__test_quoted_value_is_redacted_whole();
    jw__test_unterminated_quote_redacts_to_end_of_line();
    jw__test_key_match_is_case_insensitive();
    jw__test_pin_and_secret_and_apikey();
    jw__test_multiple_sensitive_pairs_all_redacted();
    jw__test_crypt_hash_marker_redacts_whole_line();
    jw__test_shadow_entry_redacts_whole_line();
    jw__test_private_key_marker_redacts_whole_line();
    jw__test_truncation_still_reports_redacted();
    jw__test_invalid_arguments();
    jw__test_empty_line_and_bare_delimiters();
    puts("PASS log-redact-test");
    return 0;
}
