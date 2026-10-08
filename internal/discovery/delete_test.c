#include "internal/discovery/delete.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char fixture[JW_STORAGE_PATH_MAX];
static jw_storage_source_list sources;
static char *ps_extensions[] = {"cue", "chd", "toc"};
static char *playlists[] = {"m3u"};
static char *gba_extensions[] = {"gba"};
static char *archives[] = {"zip", "7z"};
static char *pc98_extensions[] = {"cmd", "hdi"};
static jw_ra_system systems[] = {
    {.id = "PS", .extensions = {ps_extensions, 3}, .playlist_extensions = {playlists, 1}},
    {.id = "GBA", .extensions = {gba_extensions, 1}, .archive_extensions = {archives, 2}},
    {.id = "PC98", .extensions = {pc98_extensions, 2}},
};
static jw_ra_catalog catalog = {.systems = systems, .system_count = 3};
static jw_delete_owner owners[16];
static size_t owner_count;

static void path(char *out, const char *relative) {
    int n = snprintf(out, JW_STORAGE_PATH_MAX, "%s/%s", fixture, relative);
    assert(n > 0 && n < JW_STORAGE_PATH_MAX);
}
static void directory(const char *relative) {
    char absolute[JW_STORAGE_PATH_MAX]; path(absolute, relative);
    assert(mkdir(absolute, 0700) == 0);
}
static void put(const char *relative, const char *data) {
    char absolute[JW_STORAGE_PATH_MAX]; path(absolute, relative);
    FILE *fp = fopen(absolute, "wb"); assert(fp);
    assert(fwrite(data, 1, strlen(data), fp) == strlen(data)); assert(!fclose(fp));
}
static bool exists(const char *relative) {
    char absolute[JW_STORAGE_PATH_MAX]; path(absolute, relative);
    return access(absolute, F_OK) == 0;
}
static void remove_file(const char *relative) {
    char absolute[JW_STORAGE_PATH_MAX]; path(absolute, relative); assert(!unlink(absolute));
}
static void owner(const char *source, const char *relative, const char *name) {
    assert(owner_count < sizeof(owners) / sizeof(owners[0]));
    jw_delete_owner *o = &owners[owner_count++]; memset(o, 0, sizeof(*o));
    snprintf(o->source_id, sizeof(o->source_id), "%s", source);
    snprintf(o->rom_relpath, sizeof(o->rom_relpath), "%s", relative);
    snprintf(o->name, sizeof(o->name), "%s", name);
}
static jw_delete_plan preview(const char *relative, const char *system) {
    jw_delete_plan plan;
    char error[512];
    int rc = jw_delete_plan_build(&sources, &catalog, "primary", relative, system,
        owners, owner_count, NULL, 0, NULL, NULL, &plan, error, sizeof(error));
    if (rc) fprintf(stderr, "preview %s: %s\n", relative, error);
    assert(rc == 0);
    return plan;
}
static void blocked(const char *relative, const char *system, const char *message) {
    jw_delete_plan plan;
    char error[512];
    assert(jw_delete_plan_build(&sources, &catalog, "primary", relative, system,
        owners, owner_count, NULL, 0, NULL, NULL, &plan, error, sizeof(error)) == -1);
    if (!strstr(error, message)) fprintf(stderr, "expected %s, got %s\n", message, error);
    assert(strstr(error, message)); assert(!plan.files && !plan.snapshot);
}
static size_t slot(const jw_delete_plan *plan, const char *relative) {
    for (size_t i = 0; i < plan->file_count; i++) if (!strcmp(plan->files[i].rom_relpath, relative)) return i;
    assert(0); return 0;
}
static jw_delete_result execute(jw_delete_plan *plan) {
    jw_delete_result result; char error[512];
    int rc = jw_delete_execute(plan, NULL, NULL, &result, error, sizeof(error));
    if (rc) fprintf(stderr, "execute: %s\n", error);
    assert(!rc); return result;
}
static void test_eligibility(void) {
    assert(jw_delete_supported(&systems[0]) && jw_delete_supported(&systems[1]));
    const char *denied[] = {"SCUMMVM", "PORTS", "DOS", "AMIGA", "ARCADE", "MAME", "DC", "PCE", "SATURN"};
    for (size_t i = 0; i < sizeof(denied) / sizeof(denied[0]); i++) {
        jw_ra_system s = {.id = (char *)denied[i]}; assert(!jw_delete_supported(&s));
    }
    jw_ra_system s = systems[0]; s.provider = "shared/Content.pak"; assert(!jw_delete_supported(&s));
    char *unknown[] = {"cue", "ccd"}; s = systems[0]; s.extensions = (jw_ra_string_list){unknown, 2};
    assert(!jw_delete_supported(&s));
}
static void test_single_and_stale(void) {
    put("A/Roms/GBA/Game.zip", "container"); owner("primary", "GBA/Game.zip", "Game");
    jw_delete_plan first = preview("GBA/Game.zip", "GBA");
    assert(first.file_count == 1 && first.remove_count == 1 && first.bytes == 9);
    jw_delete_plan again = preview("GBA/Game.zip", "GBA"); assert(jw_delete_plan_equal(&first, &again));
    jw_delete_plan_free(&again);
    sources.sources[0].mount_id++;
    again = preview("GBA/Game.zip", "GBA"); assert(!jw_delete_plan_equal(&first, &again));
    jw_delete_plan_free(&again); sources.sources[0].mount_id--;
    put("A/Roms/GBA/Game.zip", "replacement");
    again = preview("GBA/Game.zip", "GBA"); assert(!jw_delete_plan_equal(&first, &again));
    jw_delete_result result; char error[512];
    assert(jw_delete_execute(&first, NULL, NULL, &result, error, sizeof(error)) == -1);
    assert(!result.removed_count && exists("A/Roms/GBA/Game.zip")); jw_delete_result_free(&result);
    jw_delete_plan_free(&first);
    result = execute(&again); assert(result.removed_count == 1 && !exists("A/Roms/GBA/Game.zip"));
    jw_delete_result_free(&result); jw_delete_plan_free(&again); owner_count = 0;
}
static void test_owners(void) {
    put("A/Roms/PS/sub/Disc.cue", "FILE track.bin BINARY\n"); put("A/Roms/PS/sub/track.bin", "payload");
    put("A/Roms/PS/Game.m3u", "sub/Disc.cue|Disc One\n#SAVEDISK:Save Disk\n");
    owner("primary", "PS/Game.m3u", "Parent Game"); owner("primary", "PS/sub/Disc.cue", "Standalone Disc");
    blocked("PS/sub/Disc.cue", "PS", "Parent Game");
    jw_delete_plan plan = preview("PS/Game.m3u", "PS");
    assert(plan.disc_count == 1 && plan.kept_count == 2 && plan.remove_count == 1);
    assert(plan.files[slot(&plan, "PS/sub/Disc.cue")].keep == JW_DELETE_SHARED);
    jw_delete_result result = execute(&plan);
    assert(result.removed_count == 1 && exists("A/Roms/PS/sub/Disc.cue") && exists("A/Roms/PS/sub/track.bin"));
    jw_delete_result_free(&result); jw_delete_plan_free(&plan); owner_count = 0;
    remove_file("A/Roms/PS/sub/Disc.cue"); remove_file("A/Roms/PS/sub/track.bin");
}
static void test_cross_card_and_missing(void) {
    put("A/Roms/PS/shared.chd", "shared"); put("A/Roms/PS/exclusive.chd", "exclusive");
    put("A/Roms/PS/Game.m3u", "shared.chd\nexclusive.chd\n");
    char reference[JW_STORAGE_PATH_MAX]; path(reference, "A/Roms/PS/shared.chd");
    put("B/Roms/Other/Hidden.m3u", reference);
    jw_delete_plan plan = preview("PS/Game.m3u", "PS");
    assert(plan.files[slot(&plan, "PS/shared.chd")].keep == JW_DELETE_SHARED);
    assert(plan.kept_count == 1 && plan.remove_count == 2);
    jw_delete_plan_free(&plan);
    sources.sources[1].available = false;
    plan = preview("PS/Game.m3u", "PS");
    assert(strstr(plan.missing_sources, "secondary_sd") && !plan.kept_count);
    sources.sources[1].available = true;
    jw_delete_plan fresh = preview("PS/Game.m3u", "PS"); assert(!jw_delete_plan_equal(&plan, &fresh));
    jw_delete_plan_free(&plan);
    jw_delete_result result = execute(&fresh);
    assert(result.removed_count == 2 && exists("A/Roms/PS/shared.chd"));
    jw_delete_result_free(&result); jw_delete_plan_free(&fresh);
    remove_file("B/Roms/Other/Hidden.m3u"); remove_file("A/Roms/PS/shared.chd");
}
static void test_preserved(void) {
    put("A/Roms/PS/art.bin", "artwork"); put("A/Roms/PS/progress.srm", "saved");
    put("A/Roms/PS/states/state.bin", "state"); put("A/Roms/PS/track.bin", "payload");
    put("A/Roms/PS/Game.m3u", "track.bin\nart.bin\nprogress.srm\nstates/state.bin\n");
    char art[JW_STORAGE_PATH_MAX]; path(art, "A/Roms/PS/art.bin"); const char *protected[] = {art};
    jw_delete_plan plan; char error[512];
    assert(!jw_delete_plan_build(&sources, &catalog, "primary", "PS/Game.m3u", "PS",
        NULL, 0, protected, 1, NULL, NULL, &plan, error, sizeof(error)));
    assert(plan.kept_count == 3 && plan.remove_count == 2);
    jw_delete_result result = execute(&plan);
    assert(result.removed_count == 2 && exists("A/Roms/PS/art.bin") && exists("A/Roms/PS/progress.srm") && exists("A/Roms/PS/states/state.bin"));
    jw_delete_result_free(&result); jw_delete_plan_free(&plan);
    remove_file("A/Roms/PS/art.bin"); remove_file("A/Roms/PS/progress.srm"); remove_file("A/Roms/PS/states/state.bin");
}
typedef struct { int calls; int fail_at; char last[512]; } fault;
static int fail_guard(void *context, const jw_delete_file *file, char *error, size_t size) {
    fault *f = context; f->calls++; snprintf(f->last, sizeof(f->last), "%s", file->rom_relpath);
    if (f->calls == f->fail_at) { snprintf(error, size, "Injected I/O error"); return -1; }
    return 0;
}
static bool cancelled(void *context) { (void)context; return true; }
static void test_retry_and_order(void) {
    put("A/Roms/PS/Track.bin", "payload"); put("A/Roms/PS/Disc.cue", "FILE Track.bin BINARY\r\n");
    put("A/Roms/PS/Game.m3u", "Disc.cue\r\nAbsent.cue\r\n");
    jw_delete_plan plan = preview("PS/Game.m3u", "PS");
    assert(plan.missing_count == 1 && plan.missing_descriptor && plan.disc_count == 2);
    fault injection = {.fail_at = 2}; jw_delete_result result; char error[512];
    assert(jw_delete_execute(&plan, fail_guard, &injection, &result, error, sizeof(error)) == -1);
    assert(result.removed_count == 1 && !exists("A/Roms/PS/Track.bin"));
    assert(!strcmp(injection.last, "PS/Disc.cue") && exists("A/Roms/PS/Disc.cue") && exists("A/Roms/PS/Game.m3u"));
    jw_delete_result_free(&result); jw_delete_plan_free(&plan);
    plan = preview("PS/Game.m3u", "PS"); assert(plan.missing_count == 2);
    result = execute(&plan); assert(result.removed_count == 2 && result.absent_count == 2);
    assert(result.completed[plan.launch_file] && !exists("A/Roms/PS/Game.m3u"));
    jw_delete_result_free(&result); jw_delete_plan_free(&plan);
    plan = preview("PS/Game.m3u", "PS"); assert(plan.file_count == 1 && plan.missing_descriptor);
    put("A/Roms/PS/Game.m3u", "Replacement.chd\n");
    assert(jw_delete_execute(&plan, NULL, NULL, &result, error, sizeof(error)) == -1);
    assert(!result.completed[plan.launch_file] && exists("A/Roms/PS/Game.m3u"));
    jw_delete_result_free(&result); jw_delete_plan_free(&plan); remove_file("A/Roms/PS/Game.m3u");
    assert(jw_delete_plan_build(&sources, &catalog, "primary", "PS/Game.m3u", "PS", NULL, 0,
        NULL, 0, cancelled, NULL, &plan, error, sizeof(error)) == -1);
    assert(strstr(error, "cancelled"));
}
static void test_sync_failure(void) {
#ifdef JW_ENABLE_FAULT_INJECTION
    put("A/Roms/PS/Track.bin", "payload"); put("A/Roms/PS/Disc.cue", "FILE Track.bin BINARY\n");
    jw_delete_plan plan = preview("PS/Disc.cue", "PS");
    setenv("JAWAKA_TEST_DELETE_SYNC_FAIL", "PS/Track.bin", 1);
    jw_delete_result result; char error[512];
    assert(jw_delete_execute(&plan, NULL, NULL, &result, error, sizeof(error)) == -1);
    assert(strstr(error, "sync changes") && result.removed_count == 1);
    assert(!exists("A/Roms/PS/Track.bin") && exists("A/Roms/PS/Disc.cue"));
    assert(result.completed[slot(&plan, "PS/Track.bin")] && !result.completed[plan.launch_file]);
    unsetenv("JAWAKA_TEST_DELETE_SYNC_FAIL");
    jw_delete_result_free(&result); jw_delete_plan_free(&plan);
    plan = preview("PS/Disc.cue", "PS"); result = execute(&plan);
    assert(result.removed_count == 1 && result.absent_count == 1);
    jw_delete_result_free(&result); jw_delete_plan_free(&plan);
#endif
}
static void test_missing_card_reference(void) {
    put("A/Roms/GBA/Game.gba", "game");
    char reference[JW_STORAGE_PATH_MAX]; path(reference, "B/Roms/Other/missing.chd");
    put("A/Roms/PS/Other.m3u", reference);
    sources.sources[1].available = false;
    jw_delete_plan unrelated = preview("GBA/Game.gba", "GBA");
    assert(strstr(unrelated.missing_sources, "secondary_sd"));
    jw_delete_plan selected = preview("PS/Other.m3u", "PS");
    assert(selected.kept_count == 1 && selected.remove_count == 1);
    assert(selected.files[slot(&selected, "Other/missing.chd")].keep == JW_DELETE_PRESERVED);
    jw_delete_result result = execute(&selected); assert(result.removed_count == 1);
    jw_delete_result_free(&result); jw_delete_plan_free(&selected); jw_delete_plan_free(&unrelated);
    sources.sources[1].available = true; remove_file("A/Roms/GBA/Game.gba");
}
static void test_cross_system_cmd(void) {
    put("A/Roms/PC98/disk.hdi", "image");
    put("A/Roms/PC98/game.cmd", "np2kai disk.hdi\n");
    put("A/Roms/GBA/owner.m3u", "../PC98/game.cmd\n");
    owner("primary", "GBA/owner.m3u", "Cross-system owner");
    blocked("PC98/disk.hdi", "PC98", "Cross-system owner");
    remove_file("A/Roms/GBA/owner.m3u"); remove_file("A/Roms/PC98/game.cmd");
    remove_file("A/Roms/PC98/disk.hdi"); owner_count = 0;
}
static void test_symlinks_and_absent_root(void) {
    put("A/Roms/GBA/Game.gba", "game");
    char target[JW_STORAGE_PATH_MAX], link[JW_STORAGE_PATH_MAX];
    path(target, "A/Roms/GBA/Game.gba"); path(link, "A/Roms/GBA/Alias.gba");
    assert(!symlink(target, link));
    blocked("GBA/Alias.gba", "GBA", "symlink launch");
    char lower[JW_STORAGE_PATH_MAX]; path(lower, "A/Roms/GBA/game.gba");
    if (!access(lower, F_OK)) blocked("GBA/game.gba", "GBA", "identity changed");
    owner("primary", "GBA/Alias.gba", "Alias");
    blocked("GBA/Game.gba", "GBA", "Alias");
    owner_count = 0; remove_file("A/Roms/GBA/Alias.gba"); remove_file("A/Roms/GBA/Game.gba");
    put("A/Roms/PS/Parent.m3u", "Absent.cue\n");
    jw_delete_plan plan = preview("PS/Absent.cue", "PS");
    assert(plan.remove_count == 1 && !plan.kept_count);
    jw_delete_result result = execute(&plan); assert(result.absent_count == 1 && !result.removed_count);
    jw_delete_result_free(&result); jw_delete_plan_free(&plan); remove_file("A/Roms/PS/Parent.m3u");
}
static void test_descriptor_errors_and_snapshots(void) {
    put("A/Roms/GBA/Game.gba", "game"); put("B/Roms/Other/Bad.cue", "FILE\n");
    blocked("GBA/Game.gba", "GBA", "FILE filename"); remove_file("B/Roms/Other/Bad.cue");
    put("B/Roms/Other/Other.m3u", "other.chd|First\n");
    jw_delete_plan a = preview("GBA/Game.gba", "GBA");
    put("B/Roms/Other/Other.m3u", "other.chd|Later\n");
    jw_delete_plan b = preview("GBA/Game.gba", "GBA"); assert(!jw_delete_plan_equal(&a, &b));
    jw_delete_plan_free(&a); jw_delete_plan_free(&b);
    remove_file("B/Roms/Other/Other.m3u"); remove_file("A/Roms/GBA/Game.gba");
    put("A/Roms/PC98/Game.cmd", "np2kai disk.hdi --speed 2\n"); put("A/Roms/PC98/disk.hdi", "progress inside image");
    a = preview("PC98/Game.cmd", "PC98"); assert(a.writable_image && a.file_count == 2);
    jw_delete_result result = execute(&a); assert(result.removed_count == 2);
    jw_delete_result_free(&result); jw_delete_plan_free(&a);
}
static jw_content_disc disc_at(const char *parent, size_t index) {
    jw_content content; char error[512];
    assert(!jw_content_inspect(&sources, "primary", parent, &systems[0], &content, error, sizeof(error)));
    assert(index < content.disc_count);
    jw_content_disc disc = content.discs[index];
    jw_content_free(&content);
    return disc;
}
static jw_delete_plan disc_preview(const char *parent, const jw_content_disc *disc) {
    jw_delete_plan plan; char error[512];
    int rc = jw_delete_disc_plan_build(&sources, &catalog, "primary", parent, "PS", disc,
        owners, owner_count, NULL, 0, NULL, NULL, &plan, error, sizeof(error));
    if (rc) fprintf(stderr, "disc preview: %s\n", error);
    assert(!rc); return plan;
}
static void disc_blocked(const char *parent, const jw_content_disc *disc, const char *message) {
    jw_delete_plan plan; char error[512];
    assert(jw_delete_disc_plan_build(&sources, &catalog, "primary", parent, "PS", disc,
        owners, owner_count, NULL, 0, NULL, NULL, &plan, error, sizeof(error)) == -1);
    if (!strstr(error, message)) fprintf(stderr, "disc expected %s, got %s\n", message, error);
    assert(strstr(error, message));
}
static void contents(const char *relative, const char *expected) {
    char absolute[JW_STORAGE_PATH_MAX], actual[8192]; path(absolute, relative);
    FILE *fp = fopen(absolute, "rb"); assert(fp);
    size_t size = fread(actual, 1, sizeof(actual), fp); assert(!ferror(fp)); assert(!fclose(fp));
    if (size != strlen(expected) || memcmp(actual, expected, size)) {
        fprintf(stderr, "unexpected contents of %s: %.*s\n", relative, (int)size, actual); assert(0);
    }
}
static void no_temporary(void) {
    char absolute[JW_STORAGE_PATH_MAX]; path(absolute, "A/Roms/PS");
    DIR *directory = opendir(absolute); assert(directory);
    struct dirent *entry;
    while ((entry = readdir(directory))) assert(strncmp(entry->d_name, ".jawaka-delete-", 15));
    assert(!closedir(directory));
}
static void test_disc_edit_and_final(void) {
    put("A/Roms/PS/One.cue", "FILE One.bin BINARY\r\n"); put("A/Roms/PS/One.bin", "one");
    put("A/Roms/PS/Two.cue", "FILE Two.bin BINARY\n"); put("A/Roms/PS/Two.bin", "second");
    const char *original = "\xef\xbb\xbf#EXTM3U\r\n#Keep header\r\n#EXTINF:0,Old label\r\n"
        "#Keep this comment\r\n#EXTINF:0,First\r\n \".\\One.cue\" |Opening disc\r\n"
        "#SAVEDISK:Save Disk\r\n\r\n#EXTINF:0,Second\r\nTwo.cue|Closing disc";
    const char *replacement = "\xef\xbb\xbf#EXTM3U\r\n#Keep header\r\n#Keep this comment\r\n"
        "#SAVEDISK:Save Disk\r\n\r\n#EXTINF:0,Second\r\nTwo.cue|Closing disc";
    put("A/Roms/PS/Game.m3u", original);
    jw_content_disc disc = disc_at("PS/Game.m3u", 0);
    strcpy(disc.label, "Untrusted display name"); disc.file_index = 99999;
    jw_delete_plan plan = disc_preview("PS/Game.m3u", &disc);
    assert(plan.playlist_edit && !plan.final_disc && plan.remaining_discs == 1 && plan.disc_count == 1);
    assert(!strcmp(plan.disc_name, "Opening disc") && plan.file_count == 3 && plan.remove_count == 2);
    assert(plan.files[plan.launch_file].keep == JW_DELETE_PRESERVED);
    jw_delete_plan fresh = disc_preview("PS/Game.m3u", &disc); assert(jw_delete_plan_equal(&plan, &fresh)); jw_delete_plan_free(&fresh);
    jw_delete_result result = execute(&plan);
    assert(result.playlist_replaced && result.removed_count == 2 && !result.completed[plan.launch_file]);
    contents("A/Roms/PS/Game.m3u", replacement);
    assert(!exists("A/Roms/PS/One.cue") && !exists("A/Roms/PS/One.bin") && exists("A/Roms/PS/Two.cue"));
    jw_delete_result_free(&result); jw_delete_plan_free(&plan); no_temporary();
    disc = disc_at("PS/Game.m3u", 0); plan = disc_preview("PS/Game.m3u", &disc);
    assert(plan.final_disc && !plan.playlist_edit && !plan.remaining_discs && plan.disc_count == 1);
    result = execute(&plan); assert(!result.playlist_replaced && result.removed_count == 3 && result.completed[plan.launch_file]);
    assert(!exists("A/Roms/PS/Game.m3u"));
    jw_delete_result_free(&result); jw_delete_plan_free(&plan);
}
static void test_disc_duplicates_and_members(void) {
    put("A/Roms/PS/one.chd", "one"); put("A/Roms/PS/two.chd", "two");
    put("A/Roms/PS/Game.m3u", "one.chd|First\n#keep\none.chd|Duplicate\ntwo.chd|Hidden disc\n");
    jw_content_disc disc = disc_at("PS/Game.m3u", 0);
    jw_delete_plan plan = disc_preview("PS/Game.m3u", &disc);
    assert(plan.playlist_edit && plan.remaining_discs == 1 && plan.disc_count == 2);
    jw_delete_result result = execute(&plan); assert(result.removed_count == 1 && result.playlist_replaced);
    contents("A/Roms/PS/Game.m3u", "#keep\ntwo.chd|Hidden disc\n");
    jw_delete_result_free(&result); jw_delete_plan_free(&plan);
    put("A/Roms/PS/Game.m3u", "two.chd|Hidden one\ntwo.chd|Hidden duplicate\n");
    disc = disc_at("PS/Game.m3u", 1); plan = disc_preview("PS/Game.m3u", &disc);
    assert(plan.final_disc && plan.disc_count == 2 && !plan.remaining_discs);
    result = execute(&plan); assert(result.removed_count == 2 && !result.playlist_replaced);
    jw_delete_result_free(&result); jw_delete_plan_free(&plan);

    put("A/Roms/PS/archive.zip", "archive");
    put("A/Roms/PS/Game.m3u", "archive.zip#DiscA.cue|A\narchive.zip#DiscB.cue|B\n");
    disc = disc_at("PS/Game.m3u", 0); plan = disc_preview("PS/Game.m3u", &disc);
    assert(plan.playlist_edit && !plan.remove_count && !plan.bytes);
    assert(plan.files[slot(&plan, "PS/archive.zip")].keep == JW_DELETE_SHARED);
    result = execute(&plan); assert(result.playlist_replaced && !result.removed_count && !result.absent_count);
    assert(exists("A/Roms/PS/archive.zip")); contents("A/Roms/PS/Game.m3u", "archive.zip#DiscB.cue|B\n");
    jw_delete_result_free(&result); jw_delete_plan_free(&plan);
    strcpy(disc.member, "Not there"); disc_blocked("PS/Game.m3u", &disc, "selected disc changed");
    disc = disc_at("PS/Game.m3u", 0); plan = disc_preview("PS/Game.m3u", &disc); assert(plan.final_disc);
    result = execute(&plan); assert(result.removed_count == 2);
    jw_delete_result_free(&result); jw_delete_plan_free(&plan);
}
static void test_disc_shared_ownership(void) {
    put("A/Roms/PS/One.cue", "FILE common.bin BINARY\nFILE one.bin BINARY\n");
    put("A/Roms/PS/Two.cue", "FILE common.bin BINARY\nFILE two.bin BINARY\n");
    put("A/Roms/PS/common.bin", "common"); put("A/Roms/PS/one.bin", "one"); put("A/Roms/PS/two.bin", "two");
    put("A/Roms/PS/Game.m3u", "One.cue\nTwo.cue\n");
    jw_content_disc disc = disc_at("PS/Game.m3u", 0);
    put("B/Roms/Other/Parent.m3u", "unused.chd\n");
    char reference[JW_STORAGE_PATH_MAX]; path(reference, "A/Roms/PS/Game.m3u"); put("B/Roms/Other/Parent.m3u", reference);
    owner("secondary_sd", "Other/Parent.m3u", "Outer game"); disc_blocked("PS/Game.m3u", &disc, "Outer game");
    owner_count = 0; remove_file("B/Roms/Other/Parent.m3u");
    owner("primary", "PS/One.cue", "Standalone disc");
    jw_delete_plan plan = disc_preview("PS/Game.m3u", &disc);
    assert(!plan.remove_count && plan.files[slot(&plan, "PS/One.cue")].keep == JW_DELETE_SHARED);
    jw_delete_plan_free(&plan); owner_count = 0;
    plan = disc_preview("PS/Game.m3u", &disc);
    assert(plan.remove_count == 2 && plan.files[slot(&plan, "PS/common.bin")].keep == JW_DELETE_SHARED);
    jw_delete_result result = execute(&plan);
    assert(result.removed_count == 2 && exists("A/Roms/PS/common.bin") && exists("A/Roms/PS/Two.cue"));
    jw_delete_result_free(&result); jw_delete_plan_free(&plan);
    disc = disc_at("PS/Game.m3u", 0); plan = disc_preview("PS/Game.m3u", &disc); result = execute(&plan);
    jw_delete_result_free(&result); jw_delete_plan_free(&plan);
}
static void test_disc_retry_and_replacement_errors(void) {
    const char *original = "One.cue|One\r\nTwo.chd|Two\r\n";
    put("A/Roms/PS/One.cue", "FILE one.bin BINARY\n"); put("A/Roms/PS/one.bin", "one");
    put("A/Roms/PS/Two.chd", "two"); put("A/Roms/PS/Game.m3u", original);
    jw_content_disc disc = disc_at("PS/Game.m3u", 0); jw_delete_plan plan = disc_preview("PS/Game.m3u", &disc);
    /* Preparation guards the parent, then removes payload, then its descriptor. */
    fault injection = {.fail_at = 3}; jw_delete_result result; char error[512];
    assert(jw_delete_execute(&plan, fail_guard, &injection, &result, error, sizeof(error)) == -1);
    assert(result.removed_count == 1 && !result.playlist_replaced && exists("A/Roms/PS/One.cue"));
    contents("A/Roms/PS/Game.m3u", original); no_temporary();
    jw_delete_result_free(&result); jw_delete_plan_free(&plan);
    plan = disc_preview("PS/Game.m3u", &disc);
#ifdef JW_ENABLE_FAULT_INJECTION
    char marker[JW_STORAGE_PATH_MAX]; path(marker, "failure-marker"); put("failure-marker", "fail");
    setenv("JAWAKA_TEST_DELETE_FAIL_TEMP_FILE", marker, 1);
    assert(jw_delete_execute(&plan, NULL, NULL, &result, error, sizeof(error)) == -1);
    assert(!result.removed_count && !result.playlist_replaced && exists("A/Roms/PS/One.cue"));
    no_temporary(); jw_delete_result_free(&result); unsetenv("JAWAKA_TEST_DELETE_FAIL_TEMP_FILE");
    setenv("JAWAKA_TEST_DELETE_FAIL_BEFORE_RENAME_FILE", marker, 1);
    assert(jw_delete_execute(&plan, NULL, NULL, &result, error, sizeof(error)) == -1);
    assert(result.removed_count == 1 && result.absent_count == 1 && !result.playlist_replaced);
    assert(!exists("A/Roms/PS/One.cue")); contents("A/Roms/PS/Game.m3u", original); no_temporary();
    jw_delete_result_free(&result); jw_delete_plan_free(&plan); unsetenv("JAWAKA_TEST_DELETE_FAIL_BEFORE_RENAME_FILE");
    remove_file("failure-marker"); plan = disc_preview("PS/Game.m3u", &disc);
    assert(plan.missing_descriptor);
    setenv("JAWAKA_TEST_DELETE_SYNC_FAIL", "PS/Game.m3u", 1);
    assert(jw_delete_execute(&plan, NULL, NULL, &result, error, sizeof(error)) == -1);
    assert(result.playlist_replaced && !result.completed[plan.launch_file]);
    contents("A/Roms/PS/Game.m3u", "Two.chd|Two\r\n"); no_temporary();
    unsetenv("JAWAKA_TEST_DELETE_SYNC_FAIL");
#else
    result = execute(&plan);
#endif
    jw_delete_result_free(&result); jw_delete_plan_free(&plan);
    disc = disc_at("PS/Game.m3u", 0); plan = disc_preview("PS/Game.m3u", &disc); result = execute(&plan);
    jw_delete_result_free(&result); jw_delete_plan_free(&plan);
}
static void test_disc_stale_parent(void) {
    put("A/Roms/PS/one.chd", "one"); put("A/Roms/PS/two.chd", "two");
    put("A/Roms/PS/Game.m3u", "one.chd\ntwo.chd\n");
    jw_content_disc disc = disc_at("PS/Game.m3u", 0); jw_delete_plan plan = disc_preview("PS/Game.m3u", &disc);
    put("A/Roms/PS/Game.m3u", "one.chd|Updated\ntwo.chd\n");
    jw_delete_plan fresh = disc_preview("PS/Game.m3u", &disc); assert(!jw_delete_plan_equal(&plan, &fresh));
    jw_delete_result result; char error[512];
    assert(jw_delete_execute(&plan, NULL, NULL, &result, error, sizeof(error)) == -1);
    assert(!result.removed_count && !result.playlist_replaced && exists("A/Roms/PS/one.chd")); no_temporary();
    jw_delete_result_free(&result); jw_delete_plan_free(&plan); jw_delete_plan_free(&fresh);
    plan = preview("PS/Game.m3u", "PS"); result = execute(&plan);
    jw_delete_result_free(&result); jw_delete_plan_free(&plan);
}

typedef struct { bool temporary; int root_checks; char swapped[512]; } swap_fault;
static int swap_guard(void *context, const jw_delete_file *file, char *error, size_t size) {
    (void)error; (void)size;
    swap_fault *fault = context;
    if (strcmp(file->rom_relpath, "PS/Game.m3u") || ++fault->root_checks != 2) return 0;
    if (!fault->temporary) put("A/Roms/PS/Game.m3u", "two.chd|External edit\n");
    else {
        char absolute[JW_STORAGE_PATH_MAX]; path(absolute, "A/Roms/PS");
        DIR *directory = opendir(absolute); assert(directory);
        struct dirent *entry;
        while ((entry = readdir(directory))) {
            if (strncmp(entry->d_name, ".jawaka-delete-", 15)) continue;
            snprintf(fault->swapped, sizeof(fault->swapped), "A/Roms/PS/%s", entry->d_name);
            /* Keep the original inode alive so filesystem reuse cannot hide the swap. */
            path(absolute, fault->swapped);
            char held[JW_STORAGE_PATH_MAX]; path(held, "A/Roms/PS/original.tmp");
            assert(!rename(absolute, held)); put(fault->swapped, "External temporary"); break;
        }
        assert(fault->swapped[0]); assert(!closedir(directory));
    }
    return 0;
}
static void test_disc_external_swaps(void) {
    for (int temporary = 0; temporary < 2; temporary++) {
        put("A/Roms/PS/one.chd", "one"); put("A/Roms/PS/two.chd", "two");
        put("A/Roms/PS/Game.m3u", "one.chd\ntwo.chd\n");
        jw_content_disc disc = disc_at("PS/Game.m3u", 0); jw_delete_plan plan = disc_preview("PS/Game.m3u", &disc);
        swap_fault fault = {.temporary = temporary != 0}; jw_delete_result result; char error[512];
        assert(jw_delete_execute(&plan, swap_guard, &fault, &result, error, sizeof(error)) == -1);
        assert(result.removed_count == 1 && !result.playlist_replaced && !result.completed[plan.launch_file]);
        assert(exists("A/Roms/PS/two.chd"));
        if (temporary) {
            assert(strstr(error, "Replacement playlist changed"));
            contents("A/Roms/PS/Game.m3u", "one.chd\ntwo.chd\n");
            contents(fault.swapped, "External temporary");
            remove_file(fault.swapped); remove_file("A/Roms/PS/original.tmp");
        } else contents("A/Roms/PS/Game.m3u", "two.chd|External edit\n");
        no_temporary(); jw_delete_result_free(&result); jw_delete_plan_free(&plan);
        remove_file("A/Roms/PS/Game.m3u"); remove_file("A/Roms/PS/two.chd");
    }
}

static void *test_bounded_depth_worker(void *unused) {
    (void)unused;
    /* Match jawakad's worker stack, including the walker and reader bounds. */
    char relative[512] = "A/Roms/PS";
    for (int level = 2; level <= 64; level++) {
        strcat(relative, "/d");
        directory(relative);
    }
    put("A/Roms/GBA/Deep.gba", "game");
    jw_delete_plan plan = preview("GBA/Deep.gba", "GBA");
    jw_delete_plan_free(&plan);
    strcat(relative, "/d"); directory(relative);
    blocked("GBA/Deep.gba", "GBA", "nested too deeply");
    char absolute[JW_STORAGE_PATH_MAX]; path(absolute, relative); assert(!rmdir(absolute));
    remove_file("A/Roms/GBA/Deep.gba");

    char name[128], descriptor[512];
    for (int level = 0; level < 32; level++) {
        snprintf(name, sizeof(name), "A/Roms/PC98/deep%02d.cmd", level);
        if (level < 31) snprintf(descriptor, sizeof(descriptor), "np2kai deep%02d.cmd\n", level + 1);
        else {
            strcpy(descriptor, "np2kai ");
            for (int missing = 0; missing < 30; missing++) strcat(descriptor, "absent/");
            strcat(descriptor, "disk.hdi\n");
        }
        put(name, descriptor);
    }
    plan = preview("PC98/deep00.cmd", "PC98");
    assert(plan.file_count == 33 && plan.missing_count == 1);
    jw_delete_plan_free(&plan);
    for (int level = 0; level < 32; level++) {
        snprintf(name, sizeof(name), "A/Roms/PC98/deep%02d.cmd", level);
        remove_file(name);
    }
    return NULL;
}

static void test_bounded_depth(void) {
    pthread_attr_t attributes;
    pthread_t worker;
    assert(!pthread_attr_init(&attributes));
    assert(!pthread_attr_setstacksize(&attributes, 2 * 1024 * 1024));
    assert(!pthread_create(&worker, &attributes, test_bounded_depth_worker, NULL));
    assert(!pthread_attr_destroy(&attributes));
    assert(!pthread_join(worker, NULL));
}

int main(void) {
    char temporary[] = "/tmp/jw-delete-XXXXXX"; assert(mkdtemp(temporary)); assert(realpath(temporary, fixture));
    directory("A"); directory("B"); directory("A/Roms"); directory("B/Roms");
    directory("A/Roms/PS"); directory("A/Roms/PS/sub"); directory("A/Roms/PS/states");
    directory("A/Roms/GBA"); directory("A/Roms/PC98"); directory("B/Roms/Other");
    sources.count = 2;
    for (int i = 0; i < 2; i++) {
        jw_storage_source *s = &sources.sources[i]; strcpy(s->id, i ? "secondary_sd" : "primary");
        path(s->root, i ? "B" : "A"); strcpy(s->root_abs, s->root);
        path(s->roms_path, i ? "B/Roms" : "A/Roms"); s->available = true;
    }
    path(sources.sources[0].states_path, "A/Roms/PS/states");
    test_eligibility(); test_single_and_stale(); test_owners(); test_cross_card_and_missing();
    test_preserved(); test_retry_and_order(); test_sync_failure(); test_missing_card_reference(); test_cross_system_cmd(); test_symlinks_and_absent_root();
    test_descriptor_errors_and_snapshots();
    test_disc_edit_and_final(); test_disc_duplicates_and_members(); test_disc_shared_ownership();
    test_disc_retry_and_replacement_errors(); test_disc_stale_parent(); test_disc_external_swaps(); test_bounded_depth();
    char command[JW_STORAGE_PATH_MAX + 32]; snprintf(command, sizeof(command), "rm -rf '%s'", fixture);
    assert(system(command) == 0); puts("delete engine tests passed"); return 0;
}
