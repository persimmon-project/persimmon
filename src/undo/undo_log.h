// Included by `instrument.cc`.
#ifndef PSM_SRC_UNDO_UNDO_LOG_H
#define PSM_SRC_UNDO_UNDO_LOG_H

#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>

#include "dr_api.h"

#include "flush.h"
#include "mem_region/mem_region.h"
#include "mem_region/ranges.h"
#include "my_libc/my_libc.h"
#include "undo_bg.h"

// Singleton undo log.
namespace ul {

constexpr size_t UNDO_BLK_SIZE_B = 32;
constexpr size_t UNDO_NUM_ENTRIES = 1024 * 512;

#define OPTIMIZED 1

constexpr size_t CACHE_LINE_SIZE_B = 64;
constexpr size_t LOGGED_ADDR_HASH_SIZE = 16384;

// Commit when undo log length exceeds this threshold.
constexpr int COMMIT_THRESHOLD = LOGGED_ADDR_HASH_SIZE / 2;

struct alignas(CACHE_LINE_SIZE_B) undo_log_entry {
    // TODO(zhangwen): this is a waste of space.
    char blk[UNDO_BLK_SIZE_B];
    app_pc addr;
    uint64_t commit_tail; /* If > 0, this is a commit record and `commit_tail - 1` is the tail. */

    [[nodiscard]] bool is_null() const { return addr == nullptr && commit_tail == 0; }
};
static_assert(sizeof(undo_log_entry) == CACHE_LINE_SIZE_B, "undo_log_entry has different size from cache line");

static struct {
    undo_log_entry *log; // In persistent memory.
    size_t len;

    // This is a hash table "index" for addresses logged in `undo_log`,
    // array of LOGGED_ADDR_HASH_SIZE (void *).
    void **logged_addrs_hash;

    ranges<uintptr_t> *fresh_regions;
} undo_log;

#define barrier() asm volatile("" ::: "memory")

static void *map_undo_log(const char *pmem_path) {
    int dirfd = my_open(pmem_path, O_DIRECTORY);
    DR_ASSERT_MSG(dirfd >= 0, "open pmem directory failed");

    int fd = my_openat(dirfd, "undo_log", O_CREAT | O_RDWR, 0666);
    DR_ASSERT_MSG(fd >= 0, "open undo log file failed");
    {
        int ret = my_close(dirfd);
        DR_ASSERT_MSG(ret >= 0, "close pmem directory failed");
    }

    const size_t undo_log_size = sizeof(undo_log_entry) * UNDO_NUM_ENTRIES;
    if (my_ftruncate(fd, undo_log_size) < 0) {
        DR_ASSERT_MSG(false, "truncate undo log file failed");
    }

    void *const addr = my_mmap(nullptr, undo_log_size, PROT_READ | PROT_WRITE, MAP_SHARED_VALIDATE | MAP_SYNC, fd,
                               /* offset */ 0);
#if INSTRUMENT_LOGGING
    dr_fprintf(STDERR, "[bg: map_undo_log] undo log mapped at %p...\n", addr);
#endif
    DR_ASSERT_MSG(addr != MAP_FAILED, "mmap undo log failed");

    if (my_close(fd) < 0) {
        DR_ASSERT_MSG(false, "close undo log file failed");
    }

    return addr;
}

// If the address already exists, returns false.
// Otherwise, inserts the address if there's space, and returns true.
// Returns false if the address already exists, true otherwise.
static bool undo_log_insert_logged_addr(void *addr) {
    auto addr_n = reinterpret_cast<uintptr_t>(addr);
#if !OPTIMIZED
    DR_ASSERT(addr_n % UNDO_BLK_SIZE_B == 0);
#endif

    static_assert((LOGGED_ADDR_HASH_SIZE & (LOGGED_ADDR_HASH_SIZE - 1)) == 0,
                  "LOGGED_ADDR_HASH_SIZE is not a power of two");
    auto hash = addr_n / UNDO_BLK_SIZE_B;

    size_t i = hash;
    uintptr_t perturb = hash;

    // It takes < 13 shifts to get perturb to zero.
    for (size_t count = 0; count < LOGGED_ADDR_HASH_SIZE + 13; count++) {
        void **p_curr = &undo_log.logged_addrs_hash[i % LOGGED_ADDR_HASH_SIZE];
        if (*p_curr == nullptr) { // We've found an empty slot -- `addr` doesn't exist yet; add it.
            *p_curr = addr;
            return true;
        } else if (*p_curr == addr) {
            return false;
        }

        i = 5 * i + perturb + 1;
        perturb >>= 5u;
    }

#if !OPTIMIZED
    dr_fprintf(STDERR, "[bg: undo_log_insert_logged_addr] hash table full?\t%p\n", addr);
#endif
    return true;
}

static void undo_log_clear() {
    pmem_memset(reinterpret_cast<char *>(undo_log.log), 0, undo_log.len * sizeof(undo_log_entry));
    undo_log.len = 0;
    memset(undo_log.logged_addrs_hash, 0, sizeof(void *) * LOGGED_ADDR_HASH_SIZE);
    undo_log.fresh_regions->clear();
    pmem_drain();
}

static void undo_log_init(const char *pmem_path, bool recovered) {
    void *log = map_undo_log(pmem_path);
    DR_ASSERT(reinterpret_cast<uintptr_t>(log) % CACHE_LINE_SIZE_B == 0);
    undo_log.log = static_cast<undo_log_entry *>(log);

    /* Am I supposed to call placement new for this array?  I give up... */
    undo_log.logged_addrs_hash = (void **)dr_global_alloc(sizeof(*undo_log.logged_addrs_hash) * LOGGED_ADDR_HASH_SIZE);
    /* We should have configured DynamoRIO to allocate in the lowest 4GB of the
     * address space, making it easier to access this array from vmcode. */
    DR_ASSERT_MSG(reinterpret_cast<uintptr_t>(undo_log.logged_addrs_hash) < 0xFFFFFFFF,
                  "logged_addrs_hash address exceeds 32 bits");

    void *mem = dr_global_alloc(sizeof(*undo_log.fresh_regions));
    undo_log.fresh_regions = new (mem) ranges<uintptr_t>();

    if (recovered) { // Recover other fields.
        memset(undo_log.logged_addrs_hash, 0, sizeof(void *) * LOGGED_ADDR_HASH_SIZE);
        int i = 0;
        for (auto entry = undo_log.log; !entry->is_null(); ++entry, ++i) {
            undo_log_insert_logged_addr(entry->addr);
            if (entry->commit_tail > 0) {
                DR_ASSERT(entry->addr == nullptr);
            }
        }
        undo_log.len = i;
    } else {
        undo_log_clear();
    }
}

// Records memory write to [addr, addr + size).
// Returns `true` if it's time to commit; as soon as this function returns true,
// should commit as soon as possible, ignoring the return value of future calls
// to this function until commit.
[[gnu::always_inline]] static inline bool undo_log_record
#if INSTRUMENT_LOGGING
    (uintptr_t addr, uint size, uintptr_t pc)
#else
    (uintptr_t addr, uint size)
#endif
{
    if (undo_log.fresh_regions->find(addr, size)) {
        // This region was newly allocated after the previous commit.
        // No need to save the original value for undo.
#if INSTRUMENT_LOGGING
        dr_fprintf(STDERR, "[bg: undo_log_record] fresh: %p\t%u\t%p\n", addr, size, pc);
#endif
        return false;
    }

    static_assert((UNDO_BLK_SIZE_B & (UNDO_BLK_SIZE_B - 1)) == 0, "UNDO_BLK_SIZE_B is not a power of two");
    uintptr_t blk_start = addr & ~(UNDO_BLK_SIZE_B - 1);
    for (auto pn = blk_start; pn < addr + size; pn += UNDO_BLK_SIZE_B) {
#if !OPTIMIZED
        DR_ASSERT(pn % UNDO_BLK_SIZE_B == 0);
#endif

        auto p = reinterpret_cast<app_pc>(pn);
        if (!undo_log_insert_logged_addr(p)) {
            continue;
        }

        auto *entry = static_cast<undo_log_entry *>(
            // The hint helps the compiler pick instructions that assume
            // alignment. (Not sure if it matters, though...)
            __builtin_assume_aligned(&undo_log.log[undo_log.len], CACHE_LINE_SIZE_B));

        // The following writes are to the same cache line and are thus ordered.
        memcpy(entry->blk, p, UNDO_BLK_SIZE_B);
        barrier();
        entry->addr = p;
        barrier();
        entry->commit_tail = 0;
        pmem_flush(entry);
        // We don't care about the order in which these log entries get
        // persisted, as long as they all get persisted by the end of this
        // function.
        undo_log.len++;
#if !OPTIMIZED
        DR_ASSERT(undo_log.len < UNDO_NUM_ENTRIES);
#endif

#if INSTRUMENT_LOGGING
        dr_fprintf(STDERR, "[bg: undo_log_record] %p\t%p\n", p, pc);
#endif
    }
    pmem_drain();
    return undo_log.len > COMMIT_THRESHOLD;
}

// Records newly allocated memory [addr, addr + size).
// Writes to this region will not be logged till the next "commit".
// Upon commit, all this memory is flushed.
// This is an optimization; it is not necessary to call this function
// for all new memory.
static void undo_log_record_fresh_region(app_pc addr, uint size) {
    undo_log.fresh_regions->insert(reinterpret_cast<uintptr_t>(addr), size);
#if INSTRUMENT_LOGGING
    dr_fprintf(STDERR, "[bg: undo_log_record_fresh_region] recorded fresh region\t%p\t%u\n", addr, size);
#endif
}

static void undo_log_remove_fresh_region(app_pc addr, uint size) {
    undo_log.fresh_regions->remove(reinterpret_cast<uintptr_t>(addr), size);
#if INSTRUMENT_LOGGING
    dr_fprintf(STDERR, "[bg: undo_log_remove_fresh_region] removed fresh region\t%p\t%u\n", addr, size);
#endif
}

static void undo_log_commit(int tail) {
    // This makes sure that each logged block does not straddle a cache line.
    static_assert(CACHE_LINE_SIZE_B % UNDO_BLK_SIZE_B == 0, "undo-logged block straddles cache line");

    for (size_t i = 0; i < undo_log.len; i++) {
        pmem_flush(undo_log.log[i].addr);
    }
    undo_log.fresh_regions->foreach ([](uintptr_t addr_n, size_t size) {
        auto addr = reinterpret_cast<app_pc>(addr_n);
        pmem_flush(addr);
        uintptr_t blk_start = addr_n & ~(CACHE_LINE_SIZE_B - 1);

        for (auto p = reinterpret_cast<app_pc>(blk_start) + CACHE_LINE_SIZE_B; p < addr + size;
             p += CACHE_LINE_SIZE_B) {
            pmem_flush(p);
        }
    });
    pmem_drain();

    // Write commit record.
    undo_log_entry *entry = &undo_log.log[undo_log.len];
    entry->addr = nullptr;
    entry->commit_tail = tail + 1;
    pmem_flush(entry);
    undo_log.len++;
    DR_ASSERT(undo_log.len < UNDO_NUM_ENTRIES);
    pmem_drain();

#if INSTRUMENT_LOGGING
    dr_fprintf(STDERR, "[bg: instrument_commit] undo_log_len:\t%d\n", undo_log.len);
#endif
}

static void undo_log_post_commit_cleanup() {
    // TODO(zhangwen): this is a horrible name.
#if INSTRUMENT_LOGGING
    dr_fprintf(STDERR, "[bg: instrument_cleanup] undo_log_len:\t%d\n", undo_log.len);
#else
    if (undo_log.len > 10000) {
        dr_fprintf(STDERR, "[bg: instrument_cleanup] undo_log_len:\t%d\n", undo_log.len);
    }
#endif
    // Precondition: the last record must be a commit record.
    DR_ASSERT(undo_log.len > 0);
    DR_ASSERT(undo_log.log[undo_log.len - 1].commit_tail > 0);
    undo_log_clear();
}

static void undo_log_exit() {
    size_t undo_log_size = sizeof(undo_log_entry) * UNDO_NUM_ENTRIES;
    my_munmap(undo_log.log, undo_log_size);
}

// Goes through log entries from back to front and applies them until a commit
// record, then discards the remaining records.
// This is valid because all writes captured by the log records before a commit
// records should have been persisted.
// Returns the commit tail, or -1 if one doesn't exist. If one exists, it should
// be used as the PSM log tail.
// Also recovers memory regions.
[[nodiscard]] static int undo_log_recover(mem_region_manager *mrm) {
#if INSTRUMENT_LOGGING
    dr_fprintf(STDERR, "[bg: apply_undo_log] applying undo log...\n");
#endif
    if (undo_log.len > 0) {
        undo_log_entry *last_entry = &undo_log.log[undo_log.len - 1];
        if (last_entry->commit_tail > 0) { // A commit entry exists.
            int tail = last_entry->commit_tail - 1; // By definition.
            mrm->commit_new_region_table();
            mrm->recover();
            undo_log_clear();
#if INSTRUMENT_LOGGING
            dr_fprintf(STDERR, "[bg: apply_undo_log] recovered tail:\t%d\n", tail);
#endif
            return tail;
        }
    }
    mrm->clear_new_region_table();
    mrm->recover();

    for (size_t i = undo_log.len; i > 0; --i) {
        undo_log_entry *entry = &undo_log.log[i - 1];
        DR_ASSERT_MSG(entry->commit_tail == 0, "there should be no commit entry");

        app_pc addr = entry->addr;
        DR_ASSERT_MSG(addr != nullptr, "entry->addr == nullptr");
        // Writes to newly allocated regions should have been filtered out.
        DR_ASSERT_MSG(mrm->does_manage(addr), "undo log entry addr not in a managed region?");

        memcpy(entry->addr, &entry->blk, UNDO_BLK_SIZE_B);
        pmem_flush(entry->addr);
#if INSTRUMENT_LOGGING
        dr_fprintf(STDERR, "[bg: apply_undo_log] applied undo log entry: %p\n", addr);
#endif
    }
    pmem_drain();

    undo_log_clear();
#if INSTRUMENT_LOGGING
    dr_fprintf(STDERR, "[bg: apply_undo_log] undo log applied\n");
#endif
    return -1;
}

/* Expects `value` to be a power of 2. */
constexpr uint8_t log2(size_t value) { return value == 1 ? 0 : 1 + log2(value >> 1u); }

#if OPTIMIZE_SKIP_RECORD
#define MINSERT instrlist_meta_preinsert
static void undo_insert_fast_path(void *drcontext, instrlist_t *ilist, instr_t *where, uint size,
                                  instr_t *slow_path_label, instr_t *skip_label, reg_id_t reg_dst, reg_id_t reg_t1) {
    // The code here is based on assembly generated by Clang.
    // Alignment check: use slow path if write straddles blocks.
    DR_ASSERT(size > 0 && (size & (size - 1)) == 0);
    if (size > 1) { // A write of size 1 always passes the check.
        {           // lea [reg_dst + (size-1)] ==> reg_t1
            auto opnd1 = opnd_create_reg(reg_t1);
            auto opnd2 = opnd_create_base_disp(reg_dst, DR_REG_NULL, 0, static_cast<int>(size - 1), OPSZ_lea);
            auto instr = INSTR_CREATE_lea(drcontext, opnd1, opnd2);
            MINSERT(ilist, where, instr);
        }

        { // xor reg_dst, reg_t1
            auto opnd1 = opnd_create_reg(reg_t1);
            auto opnd2 = opnd_create_reg(reg_dst);
            auto instr = INSTR_CREATE_xor(drcontext, opnd1, opnd2);
            MINSERT(ilist, where, instr);
        }

        { // cmpq (UNDO_BLK_SIZE_B-1), $reg_t1
            auto opnd1 = opnd_create_reg(reg_t1);
            auto opnd2 = OPND_CREATE_INT8(UNDO_BLK_SIZE_B - 1);
            auto instr = INSTR_CREATE_cmp(drcontext, opnd1, opnd2);
            MINSERT(ilist, where, instr);
        }

        { // ja SLOW_PATH
            auto instr = INSTR_CREATE_jcc_short(drcontext, OP_ja_short, opnd_create_instr(slow_path_label));
            MINSERT(ilist, where, instr);
        }
    }

    // Check the hash table (with no probing) to see if we're sure the block has
    // been logged.  If we don't find it at first try, defer to slow path.
    { // mov %reg_dst, %reg_t1
        auto opnd1 = opnd_create_reg(reg_t1);
        auto opnd2 = opnd_create_reg(reg_dst);
        auto instr = INSTR_CREATE_mov_ld(drcontext, opnd1, opnd2);
        MINSERT(ilist, where, instr);
    }

    // Compute %reg_t1 <- (%reg_t1/UNDO_BLK_SIZE_B)%LOGGED_ADDR_HASH_SIZE*8.
    constexpr uint8_t log2_ptr_size = log2(sizeof(void *));
    constexpr uint8_t log2_UNDO_BLK_SIZE_B = log2(UNDO_BLK_SIZE_B);

    static_assert(log2_UNDO_BLK_SIZE_B > log2_ptr_size, "UNDO_BLK_SIZE_B <= sizeof(void *)");
    { // shrq log2(UNDO_BLK_SIZE_B) - log2(ptr_size), %reg_t1
        auto opnd1 = opnd_create_reg(reg_t1);
        auto opnd2 = OPND_CREATE_INT8(log2_UNDO_BLK_SIZE_B - log2_ptr_size);
        auto instr = INSTR_CREATE_shr(drcontext, opnd1, opnd2);
        MINSERT(ilist, where, instr);
    }

    { // andl 8*(LOGGED_ADDR_HASH_SIZE-1), %reg_t1
        auto opnd1 = opnd_create_reg(reg_t1);
        auto opnd2 = OPND_CREATE_INT32((LOGGED_ADDR_HASH_SIZE - 1) * sizeof(void *));
        auto instr = INSTR_CREATE_and(drcontext, opnd1, opnd2);
        MINSERT(ilist, where, instr);
    }

    { // movq logged_addrs_hash(%reg_t1), %reg_t1
        auto opnd1 = opnd_create_reg(reg_t1);
        auto opnd2 = OPND_CREATE_MEMPTR(reg_t1, reinterpret_cast<uintptr_t>(undo_log.logged_addrs_hash));
        auto instr = INSTR_CREATE_mov_ld(drcontext, opnd1, opnd2);
        MINSERT(ilist, where, instr);
    }

    { // xorq %reg_dst, %reg_t1
        auto opnd1 = opnd_create_reg(reg_t1);
        auto opnd2 = opnd_create_reg(reg_dst);
        auto instr = INSTR_CREATE_xor(drcontext, opnd1, opnd2);
        MINSERT(ilist, where, instr);
    }

    { // cmpq UNDO_BLK_SIZE_B, $reg_t1
        auto opnd1 = opnd_create_reg(reg_t1);
        auto opnd2 = OPND_CREATE_INT8(UNDO_BLK_SIZE_B);
        auto instr = INSTR_CREATE_cmp(drcontext, opnd1, opnd2);
        MINSERT(ilist, where, instr);
    }

    { // jb SKIP
        auto instr = INSTR_CREATE_jcc(drcontext, OP_jb, opnd_create_instr(skip_label));
        MINSERT(ilist, where, instr);
    }
}
#undef MINSERT
#endif
} // namespace ul

#endif // PSM_SRC_UNDO_UNDO_LOG_H
