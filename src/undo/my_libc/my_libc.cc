#include "my_libc.h"

#include <sys/types.h>
#include <sys/syscall.h>

#define ARG_SYSCALL_NUM "a" // rax
#define ARG1 "D" // rdi
#define ARG2 "S" // rsi
#define ARG3 "d" // rdx
#define ARG4_REG "r10"
#define ARG5_REG "r8"
#define ARG6_REG "r9"

static inline ssize_t syscall(int syscall_num) {
    ssize_t ret;
    asm volatile("syscall"
    : "=" ARG_SYSCALL_NUM(ret)
    : "0"(syscall_num)
    : "cc", "rcx", "r11", "memory");
    return ret;
}

static inline ssize_t syscall(int syscall_num, ssize_t arg) {
    ssize_t ret;
    asm volatile("syscall"
    : "=" ARG_SYSCALL_NUM(ret)
    : "0"(syscall_num), ARG1(arg)
    : "cc", "rcx", "r11", "memory");
    return ret;
}

static inline ssize_t syscall(int syscall_num, ssize_t arg1, ssize_t arg2) {
    ssize_t ret;
    asm volatile("syscall"
    : "=" ARG_SYSCALL_NUM(ret)
    : "0"(syscall_num), ARG1(arg1), ARG2(arg2)
    : "cc", "rcx", "r11", "memory");
    return ret;
}

static inline ssize_t syscall(int syscall_num, ssize_t arg1, ssize_t arg2,
                              ssize_t arg3) {
    ssize_t ret;
    asm volatile("syscall"
    : "=" ARG_SYSCALL_NUM(ret)
    : "0"(syscall_num), ARG1(arg1), ARG2(arg2), ARG3(arg3)
    : "cc", "rcx", "r11", "memory");
    return ret;
}

static inline ssize_t syscall(int syscall_num, ssize_t arg1, ssize_t arg2,
                              ssize_t arg3, ssize_t arg4) {
    ssize_t ret;
    register ssize_t arg4_reg asm(ARG4_REG) = arg4;
    asm volatile("syscall"
    : "=" ARG_SYSCALL_NUM(ret)
    : "0"(syscall_num), ARG1(arg1), ARG2(arg2), ARG3(arg3),
        "r"(arg4_reg)
    : "cc", "rcx", "r11", "memory");
    return ret;
}

static inline ssize_t syscall(int syscall_num, ssize_t arg1, ssize_t arg2,
                              ssize_t arg3, ssize_t arg4, ssize_t arg5, ssize_t arg6) {
    ssize_t ret;
    register ssize_t arg4_reg asm(ARG4_REG) = arg4;
    register ssize_t arg5_reg asm(ARG5_REG) = arg5;
    register ssize_t arg6_reg asm(ARG6_REG) = arg6;
    asm volatile("syscall"
    : "=" ARG_SYSCALL_NUM(ret)
    : "0"(syscall_num), ARG1(arg1), ARG2(arg2), ARG3(arg3),
        "r"(arg4_reg), "r"(arg5_reg), "r"(arg6_reg)
    : "cc", "rcx", "r11", "memory");
    return ret;
}

int my_fsync(int fd) { return syscall(SYS_fsync, fd); }

int my_close(int fd) { return syscall(SYS_close, fd); }

int my_open(const char *pathname, int flags, mode_t mode) {
    return syscall(SYS_open, reinterpret_cast<ssize_t>(pathname), flags, mode);
}

int my_openat(int dirfd, const char *pathname, int flags, mode_t mode) {
    return syscall(SYS_openat, dirfd, reinterpret_cast<ssize_t>(pathname),
                   flags, mode);
}

int my_unlinkat(int fd, const char *name, int flag) {
    return syscall(SYS_unlinkat, fd, reinterpret_cast<ssize_t>(name), flag);
}

int my_ftruncate(int fd, off_t length) {
    return syscall(SYS_ftruncate, fd, length);
}

ssize_t my_write(int fd, const void *buf, size_t count) {
    return syscall(SYS_write, fd, reinterpret_cast<ssize_t>(buf), count);
}

ssize_t my_read(int fd, void *buf, size_t count) {
    return syscall(SYS_read, fd, reinterpret_cast<ssize_t>(buf), count);
}

int my_renameat(int olddirfd, const char *oldpath, int newdirfd,
                const char *newpath) {
    return syscall(SYS_renameat, olddirfd, reinterpret_cast<ssize_t>(oldpath),
                   newdirfd, reinterpret_cast<ssize_t>(newpath));
}

void *my_mmap(void *addr, size_t length, int prot, int flags, int fd,
              off_t offset) {
    auto ret = syscall(SYS_mmap, reinterpret_cast<ssize_t>(addr), length, prot,
                       flags, fd, offset);
    return reinterpret_cast<void *>(ret);
}

int my_munmap(void *addr, size_t length) {
    return syscall(SYS_munmap, reinterpret_cast<ssize_t>(addr), length);
}

int my_dup2(int oldfd, int newfd) {
    return syscall(SYS_dup2, oldfd, newfd);
}

int my_setsid() {
    return syscall(SYS_setsid);
}

int my_mkdirat(int dirfd, const char *pathname, mode_t mode) {
    return syscall(SYS_mkdirat, dirfd, reinterpret_cast<ssize_t>(pathname), mode);
}

int my_getdents(int dirfd, void *dirp, int count) {
    return syscall(SYS_getdents, dirfd, reinterpret_cast<ssize_t>(dirp), count);
}

int my_fstat(int fd, struct stat *statbuf) {
    return syscall(SYS_fstat, fd, reinterpret_cast<ssize_t>(statbuf));
}

