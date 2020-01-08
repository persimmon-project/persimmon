#ifndef PSM_SRC_UNDO_STATE_H
#define PSM_SRC_UNDO_STATE_H

#include <csetjmp>

constexpr int PIPE_READ_END = 0;
constexpr int PIPE_WRITE_END = 1;

typedef struct {
    const char *pmem_path;
    void *psm_log_base;
    const char *criu_service_path;

    jmp_buf recovery_point;
    bool recovered;          // true if recovered from a previous execution.
    int recovery_fds_btf[2]; // background to foreground
    int recovery_fds_ftb[2]; // foreground to background
    int recovered_tail;

    bool should_commit;
} instrument_args_t;

extern instrument_args_t instrument_args;

#endif // PSM_SRC_UNDO_STATE_H
