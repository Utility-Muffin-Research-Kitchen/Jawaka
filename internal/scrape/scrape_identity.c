#include "internal/scrape/scrape_identity.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static bool jw_scrape__descriptor(const char *path, char out[129]) {
    out[0] = '\0';
    FILE *file = path ? fopen(path, "rb") : NULL;
    if (!file) {
        return false;
    }
    unsigned char bytes[257];
    size_t count = fread(bytes, 1u, sizeof(bytes), file);
    bool read_error = ferror(file) != 0;
    fclose(file);
    if (read_error || count == 0u || count > 256u) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        unsigned char c = bytes[i];
        if (c == 0u || (c < 0x20u && c != '\t' && c != '\r' && c != '\n') ||
            c >= 0x7fu) {
            return false;
        }
    }

    size_t line_end = count;
    size_t trailing = count;
    for (size_t i = 0; i < count; i++) {
        if (bytes[i] == '\n') {
            line_end = i;
            trailing = i + 1u;
            break;
        }
        if (bytes[i] == '\r') {
            if (i + 1u >= count || bytes[i + 1u] != '\n') {
                return false;
            }
            line_end = i;
            trailing = i + 2u;
            break;
        }
    }
    for (size_t i = trailing; i < count; i++) {
        if (bytes[i] == ' ' || bytes[i] == '\t' || bytes[i] == '\n') {
            continue;
        }
        if (bytes[i] == '\r' && i + 1u < count && bytes[i + 1u] == '\n') {
            i++;
            continue;
        }
        return false;
    }

    size_t start = 0;
    while (start < line_end &&
           (bytes[start] == ' ' || bytes[start] == '\t')) {
        start++;
    }
    while (line_end > start &&
           (bytes[line_end - 1u] == ' ' || bytes[line_end - 1u] == '\t')) {
        line_end--;
    }
    size_t length = line_end - start;
    if (length < 1u || length > 128u) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        unsigned char c = bytes[start + i];
        bool alphanumeric = (c >= 'A' && c <= 'Z') ||
                            (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9');
        if (!alphanumeric &&
            (i == 0u || (c != '_' && c != '.' && c != ':' && c != '-'))) {
            return false;
        }
    }
    memcpy(out, bytes + start, length);
    out[length] = '\0';
    return true;
}

static void jw_scrape__add(jw_scrape_identity_candidates *out,
                           const char *name) {
    if (!name || !name[0] || out->count >= JW_SCRAPE_IDENTITY_CANDIDATE_MAX) {
        return;
    }
    for (size_t i = 0; i < out->count; i++) {
        if (strcasecmp(out->names[i], name) == 0) {
            return;
        }
    }
    size_t length = strlen(name);
    if (length >= JW_SCRAPE_IDENTITY_NAME_MAX) {
        return;
    }
    memcpy(out->names[out->count++], name, length + 1u);
}

static void jw_scrape__add_with_extension(
    jw_scrape_identity_candidates *out,
    const char *name,
    const char *extension,
    bool keep_existing_extension) {
    if (!name || !name[0] || !extension || !extension[0]) {
        return;
    }
    size_t name_len = strlen(name);
    size_t extension_len = strlen(extension);
    if (keep_existing_extension && name_len > extension_len &&
        name[name_len - extension_len - 1u] == '.' &&
        strcasecmp(name + name_len - extension_len, extension) == 0) {
        jw_scrape__add(out, name);
        return;
    }
    char candidate[JW_SCRAPE_IDENTITY_NAME_MAX];
    int written = snprintf(candidate, sizeof(candidate), "%s.%s", name, extension);
    if (written > 0 && (size_t)written < sizeof(candidate)) {
        jw_scrape__add(out, candidate);
    }
}

void jw_scrape_identity_build(const char *descriptor_path,
                              const char *rom_name,
                              const char *effective_title,
                              const char *lookup_extension,
                              jw_scrape_identity_candidates *out) {
    memset(out, 0, sizeof(*out));
    char descriptor[129];
    if (jw_scrape__descriptor(descriptor_path, descriptor)) {
        jw_scrape__add_with_extension(out, descriptor, lookup_extension, false);
    }
    jw_scrape__add(out, rom_name);
    jw_scrape__add_with_extension(out, effective_title, lookup_extension, true);
}

static bool jw_scrape__is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool jw_scrape__is_letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* True when token is a whole version: "v2", "V1_20", "ver1.2", "1.1",
   "0.123c". Without a "v" it needs a dotted number, so "2" and "2D" are not
   versions. */
static bool jw_scrape__is_version(const char *token) {
    const char *p = token;
    bool prefixed = false;
    static const char *const prefixes[] = { "version", "ver", "v" };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t len = strlen(prefixes[i]);
        if (strncasecmp(p, prefixes[i], len) == 0 && jw_scrape__is_digit(p[len])) {
            p += len;
            prefixed = true;
            break;
        }
    }
    size_t groups = 0;
    while (jw_scrape__is_digit(*p)) {
        while (jw_scrape__is_digit(*p)) p++;
        groups++;
        if ((*p == '.' || *p == '_') && jw_scrape__is_digit(p[1])) {
            p++;
        }
    }
    if (groups == 0 || (!prefixed && groups < 2)) {
        return false;
    }
    if (jw_scrape__is_letter(*p)) {
        p++;
    }
    return *p == '\0';
}

static bool jw_scrape__is_separator(char c) {
    return c == ' ' || c == '_' || c == '-';
}

void jw_scrape_identity_build_folder(const char *folder_name,
                                     jw_scrape_identity_candidates *out) {
    memset(out, 0, sizeof(*out));
    jw_scrape__add(out, folder_name);
    if (!folder_name) {
        return;
    }
    size_t len = strlen(folder_name);
    if (len >= JW_SCRAPE_IDENTITY_NAME_MAX) {
        return;
    }
    /* The version can hold a separator itself ("V1_20"), so try every
       separator from the right until the rest is one whole version. */
    for (size_t i = len; i-- > 1;) {
        if (!jw_scrape__is_separator(folder_name[i]) ||
            !jw_scrape__is_version(folder_name + i + 1)) {
            continue;
        }
        size_t keep = i;
        while (keep > 0 && jw_scrape__is_separator(folder_name[keep - 1])) {
            keep--;
        }
        if (keep > 0) {
            char stripped[JW_SCRAPE_IDENTITY_NAME_MAX];
            memcpy(stripped, folder_name, keep);
            stripped[keep] = '\0';
            jw_scrape__add(out, stripped);
        }
        return;
    }
}

void jw_scrape_identity_build_filename(const char *rom_name,
                                       jw_scrape_identity_candidates *out) {
    memset(out, 0, sizeof(*out));
    jw_scrape__add(out, rom_name);
}

bool jw_scrape_identity_add_manual_name(jw_scrape_identity_candidates *names,
                                        const char *manual_title,
                                        const char *extension) {
    if (!names) {
        return false;
    }
    const char *start = manual_title ? manual_title : "";
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }
    size_t length = strlen(start);
    while (length > 0u && (start[length - 1u] == ' ' || start[length - 1u] == '\t' ||
                           start[length - 1u] == '\r' || start[length - 1u] == '\n')) {
        length--;
    }
    char title[JW_SCRAPE_IDENTITY_NAME_MAX];
    if (length == 0u || length >= sizeof(title)) {
        return false;
    }
    memcpy(title, start, length);
    title[length] = '\0';

    /* The add helpers skip a name equal to one already in the list. */
    size_t sent = names->count;
    if (extension && extension[0]) {
        jw_scrape__add_with_extension(names, title, extension, true);
    } else {
        jw_scrape__add(names, title);
    }
    return names->count > sent;
}
