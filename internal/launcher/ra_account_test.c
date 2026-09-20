/* standalone-ra-account-v1 launch authorization tests: the exact target and
   capability checks that decide whether a standalone child receives the
   account snapshot. Covers the bundled-Flycast launcher/marker pair, the
   DSperate provider/core/path/manifest matrix, and the spoof cases that must
   every time fall to refusal. */

#include "internal/launcher/ra_account.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

static void expect(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "ra-account-launch-test: %s\n", what);
        failures++;
    }
}

static char root[PATH_MAX];

static void write_file(const char *rel, const char *content) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", root, rel);
    char *slash = strrchr(path, '/');
    *slash = '\0';
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", path);
    if (system(cmd) != 0) exit(1);
    *slash = '/';
    FILE *f = fopen(path, "wb");
    if (!f) exit(1);
    fputs(content, f);
    fclose(f);
}

/* mlp1/DSperate.pak lives under an apps root; run.sh must be executable. */
static void make_dsperate_pak(const char *pak_json) {
    write_file("apps/mlp1/DSperate.pak/pak.json", pak_json);
    char run_sh[PATH_MAX];
    snprintf(run_sh, sizeof(run_sh), "%s/%s", root,
             "apps/mlp1/DSperate.pak/scripts/run.sh");
    write_file("apps/mlp1/DSperate.pak/scripts/run.sh", "#!/bin/sh\n");
    chmod(run_sh, 0755);
}

static const jw_standalone_policy FLYCAST_RELEASE = {
    .provider_bound = false, .release = JW_STANDALONE_RELEASE_FLYCAST,
};
static const jw_standalone_policy PROVIDER_BOUND = {
    .provider_bound = true, .release = JW_STANDALONE_RELEASE_NONE,
};
static const jw_standalone_policy DRASTIC_RELEASE = {
    .provider_bound = false, .release = JW_STANDALONE_RELEASE_DRASTIC,
};

/* Helpers naming the resolved pieces the way the daemon resolves them. */
static void platform_dir(char *out, size_t size) {
    snprintf(out, size, "%s/platform", root);
}
static void flycast_launcher(char *out, size_t size) {
    snprintf(out, size, "%s/platform/emulators/flycast/launch.sh", root);
}
static void dsperate_launcher(char *out, size_t size) {
    snprintf(out, size, "%s/apps/mlp1/DSperate.pak/scripts/run.sh", root);
}

static void test_flycast(void) {
    char platform[PATH_MAX], launcher[PATH_MAX];
    platform_dir(platform, sizeof(platform));
    flycast_launcher(launcher, sizeof(launcher));

    write_file("platform/emulators/flycast/launch.sh", "#!/bin/sh\n");
    write_file("platform/emulators/flycast/ra-account-v1",
               "standalone-ra-account-v1\n");

    expect(jw_ra_account_target_authorized(launcher, "flycast_standalone",
                                           &FLYCAST_RELEASE, NULL, platform),
           "bundled flycast: exact launcher + capability record authorized");

    /* The capability record is required: a filename match alone grants
       nothing. */
    expect(!jw_ra_account_target_authorized(launcher, "flycast_standalone",
                                            &FLYCAST_RELEASE, NULL,
                                            "/nonexistent-platform"),
           "bundled flycast: wrong platform dir denied");

    /* Spoofed provider naming itself after the release: provider-bound cores
       never take the release branch, whatever the files say. */
    expect(!jw_ra_account_target_authorized(launcher, "flycast",
                                            &PROVIDER_BOUND,
                                            "mlp1/not-a-release.pak", platform),
           "provider-bound flycast-named core denied");

    /* A same-named launcher outside the release payload. */
    char other_launcher[PATH_MAX];
    snprintf(other_launcher, sizeof(other_launcher),
             "%s/apps/mlp1/Flycast.pak/launch.sh", root);
    write_file("apps/mlp1/Flycast.pak/launch.sh", "#!/bin/sh\n");
    write_file("apps/mlp1/Flycast.pak/ra-account-v1",
               "standalone-ra-account-v1\n");
    expect(!jw_ra_account_target_authorized(other_launcher, "flycast",
                                            &FLYCAST_RELEASE, NULL, platform),
           "flycast identity outside the release-owned launcher denied");

    /* Wrong marker content. */
    write_file("platform/emulators/flycast/ra-account-v1",
               "standalone-ra-account-v0\n");
    expect(!jw_ra_account_target_authorized(launcher, "flycast_standalone",
                                            &FLYCAST_RELEASE, NULL, platform),
           "stale capability record denied");
    write_file("platform/emulators/flycast/ra-account-v1",
               "standalone-ra-account-v1\n");

    /* Other release standalones never qualify for the Flycast branch. */
    expect(!jw_ra_account_target_authorized(launcher, "drastic",
                                            &DRASTIC_RELEASE, NULL, platform),
           "drastic identity on the flycast path denied");
}

static void test_dsperate(void) {
    char platform[PATH_MAX], launcher[PATH_MAX];
    platform_dir(platform, sizeof(platform));
    dsperate_launcher(launcher, sizeof(launcher));

    make_dsperate_pak(
        "{ \"id\": \"org.umrk.dsperate\", \"name\": \"DSperate\","
        "  \"platform\": \"mlp1\", \"pak_version\": \"2.1.1\","
        "  \"min_leaf_version\": \"0.12.0\" }\n");

    expect(jw_ra_account_target_authorized(launcher, "dsperate",
                                           &PROVIDER_BOUND,
                                           "mlp1/DSperate.pak", platform),
           "validated dsperate pak authorized");

    /* The published 2.0.0 build cannot consume the handoff. */
    make_dsperate_pak(
        "{ \"id\": \"org.umrk.dsperate\", \"platform\": \"mlp1\","
        "  \"pak_version\": \"2.0.0\" }\n");
    expect(!jw_ra_account_target_authorized(launcher, "dsperate",
                                            &PROVIDER_BOUND,
                                            "mlp1/DSperate.pak", platform),
           "dsperate 2.0.0 denied");
    make_dsperate_pak(
        "{ \"id\": \"org.umrk.dsperate\", \"platform\": \"mlp1\","
        "  \"pak_version\": \"2.1.1\" }\n");

    /* A malformed version is not a capability. */
    make_dsperate_pak(
        "{ \"id\": \"org.umrk.dsperate\", \"platform\": \"mlp1\","
        "  \"pak_version\": \"2.1.1+leaf.1\" }\n");
    expect(!jw_ra_account_target_authorized(launcher, "dsperate",
                                            &PROVIDER_BOUND,
                                            "mlp1/DSperate.pak", platform),
           "unparseable dsperate version denied");
    make_dsperate_pak(
        "{ \"id\": \"org.umrk.dsperate\", \"platform\": \"mlp1\","
        "  \"pak_version\": \"2.1.1\" }\n");

    /* A pak claiming DSperate's identity from the wrong provider path, or the
       right pak with the wrong store id, is denied. */
    expect(!jw_ra_account_target_authorized(launcher, "dsperate",
                                            &PROVIDER_BOUND,
                                            "mlp1/Impostor.pak", platform),
           "wrong provider denied");
    make_dsperate_pak(
        "{ \"id\": \"org.umrk.impostor\", \"platform\": \"mlp1\","
        "  \"pak_version\": \"2.1.1\" }\n");
    expect(!jw_ra_account_target_authorized(launcher, "dsperate",
                                            &PROVIDER_BOUND,
                                            "mlp1/DSperate.pak", platform),
           "wrong manifest id denied");
    make_dsperate_pak(
        "{ \"id\": \"org.umrk.dsperate\", \"platform\": \"mlp1\","
        "  \"pak_version\": \"2.1.1\" }\n");

    /* Right provider, wrong core id or wrong pak-relative launcher. */
    expect(!jw_ra_account_target_authorized(launcher, "scummvm",
                                            &PROVIDER_BOUND,
                                            "mlp1/DSperate.pak", platform),
           "wrong core id denied");
    char wrong_launcher[PATH_MAX];
    snprintf(wrong_launcher, sizeof(wrong_launcher),
             "%s/apps/mlp1/DSperate.pak/launch.sh", root);
    expect(!jw_ra_account_target_authorized(wrong_launcher, "dsperate",
                                            &PROVIDER_BOUND,
                                            "mlp1/DSperate.pak", platform),
           "non-run.sh launcher denied");

    /* A release-recognized path-core (never provider-bound in the real
       catalog) cannot reach the provider branch. */
    expect(!jw_ra_account_target_authorized(launcher, "dsperate",
                                            &FLYCAST_RELEASE,
                                            "mlp1/DSperate.pak", platform),
           "release policy cannot take the provider branch");

    /* Missing manifest. */
    char manifest[PATH_MAX];
    snprintf(manifest, sizeof(manifest), "%s/apps/mlp1/DSperate.pak/pak.json",
             root);
    char saved[PATH_MAX];
    snprintf(saved, sizeof(saved), "%s.pakjson-saved", manifest);
    rename(manifest, saved);
    expect(!jw_ra_account_target_authorized(launcher, "dsperate",
                                            &PROVIDER_BOUND,
                                            "mlp1/DSperate.pak", platform),
           "missing manifest denied");
    rename(saved, manifest);
}

static void test_env_contract_names(void) {
    expect(strcmp(jw_ra_account_state_name(JW_RA_ACCOUNT_CONFIGURED),
                  "configured") == 0, "state configured");
    expect(strcmp(jw_ra_account_state_name(JW_RA_ACCOUNT_NEVER_CONFIGURED),
                  "never-configured") == 0, "state never-configured");
    expect(strcmp(jw_ra_account_state_name(JW_RA_ACCOUNT_SIGNED_OUT),
                  "signed-out") == 0, "state signed-out");
    expect(strcmp(jw_ra_account_state_name(JW_RA_ACCOUNT_INVALID),
                  "invalid") == 0, "state invalid");
    expect(strcmp(jw_ra_account_state_name(JW_RA_ACCOUNT_UNREADABLE),
                  "unreadable") == 0, "state unreadable");
    expect(jw_ra_account_state_name((jw_ra_account_state)999) == NULL,
           "out-of-range state has no name");
}

int main(void) {
    snprintf(root, sizeof(root), "/tmp/jawaka-ra-launch.XXXXXX");
    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 1;
    }

    test_flycast();
    test_dsperate();
    test_env_contract_names();

    if (failures) {
        fprintf(stderr, "ra-account-launch-test: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("ra-account-launch-test: ok\n");
    return 0;
}
