#ifndef JW_UPDATE_SHA256_H
#define JW_UPDATE_SHA256_H

#include <stddef.h>

int jw_sha256_file_hex(const char *path,
                       char out_hex[65],
                       char *error,
                       size_t error_size);

/* One-shot SHA-256 of an in-memory buffer, written as a 64-char lowercase hex
   string (NUL-terminated) into out_hex. */
void jw_sha256_buf_hex(const void *data, size_t len, char out_hex[65]);

#endif /* JW_UPDATE_SHA256_H */
