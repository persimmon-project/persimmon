#include <atomic>
#include <cassert>
#include <cerrno>
#include <csetjmp>
#include <cstring>
#include <new>
#include <string>

#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>

#include <libpsm/psm.h>

#include <libpmem.h>

#include "bg.h"
#include "chkpt/chkpt.h"
#include "internal.h"
#include "undo/flush.h"
#include "undo/undo_fg.h"

static const char *PSM_LOG_FILE_NAME = "psm_log";
static constexpr char ZERO = '\0';

static psm_t _psm;

psm_log::psm_log() : head(0), tail(0), buf{} {
    pmem_flush(&head);
    pmem_flush(&tail);
    pmem_drain();
}

[[gnu::always_inline]] static inline int pin_thread_to_core(int id) {
    // Adapted from https://github.com/PlatformLab/PerfUtils.
    assert(id >= 0);
    cpu_set_t cpuset;

    CPU_ZERO(&cpuset);
    CPU_SET(id, &cpuset);
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0) {
        return errno;
    }

    return 0;
}

int psm_init(const psm_config_t *config) {
    // FIXME(zhangwen): assert that initialization hasn't occurred.
    if (config == nullptr) {
        return EINVAL;
    }

    // Create shared memory region.
    std::string log_file_path = std::string(config->pmem_path) + "/" + PSM_LOG_FILE_NAME;

    // TODO(zhangwen): do I need an fsync to flush file metadata?
    int is_pmem;
    void *mem = pmem_map_file(log_file_path.c_str(), sizeof(psm_log), PMEM_FILE_CREATE, 0666, nullptr, &is_pmem);
    if (nullptr == mem) {
        return errno;
    }
    if (!is_pmem) { // We require the log to be on persistent memory.
        return ENOTSUP;
    }

    _psm.log = new (mem) psm_log;
    _psm.mode = config->mode;
    _psm.local_head = _psm.local_tail = 0;

    if (config->consume_func == nullptr) {
        return EINVAL;
    }
    _psm.consume_func = config->consume_func;

    switch (config->mode) {
    case PSM_MODE_NO_PERSIST:
        break;
    case PSM_MODE_UNDO:
        instrument_args.pmem_path = config->pmem_path;
        instrument_args.psm_log_base = _psm.log;
        instrument_args.criu_service_path = config->undo.criu_service_path;
        if (setjmp(instrument_args.recovery_point) == 0) {
            instrument_args.recovered = false;
            // The initial checkpoint will be taken in the child after fork().
        } else {
            instrument_args.recovered = true;
            if (pipe(instrument_args.recovery_fds_ftb) != 0 || pipe(instrument_args.recovery_fds_btf) != 0) {
                return errno;
            }
            _psm.local_head = _psm.log->head;
            _psm.local_tail = _psm.log->tail;
        }
        break;
    case PSM_MODE_CHKPT:
        // FIXME(zhangwen): this mode probably doesn't work.
        auto state = new chkpt_state(&config->chkpt);
        _psm.state.chkpt = state;
        setjmp(state->restore_point);
        break;
    }

    int res = fork();
    if (res == -1) {
        return errno;
    } else if (res == 0) { // Child process.
        if (config->pin_core != -1) {
            int ret = pin_thread_to_core(config->pin_core);
            if (ret != 0) {
                return ret;
            }
        }

        bg_run(&_psm, config->use_sga);
    }

    // Re-execute any logged commands that haven't been replayed,
    // i.e., [initial_tail..initial_head).
    if (config->use_sga) {
        // I was too lazy to support SGA here -- just need to use the SGA consume function.
        // FIXME(zhangwen): support SGA?
        abort();
    }

    // In parent.
    if (config->mode == PSM_MODE_UNDO && instrument_args.recovered) {
        int tail;
        int ret = undo_recover_foreground(&tail);
        if (ret != 0) {
            return ret;
        }
#if PSM_LOGGING
        fprintf(stderr,
                "[fg: psm_init] Recovered!\tPSM log head = %d,\t"
                "PSM log tail = %d\n",
                _psm.log->head.load(), _psm.log->tail.load());
#endif

        const int head = _psm.log->head;
        int num_replayed = 0;
        while ((tail = consume(&_psm, _psm.consume_func, head, tail)) != -1) {
            // While we're looping, the background process might be replaying
            // these same commands and advancing `tail`.
            // This is fine -- we have saved the original `tail` in the local
            // `initial_tail` variable, and the background doesn't modify the
            // content of the log.
            ++num_replayed;
        }
#if PSM_LOGGING
        fprintf(stderr, "[fg: psm_init] Recovery -- replayed %d command(s)\n", num_replayed);
#endif
    }

    return 0;
}

void *psm_reserve(size_t len) {
    len = align_to_cache_line_size(len);

    assert(len > 0 && "must reserve a non-zero number of bytes");
    assert(len <= PSM_LOG_SIZE_B - 1 && "log entry length exceeds log length");

    psm_log *log = _psm.log;
    size_t local_head = _psm.local_head;
    bool truncated_tail = false;
    if (local_head + len > PSM_LOG_SIZE_B) { // FIXME(zhangwen): do this properly.
        // The consecutive space after `local_head` is not enough.
        // Discard this space and start from the front.
        truncated_tail = true;
        len += PSM_LOG_SIZE_B - local_head;
    }

    // Spin until there's enough free space starting from `local_head`.
    // FIXME(zhangwen): commit if there's not enough space in [local_head, head).
    size_t local_tail = _psm.local_tail;
    for (; (local_tail + PSM_LOG_SIZE_B - local_head - 1) % PSM_LOG_SIZE_B < len;
         local_tail = log->tail.load(std::memory_order_acquire))
        ;
    // FIXME(zhangwen): this might be slow.
    pmem_flush(&log->tail);
    pmem_drain();
    _psm.local_tail = local_tail;

    void *p = log->buf + local_head;
    assert((uintptr_t)p % CACHE_LINE_SIZE_B == 0 && "BUG: head pointer is not aligned");
    if (truncated_tail) {
        // Write a zero byte at `local_head` to signal that the space between here and
        // the end of the log is not used.
        memset(p, 0, /* n */ 1);
        pmem_flush_invalidate(p);
        p = log->buf; // Start from the front.
    }

    _psm.local_head = (local_head + len) % PSM_LOG_SIZE_B;
    return p;
}

void psm_push(const void *_src, size_t len) {
    auto src = static_cast<const char *>(_src);
    auto dest = static_cast<char *>(psm_reserve(len));
    pmem_memcpy_nodrain(dest, src, len);
}

void psm_commit(bool push_only) {
    psm_log *log = _psm.log;
    if (log->head == _psm.local_head)
        return;

#if PSM_LOGGING
    fprintf(stderr, "[fg: psm_commit] head = %d\tlocal_head = %d\ttail = %d\n", log->head.load(), _psm.local_head,
            log->tail.load());
#endif

    if (!push_only) {
        /* Flush log data [head .. local_head). */
        size_t end = _psm.local_head;
        for (size_t i = log->head; i != end; i = (i + CACHE_LINE_SIZE_B) % PSM_LOG_SIZE_B) {
            const void *p = log->buf + i;
            assert((uintptr_t)p % CACHE_LINE_SIZE_B == 0 && "p is not cache-line aligned");
            pmem_flush(p);
        }
    }
    pmem_drain();

    /* Update and persist head. */
    log->head.store(_psm.local_head, std::memory_order_release);
    pmem_flush(&log->head);
    pmem_drain();
}
