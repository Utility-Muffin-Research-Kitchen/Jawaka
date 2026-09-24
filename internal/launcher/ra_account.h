#ifndef JW_LAUNCHER_RA_ACCOUNT_H
#define JW_LAUNCHER_RA_ACCOUNT_H

#include <stdbool.h>

#include "internal/db/db.h"
#include "internal/launcher/standalone_policy.h"

/* Producer side of standalone-ra-account-v1: the versioned RetroAchievements
   account snapshot the daemon exports to one authorized standalone emulator
   after fork(). The five variables are child-only per-launch state; they are
   never written to env.sh, logs, argv, manifests or the daemon's own
   environment. */

#define JW_RA_ACCOUNT_CONTRACT_ID   "standalone-ra-account-v1"
#define JW_RA_ACCOUNT_ENV_VERSION   "UMRK_RA_ACCOUNT_VERSION"
#define JW_RA_ACCOUNT_ENV_STATE     "UMRK_RA_ACCOUNT_STATE"
#define JW_RA_ACCOUNT_ENV_USERNAME  "UMRK_RA_ACCOUNT_USERNAME"
#define JW_RA_ACCOUNT_ENV_PASSWORD  "UMRK_RA_ACCOUNT_PASSWORD"
#define JW_RA_ACCOUNT_ENV_REVISION  "UMRK_RA_ACCOUNT_REVISION"

/* Contract state strings, by jw_ra_account_state value. NULL for out-of-range
   values (a caller bug; treat as no handoff). */
const char *jw_ra_account_state_name(jw_ra_account_state state);

/* Decide whether a fully resolved standalone launch target may receive the
   account snapshot. This is deliberately NOT display policy: it never grants
   direct DRM, and direct-DRM grants never grant credentials.

   Authorized today, and only today:

   - Bundled Flycast: a release-owned core whose resolved launcher is EXACTLY
     <platform_dir>/emulators/flycast/launch.sh, and whose installed payload
     carries the narrow capability record
     <platform_dir>/emulators/flycast/ra-account-v1 with the contract id as
     its content. A matching core id, filename or substring alone is not
     enough, and a provider-bound core never takes this branch.

   - DSperate: a provider-bound core with provider "mlp1/DSperate.pak", core
     id "dsperate", resolved launcher exactly <pak>/scripts/run.sh, an
     installed <pak>/pak.json with id "org.umrk.dsperate" and
     pak_version >= 2.1.1. The published 2.0.0 build does not consume the
     contract, so it receives no credentials.

   Everything else, and every check that cannot be positively established
   (missing files, unreadable manifests, malformed versions), is refused. */
bool jw_ra_account_target_authorized(const char *launcher_path,
                                     const char *core_id,
                                     const jw_standalone_policy *policy,
                                     const char *provider,
                                     const char *platform_dir);

/* Child-side environment for a standalone emulator launch, called after
   fork() and before exec(). Clears the RetroArch credential channel
   (JAWAKA_CHEEVOS_*) and any inherited account snapshot, then applies
   `account` only when `authorized`. An unauthorized target ends up with
   total absence, the contract's unmanaged case. */
void jw_ra_account_prepare_standalone_env(const jw_ra_account *account,
                                          bool authorized);

/* Drop the five UMRK_RA_ACCOUNT_* variables from the current environment. */
void jw_ra_account_clear_env(void);

/* Apply the snapshot over a cleared set. Producer-side validation runs here
   too, so a caller bug can never put an out-of-contract snapshot on the
   wire: CONFIGURED needs credentials that pass jw_ra_credentials_check_values
   (else the verdict is "invalid") and a revision in 1..2^62 (else
   "unreadable", since the counter could not be established); SIGNED_OUT
   needs a revision in range (else "unreadable"). An out-of-range state
   exports nothing. */
void jw_ra_account_apply_env(const jw_ra_account *account);

/* The RetroArch per-launch handoff: JAWAKA_CHEEVOS_USERNAME/PASSWORD are set
   for a CONFIGURED account and unset for every other state, so the session
   config falls back to whatever the user set inside RetroArch. */
void jw_ra_account_apply_retroarch_env(const jw_ra_account *account);
void jw_ra_account_clear_retroarch_env(void);

#endif
