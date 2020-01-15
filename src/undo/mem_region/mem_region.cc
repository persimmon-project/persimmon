#include <cstdint>
#include <cstring> // Only use string functions that don't do weird things (like memory allocations).
#include <new>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "dr_api.h"
#include "dr_tools.h"
#include "drvector.h"

#include "mem_region.h"
#include "common.h"

#include "../my_libc/my_libc.h"
#include "dir_iter.h"

using result = mem_region_manager::result;

static char *dr_global_strdup(const char *s) {
    int len = strlen(s);
    char *a = static_cast<char *>(dr_global_alloc(len + 1));
    strcpy(a, s);
    return a;
}

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
    // Gather names of files to delete here, to avoid deleting them while
    // iterating.
    drvector_t files_to_delete;
    bool success = drvector_init(&files_to_delete, /* initial capacity */ 0, false, nullptr);
    DR_ASSERT(success);

    iterate_dir(pmem_path, [this, &files_to_delete](int dirfd, const char *file_name) -> int {
        uintptr_t n_base;
        int n = dr_sscanf(file_name, FILE_NAME_FORMAT, &n_base);
        DR_ASSERT_MSG(n >= 0, "dr_sscanf failed");
        if (n < 1) { // Not a file we're interested in.
            return 0;
        }
        auto base = reinterpret_cast<app_pc>(n_base);

        int fd = my_openat(dirfd, file_name, O_RDWR);
        DR_ASSERT_MSG(fd >= 0, "open memory region file failed");

        struct stat st;
        if (my_fstat(fd, &st) < 0) {
            DR_ASSERT_MSG(false, "stat memory region file failed");
        }
        off_t size = st.st_size;

        region new_r(base, size);
        if (int i = find_overlap(new_r); i != -1) {
            // If an overlapping region exists, one must include the
            // other. We keep the larger region (see comments in
            // `remove_region`).
            auto existing_r = static_cast<const region *>(regions.array[i]);
            if (existing_r->does_include(new_r)) {
#if MEM_REGION_LOGGING
                dr_fprintf(STDERR, "region file deleted:\t\t%lx-%lx\n", base, base + size);
#endif
                drvector_append(&files_to_delete, dr_global_strdup(file_name));
                return 0;
            }

            DR_ASSERT_MSG(new_r.does_include(existing_r), "regions overlap but neither includes the other");
            // We will map the new region in favor of the existing
            // one. Mark the existing region for deletion.
            char existing_file_name[FILE_NAME_BUF_LEN];
            make_file_name(existing_file_name, existing_r->base);
            drvector_append(&files_to_delete, dr_global_strdup(existing_file_name));
#if MEM_REGION_LOGGING
            dr_fprintf(STDERR, "region file deleted:\t\t%lx-%lx\n", existing_r->base,
                       existing_r->base + existing_r->size);
#endif
        }

        {
            void *ret = my_mmap(base, size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED_VALIDATE | MAP_SYNC, fd,
                                /* offset */ 0);
            DR_ASSERT_MSG(ret != MAP_FAILED, "mmap memory region file failed");
            DR_ASSERT_MSG(ret == base, "mmap returned a different address?");
        }
        if (my_close(fd) < 0) {
            DR_ASSERT_MSG(false, "close memory region file failed");
        }

#if MEM_REGION_LOGGING
        dr_fprintf(STDERR, "[bg: mem_region_manager::recover] region recovered:\t\t%lx-%lx\n", base, base + size);
#endif

        {
            void *mem = dr_global_alloc(sizeof(region));
            drvector_append(&regions, new (mem) region(base, size));
            rs.insert(reinterpret_cast<uintptr_t>(base), size);
        }
        return 0;
    });

    for (uint i = 0; i < files_to_delete.entries; i++) {
        char *file_name = static_cast<char *>(files_to_delete.array[i]);
        if (my_unlinkat(pmem_dirfd, file_name, /* flags */ 0) < 0) {
            DR_ASSERT_MSG(false, "unlink duplicate region file failed");
        }
        dr_global_free(file_name, strlen(file_name) + 1);
    }
    drvector_delete(&files_to_delete);

    // We might have deleted some files.  Persist!
    if (my_fsync(pmem_dirfd) < 0) {
        DR_ASSERT_MSG(false, "fsync pmem_dir failed");
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

    region sentinel(nullptr, 0);
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
int mem_region_manager::persist_region(app_pc base, size_t size) const {
    int fd = my_openat(pmem_dirfd, TEMP_FILE_NAME, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        print_error("persist_region -- openat", -fd);
        goto err;
    }

    if (int nb = my_write(fd, base, size); nb < 0) {
        print_error("persist_region -- write", -nb);
        goto err;
    } else if (static_cast<size_t>(nb) != size) {
        // I'm too lazy to deal with the case where 0 <= nb < size.
        DR_ASSERT_MSG(false, "persist_region: write is not complete");
    }

    if (int ret = my_fsync(fd) < 0; ret < 0) {
        print_error("persist_region -- fsync(fd)", -ret);
        goto err;
    }

    // Now, atomically rename the file to a name identifying the base address.
    {
        char file_name[FILE_NAME_BUF_LEN];
        make_file_name(file_name, base);
        if (int ret = my_renameat(pmem_dirfd, TEMP_FILE_NAME, pmem_dirfd, file_name); ret < 0) {
            print_error("persist_region -- renameat", -ret);
            goto err;
        }
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
    region replace_r(base, size);
    DR_ASSERT(-1 == find_overlap(replace_r));

    int fd = persist_region(base, size);
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
    dr_fprintf(STDERR, "region replaced:\t%lx-%lx\n", reinterpret_cast<uintptr_t>(base),
               reinterpret_cast<uintptr_t>(base) + size);
#endif

    void *new_r = dr_global_alloc(sizeof(region));
    drvector_append(&regions, new (new_r) region(base, size));
    rs.insert(reinterpret_cast<uintptr_t>(base), size);
    return result::SUCCESS;
}

result mem_region_manager::remove_region(app_pc base, size_t size) {
    region remove_r(base, size);
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

    char r_file_name[FILE_NAME_BUF_LEN];
    make_file_name(r_file_name, r->base);

    // FIXME(zhangwen): we don't remove the region from `rs`, which should be fine --
    // the application shouldn't access munmap'ed memory anyway.

    if (r->end() != remove_r.end()) {
        DR_ASSERT(r->end() > remove_r.end());
        // TODO(zhangwen): this copy isn't awfully efficient.
        auto new_size = r->end() - remove_r.end();
        // This will create a new file for region starting at `remove_r.end`.
        auto end = reinterpret_cast<app_pc>(remove_r.end());
        if (result::ERROR == replace_region(end, new_size, PROT_READ | PROT_WRITE)) {
            return result::ERROR;
        }
    }
    // FIXME(zhangwen): recovery---a crash around here can result in overlapping region files;
    //      should keep the larger region.
    if (r->base != remove_r.base) {
        DR_ASSERT(r->base < remove_r.base);
        // Some portion of the original region (at the front) remains.
        // Simply truncate the file.
        int fd = my_openat(pmem_dirfd, r_file_name, O_WRONLY);
        if (fd < 0) {
            return result::ERROR;
        }
        auto new_size = remove_r.base - r->base;
        if (my_ftruncate(fd, new_size) < 0) {
            return result::ERROR;
        }
        if (my_fsync(fd) < 0) {
            return result::ERROR;
        }
        if (my_close(fd) < 0) {
            return result::ERROR;
        }

        // Add back a region for this truncated entry.
        void *new_r = dr_global_alloc(sizeof(region));
        drvector_append(&regions, new (new_r) region(r->base, new_size));
        rs.insert(reinterpret_cast<uintptr_t>(r->base), new_size);
    } else {
        // The useful parts of the original region file (possibly the tail) has been duplicated.
        // We can now delete the orignal file.
        if (my_unlinkat(pmem_dirfd, r_file_name, /* flags */ 0) < 0) {
            return result::ERROR;
        }

        if (my_fsync(pmem_dirfd) < 0) {
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
