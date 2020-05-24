#include <libpsm/psm.h>

#include <atomic>
#include <cassert>

#include "bg.h"
#include "chkpt/chkpt.h"
#include "internal.h"
#include "undo/undo_bg.h"

#define PRINT_BG_THROUGHPUT 0

#define BATCH_COMMIT 0

// TODO(zhangwen): Better batching scheme?  Also, these numbers were picked arbitrarily.
constexpr int COMMIT_BATCH = 1;

// Commit after this many idle spin loops.
// This prevents "deadlocks" where the log has insufficient space left but the
// background process doesn't clear the log.
// FIXME(zhangwen): pick this number less arbitrarily?
constexpr int IDLE_SPIN = 10;

template <typename F>[[nodiscard]] static size_t _bg_consume(psm_t *psm, F f, size_t tail) {
    assert_not_instrumented();
    assert(nullptr != psm);

#if PRINT_BG_THROUGHPUT
    static uint64_t commands_since_last_report = 0;
    static uint64_t last_report_ns = 0;

    uint64_t now_ns = dmtr_now_ns();
    uint64_t elapsed_ns = now_ns - last_report_ns;
    if (elapsed_ns > 500000000ull) { // Half a second.
        unsigned long rps = commands_since_last_report * 1000000000ul / elapsed_ns;
        instrument_log("[bg: _bg_consume] throughput: %lu rps\n", rps);
        last_report_ns = now_ns;
        commands_since_last_report = 0;
    }
#endif

    int consumed = 0;
#if BATCH_COMMIT
    while ((psm->mode == PSM_MODE_UNDO && !instrument_args.should_commit) ||
        (psm->mode != PSM_MODE_UNDO && consumed < COMMIT_BATCH)) {
#else
    while (consumed < 1) {
#endif
        size_t head;
        int new_tail;
        uint64_t spin = 0;
        do {
            head = psm->head.load(std::memory_order_acquire);
            new_tail = consume(psm, f, head, tail);
            ++spin;
        } while (new_tail == -1 && (spin < IDLE_SPIN || consumed == 0));

        if (new_tail == -1) { // We've been spinning for too long.  Just commit.
            break;
        }

#if PSM_LOGGING
        if (psm->mode == PSM_MODE_UNDO) {
            instrument_log("[bg: _bg_consume] PSM consume\ttail = %lu\thead = %lu\n", tail, head);
        }
#endif
        ++consumed;
        tail = new_tail;
    }
    instrument_args.should_commit = false;

#if PSM_LOGGING
    if (psm->mode == PSM_MODE_UNDO) {
        instrument_log("[bg: _bg_consume] PSM commit\t%d command(s) consumed\n", consumed);
    }
#endif
    switch (psm->mode) {
    case PSM_MODE_NO_PERSIST:
        break;
    case PSM_MODE_UNDO:
        instrument_commit(tail);
        break;
    case PSM_MODE_CHKPT:
        chkpt_commit(psm->state.chkpt);
        break;
    default:
        __builtin_unreachable();
    }

    psm->update_tail(tail);

    // FIXME(zhangwen): have some API for implementation strategies.
    if (psm->mode == PSM_MODE_UNDO) {
        instrument_cleanup();
    }

#if PRINT_BG_THROUGHPUT
    commands_since_last_report += consumed;
#endif

    return tail;
}

template <typename F>[[noreturn]] static void run_consumer(psm_t *psm, F consume_func) {
    assert_not_instrumented();

    size_t tail = psm->tail.load(std::memory_order_acquire);
    while (true) {
        tail = _bg_consume(psm, consume_func, tail);
    }
}

[[gnu::visibility("default")]] void bg_run(psm_t *psm, bool use_sga) {
    int res;
    switch (psm->mode) {
    case PSM_MODE_NO_PERSIST:
        res = 0;
        break;
    case PSM_MODE_UNDO:
        instrument_args.recovered_tail = psm->log->tail;
        instrument_args.should_commit = false;
        res = instrument_init();
        if (!instrument_args.recovered) {
            break;
        }

        if (instrument_args.recovered_tail != -1) { // Set by `dr_client_main`.
            psm->update_tail(instrument_args.recovered_tail);
        }
        break;
    case PSM_MODE_CHKPT:
        res = chkpt_init(psm->state.chkpt);
        break;
    default:
        __builtin_unreachable();
    }
    if (res != 0) {
        abort();
    }

    if (use_sga) {
        run_consumer(psm, [psm](const void *buf) {
            const char *p = (const char *)buf;
            psm_sgarray_t sga;

            memcpy(&sga.num_segs, p, sizeof(sga.num_segs));
            p += sizeof(sga.num_segs);
            for (int i = 0; i < sga.num_segs; i++) {
                psm_sgaseg_t *seg = &sga.segs[i];
                memcpy(&seg->len, p, sizeof(seg->len));
                p += sizeof(seg->len);
                seg->buf = p;
                p += seg->len;
            }

            psm->consume_func(&sga);
            return p - (const char *)buf;
        });
    } else {
        run_consumer(psm, psm->consume_func);
    }
    __builtin_unreachable();
}
