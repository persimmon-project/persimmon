#include "chkpt.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include <boost/filesystem.hpp>
#include <criu/criu.h>

#define INCREMENTAL_DUMP 1

int chkpt_init(const chkpt_state *state) {
    // TODO(zhangwen): specify a log file?
    {
        int dev_null_ro = open("/dev/null", O_RDONLY);
        if (dev_null_ro == -1) {
            return errno;
        }
        if (dup2(dev_null_ro, STDIN_FILENO) == -1) {
            return errno;
        }
    }

    {
        int out_fd = open(state->log_path.c_str(), O_WRONLY | O_CREAT, 0666);
        if (out_fd == -1) {
            return errno;
        }
        if (dup2(out_fd, STDOUT_FILENO) == -1 || dup2(out_fd, STDERR_FILENO) == -1) {
            return errno;
        }
    }

    if (setsid() == -1) {
        return errno;
    }

    int res;
    if ((res = criu_init_opts()) != 0) {
        return res;
    }

    if ((res = criu_set_service_address(state->service_path.c_str())) != 0) {
        return res;
    }

    criu_set_log_level(4);
    criu_set_leave_running(true);
#if INCREMENTAL_DUMP
    criu_set_track_mem(true);
    criu_set_auto_dedup(true);
#endif

    return 0;
}

void chkpt_commit(chkpt_state *state) {
    auto start = std::chrono::steady_clock::now();

    boost::filesystem::path curr_imgs_dir = state->imgs_dir;
    curr_imgs_dir /= std::to_string(state->seq);
    boost::filesystem::create_directories(curr_imgs_dir);

    int fd = open(curr_imgs_dir.c_str(), O_DIRECTORY);
    if (fd == -1) {
        perror("open");
        abort();
    }
    criu_set_images_dir_fd(fd);

#if INCREMENTAL_DUMP
    if (state->seq > 0) {
        boost::filesystem::path prev_imgs_dir{".."};
        prev_imgs_dir /= std::to_string(state->seq - 1);
        criu_set_parent_images(prev_imgs_dir.c_str());
    }
#endif

    int res = criu_dump();
    state->seq++;

    auto end = std::chrono::steady_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    fprintf(stderr, "dump: %lu\n", duration_ms);

    if (res == 0) { // Dumped.
        return;
    }
    if (res > 0) { // Restored.
        longjmp(state->restore_point, 42);
    }

    // Failed.
    abort();
}
