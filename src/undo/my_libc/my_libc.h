#ifndef PSM_SRC_UNDO_MY_LIBC_MY_LIBC_H
#define PSM_SRC_UNDO_MY_LIBC_MY_LIBC_H

#include <sys/types.h>

#include "prohibit_libc.h"

int my_fsync(int fd);
int my_close(int fd);
int my_open(const char *pathname, int flags, mode_t mode = 0);
int my_openat(int dirfd, const char *pathname, int flags, mode_t mode = 0);
int my_unlinkat(int fd, const char *name, int flag);
int my_ftruncate(int fd, off_t length);
ssize_t my_write(int fd, const void *buf, size_t count);
ssize_t my_read(int fd, void *buf, size_t count);
int my_renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath);
void *my_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int my_munmap(void *addr, size_t length);
int my_dup2(int oldfd, int newfd);
int my_setsid();
int my_mkdirat(int dirfd, const char *pathname, mode_t mode);
int my_getdents(int dirfd, void *dirp, int count);
int my_fstat(int fd, struct stat *statbuf);

#endif // PSM_SRC_UNDO_MY_LIBC_MY_LIBC_H
