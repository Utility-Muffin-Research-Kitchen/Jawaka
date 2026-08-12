#include "internal/i18n/i18n.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal/core/log.h"

/* Compiled table layout, written by tools/i18n-compile.py. One read, one
 * allocation, no per-lookup allocation and no filesystem access after load --
 * this sits on the render path, and the Display & Sound lag was a lesson in
 * what per-frame work costs there.
 *
 *   magic   "JWI18N\0\0"           8 bytes
 *   u32     version                1
 *   u32     count                  entries
 *   u32     pool_size              bytes of string pool
 *   u32     reserved               0
 *   entry[count]                   { u32 hash; u32 key_off; u32 val_off; }
 *                                  sorted by hash, then by key
 *   pool[pool_size]                NUL-terminated UTF-8
 *
 * All integers little-endian. Both the build host and every supported device
 * are little-endian; the loader checks the magic and every offset rather than
 * trusting the file, so a mismatched or truncated table is rejected, not
 * misread.
 */

#define JW_I18N_MAGIC   "JWI18N\0\0"
#define JW_I18N_VERSION 1u
#define JW_I18N_HEADER  24u

typedef struct {
    uint32_t hash;
    uint32_t key_off;
    uint32_t val_off;
} jw__i18n_entry;

static struct {
    unsigned char  *blob;        /* whole file, owns the pool */
    size_t          blob_size;
    const jw__i18n_entry *entries;
    uint32_t        count;
    const char     *pool;
    uint32_t        pool_size;
    char            lang[16];
} g_i18n = { NULL, 0, NULL, 0, NULL, 0, "en" };

/* FNV-1a. Chosen for being three lines and dependency-free; the table is a few
 * thousand entries, so distribution matters more than speed and collisions are
 * resolved by strcmp anyway. */
static uint32_t jw__i18n_hash(const char *s) {
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= (uint32_t)*p;
        h *= 16777619u;
    }
    return h;
}

static uint32_t jw__i18n_rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* A key may carry a "context|" prefix so identical English can translate two
 * ways. Display strips it, so an untranslated T("verb|Open") still shows
 * "Open" rather than leaking the disambiguator to the user. Only a prefix
 * without spaces counts, so a literal pipe in real UI text is left alone. */
static const char *jw__i18n_strip_context(const char *key) {
    const char *bar = strchr(key, '|');
    if (!bar || bar == key) return key;
    for (const char *p = key; p < bar; p++) {
        if (*p == ' ' || *p == '\t') return key;
    }
    return bar + 1;
}

static void jw__i18n_reset(void) {
    free(g_i18n.blob);
    g_i18n.blob      = NULL;
    g_i18n.blob_size = 0;
    g_i18n.entries   = NULL;
    g_i18n.count     = 0;
    g_i18n.pool      = NULL;
    g_i18n.pool_size = 0;
    snprintf(g_i18n.lang, sizeof(g_i18n.lang), "%s", "en");
}

static bool jw__i18n_path(char *out, size_t out_size, const char *env,
                          const char *fallback, const char *lang,
                          const char *ext) {
    const char *root = getenv(env);
    if ((!root || !root[0]) && fallback) root = fallback;
    if (!root || !root[0]) return false;
    return snprintf(out, out_size, "%s/i18n/%s.%s", root, lang, ext) < (int)out_size;
}

/* ── Compiled table ──────────────────────────────────────────────────────── */

static bool jw__i18n_load_compiled(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return false; }
    long size = ftell(fp);
    if (size < (long)JW_I18N_HEADER || size > 64L * 1024 * 1024) {
        fclose(fp);
        return false;
    }
    rewind(fp);

    unsigned char *blob = (unsigned char *)malloc((size_t)size);
    if (!blob) { fclose(fp); return false; }
    size_t got = fread(blob, 1, (size_t)size, fp);
    fclose(fp);
    if (got != (size_t)size) { free(blob); return false; }

    if (memcmp(blob, JW_I18N_MAGIC, 8) != 0 ||
        jw__i18n_rd32(blob + 8) != JW_I18N_VERSION) {
        jw_log_warn("i18n: %s is not a v%u table; ignoring", path, JW_I18N_VERSION);
        free(blob);
        return false;
    }

    uint32_t count     = jw__i18n_rd32(blob + 12);
    uint32_t pool_size = jw__i18n_rd32(blob + 16);

    /* Validate the geometry before trusting a single offset. A truncated table
       must be rejected here, not discovered by reading past the buffer on some
       later lookup. */
    uint64_t need = (uint64_t)JW_I18N_HEADER +
                    (uint64_t)count * sizeof(jw__i18n_entry) + pool_size;
    if (need != (uint64_t)size) {
        jw_log_warn("i18n: %s has inconsistent size; ignoring", path);
        free(blob);
        return false;
    }
    const char *pool = (const char *)(blob + JW_I18N_HEADER +
                                      (size_t)count * sizeof(jw__i18n_entry));
    if (pool_size == 0 || pool[pool_size - 1] != '\0') {
        jw_log_warn("i18n: %s pool is not NUL-terminated; ignoring", path);
        free(blob);
        return false;
    }

    const unsigned char *raw = blob + JW_I18N_HEADER;
    jw__i18n_entry *entries  = (jw__i18n_entry *)raw;
    for (uint32_t i = 0; i < count; i++) {
        const unsigned char *e = raw + (size_t)i * sizeof(jw__i18n_entry);
        uint32_t h = jw__i18n_rd32(e);
        uint32_t k = jw__i18n_rd32(e + 4);
        uint32_t v = jw__i18n_rd32(e + 8);
        if (k >= pool_size || v >= pool_size) {
            jw_log_warn("i18n: %s entry %u points outside the pool; ignoring", path, i);
            free(blob);
            return false;
        }
        entries[i].hash = h;
        entries[i].key_off = k;
        entries[i].val_off = v;
    }

    g_i18n.blob      = blob;
    g_i18n.blob_size = (size_t)size;
    g_i18n.entries   = entries;
    g_i18n.count     = count;
    g_i18n.pool      = pool;
    g_i18n.pool_size = pool_size;
    return true;
}

/* ── Live TSV override ───────────────────────────────────────────────────── */

/* Builds the same in-memory shape as the compiled table so lookup has one code
 * path. Format is "english<TAB>translation", # comments, blank lines skipped --
 * what a translator exports from a spreadsheet. Deliberately forgiving: a bad
 * line is dropped rather than failing the file, because this is a hand-edited
 * file on an SD card and losing the whole translation to one stray tab would be
 * a miserable way to find out. */
static int jw__i18n_entry_cmp(const void *a, const void *b) {
    const jw__i18n_entry *x = (const jw__i18n_entry *)a;
    const jw__i18n_entry *y = (const jw__i18n_entry *)b;
    if (x->hash < y->hash) return -1;
    if (x->hash > y->hash) return 1;
    return 0;
}

static bool jw__i18n_load_tsv(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return false; }
    long size = ftell(fp);
    if (size <= 0 || size > 16L * 1024 * 1024) { fclose(fp); return false; }
    rewind(fp);

    /* One allocation holds the entry array and the pool: the pool is the file
       text itself, rewritten in place with NULs at the tabs and newlines. */
    size_t cap = 256;
    jw__i18n_entry *entries = (jw__i18n_entry *)malloc(cap * sizeof(*entries));
    char *text = (char *)malloc((size_t)size + 1);
    if (!entries || !text) { free(entries); free(text); fclose(fp); return false; }

    size_t got = fread(text, 1, (size_t)size, fp);
    fclose(fp);
    text[got] = '\0';

    uint32_t count = 0;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r') line[--len] = '\0';
        if (!len || line[0] == '#') continue;

        char *tab = strchr(line, '\t');
        if (!tab || tab == line) continue;        /* no key or no separator */
        *tab = '\0';
        char *val = tab + 1;
        if (!val[0]) continue;                    /* untranslated, leave English */

        if (count == cap) {
            size_t ncap = cap * 2;
            jw__i18n_entry *grown =
                (jw__i18n_entry *)realloc(entries, ncap * sizeof(*entries));
            if (!grown) { free(entries); free(text); return false; }
            entries = grown;
            cap = ncap;
        }
        entries[count].hash    = jw__i18n_hash(line);
        entries[count].key_off = (uint32_t)(line - text);
        entries[count].val_off = (uint32_t)(val - text);
        count++;
    }

    if (count == 0) { free(entries); free(text); return false; }
    qsort(entries, count, sizeof(*entries), jw__i18n_entry_cmp);

    g_i18n.blob      = (unsigned char *)text;
    g_i18n.blob_size = got + 1;
    g_i18n.entries   = entries;
    g_i18n.count     = count;
    g_i18n.pool      = text;
    g_i18n.pool_size = (uint32_t)(got + 1);
    return true;
}

/* The TSV path allocates its entry array separately from the pool; the
 * compiled path carves both out of one blob. Tracking which is which keeps
 * shutdown from freeing an interior pointer. */
static bool g_i18n_entries_owned = false;

/* ── Public API ──────────────────────────────────────────────────────────── */

bool jw_i18n_load(const char *lang) {
    jw_i18n_shutdown();

    if (!lang || !lang[0] || strcmp(lang, "en") == 0) return false;

    char path[PATH_MAX];

    if (jw__i18n_path(path, sizeof(path), "UMRK_INTERNAL_DATA_PATH", NULL,
                      lang, "tsv") &&
        jw__i18n_load_tsv(path)) {
        g_i18n_entries_owned = true;
        snprintf(g_i18n.lang, sizeof(g_i18n.lang), "%s", lang);
        jw_log_info("i18n: %s from live override %s (%u entries)",
                    lang, path, g_i18n.count);
        return true;
    }

    if (jw__i18n_path(path, sizeof(path), "UMRK_PLATFORM_PATH", NULL,
                      lang, "jwi") &&
        jw__i18n_load_compiled(path)) {
        g_i18n_entries_owned = false;
        snprintf(g_i18n.lang, sizeof(g_i18n.lang), "%s", lang);
        jw_log_info("i18n: %s (%u entries)", lang, g_i18n.count);
        return true;
    }

    jw_log_warn("i18n: no table for %s; falling back to English", lang);
    return false;
}

void jw_i18n_shutdown(void) {
    if (g_i18n_entries_owned) free((void *)g_i18n.entries);
    g_i18n_entries_owned = false;
    jw__i18n_reset();
}

const char *jw_i18n(const char *english) {
    if (!english) return NULL;
    if (g_i18n.count == 0) return jw__i18n_strip_context(english);

    uint32_t h = jw__i18n_hash(english);

    /* Binary search to the first entry with this hash, then walk equals and
       strcmp. Collisions are rare but must not silently return the wrong
       string, which is exactly the sort of bug nobody who reads the language
       would ever report. */
    uint32_t lo = 0, hi = g_i18n.count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (g_i18n.entries[mid].hash < h) lo = mid + 1;
        else hi = mid;
    }
    for (uint32_t i = lo; i < g_i18n.count && g_i18n.entries[i].hash == h; i++) {
        const char *key = g_i18n.pool + g_i18n.entries[i].key_off;
        if (strcmp(key, english) == 0)
            return g_i18n.pool + g_i18n.entries[i].val_off;
    }
    return jw__i18n_strip_context(english);
}

const char *jw_i18n_language(void) {
    return g_i18n.lang;
}

size_t jw_i18n_count(void) {
    return (size_t)g_i18n.count;
}
