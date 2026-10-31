#ifndef JW_CORE_TITLE_ORDER_H
#define JW_CORE_TITLE_ORDER_H

#include <stddef.h>
#include <string.h>

/* The order titles and folder names are listed in: ASCII case folded, and runs
   of digits compared as numbers, so "Mega Man 2" comes before "Mega Man 10" and
   "Game 007" sits with "Game 7". Other bytes compare by value, which keeps
   non-ASCII titles in their usual relative order. Names that are still equal
   fall back to their raw bytes, so only identical names compare equal. */
static inline int jw_title_compare(const char *a, size_t a_len,
                                   const char *b, size_t b_len) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    size_t i = 0;
    size_t j = 0;
    while (i < a_len && j < b_len) {
        unsigned char cx = x[i];
        unsigned char cy = y[j];
        if (cx >= '0' && cx <= '9' && cy >= '0' && cy <= '9') {
            /* Without leading zeros the longer run is the larger number, however
               many digits it has, so nothing here can overflow. */
            while (i < a_len && x[i] == '0') i++;
            while (j < b_len && y[j] == '0') j++;
            size_t x_start = i;
            size_t y_start = j;
            while (i < a_len && x[i] >= '0' && x[i] <= '9') i++;
            while (j < b_len && y[j] >= '0' && y[j] <= '9') j++;
            size_t x_digits = i - x_start;
            size_t y_digits = j - y_start;
            if (x_digits != y_digits) {
                return x_digits < y_digits ? -1 : 1;
            }
            int digits = x_digits ? memcmp(x + x_start, y + y_start, x_digits) : 0;
            if (digits != 0) {
                return digits < 0 ? -1 : 1;
            }
            continue;
        }
        if (cx >= 'A' && cx <= 'Z') cx = (unsigned char)(cx - 'A' + 'a');
        if (cy >= 'A' && cy <= 'Z') cy = (unsigned char)(cy - 'A' + 'a');
        if (cx != cy) {
            return cx < cy ? -1 : 1;
        }
        i++;
        j++;
    }
    if (i < a_len) return 1;
    if (j < b_len) return -1;

    size_t common = a_len < b_len ? a_len : b_len;
    int bytes = common ? memcmp(a, b, common) : 0;
    if (bytes != 0) {
        return bytes < 0 ? -1 : 1;
    }
    return a_len == b_len ? 0 : (a_len < b_len ? -1 : 1);
}

#endif /* JW_CORE_TITLE_ORDER_H */
