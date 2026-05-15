#ifndef JW_SETTINGS_THEME_RESOLVE_H
#define JW_SETTINGS_THEME_RESOLVE_H

#include <stddef.h>

/* Resolves the launcher/menu theme name following the locked precedence:
 *   1. JAWAKA_THEME env var (explicit dev/test override)
 *   2. settings.theme_name from the SQLite DB
 *   3. hard default: "Jawaka-Tabs"
 *
 * Writes the resolved name into `out` (always NUL-terminated). Returns 0
 * on success, -1 only if out is NULL/zero-sized.
 *
 * db_path may be NULL — only env / default are consulted in that case.
 */
int jw_resolve_theme_name(const char *db_path, char *out, size_t out_size);

#endif
