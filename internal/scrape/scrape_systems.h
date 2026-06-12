#ifndef JW_SCRAPE_SYSTEMS_H
#define JW_SCRAPE_SYSTEMS_H

/* Map a Jawaka system folder tag (Roms/<TAG>/) to a screenscraper.fr
   platform id. Returns -1 for systems without a mapping (e.g. PORTS);
   callers skip those items instead of failing the run. */
int jw_scrape_platform_id(const char *system_tag);

#endif /* JW_SCRAPE_SYSTEMS_H */
