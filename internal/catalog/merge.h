#ifndef JW_CATALOG_MERGE_H
#define JW_CATALOG_MERGE_H

typedef struct cJSON cJSON;

/* Pure CONTENT-1 merge. Inputs are not modified. `contributors` is an array
   of {provider, provides}; outputs are newly allocated cJSON values. */
int jw_catalog_merge(const cJSON *base,
                     const cJSON *contributors,
                     cJSON **out_catalog,
                     cJSON **out_diagnostics);

#endif
