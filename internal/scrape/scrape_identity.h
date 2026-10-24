#ifndef JW_SCRAPE_IDENTITY_H
#define JW_SCRAPE_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>

#define JW_SCRAPE_IDENTITY_CANDIDATE_MAX 3
#define JW_SCRAPE_IDENTITY_NAME_MAX 256

typedef struct {
    char names[JW_SCRAPE_IDENTITY_CANDIDATE_MAX]
              [JW_SCRAPE_IDENTITY_NAME_MAX];
    size_t count;
} jw_scrape_identity_candidates;

/* Build the descriptor-mode lookup ladder. An invalid descriptor simply
   omits the strongest candidate; the actual filename and effective title
   remain available as bounded fallbacks. */
void jw_scrape_identity_build(const char *descriptor_path,
                              const char *rom_name,
                              const char *effective_title,
                              const char *lookup_extension,
                              jw_scrape_identity_candidates *out);

/* Build the lookup ladder for a folder game (EasyRPG's Folder Game/RPG_RT.ldb),
   searched by name only: the folder name, then the same name without a
   trailing version ("Dragon Destiny v1.2" -> "Dragon Destiny"). A version is
   a "v" number ("v2", "V1_20", "v0.123c") or a dotted one ("1.1", "0.10")
   after a space, '_' or '-'. A bare number ("Vampires Dawn 2") is part of
   the title and stays. */
void jw_scrape_identity_build_folder(const char *folder_name,
                                     jw_scrape_identity_candidates *out);

/* Filename mode's ladder before any manual name: the actual filename alone,
   which its one call searches by hash and then by name. */
void jw_scrape_identity_build_filename(const char *rom_name,
                                       jw_scrape_identity_candidates *out);

/* Append the last lookup for a game outside descriptor mode to names, the
   names its earlier lookups sent: the user's own Display Name with
   extension ("Super Mario Bros.nes" for smb.nes), kept as typed when it
   already ends in it. extension is NULL or "" for a name sent without one,
   such as a folder game's. Surrounding whitespace is dropped. Returns false,
   leaving names unchanged, when no name is left, it equals one already sent
   ignoring case (that request was already made), or it does not fit. */
bool jw_scrape_identity_add_manual_name(jw_scrape_identity_candidates *names,
                                        const char *manual_title,
                                        const char *extension);

#endif
