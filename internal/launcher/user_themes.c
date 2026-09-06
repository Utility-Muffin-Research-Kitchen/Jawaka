#include "internal/launcher/user_themes.h"
#include "internal/core/log.h"
#include "cJSON.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static bool jw__ut_file_exists(const char *path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool jw__ut_dir_exists(const char *path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool jw__ut_join(char *out, size_t n, const char *a, const char *b) {
    int r = snprintf(out, n, "%s/%s", a, b);
    return r > 0 && (size_t)r < n;
}

/* Bounded copy that tolerates overlapping src/dst (name defaults to dir, both
   inside the same entry) and gives gcc nothing to flag. */
static void jw__ut_copy(char *dst, size_t n, const char *src) {
    if (!dst || n == 0) return;
    size_t len = src ? strnlen(src, n - 1) : 0;
    memmove(dst, src, len);
    dst[len] = '\0';
}

static char *jw__ut_read_file(const char *path, long max_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len <= 0 || len > max_len) { fclose(fp); return NULL; }
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t got = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    buf[got] = '\0';
    return buf;
}

static void jw__ut_json_str(cJSON *obj, const char *key, char *dst, size_t n) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring && v->valuestring[0])
        jw__ut_copy(dst, n, v->valuestring);
}

/* Parse theme.json into `t`. Returns false when the file is missing or not a
   JSON object -- the theme is then skipped, since the file is required. */
static bool jw__ut_parse(jw_user_theme *t, const char *theme_dir) {
    char path[PATH_MAX];
    if (!jw__ut_join(path, sizeof(path), theme_dir, "theme.json")) return false;
    char *text = jw__ut_read_file(path, 64 * 1024);
    if (!text) return false;
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) return false;
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return false; }

    jw__ut_json_str(root, "name",    t->name,    sizeof(t->name));
    jw__ut_json_str(root, "author",  t->author,  sizeof(t->author));
    jw__ut_json_str(root, "version", t->version, sizeof(t->version));

    cJSON *grid = cJSON_GetObjectItemCaseSensitive(root, "grid");
    if (cJSON_IsObject(grid)) {
        cJSON *c = cJSON_GetObjectItemCaseSensitive(grid, "cols");
        cJSON *r = cJSON_GetObjectItemCaseSensitive(grid, "rows");
        if (cJSON_IsNumber(c) && c->valueint >= 1 && c->valueint <= 8) t->grid_cols = c->valueint;
        if (cJSON_IsNumber(r) && r->valueint >= 1 && r->valueint <= 6) t->grid_rows = r->valueint;
    }
    cJSON *ss = cJSON_GetObjectItemCaseSensitive(root, "status_style");
    if (cJSON_IsString(ss) && ss->valuestring) {
        if (strcmp(ss->valuestring, "light") == 0)      t->status_style = JW_USER_THEME_STATUS_LIGHT;
        else if (strcmp(ss->valuestring, "dark") == 0)  t->status_style = JW_USER_THEME_STATUS_DARK;
        else                                            t->status_style = JW_USER_THEME_STATUS_AUTO;
    }
    cJSON_Delete(root);
    return true;
}

static bool jw__ut_has_any_image(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while (!found && (e = readdir(d)) != NULL) {
        const char *ext = strrchr(e->d_name, '.');
        if (e->d_name[0] == '.' || !ext) continue;
        found = strcasecmp(ext, ".png") == 0;
    }
    closedir(d);
    return found;
}

static bool jw__ut_wallpaper_in(const char *dir, char *out, size_t n) {
    static const char *const names[] = { "wallpaper.png", "wallpaper.jpg", "wallpaper.jpeg" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (jw__ut_join(out, n, dir, names[i]) && jw__ut_file_exists(out)) return true;
    }
    if (out && n) out[0] = '\0';
    return false;
}

static int jw__ut_cmp(const void *a, const void *b) {
    return strcasecmp(((const jw_user_theme *)a)->dir, ((const jw_user_theme *)b)->dir);
}

int jw_user_themes_scan(jw_user_theme_catalog *cat, const char *sdcard_root) {
    if (!cat) return 0;
    memset(cat, 0, sizeof(*cat));
    if (!sdcard_root || !sdcard_root[0]) return 0;
    if (!jw__ut_join(cat->root, sizeof(cat->root), sdcard_root, "Themes")) return 0;

    DIR *d = opendir(cat->root);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && cat->count < JW_USER_THEME_MAX) {
        if (e->d_name[0] == '.') continue;
        char dir[PATH_MAX];
        if (!jw__ut_join(dir, sizeof(dir), cat->root, e->d_name)) continue;
        if (!jw__ut_dir_exists(dir)) continue;

        jw_user_theme *t = &cat->items[cat->count];
        memset(t, 0, sizeof(*t));
        jw__ut_copy(t->dir, sizeof(t->dir), e->d_name);
        if (!jw__ut_parse(t, dir)) {
            jw_log_warn("user theme %s: no readable theme.json; skipped", e->d_name);
            continue;
        }
        if (!t->name[0]) jw__ut_copy(t->name, sizeof(t->name), t->dir);

        char sub[PATH_MAX];
        if (jw__ut_join(sub, sizeof(sub), dir, "grid/icons"))      t->has_grid_icons      = jw__ut_has_any_image(sub);
        if (jw__ut_join(sub, sizeof(sub), dir, "grid/labels"))     t->has_grid_labels     = jw__ut_has_any_image(sub);
        if (jw__ut_join(sub, sizeof(sub), dir, "coverflow/icons")) t->has_coverflow_icons = jw__ut_has_any_image(sub);
        char wp[PATH_MAX];
        t->has_wallpaper = jw__ut_wallpaper_in(dir, wp, sizeof(wp));
        if (!t->has_wallpaper && jw__ut_join(sub, sizeof(sub), dir, "grid"))
            t->has_wallpaper = jw__ut_wallpaper_in(sub, wp, sizeof(wp));
        cat->count++;
    }
    closedir(d);
    if (cat->count > 1)
        qsort(cat->items, (size_t)cat->count, sizeof(cat->items[0]), jw__ut_cmp);
    return cat->count;
}

int jw_user_themes_find(const jw_user_theme_catalog *cat, const char *dir) {
    if (!cat || !dir || !dir[0]) return -1;
    for (int i = 0; i < cat->count; i++)
        if (strcmp(cat->items[i].dir, dir) == 0) return i;
    return -1;
}

static bool jw__ut_asset(const jw_user_theme_catalog *cat, int idx, const char *view,
                         const char *kind, const char *system_code,
                         char *out, size_t out_size) {
    if (out && out_size) out[0] = '\0';
    if (!cat || idx < 0 || idx >= cat->count || !view || !system_code || !system_code[0])
        return false;
    /* _default is Leaf's safety net, not a tile: never themable. */
    if (strcmp(system_code, "_default") == 0) return false;
    int r = snprintf(out, out_size, "%s/%s/%s/%s/%s.png",
                     cat->root, cat->items[idx].dir, view, kind, system_code);
    return r > 0 && (size_t)r < out_size;
}

bool jw_user_theme_icon_path(const jw_user_theme_catalog *cat, int idx,
                             const char *view, const char *system_code,
                             char *out, size_t out_size) {
    return jw__ut_asset(cat, idx, view, "icons", system_code, out, out_size);
}

bool jw_user_theme_label_path(const jw_user_theme_catalog *cat, int idx,
                              const char *view, const char *system_code,
                              char *out, size_t out_size) {
    return jw__ut_asset(cat, idx, view, "labels", system_code, out, out_size);
}

bool jw_user_theme_wallpaper_path(const jw_user_theme_catalog *cat, int idx,
                                  const char *view, char *out, size_t out_size) {
    if (out && out_size) out[0] = '\0';
    if (!cat || idx < 0 || idx >= cat->count) return false;
    char dir[PATH_MAX], sub[PATH_MAX];
    if (!jw__ut_join(dir, sizeof(dir), cat->root, cat->items[idx].dir)) return false;
    if (view && view[0] && jw__ut_join(sub, sizeof(sub), dir, view) &&
        jw__ut_wallpaper_in(sub, out, out_size))
        return true;
    return jw__ut_wallpaper_in(dir, out, out_size);
}

bool jw_user_theme_png_dims(const char *path, int *w, int *h) {
    if (w) *w = 0;
    if (h) *h = 0;
    FILE *fp = path && path[0] ? fopen(path, "rb") : NULL;
    if (!fp) return false;
    unsigned char hdr[24];
    size_t got = fread(hdr, 1, sizeof(hdr), fp);
    fclose(fp);
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    if (got < 24 || memcmp(hdr, sig, 8) != 0 || memcmp(hdr + 12, "IHDR", 4) != 0) return false;
    unsigned int pw = ((unsigned)hdr[16] << 24) | ((unsigned)hdr[17] << 16) | ((unsigned)hdr[18] << 8) | hdr[19];
    unsigned int ph = ((unsigned)hdr[20] << 24) | ((unsigned)hdr[21] << 16) | ((unsigned)hdr[22] << 8) | hdr[23];
    if (pw == 0 || ph == 0 || pw > 65535u || ph > 65535u) return false;
    if (w) *w = (int)pw;
    if (h) *h = (int)ph;
    return true;
}

int jw_user_theme_validate(const jw_user_theme_catalog *cat, int idx,
                           int *present, int *flagged) {
    int rejected = 0, fl = 0, pr = 0;
    if (present) *present = 0;
    if (flagged) *flagged = 0;
    if (!cat || idx < 0 || idx >= cat->count) return 0;

    char dir[PATH_MAX];
    if (snprintf(dir, sizeof(dir), "%s/%s/grid/icons", cat->root, cat->items[idx].dir)
            >= (int)sizeof(dir))
        return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *ext = strrchr(e->d_name, '.');
        if (e->d_name[0] == '.' || !ext || strcasecmp(ext, ".png") != 0) continue;
        char p[PATH_MAX];
        if (!jw__ut_join(p, sizeof(p), dir, e->d_name) || !jw__ut_file_exists(p)) continue;
        pr++;
        int w = 0, h = 0;
        if (!jw_user_theme_png_dims(p, &w, &h)) { rejected++; continue; }
        if (w > JW_USER_THEME_ICON_MAX_PX || h > JW_USER_THEME_ICON_MAX_PX) { rejected++; continue; }
        if (w != h || w != JW_USER_THEME_ICON_TARGET_PX) fl++;
    }
    closedir(d);
    if (present) *present = pr;
    if (flagged) *flagged = fl;
    return rejected;
}
