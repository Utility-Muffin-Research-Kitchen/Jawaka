#include "internal/launcher/ra_account.h"

#include "internal/platform/leaf_version.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *jw_ra_account_state_name(jw_ra_account_state state) {
    switch (state) {
    case JW_RA_ACCOUNT_CONFIGURED:       return "configured";
    case JW_RA_ACCOUNT_NEVER_CONFIGURED: return "never-configured";
    case JW_RA_ACCOUNT_SIGNED_OUT:       return "signed-out";
    case JW_RA_ACCOUNT_INVALID:          return "invalid";
    case JW_RA_ACCOUNT_UNREADABLE:       return "unreadable";
    }
    return NULL;
}

/* Read a small capability file byte-exactly. Returns 0 and writes the content
   (empty file allowed) into out; -1 when the file is missing, unreadable or
   larger than out_size - 1. */
static int jw__ra_read_file(const char *path, char *out, size_t out_size) {
    out[0] = '\0';
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(out, 1, out_size - 1, f);
    int read_failed = ferror(f);
    fclose(f);
    if (read_failed) return -1;
    out[n] = '\0';
    return 0;
}

/* The capability record is exactly the contract id, optionally followed by
   one trailing newline. No versions, no flags: support is advertised by the
   payload carrying the record at all, inside a target directory whose
   ownership this function's caller already established. */
static bool jw__ra_capability_matches(const char *content) {
    static const char id[] = JW_RA_ACCOUNT_CONTRACT_ID;
    size_t len = strlen(content);
    if (len > 0 && content[len - 1] == '\n') len--;
    return len == sizeof(id) - 1 && memcmp(content, id, sizeof(id) - 1) == 0;
}

/* DSperate's account adapter first shipped in pak 2.1.1. Older installed
   builds ignore the snapshot and must receive none, which is exactly the
   "unknown/older consumer gets no secrets" rule. */
static bool jw__ra_dsperate_version_capable(const char *text) {
    int version[3];
    if (jw_pak_version_parse(text, version) != 0) return false;
    return version[0] > 2 || (version[0] == 2 && version[1] > 1) ||
           (version[0] == 2 && version[1] == 1 && version[2] >= 1);
}

static bool jw__ra_dsperate_manifest_capable(const char *pak_json_path) {
    char *raw = malloc(65536);
    if (!raw) return false;
    bool capable = false;
    if (jw__ra_read_file(pak_json_path, raw, 65536) == 0) {
        cJSON *root = cJSON_Parse(raw);
        if (root) {
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
            const cJSON *version =
                cJSON_GetObjectItemCaseSensitive(root, "pak_version");
            capable = cJSON_IsString(id) && id->valuestring &&
                      strcmp(id->valuestring, "org.umrk.dsperate") == 0 &&
                      cJSON_IsString(version) && version->valuestring &&
                      jw__ra_dsperate_version_capable(version->valuestring);
            cJSON_Delete(root);
        }
    }
    free(raw);
    return capable;
}

/* The launcher path must be exactly <...>/mlp1/DSperate.pak/scripts/run.sh;
   the pak root for the manifest check falls out of the same path. */
static bool jw__ra_dsperate_launcher_root(const char *launcher_path,
                                          char *out, size_t out_size) {
    static const char suffix[] = "/mlp1/DSperate.pak/scripts/run.sh";
    size_t path_len = strlen(launcher_path);
    size_t suffix_len = sizeof(suffix) - 1;
    if (path_len <= suffix_len ||
        strcmp(launcher_path + path_len - suffix_len, suffix) != 0) {
        return false;
    }
    size_t root_len = path_len - suffix_len + strlen("/mlp1/DSperate.pak");
    if (root_len + 1 > out_size) return false;
    memcpy(out, launcher_path, root_len);
    out[root_len] = '\0';
    return true;
}

bool jw_ra_account_target_authorized(const char *launcher_path,
                                     const char *core_id,
                                     const jw_standalone_policy *policy,
                                     const char *provider,
                                     const char *platform_dir) {
    if (!launcher_path || !launcher_path[0] || !core_id || !policy ||
        !platform_dir || !platform_dir[0]) {
        return false;
    }

    if (!policy->provider_bound &&
        policy->release == JW_STANDALONE_RELEASE_FLYCAST) {
        char expected[1024];
        char marker[1024];
        if (snprintf(expected, sizeof(expected),
                     "%s/emulators/flycast/launch.sh", platform_dir) >=
                (int)sizeof(expected) ||
            strcmp(launcher_path, expected) != 0 ||
            snprintf(marker, sizeof(marker),
                     "%s/emulators/flycast/ra-account-v1", platform_dir) >=
                (int)sizeof(marker)) {
            return false;
        }
        char capability[64];
        return jw__ra_read_file(marker, capability, sizeof(capability)) == 0 &&
               jw__ra_capability_matches(capability);
    }

    if (policy->provider_bound && provider &&
        strcmp(provider, "mlp1/DSperate.pak") == 0 &&
        strcmp(core_id, "dsperate") == 0) {
        char pak_root[1024];
        char manifest[1024 + 12];
        return jw__ra_dsperate_launcher_root(launcher_path, pak_root,
                                             sizeof(pak_root)) &&
               snprintf(manifest, sizeof(manifest), "%s/pak.json", pak_root) <
                   (int)sizeof(manifest) &&
               jw__ra_dsperate_manifest_capable(manifest);
    }

    return false;
}
