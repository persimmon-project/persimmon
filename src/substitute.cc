#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

static char substitute_pm_path[256];
constexpr size_t MAX_PM_PATH_LEN = sizeof(substitute_pm_path);

// FIXME(zhangwen): finish this?

static void do_substitute([[gnu::unused]] int signum) {
    int pmem_fd = open(substitute_pm_path, O_CREAT | O_RDWR, 0666);
    if (pmem_fd == -1) {
        perror("open pmem_file");
        return;
    }

    FILE *map_f = fopen("/proc/self/maps", "r");
    if (map_f == nullptr) {
        perror("open maps");
        return;
    }

    char *line;
    ssize_t read = nullptr;
    while ((read = getline()) != -1) {

    }

    if (fclose(map_f) != 0) {
        perror("close maps");
        return;
    }

    if (close(pmem_fd) == -1) {
        perror("close pmem_file");
        return;
    }
}

/* Sets up a signal handler for `signum` that substitutes all writable pages of the current process with persistent
 * memory.  Note that this function does not account for any subsequent memory page allocations.  `path` is a file. */
int enable_pm_substitute(int signum, const char *path) {
    strncpy(substitute_pm_path, path, MAX_PM_PATH_LEN);
    if (substitute_pm_path[MAX_PM_PATH_LEN - 1] != '\0') {
        errno = EINVAL;
        perror("path too long");
        return -1;
    }

    struct sigaction action {};
    action.sa_handler = do_substitute;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (sigaction(signum, &action, nullptr) == -1) {
        perror("sigaction");
        return -1;
    }

    return 0;
}
