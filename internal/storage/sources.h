#ifndef JW_STORAGE_SOURCES_H
#define JW_STORAGE_SOURCES_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>

#ifndef PATH_MAX
#define JW_STORAGE_PATH_MAX 4096
#else
#define JW_STORAGE_PATH_MAX PATH_MAX
#endif

#define JW_STORAGE_MAX_SOURCES 8
#define JW_STORAGE_SOURCE_ID_MAX 32

/* What can be said about which card is in a slot. */
typedef enum {
    JW_CARD_KEY_NONE = 0,    /* can't be told apart: not mounted, or a platform with
                                one source */
    JW_CARD_KEY_KNOWN,       /* card_key names this card */
    JW_CARD_KEY_UNREADABLE,  /* a FAT card whose boot sector can't be read right now */
} jw_card_key_state;

typedef struct {
    char id[JW_STORAGE_SOURCE_ID_MAX];
    char root[JW_STORAGE_PATH_MAX];
    char root_abs[JW_STORAGE_PATH_MAX];
    char userdata_path[JW_STORAGE_PATH_MAX];
    char shared_userdata_path[JW_STORAGE_PATH_MAX];
    char roms_path[JW_STORAGE_PATH_MAX];
    char images_path[JW_STORAGE_PATH_MAX];
    char music_path[JW_STORAGE_PATH_MAX];
    char video_path[JW_STORAGE_PATH_MAX];
    char apps_path[JW_STORAGE_PATH_MAX];
    char bios_path[JW_STORAGE_PATH_MAX];
    char saves_path[JW_STORAGE_PATH_MAX];
    char states_path[JW_STORAGE_PATH_MAX];
    char cheats_path[JW_STORAGE_PATH_MAX];
    bool primary;
    bool configured;
    bool available;
    unsigned long long device_id;
    unsigned long long roms_device_id;
    unsigned long long images_device_id;
    int mount_id;
    int roms_mount_id;
    int images_mount_id;
    char filesystem_fingerprint[128];
    char roms_filesystem_fingerprint[128];
    char images_filesystem_fingerprint[128];
    /* One spelling per card, unlike the fingerprints above, which can read as
       uuid: or fat: for the same card depending on whether the by-uuid link
       exists yet. Those stay as they are because records on cards store them. */
    char card_key[64];
    jw_card_key_state card_key_state;
} jw_storage_source;

typedef struct {
    jw_storage_source sources[JW_STORAGE_MAX_SOURCES];
    int count;
} jw_storage_source_list;

int jw_storage_sources_resolve(const char *primary_root, jw_storage_source_list *out);

/* True only for a complete PATH-2 environment: version 2, all five required
   aligned plural lists, byte-identical Primary singular aliases, unique
   absolute roots, and every per-source path contained by its indexed root. */
bool jw_storage_source_paths_v2_valid(void);

const jw_storage_source *jw_storage_sources_primary(const jw_storage_source_list *list);
const jw_storage_source *jw_storage_sources_find_for_path(const jw_storage_source_list *list,
                                                          const char *path);
const jw_storage_source *jw_storage_sources_find_by_id(const jw_storage_source_list *list,
                                                       const char *id);

int jw_storage_resolve_path(const jw_storage_source_list *list, const char *path,
                            char *out, size_t out_size);
int jw_storage_db_path_for_source(const jw_storage_source *source,
                                  const char *relative_path,
                                  const char *absolute_path,
                                  char *out, size_t out_size);
bool jw_storage_relative_path_valid(const char *path);
/* Writes a card's "fat:%08x" key from its 512-byte boot sector. A card with no
   extended boot signature has no serial, so it reads as a zero serial, and cards
   sharing a serial count as one card. False when this isn't a boot sector. */
bool jw_storage_card_key_from_boot_sector(const unsigned char *sector,
                                          char *out, size_t out_size);
int jw_storage_resolve_rom(const jw_storage_source *source,
                           const char *rom_relpath,
                           bool require_regular_file,
                           char *out, size_t out_size);
/* Resolve a BIOS-root-relative path to an absolute path inside that source's
   configured BIOS root. Containment is checked after realpath(), so a symlink
   or a ".." component that leaves the root fails rather than resolving. The
   caller still stats the result: this says where the file may live, not
   whether it is a usable image. */
int jw_storage_resolve_bios(const jw_storage_source *source,
                            const char *bios_relpath,
                            char *out, size_t out_size);

#endif /* JW_STORAGE_SOURCES_H */
