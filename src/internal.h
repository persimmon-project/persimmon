#ifndef PSM_INTERNAL_H
#define PSM_INTERNAL_H

#include <atomic>
#include <cstddef>

#include <libpsm/psm.h>

#include "undo/flush.h"

static constexpr size_t align_to_cache_line_size(size_t len) {
    return ((size_t)len + (CACHE_LINE_SIZE_B - 1)) & ~(CACHE_LINE_SIZE_B - 1);
}

struct psm_log {
    // The code assumes that `head` and `tail` do not straddle cache lines.
    alignas(CACHE_LINE_SIZE_B) std::atomic<size_t> head;
    alignas(CACHE_LINE_SIZE_B) std::atomic<size_t> tail;

    /* Circular buffer, where each log entry must be contiguous in memory. */
    alignas(CACHE_LINE_SIZE_B) char buf[PSM_LOG_SIZE_B];

    psm_log();
};

struct chkpt_state;

struct psm {
    psm_log *log; /* Persistent data. */
    psm_mode_t mode;
    union {
        chkpt_state *chkpt;
    } state;

    /* Used only by the producer. */
    size_t local_head;
    size_t local_tail;

    consume_func_t consume_func;
};

#define PSM_LOGGING 0

// Returns new tail (if an entry is consumed), or -1 if there's no entry to consume.
template <typename F>
[[gnu::always_inline, gnu::warn_unused_result]] static inline size_t consume(psm_t *psm, F f, size_t head,
                                                                             size_t tail) {
    static const char ZERO = '\0';

    if (tail == head) {
        return -1;
    }

    const struct psm_log *const log = psm->log;
    const void *entry = log->buf + tail;
    if (memcmp(entry, &ZERO, /* n */ 1) == 0) {
        // This is padding.  Skip over it.
        assert(tail > head);
        tail = 0;
        return consume(psm, f, head, tail);
    }

    assert(reinterpret_cast<uintptr_t>(entry) % CACHE_LINE_SIZE_B == 0 && "BUG: tail pointer is not aligned");
    int consumed_len = align_to_cache_line_size(f(entry));
    assert(tail + consumed_len <= PSM_LOG_SIZE_B);
    tail = (tail + consumed_len) % PSM_LOG_SIZE_B;
    return tail;
}

#endif // PSM_INTERNAL_H
