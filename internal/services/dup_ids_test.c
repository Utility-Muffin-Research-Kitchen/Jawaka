#include "internal/services/dup_ids.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>

static void jw__test_empty_and_singleton(void) {
    assert(!jw_svc_find_duplicate_ids(NULL, 0, NULL));

    const char *one[] = {"org.umrk.a"};
    bool out1[1];
    assert(!jw_svc_find_duplicate_ids(one, 1, out1));
    assert(!out1[0]);

    puts("PASS dup-ids-test empty and singleton inputs report no duplicates");
}

static void jw__test_all_unique(void) {
    const char *ids[] = {"org.umrk.a", "org.umrk.b", "org.umrk.c"};
    bool out[3];
    assert(!jw_svc_find_duplicate_ids(ids, 3, out));
    assert(!out[0] && !out[1] && !out[2]);

    puts("PASS dup-ids-test all-unique ids report no duplicates");
}

static void jw__test_one_colliding_pair(void) {
    const char *ids[] = {"org.umrk.a", "org.umrk.b", "org.umrk.a"};
    bool out[3];
    assert(jw_svc_find_duplicate_ids(ids, 3, out));
    assert(out[0] && !out[1] && out[2]);

    puts("PASS dup-ids-test flags exactly the colliding pair, leaves the rest clear");
}

static void jw__test_triple_collision(void) {
    const char *ids[] = {"org.umrk.a", "org.umrk.a", "org.umrk.a"};
    bool out[3];
    assert(jw_svc_find_duplicate_ids(ids, 3, out));
    assert(out[0] && out[1] && out[2]);

    puts("PASS dup-ids-test flags every member of a 3-way collision, not just the first two");
}

static void jw__test_multiple_independent_collisions(void) {
    const char *ids[] = {"org.umrk.a", "org.umrk.b", "org.umrk.a", "org.umrk.c", "org.umrk.b"};
    bool out[5];
    assert(jw_svc_find_duplicate_ids(ids, 5, out));
    assert(out[0] && out[1] && out[2] && !out[3] && out[4]);

    puts("PASS dup-ids-test flags two independent colliding groups without cross-contamination");
}

static void jw__test_case_sensitive(void) {
    const char *ids[] = {"org.umrk.Foo", "org.umrk.foo"};
    bool out[2];
    assert(!jw_svc_find_duplicate_ids(ids, 2, out));
    assert(!out[0] && !out[1]);

    puts("PASS dup-ids-test comparison is case-sensitive, not a duplicate across case");
}

static void jw__test_null_entries_never_match(void) {
    const char *ids[] = {NULL, "org.umrk.a", NULL, "org.umrk.a"};
    bool out[4];
    assert(jw_svc_find_duplicate_ids(ids, 4, out));
    assert(!out[0] && out[1] && !out[2] && out[3]);

    const char *all_null[] = {NULL, NULL, NULL};
    bool out2[3];
    assert(!jw_svc_find_duplicate_ids(all_null, 3, out2));
    assert(!out2[0] && !out2[1] && !out2[2]);

    puts("PASS dup-ids-test NULL entries never match anything, including each other");
}

static void jw__test_null_buffers_with_nonzero_count(void) {
    const char *ids[] = {"org.umrk.a", "org.umrk.a"};
    bool out[2];
    assert(!jw_svc_find_duplicate_ids(NULL, 2, out));
    assert(!jw_svc_find_duplicate_ids(ids, 2, NULL));

    puts("PASS dup-ids-test NULL service_ids or is_duplicate with nonzero count is handled safely");
}

int main(void) {
    jw__test_empty_and_singleton();
    jw__test_all_unique();
    jw__test_one_colliding_pair();
    jw__test_triple_collision();
    jw__test_multiple_independent_collisions();
    jw__test_case_sensitive();
    jw__test_null_entries_never_match();
    jw__test_null_buffers_with_nonzero_count();
    puts("PASS dup-ids-test");
    return 0;
}
