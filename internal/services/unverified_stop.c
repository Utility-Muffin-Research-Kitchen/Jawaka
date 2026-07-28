#include "internal/services/unverified_stop.h"

jw_svc_unverified_stop_action jw_svc_unverified_stop_action_for(jw_svc_stop_caller caller) {
    switch (caller) {
        case JW_SVC_STOP_CALLER_SHUTDOWN:
        case JW_SVC_STOP_CALLER_SUSPEND:
            return JW_SVC_UNVERIFIED_STOP_CONTINUE_WITH_WARNING;
        case JW_SVC_STOP_CALLER_SAFE_UNMOUNT:
        case JW_SVC_STOP_CALLER_PACKAGE_OP:
            return JW_SVC_UNVERIFIED_STOP_FAIL_OPERATION;
        case JW_SVC_STOP_CALLER_GAME_LAUNCH:
            return JW_SVC_UNVERIFIED_STOP_REQUIRE_OVERRIDE;
        default:
            /* Not a real jw_svc_stop_caller value (e.g. an invalid int
             * reached this function through an unchecked cast). Fail
             * toward the most conservative table row rather than
             * guessing: refuse the caller's operation rather than
             * silently continuing or silently launching. */
            return JW_SVC_UNVERIFIED_STOP_FAIL_OPERATION;
    }
}
