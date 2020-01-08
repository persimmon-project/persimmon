#include <cerrno>

#include <unistd.h>

#include "mem_region/fg.h"
#include "state.h"

int undo_recover_foreground(int *p_tail) {
    if (!instrument_args.recovered) {
        return 0;
    }

    if (close(instrument_args.recovery_fds_btf[PIPE_WRITE_END]) != 0 ||
        close(instrument_args.recovery_fds_ftb[PIPE_READ_END]) != 0) {
        return errno;
    }

    int recv_fd = instrument_args.recovery_fds_btf[PIPE_READ_END];
    int ret = map_recovered_regions(instrument_args.pmem_path, recv_fd);
    if (ret != 0) {
        return ret;
    }

    int recovered_tail;
    int nread = read(recv_fd, &recovered_tail, sizeof(recovered_tail));
    if (nread < 0) {
        return errno;
    }
    if ((size_t)nread < sizeof(recovered_tail)) {
        return EINVAL;
    }
    if (close(recv_fd) != 0) {
        return errno;
    }

    int send_fd = instrument_args.recovery_fds_ftb[PIPE_WRITE_END];
    const char buf = '\0';
    int nwritten = write(send_fd, &buf, 1);
    if (nwritten < 0) {
        return errno;
    }
    if (nwritten < 1) {
        return EINVAL;
    }
    if (close(send_fd) != 0) {
        return errno;
    }

    if (recovered_tail != -1) {
        if (recovered_tail < 0) {
            return EINVAL;
        }
        *p_tail = recovered_tail;
    }

    return 0;
}
