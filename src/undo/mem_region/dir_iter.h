#ifndef PSM_SRC_UNDO_MEM_REGION_DIR_ITER_H
#define PSM_SRC_UNDO_MEM_REGION_DIR_ITER_H

#include <sys/types.h>

// Return 0 on success, errno on failure.
// f takes (dirfd, filename) as argument and returns 0 on success, errno on failure.
template <typename F> int iterate_dir(const char *path, F f) {
    struct linux_dirent {
        long d_ino;
        off_t d_off;
        unsigned short d_reclen;
        char d_name[];
    };

    char buf[1024];

    int dirfd = my_open(path, O_DIRECTORY);
    if (dirfd < 0) {
        return -dirfd;
    }

    while (true) {
        int nread = my_getdents(dirfd, buf, sizeof(buf));
        if (nread < 0) {
            my_close(dirfd);
            return -nread;
        }

        if (nread == 0) {
            break;
        }

        for (int bpos = 0; bpos < nread;) {
            auto d = reinterpret_cast<linux_dirent *>(buf + bpos);
            int ret = f(dirfd, d->d_name);
            if (ret != 0) {
                my_close(dirfd);
                return ret;
            }
            bpos += d->d_reclen;
        }
    }

    my_close(dirfd);
    return 0;
}

#endif // PSM_SRC_UNDO_MEM_REGION_DIR_ITER_H
