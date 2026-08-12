#ifndef JW_I18N_H
#define JW_I18N_H

#include <stdbool.h>
#include <stddef.h>

/* User-facing string lookup.
 *
 * The English string is the key. `T("Settings")` returns the translation when
 * one is loaded and the literal itself otherwise, so a missing or partial
 * translation degrades to English with no call-site handling. That property is
 * the whole reason the key is the source string rather than an enum: a
 * community translation lands incomplete and stays incomplete for a while.
 *
 * Wrap ONLY user-facing text. Log messages, IPC message types, DB column names,
 * file paths and env var names must stay unwrapped -- they look like UI strings
 * to a regex, which is why the wrapping pass cannot be automated.
 *
 * Identical English needing different translations takes a context prefix that
 * the compiler strips from the displayed text: T("verb|Open") and T("noun|Open")
 * are distinct keys, both rendering "Open" when untranslated.
 *
 * The returned pointer is owned by the table and stays valid until
 * jw_i18n_shutdown(). Do not free it, and do not hold it across a language
 * change -- the launcher relaunches on a language change, so in practice that
 * means do not stash it in a long-lived global.
 */

#define T(s) jw_i18n(s)

/* Load the table for `lang` ("en" or a code like "zh_CN"). "en" (or NULL/empty)
 * loads nothing and every lookup returns its key, which is the no-op default.
 *
 * Sources, in order:
 *   1. $UMRK_INTERNAL_DATA_PATH/i18n/<lang>.tsv  -- a live override, tab
 *      separated "english<TAB>translation" with # comments. This is what a
 *      translator exports from a spreadsheet, so they can iterate on device
 *      without a build. Lines that fail to parse are skipped, not fatal.
 *   2. $UMRK_PLATFORM_PATH/i18n/<lang>.jwi       -- the shipped compiled table.
 *
 * Returns true when a table loaded. Returns false for "en", for a missing
 * table, and for a corrupt one -- all three leave lookups returning their keys,
 * so a caller that ignores the result still behaves correctly.
 */
bool jw_i18n_load(const char *lang);

void jw_i18n_shutdown(void);

/* Never returns NULL for a non-NULL argument; returns `english` on any miss. */
const char *jw_i18n(const char *english);

/* The language actually loaded, "en" when none. Never NULL. */
const char *jw_i18n_language(void);

/* Entry count in the loaded table, 0 when none. For diagnostics and the
 * coverage gate; not needed for normal use. */
size_t jw_i18n_count(void);

#endif /* JW_I18N_H */
