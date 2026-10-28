#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include <ctype.h>
#include <sys/stat.h>

#define ets_printf printf

#ifdef _WIN32
#include <io.h>
#include <direct.h>
static inline size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t len = strlen(src);
    if (size > 0) {
        size_t copy_len = (len >= size) ? size - 1 : len;
        memcpy(dst, src, copy_len);
        dst[copy_len] = '\0';
    }
    return len;
}
static inline size_t strlcat(char *dst, const char *src, size_t size) {
    size_t dst_len = strlen(dst);
    size_t src_len = strlen(src);
    if (dst_len >= size) return size + src_len;
    size_t copy_len = size - dst_len - 1;
    if (src_len < copy_len) copy_len = src_len;
    memcpy(dst + dst_len, src, copy_len);
    dst[dst_len + copy_len] = '\0';
    return dst_len + src_len;
}
#define fsync(fd) _commit(fd)
#define mkdir(path, mode) _mkdir(path)
#endif
