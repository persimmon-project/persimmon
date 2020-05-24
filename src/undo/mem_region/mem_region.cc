#include <cstdint>
#include <new>
#include <limits>

#include <fcntl.h>
#include <sys/mman.h>

#include "dr_api.h"
#include "dr_tools.h"
#include "drvector.h"

#include "mem_region.h"
#include "common.h"

#include "../my_libc/my_libc.h"

using result = mem_region_manager::result;

mem_region_manager::mem_region_manager(const char *_pmem_path)
    : pmem_path(_pmem_path), pmem_dirfd(my_open(_pmem_path, O_DIRECTORY)), regions{} {
    DR_ASSERT_MSG(pmem_dirfd >= 0, "open directory");

    bool success = drvector_init(&regions, /* initial capacity */ 10, false, nullptr);
    DR_ASSERT(success);
}

mem_region_manager::~mem_region_manager() {
    drvector_delete(&regions);
    my_close(pmem_dirfd);
}

void mem_region_manager::recover() {
    int table_fd = my_openat(pmem_dirfd, CURRENT_TABLE_FILE_NAME, O_RDONLY);
    DR_ASSERT_MSG(table_fd >= 0, "open region table file failed");

    region r(nullptr, 0, 0);
    int nread;
    while ((nread = my_read(table_fd, &r, sizeof(r))) > 0) {
        DR_ASSERT_MSG(static_cast<size_t>(nread) == sizeof(r), "read too little");

        char file_name[FILE_NAME_BUF_LEN];
        r.make_file_name(file_name);
        int fd = my_openat(pmem_dirfd, file_name, O_RDWR);
        DR_ASSERT_MSG(fd >= 0, "open region file failed");

        void *ret = my_mmap(r.base, r.size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED_VALIDATE | MAP_SYNC, fd,
            /* offset */ 0);
        DR_ASSERT_MSG(ret != MAP_FAILED, "mmap memory region file failed");
        DR_ASSERT_MSG(ret == r.base, "mmap returned a different address?");

        if (my_close(fd) < 0) {
            DR_ASSERT_MSG(false, "close region file failed");
        }

        void *mem = dr_global_alloc(sizeof(region));
        drvector_append(&regions, new (mem) region(r));
        rs.insert(reinterpret_cast<uintptr_t>(r.base), r.size);

#if MEM_REGION_LOGGING
        dr_fprintf(STDERR, "[bg: mem_region_manager::recover] region recovered:\t\t%lx-%lx\n", r.base, r.base + r.size);
#endif
    }
    DR_ASSERT_MSG(nread == 0, "read region table failed");
    if (my_close(table_fd) < 0) {
        DR_ASSERT_MSG(false, "close region table file failed");
    }

#if MEM_REGION_LOGGING
    dr_fprintf(STDERR, "[bg: mem_region_manager::recover] memory region manager recovery done!\n");
#endif
}

void mem_region_manager::send_regions(int fd) const {
    for (uint i = 0; i < regions.entries; i++) {
        auto r = static_cast<const region *>(regions.array[i]);
        int written = my_write(fd, r, sizeof(*r));
        DR_ASSERT_MSG(written >= 0, "send_regions: write failed");
        DR_ASSERT_MSG(written == sizeof(*r), "send_regions: written less than asked");
    }

    region sentinel(nullptr, 0, 0);
    int written = my_write(fd, &sentinel, sizeof(sentinel));
    DR_ASSERT_MSG(written >= 0, "send_regions: write sentinel failed");
    DR_ASSERT_MSG(written == sizeof(sentinel), "send_regions: written less than asked");
}

bool mem_region_manager::does_manage(app_pc addr) const { return rs.find(reinterpret_cast<uintptr_t>(addr)); }

int mem_region_manager::find_overlap(const region &other) const {
    for (uint i = 0; i < regions.entries; i++) {
        const auto *r = static_cast<const region *>(regions.array[i]);
        if (r->does_overlap_with(other)) {
            return i;
        }
    }

    return -1;
}

static void print_error(const char *description, int err) {
    dr_fprintf(STDERR, "ERROR: %s\terrno = %d\n", description, err);
}

// Writes a memory region to the persistent memory file system.
// Returns an open file descriptor to the file (RDWR), or -1 on error.
int mem_region_manager::persist_region(app_pc base, size_t size, char *file_name) const {
    int fd = my_openat(pmem_dirfd, file_name, O_CREAT | O_RDWR | O_EXCL, 0666);
    if (fd < 0) {
        print_error("persist_region -- openat", -fd);
        goto err;
    }

    {
        app_pc write_from = base;
        size_t to_write = size;
        while (to_write > 0) {
            int nb = my_write(fd, write_from, to_write);
            if (nb <= 0) {
                print_error("persist_region -- write", -nb);
                goto err;
            }
            write_from += nb;
            to_write -= nb;
        }
    }

    if (int ret = my_fsync(fd) < 0; ret < 0) {
        print_error("persist_region -- fsync(fd)", -ret);
        goto err;
    }

    if (int ret = my_fsync(pmem_dirfd); ret < 0) {
        print_error("persist_region -- fsync(pmem_dirfd)", -ret);
        goto err;
    }

    return fd;

err:
    if (fd >= 0) {
        my_close(fd);
    }
    return -1;
}

result mem_region_manager::replace_region(app_pc base, size_t size, int prot) {
    uint file_id = dr_get_random_value(std::numeric_limits<uint>::max());
    region r(base, size, file_id);
    DR_ASSERT(-1 == find_overlap(r));

    char file_name[FILE_NAME_BUF_LEN];
    r.make_file_name(file_name);

    int fd = persist_region(base, size, file_name);
    if (fd == -1) {
        return result::ERROR;
    }

    // Replace the memory region.  I hope DynamoRIO is fine with me doing this.
    {
        void *ret = my_mmap(base, size, prot, MAP_FIXED | MAP_SHARED_VALIDATE | MAP_SYNC, fd,
                            /* offset */ 0);
        if (ret != base) {
            auto ret_n = reinterpret_cast<size_t>(ret);
            DR_ASSERT(ret_n > -4096UL);
            print_error("replace_region -- mmap", -ret_n);
            return result::ERROR;
        }
    }

    if (int ret = my_close(fd); ret < 0) {
        print_error("replace_region -- close", ret);
        return result::ERROR;
    }

#if MEM_REGION_LOGGING
    dr_fprintf(STDERR, "region replaced:\t%lx-%lx\tfile_id = %x\n", reinterpret_cast<uintptr_t>(base),
               reinterpret_cast<uintptr_t>(base) + size, file_id);
#endif

    void *mem = dr_global_alloc(sizeof(region));
    drvector_append(&regions, new (mem) region(r));
    rs.insert(reinterpret_cast<uintptr_t>(base), size);
    return result::SUCCESS;
}

result mem_region_manager::remove_region(app_pc base, size_t size) {
    region remove_r(base, size, 0);
    int i = find_overlap(remove_r);
    if (i == -1) { // Not managed.  Ignore!
        return result::NOT_MANAGED;
    }
    auto r = static_cast<region *>(regions.array[i]);
    DR_ASSERT_MSG(r->does_include(remove_r), "doesn't support unmap across regions");
    // Remove r from our vector of regions.
    regions.array[i] = regions.array[--regions.entries];

    // First perform the `munmap`, then update our metadata.
    if (my_munmap(base, size) != 0) {
        return result::SUCCESS;
    }

    // FIXME(zhangwen): implement garbage collection.
    // FIXME(zhangwen): we don't remove the region from `rs`, which should be fine --
    // the application shouldn't access munmap'ed memory anyway.

    if (r->end() != remove_r.end()) {
        DR_ASSERT(r->end() > remove_r.end());
        auto new_size = r->end() - remove_r.end();
        // This will create a new file for region starting at `remove_r.end`.
        auto end = reinterpret_cast<app_pc>(remove_r.end());
        if (result::ERROR == replace_region(end, new_size, PROT_READ | PROT_WRITE)) {
            return result::ERROR;
        }
    }
    if (r->base != remove_r.base) {
        DR_ASSERT(r->base < remove_r.base);
        auto new_size = remove_r.base - r->base;
        auto start = reinterpret_cast<app_pc>(r->base);
        if (result::ERROR == replace_region(start, new_size, PROT_READ | PROT_WRITE)) {
            return result::ERROR;
        }
    }

#if MEM_REGION_LOGGING
    dr_fprintf(STDERR, "region removed:\t\t%lx-%lx\n", reinterpret_cast<uintptr_t>(base),
               reinterpret_cast<uintptr_t>(base) + size);
#endif

    dr_global_free(r, sizeof(*r));
    return result::SUCCESS;
}

result mem_region_manager::persist_new_region_table() {
    int fd = my_openat(pmem_dirfd, NEW_TABLE_FILE_NAME, O_CREAT | O_WRONLY);
    DR_ASSERT_MSG(fd >= 0, "open new region table file failed");

    for (uint i = 0; i < regions.entries; i++) {
        auto r = static_cast<const region *>(regions.array[i]);
        int written = my_write(fd, r, sizeof(*r));
        DR_ASSERT_MSG(written >= 0, "persist_new_region_table: write failed");
        DR_ASSERT_MSG(written == sizeof(*r), "persist_new_region_table: written less than asked");
    }

    if (my_fsync(fd) < 0) {
        DR_ASSERT_MSG(false, "persist_new_region_table: fsync(fd) failed");
    }

    if (my_close(fd) < 0) {
        DR_ASSERT_MSG(false, "close new region table file failed");
    }

    if (my_fsync(pmem_dirfd) < 0) {
        DR_ASSERT_MSG(false, "persist_new_region_table: fsync(pmem_dirfd) failed");
    }

    return result::SUCCESS;
}

result mem_region_manager::commit_new_region_table() {
    if (int ret = my_renameat(pmem_dirfd, NEW_TABLE_FILE_NAME, pmem_dirfd, CURRENT_TABLE_FILE_NAME); ret < 0) {
        if (ret == -ENOENT) {
            // There is nothing to commit.
            return result::SUCCESS;
        }
        print_error("commit_new_region_table -- renameat", -ret);
        return result::ERROR;
    }
    if (int ret = my_fsync(pmem_dirfd); ret < 0) {
        print_error("commit_new_region_table -- fsync(pmem_dirfd)", -ret);
        return result::ERROR;
    }
    return result::SUCCESS;
}

result mem_region_manager::clear_new_region_table() {
    if (int ret = my_unlinkat(pmem_dirfd, NEW_TABLE_FILE_NAME, /* flags */ 0); ret < 0) {
        if (ret == -ENOENT) {
            // There is nothing to commit.
            return result::SUCCESS;
        }
        print_error("clear_new_region_table -- unlink", -ret);
        return result::ERROR;
    }
    if (int ret = my_fsync(pmem_dirfd); ret < 0) {
        print_error("clear_new_region_table -- fsync(pmem_dirfd)", -ret);
        return result::ERROR;
    }
    return result::SUCCESS;
}
