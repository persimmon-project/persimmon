#include <stddef.h>
#include <string.h>

void *__memcpy_chk(void *dest, const void *src, size_t len, size_t destlen) {
    // No checks here...
    return memcpy(dest, src, len);
}