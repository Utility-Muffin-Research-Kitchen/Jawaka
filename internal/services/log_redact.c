#include "internal/services/log_redact.h"

#include <ctype.h>
#include <string.h>

#define JW__REDACT_PLACEHOLDER "[REDACTED]"
#define JW__REDACT_LINE_PLACEHOLDER "[REDACTED LINE]"
#define JW__REDACT_KEY_MAX 20

static const char *const JW__REDACT_SENSITIVE_KEYS[] = {
    "api_key", "apikey", "api-key", "token", "access_token",
    "refresh_token", "auth", "authorization", "secret", "password",
    "passwd", "pwd", "pin", "cookie", "set-cookie", "private_key",
    "priv_key",
};
#define JW__REDACT_SENSITIVE_KEY_COUNT \
    (sizeof(JW__REDACT_SENSITIVE_KEYS) / sizeof(JW__REDACT_SENSITIVE_KEYS[0]))

static const char *const JW__REDACT_HASH_PREFIXES[] = {
    "$1$", "$2a$", "$2b$", "$2x$", "$2y$", "$5$", "$6$",
};
#define JW__REDACT_HASH_PREFIX_COUNT \
    (sizeof(JW__REDACT_HASH_PREFIXES) / sizeof(JW__REDACT_HASH_PREFIXES[0]))

static bool jw__redact_is_key_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '-';
}

static bool jw__redact_key_matches(const char *key, size_t key_len) {
    if (key_len == 0 || key_len > JW__REDACT_KEY_MAX) {
        return false;
    }
    char lowered[JW__REDACT_KEY_MAX + 1];
    for (size_t i = 0; i < key_len; i++) {
        lowered[i] = (char)tolower((unsigned char)key[i]);
    }
    lowered[key_len] = '\0';
    for (size_t i = 0; i < JW__REDACT_SENSITIVE_KEY_COUNT; i++) {
        if (strcmp(lowered, JW__REDACT_SENSITIVE_KEYS[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool jw__redact_has_marker(const char *line) {
    for (size_t i = 0; i < JW__REDACT_HASH_PREFIX_COUNT; i++) {
        if (strstr(line, JW__REDACT_HASH_PREFIXES[i]) != NULL) {
            return true;
        }
    }
    return strstr(line, "PRIVATE KEY") != NULL;
}

static void jw__redact_append(char *out, size_t out_size, size_t *out_pos,
                               const char *text, size_t text_len) {
    if (*out_pos >= out_size - 1) {
        return;
    }
    size_t space = (out_size - 1) - *out_pos;
    size_t n = text_len < space ? text_len : space;
    memcpy(out + *out_pos, text, n);
    *out_pos += n;
}

static void jw__redact_append_str(char *out, size_t out_size, size_t *out_pos,
                                   const char *text) {
    jw__redact_append(out, out_size, out_pos, text, strlen(text));
}

/* Finds the end of the value starting at `value_start` for an '='
 * delimiter: a quoted span up to (and including) its closing quote, or
 * up to end of line if unterminated; otherwise up to the next
 * whitespace character or end of line. */
static size_t jw__redact_equals_value_end(const char *line, size_t len, size_t value_start) {
    if (value_start < len && (line[value_start] == '"' || line[value_start] == '\'')) {
        char quote = line[value_start];
        for (size_t i = value_start + 1; i < len; i++) {
            if (line[i] == quote) {
                return i + 1;
            }
        }
        return len;
    }
    size_t i = value_start;
    while (i < len && line[i] != ' ' && line[i] != '\t') {
        i++;
    }
    return i;
}

bool jw_svc_log_redact_line(const char *line, char *out, size_t out_size) {
    if (!line || !out || out_size == 0) {
        return false;
    }

    if (jw__redact_has_marker(line)) {
        size_t out_pos = 0;
        jw__redact_append_str(out, out_size, &out_pos, JW__REDACT_LINE_PLACEHOLDER);
        out[out_pos] = '\0';
        return true;
    }

    size_t len = strlen(line);
    bool redacted = false;
    size_t out_pos = 0;
    size_t cursor = 0;
    size_t scan = 0;

    while (scan < len) {
        if (line[scan] != ':' && line[scan] != '=') {
            scan++;
            continue;
        }

        size_t delim_pos = scan;
        size_t key_end = delim_pos;
        size_t key_start = key_end;
        while (key_start > 0 && jw__redact_is_key_char(line[key_start - 1])) {
            key_start--;
        }
        size_t key_len = key_end - key_start;

        if (!jw__redact_key_matches(line + key_start, key_len)) {
            scan = delim_pos + 1;
            continue;
        }

        /* Emit everything up to and including the delimiter unchanged,
         * then the placeholder in place of the value. A single space
         * after ':' matches the header-style "key: value" convention;
         * '=' gets none, matching logfmt's "key=value" convention. */
        jw__redact_append(out, out_size, &out_pos, line + cursor, (delim_pos + 1) - cursor);
        if (line[delim_pos] == ':') {
            jw__redact_append_str(out, out_size, &out_pos, " ");
        }
        jw__redact_append_str(out, out_size, &out_pos, JW__REDACT_PLACEHOLDER);
        redacted = true;

        size_t value_start = delim_pos + 1;
        while (value_start < len && line[value_start] == ' ') {
            value_start++;
        }

        if (line[delim_pos] == ':') {
            /* Header-style convention: one value per line. Nothing more
             * on this line is worth scanning. */
            cursor = len;
            break;
        }

        size_t value_end = jw__redact_equals_value_end(line, len, value_start);
        cursor = value_end;
        scan = value_end;
    }

    jw__redact_append(out, out_size, &out_pos, line + cursor, len - cursor);
    out[out_pos] = '\0';
    return redacted;
}
