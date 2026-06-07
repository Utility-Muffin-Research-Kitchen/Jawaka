#ifndef JW_UPDATE_SHA256_H
#define JW_UPDATE_SHA256_H

#include <stddef.h>

int jw_sha256_file_hex(const char *path,
                       char out_hex[65],
                       char *error,
                       size_t error_size);

#endif /* JW_UPDATE_SHA256_H */
