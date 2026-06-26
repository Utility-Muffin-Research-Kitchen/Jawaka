#ifndef JW_STORE_CATALOG_SOURCE_H
#define JW_STORE_CATALOG_SOURCE_H

#include <stddef.h>

#define JW_PAKRAT_DEV_CATALOG_FILE "store/dev-catalog-url"

/* Resolves the Pak Rat catalog base URL. Production is intentionally unset in
   the local-first MVP, so an empty out buffer means "no catalog configured".
   Developer overrides are read from PAKRAT_CATALOG_BASE_URL first, then from
   <internal_data_path>/store/dev-catalog-url (or UMRK_INTERNAL_DATA_PATH when
   internal_data_path is NULL). Returns 0 on success, -1 on invalid input or a
   disallowed URL. */
int jw_pakrat_catalog_base_url(const char *internal_data_path,
                               char *out, size_t out_size,
                               int *out_is_dev_override);

/* HTTPS is always allowed. HTTP is allowed only when the URL came from a
   developer override source. Empty/unset URLs are allowed and mean disabled. */
int jw_pakrat_catalog_url_allowed(const char *url, int is_dev_override);

#endif
