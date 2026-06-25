#include "internal/launcher/system_names.h"

#include "internal/db/db.h"

#include <stdio.h>
#include <strings.h>

/* Full console names for the system folder codes stored in the library. The
   metadata catalog still carries terse source labels for some systems, so these
   user-facing names stay here until metadata names are curated. Unknown ids
   fall back to the id itself. */
static const struct { const char *id; const char *name; } kSystemDisplayNames[] = {
    { "FC",      "Nintendo Entertainment System" },
    { "NES",     "Nintendo Entertainment System" },
    { "FDS",     "Famicom Disk System" },
    { "SFC",     "Super Nintendo" },
    { "SNES",    "Super Nintendo" },
    { "SFC_JP",  "Super Famicom" },
    { "N64",     "Nintendo 64" },
    { "GB",      "Game Boy" },
    { "GBC",     "Game Boy Color" },
    { "GBA",     "Game Boy Advance" },
    { "MGBA",    "Game Boy Advance" },
    { "SGB",     "Super Game Boy" },
    { "SUPA",    "Super Nintendo" },
    { "NDS",     "Nintendo DS" },
    { "VB",      "Virtual Boy" },
    { "MD",      "Sega Genesis" },
    { "GEN",     "Sega Genesis" },
    { "GENESIS", "Sega Genesis" },
    { "MS",      "Sega Master System" },
    { "GG",      "Game Gear" },
    { "SEGACD",  "Sega CD" },
    { "32X",     "Sega 32X" },
    { "SATURN",  "Sega Saturn" },
    { "DC",      "Dreamcast" },
    { "SG1000",  "SG-1000" },
    { "PCE",     "TurboGrafx-16" },
    { "TG16",    "TurboGrafx-16" },
    { "PCECD",   "TurboGrafx-CD" },
    { "NEOGEO",  "Neo Geo" },
    { "NGP",     "Neo Geo Pocket" },
    { "NGPC",    "Neo Geo Pocket Color" },
    { "WS",      "WonderSwan" },
    { "WSC",     "WonderSwan Color" },
    { "PS",      "PlayStation" },
    { "PSX",     "PlayStation" },
    { "PSP",     "PlayStation Portable" },
    { "ATARI",     "Atari 2600" },
    { "ATARI2600", "Atari 2600" },
    { "A2600",   "Atari 2600" },
    { "A5200",   "Atari 5200" },
    { "A7800",   "Atari 7800" },
    { "PROSYSTEM", "Atari 7800" },
    { "SEVENTYEIGHTHUNDRED", "Atari 7800" },
    { "LYNX",    "Atari Lynx" },
    { "JAGUAR",  "Atari Jaguar" },
    { "COLECO",  "ColecoVision" },
    { "INTV",    "Intellivision" },
    { "VECTREX", "Vectrex" },
    { "C64",     "Commodore 64" },
    { "AMIGA",   "Amiga" },
    { "DOS",     "MS-DOS" },
    { "MSX",     "MSX" },
    { "ARCADE",  "Arcade" },
    { "MAME",    "Arcade" },
    { "FBNEO",   "Arcade" },
    { "PORTS",   "Ports" },
};

void jw_system_display_name(const char *db_path,
                            const char *system_id,
                            char *out,
                            size_t out_size) {
    if (out_size == 0) {
        return;
    }
    snprintf(out, out_size, "%s", system_id ? system_id : "");
    if (!system_id || !system_id[0]) {
        return;
    }

    char override[64];
    if (db_path &&
        jw_db_get_system_setting(db_path, system_id,
                                 JW_CONTENT_SETTING_DISPLAY_NAME,
                                 override, sizeof(override)) == 0 &&
        override[0]) {
        snprintf(out, out_size, "%s", override);
        return;
    }

    for (size_t i = 0; i < sizeof(kSystemDisplayNames) / sizeof(kSystemDisplayNames[0]); i++) {
        if (strcasecmp(system_id, kSystemDisplayNames[i].id) == 0) {
            snprintf(out, out_size, "%s", kSystemDisplayNames[i].name);
            return;
        }
    }
}
