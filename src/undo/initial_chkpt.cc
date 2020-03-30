#include <cerrno>
#include <csetjmp>

#include <fcntl.h>
#include <unistd.h>

#include "undo_bg.h"

// We don't really need to use custom syscall wrappers here,
// since the initial checkpoint is taken before any instrumentation.
// However, we use them anyway to eliminate any reference to
// libc syscall wrappers in the background code (for easy inspection).
#include "my_libc/my_libc.h"

#include <criu/criu.h>

const char *LOG_FILE_NAME = "std.log";
const char *IMG_DIR_NAME = "initial_chkpt";

#define MUST_SUCCEED(x)                                                                                               \
    do {                                                                                                              \
        int _ret = (x);                                                                                               \
        if (_ret < 0)                                                                                                 \
            return -_ret;                                                                                             \
    } while (0)

int take_initial_chkpt(jmp_buf recovery_point) {
    { // Redirect stdin to /dev/null.
        int dev_null_ro;
        MUST_SUCCEED(dev_null_ro = my_open("/dev/null", O_RDONLY));
        MUST_SUCCEED(my_dup2(dev_null_ro, STDIN_FILENO));
        MUST_SUCCEED(my_close(dev_null_ro));
    }

    { // Redirect stdout and stderr to /dev/null.
        int dev_null_wo;
        MUST_SUCCEED(dev_null_wo = my_open("/dev/null", O_WRONLY));
        MUST_SUCCEED(my_dup2(dev_null_wo, STDOUT_FILENO));
        MUST_SUCCEED(my_dup2(dev_null_wo, STDERR_FILENO));
        MUST_SUCCEED(my_close(dev_null_wo));
    }

    MUST_SUCCEED(my_setsid());

    int dirfd;
    MUST_SUCCEED(dirfd = my_open(instrument_args.pmem_path, O_DIRECTORY));

    // Initialize criu.
    if (int res = criu_init_opts(); res != 0) {
        return res;
    }
    if (int res = criu_set_service_address(instrument_args.criu_service_path); res != 0) {
        return res;
    }
    criu_set_work_dir_fd(dirfd);
    criu_set_log_file("dump.log");
    criu_set_log_level(4);
    criu_set_leave_running(true);

    // Create and set directory for initial checkpoint.
    if (int ret = my_mkdirat(dirfd, IMG_DIR_NAME, 0666); ret < 0 && ret != -EEXIST) {
        return -ret;
    }
    // In the case of `EEXIST`, it's possible that the file exists but is not a directory,
    // in which case the following `open` would fail due to the `O_DIRECTORY` flag.
    int imgs_dirfd;
    MUST_SUCCEED(imgs_dirfd = my_openat(dirfd, IMG_DIR_NAME, O_DIRECTORY));
    criu_set_images_dir_fd(imgs_dirfd);

    // Since stdin, stdout, and stderr have all been redirected to /dev/null,
    // upon recovery criu_restore() will not complain about these file descriptors.
    int res = criu_dump();
    MUST_SUCCEED(my_close(imgs_dirfd));

    if (res < 0) {
        return res;
    }

    {
        // Now, redirect stdout and stderr to an actual file so we can inspect the outputs
        // of the background process.
        int log_fd;
        MUST_SUCCEED(log_fd = my_openat(dirfd, LOG_FILE_NAME, O_WRONLY | O_CREAT | O_APPEND, 0666));
        MUST_SUCCEED(my_dup2(log_fd, STDOUT_FILENO));
        MUST_SUCCEED(my_dup2(log_fd, STDERR_FILENO));
        MUST_SUCCEED(my_close(log_fd));
    }
    MUST_SUCCEED(my_close(dirfd));

    if (res > 0) { // Restored.
        longjmp(recovery_point, 42);
    }

    return 0;
}
