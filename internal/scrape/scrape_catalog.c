#include "internal/scrape/scrape_catalog.h"
#include "internal/i18n/i18n.h"

/* Catalog order is the order the settings editors list entries; defaults
   follow ScrapeGoat's. */

const jw_ss_option jw_ss_media_types[] = {
    {JW_UI("Screenshot (title)"),   "sstitle"},
    {JW_UI("Screenshot (in-game)"), "ss"},
    {JW_UI("Fan art"),              "fanart"},
    {JW_UI("Wheel"),                "wheel"},
    {JW_UI("Wheel (carbon)"),       "wheel-carbon"},
    {JW_UI("Wheel (steel)"),        "wheel-steel"},
    {JW_UI("Box art (2D)"),         "box-2D"},
    {JW_UI("Box art (3D)"),         "box-3D"},
    {"Mix Recalbox V1",      "mixrbv1"},
    {"Mix Recalbox V2",      "mixrbv2"},
};
const int jw_ss_media_types_count =
    (int)(sizeof(jw_ss_media_types) / sizeof(jw_ss_media_types[0]));

const jw_ss_option jw_ss_regions[] = {
    {JW_UI("World"),         "wor"},
    {JW_UI("USA"),           "us"},
    {JW_UI("Europe"),        "eu"},
    {JW_UI("Japan"),         "jp"},
    {JW_UI("France"),        "fr"},
    {JW_UI("Germany"),       "de"},
    {JW_UI("Spain"),         "es"},
    {JW_UI("Italy"),         "it"},
    {JW_UI("Portugal"),      "pt"},
    {"ScreenScraper", "ss"},
};
const int jw_ss_regions_count =
    (int)(sizeof(jw_ss_regions) / sizeof(jw_ss_regions[0]));

const char *const jw_ss_default_artwork_priority[] = {
    "box-2D", "box-3D", "mixrbv1", "ss",
};
const int jw_ss_default_artwork_priority_count =
    (int)(sizeof(jw_ss_default_artwork_priority) /
          sizeof(jw_ss_default_artwork_priority[0]));

const char *const jw_ss_default_region_priority[] = {
    "us", "eu", "jp", "wor",
};
const int jw_ss_default_region_priority_count =
    (int)(sizeof(jw_ss_default_region_priority) /
          sizeof(jw_ss_default_region_priority[0]));
