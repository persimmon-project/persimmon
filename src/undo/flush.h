#ifndef PSM_SRC_UNDO_FLUSH_H
#define PSM_SRC_UNDO_FLUSH_H

// Adapted from libpmem.

#include <cassert>
#include <cstdint>

#include <emmintrin.h>
#include <x86intrin.h>

// Flushes the cache line that contains addr.
[[gnu::always_inline]] void inline pmem_flush(const void *addr) { _mm_clwb(const_cast<void *>(addr)); }
// Flushes and invalidates the cache line that contains addr.
[[gnu::always_inline]] void inline pmem_flush_invalidate(const void *addr) {
    _mm_clflushopt(const_cast<void *>(addr));
}

[[gnu::always_inline]] void inline pmem_drain() { _mm_sfence(); }

void memset_movnt_avx(char *dest, int c, size_t len);

#define pmem_memset memset_movnt_avx

#endif // PSM_SRC_UNDO_FLUSH_H
