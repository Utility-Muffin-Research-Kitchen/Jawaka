#ifndef JW_SCRAPE_IDENTITY_H
#define JW_SCRAPE_IDENTITY_H

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

#endif
