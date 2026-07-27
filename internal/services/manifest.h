#ifndef JW_SERVICES_MANIFEST_H
#define JW_SERVICES_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>

/* app-services-v1 (SVC-1) manifest validation.
 *
 * Normative source: plans/leaf-syncthing/contracts.md#svc-1--service-manifest
 * in the umrk-workspace sibling checkout. Schema and fixtures:
 * contracts/leaf-services/{app-services-v1.schema.json,manifests/} there.
 * Rejection reason slugs returned by jw_service_manifest_validate() match
 * the fixture directory names under manifests/invalid/ exactly, so a test
 * can walk that fixture tree and assert reason-for-reason without a
 * separate translation table.
 *
 * This module validates ONE already-read pak.json's shape and (where the
 * rule requires it) the filesystem around it. It does not discover apps,
 * does not detect a duplicate service.id across two different paks (that
 * is a cross-manifest check the caller must do once it has validated every
 * candidate), and does not implement supervision, IPC, or persistence --
 * those are separate A2 work.
 */

#define JW_SVC_MAX_ARGS 16
#define JW_SVC_MAX_ARG_LEN 256
#define JW_SVC_MAX_STATE_LIST 16
#define JW_SVC_ID_BUF 128
#define JW_SVC_RUN_PATH_BUF 4096
#define JW_SVC_STATE_PATH_BUF 512
#define JW_SVC_REASON_BUF 64

typedef enum {
    JW_SVC_LIFECYCLE_GAME_IGNORE = 0, /* default */
    JW_SVC_LIFECYCLE_GAME_STOP,
    JW_SVC_LIFECYCLE_GAME_NOTIFY,
} jw_svc_lifecycle_game;

typedef struct {
    char id[JW_SVC_ID_BUF]; /* == top-level pak id, reverse-DNS */

    char run_path[JW_SVC_RUN_PATH_BUF]; /* relative to the pak root */
    /* +1: JW_SVC_MAX_ARG_LEN is the validation LIMIT (256 bytes is valid,
     * 257 is not), so storage needs one more byte for the NUL on a
     * maximum-length arg. */
    char run_args[JW_SVC_MAX_ARGS][JW_SVC_MAX_ARG_LEN + 1];
    int run_args_count;

    bool restart_on_failure; /* false means "no" */

    /* Resolved, not raw: absent input becomes 5000; a declared value above
     * 15000 is clamped here to 15000 (and the caller should log that this
     * happened -- this struct only carries the resolved outcome). */
    int stop_grace_ms;

    jw_svc_lifecycle_game lifecycle_game;
    bool stop_on_storage_change;
    bool stop_on_suspend;

    bool has_state;
    char state_root[JW_SVC_STATE_PATH_BUF]; /* single path component */
    char state_revoke_on_uninstall[JW_SVC_MAX_STATE_LIST][JW_SVC_STATE_PATH_BUF];
    int state_revoke_count;
    char state_retained_roots[JW_SVC_MAX_STATE_LIST][JW_SVC_STATE_PATH_BUF];
    int state_retained_count;
} jw_service_manifest;

/* pak_json_text: the raw bytes of pak.json (NUL-terminated).
 *
 * pak_abs_path: absolute (or test-relative, but must be a real, resolvable
 * path for the filesystem checks) path to the pak's root directory -- the
 * directory pak.json itself lives in. Used to validate run.path: it must
 * resolve (following every symlink component, not just a symlinked leaf)
 * to a path inside pak_abs_path, and that resolved path must exist, be a
 * regular file, and be executable.
 *
 * userdata_root_abs_or_null: absolute path to $USERDATA_PATH for this
 * service's installed package, or NULL/empty if not known/not yet
 * provisioned. When present, state.revoke_on_uninstall's "no existing
 * component may be a symlink" rule is checked against
 * userdata_root/state.root/<entry>; when absent, that filesystem check is
 * skipped (a component that doesn't exist yet is not a violation either
 * way -- only an EXISTING symlink is).
 *
 * On success: returns true, fills *out, reason untouched.
 * On failure: returns false, *out is not meaningfully filled, and a stable
 * reason slug (e.g. "escaping-symlink") is copied into reason.
 */
bool jw_service_manifest_validate(const char *pak_json_text,
                                   const char *pak_abs_path,
                                   const char *userdata_root_abs_or_null,
                                   jw_service_manifest *out,
                                   char *reason, size_t reason_size);

#endif
