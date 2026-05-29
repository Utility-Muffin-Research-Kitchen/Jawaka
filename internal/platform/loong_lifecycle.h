#ifndef JW_PLATFORM_LOONG_LIFECYCLE_H
#define JW_PLATFORM_LOONG_LIFECYCLE_H

/* MiniLoong (MLP1) launcher-lifecycle integration.
 *
 * On the MiniLoong the stock loong_service shows a boot "loading" transition
 * (the loong_transition process) on top of the GUI and only dismisses it once
 * the home launcher reports readiness through the loong SDK. loong_service
 * respawns loong_transition within ~2s until it gets that signal, so a custom
 * SDL launcher (jawaka) that never reports readiness leaves the splash up,
 * pinning loong_transition (~12%) and weston (~22%) on CPU.
 *
 * jw_loong_notify_home_ready() emits the same signal stock loong_pangu sends
 * when its main frame is ready: ServiceApi::eventOpend("HOME"), reached via the
 * C entry point EventOpend() in /usr/lib/libloong_sdk.so. The library is loaded
 * with dlopen so jawaka keeps no build-time dependency on stock binaries. The
 * call is idempotent (fires at most once). On non-MLP1 platforms every function
 * is a no-op.
 */
void jw_loong_notify_home_ready(void);
void jw_loong_notify_home_closing(void);

#endif
