#ifndef PSM_SRC_UNDO_MEM_REGION_FG_H
#define PSM_SRC_UNDO_MEM_REGION_FG_H

// Maps recovered regions (sent by background process through fd) in foreground process.
// Returns 0 on success, errno on failure.
// Closes fd afterwards.
int map_recovered_regions(const char *pmem_path, int read_fd);

#endif //PSM_SRC_UNDO_MEM_REGION_FG_H
