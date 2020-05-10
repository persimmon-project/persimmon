#ifndef PSM_SRC_UNDO_MEM_REGION_MEM_REGION_H
#define PSM_SRC_UNDO_MEM_REGION_MEM_REGION_H

#include <cstddef>
#include <string>

#include "drvector.h"

#include "common.h"
#include "ranges.h"

class mem_region_manager {
  public:
    enum class result : int {
        SUCCESS = 0,     // Action carried out successfully.
        NOT_MANAGED = 1, // No action taken -- memory is not managed.
        ERROR = -1,
    };

    explicit mem_region_manager(const char *pmem_path);
    ~mem_region_manager();

    void recover();

    // Sends regions to recover to the foreground (through send_fd),
    // and waits for foreground recovery to complete (through recv_fd).
    // The waiting is important because otherwise, a race condition can arise
    // where the foreground is recovering from a memory region file while
    // the background is writing to it (e.g., when re-executing logged commands.)
    void send_regions(int fd) const;

    // Replaces a memory region with one backed by non-volatile memory.  Content is preserved.
    // Only returns result::SUCCESS or result::ERROR.
    result replace_region(app_pc base, size_t size, int prot);

    // Removes a memory region if it is managed by us.
    // TODO(zhangwen): currently only supports unmapping from a beginning of
    //      an existing region.
    result remove_region(app_pc base, size_t size);

    bool does_manage(app_pc addr) const;

    // Persist the new (modified) region table.  After this function returns, can commit.
    result persist_new_region_table();

    // Commit and clear the new region table.  No-op if there's no new region table.
    result commit_new_region_table();

    // Clear the new region table.  No-op if there's no new region table.
    result clear_new_region_table();

  private:
    static constexpr const char *CURRENT_TABLE_FILE_NAME = "table.dat";
    static constexpr const char *NEW_TABLE_FILE_NAME = "new_table.dat";

    const char *const pmem_path;
    const int pmem_dirfd;
    drvector_t regions; // of regions.

    ranges<uintptr_t> rs;

    int persist_region(app_pc base, size_t size, char *file_name) const;

    // Returns the index of a region that overlaps with the specified region,
    // or -1 if not found.
    [[nodiscard]]
    int find_overlap(const region &other) const;
};

#endif // PSM_SRC_UNDO_MEM_REGION_MEM_REGION_H
