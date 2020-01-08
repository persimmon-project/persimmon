#include <cerrno>
#include <cstdio>

#include <fcntl.h>
#include <sys/mman.h>

#include "../my_libc/my_libc.h"
#include "common.h"

// This function should not use any memory other than the stack---
// because memory pages are getting replaced, this function can observe
// inconsistency in memory content between `mmap` and `read`.
int map_recovered_regions(const char *pmem_path, int pipe_fd) {
    int pmem_dirfd = my_open(pmem_path, O_DIRECTORY);
    if (pmem_dirfd < 0) {
        return -pmem_dirfd;
    }

    while (true) {
        region r(nullptr, 0);

        int nread = my_read(pipe_fd, &r, sizeof(r));
        if (nread < 0 || (size_t)nread < sizeof(r)) {
            return EINVAL;
        }

        if (r.base == nullptr && r.size == 0) { // Sentinel.
            break;
        }

        void *addr = my_mmap(r.base, r.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        // FIXME(zhangwen): what does `my_mmap` return on error?
        if (addr != r.base) {
            return EINVAL;
        }

        char file_name[FILE_NAME_BUF_LEN];
        make_file_name(file_name, r.base);
        int fd = my_openat(pmem_dirfd, file_name, O_RDWR);
        if (fd < 0) {
            return -fd;
        }

        int region_nread = my_read(fd, addr, r.size);
        if (region_nread < 0) {
            return -region_nread;
        }
        if ((size_t)region_nread < r.size) {
            return EAGAIN;
        }

        int ret = my_close(fd);
        if (ret != 0) {
            return -ret;
        }

#if MEM_REGION_LOGGING
        fprintf(stderr, "[fg: map_recovered_regions] mapped:\t%p\n", r.base);
#endif
    }

    int ret = my_close(pmem_dirfd);
    if (ret != 0) {
        return -ret;
    }
    return 0;
}
