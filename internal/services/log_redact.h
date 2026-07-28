#ifndef JW_SERVICES_LOG_REDACT_H
#define JW_SERVICES_LOG_REDACT_H

#include <stdbool.h>
#include <stddef.h>

/* CTL-1's redaction rule (contracts.md#ctl-1--control-and-status-ipc):
 * "Log tails and exports redact API keys, tokens, cookies, PINs,
 * passwords, password hashes, private keys, and shadow entries."
 *
 * jw_svc_log_redact_line() applies that rule to ONE line of log text at
 * a time (Jawaka's log tail/export is inherently line-oriented). It
 * recognizes two kinds of secret:
 *
 *   - A `key: value` or `key=value` pair whose key (case-insensitively,
 *     trimmed) names a known-sensitive field. This covers API keys,
 *     tokens (including `Authorization: Bearer ...`), cookies, PINs,
 *     and passwords. A `key:` match redacts the rest of the line (the
 *     header-style convention of one value per line); a `key=` match
 *     redacts only that one token or quoted value, then scanning
 *     continues for further pairs later on the same line (the
 *     logfmt-style convention of several `key=value` pairs per line).
 *
 *   - A small set of self-identifying markers, redacted by replacing
 *     the entire line (they have no clean "value" boundary to redact
 *     around safely): a crypt(3)-style hash prefix ($1$, $2a$/$2b$/
 *     $2x$/$2y$, $5$, $6$) -- which covers both a literal password hash
 *     and a /etc/shadow-style entry, since shadow's second
 *     colon-delimited field IS such a hash -- and the literal substring
 *     "PRIVATE KEY", covering PEM private-key markers.
 *
 * What this does NOT cover, deliberately, to stay a bounded, honest
 * primitive rather than a general secret scanner: credentials embedded
 * in a URL's userinfo (user:pass@host), a bearer/JWT-shaped token with
 * no recognizable key at all, and the body of a multi-line PEM block
 * between its BEGIN/END markers (only the marker lines themselves,
 * which contain the literal "PRIVATE KEY" substring, are caught). A
 * caller piping arbitrary untrusted multi-line blobs through this needs
 * a different tool.
 */

/* Copies `line` into `out` (bounded by out_size, always NUL-terminated
 * when out_size > 0), with every recognized secret redacted: a matched
 * key/value pair's value becomes the placeholder "[REDACTED]"; a
 * matched marker replaces the entire line with "[REDACTED LINE]".
 *
 * Returns true if any redaction was applied anywhere in `line`, even if
 * the redacted portion ended up past the point where `out` was
 * truncated -- this function always scans the whole input line before
 * returning, so a caller can trust the boolean regardless of `out_size`.
 * Returns false if `line` contained no recognized secret; `out` is then
 * a truncated copy of `line` unchanged.
 *
 * `line` and `out` must not be NULL and `out_size` must be nonzero, or
 * this returns false without touching `out` (there would be nowhere
 * safe to write a terminator). `line` should be a single line with no
 * embedded newline -- pass one line at a time. */
bool jw_svc_log_redact_line(const char *line, char *out, size_t out_size);

#endif
