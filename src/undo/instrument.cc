#include <cstdint>
#include <limits>
#include <new>

#include <sys/mman.h>
#include <sys/syscall.h>

#include "dr_api.h"
#include "dr_tools.h"
#include "drmgr.h"
#include "drreg.h"
#include "drutil.h"
#include "drvector.h"
#include "drwrap.h"

#include "undo_bg.h"
#include "mem_region/mem_region.h"

#define unlikely(x) __builtin_expect(!!(x), 0)

#define PRINT_TRACE 0
#define OPTIMIZE_SKIP_RECORD 1
#define PRINT_GENERATED_CODE 0
#define MOCK_OUT_RECORD_WRITE 0

#include "undo_log.h"
#include "my_libc/my_libc.h"

constexpr auto POINTER_MAX = std::numeric_limits<uintptr_t>::max();

struct {
    int sysnum = -1;
    app_pc addr = nullptr;
    size_t size = 0;
} last_syscall;

// Passed in by psm.  I don't know how to otherwise pass arguments to this tool.
DR_EXPORT instrument_args_t instrument_args;
static mem_region_manager *mrm;

static module_data_t *psm_module;

// ***** GLOBALS ABOVE *****

static void event_exit() {
    drwrap_exit();
    drreg_exit();
    drutil_exit();
    drmgr_exit();

    ul::undo_log_exit();

    dr_global_free(mrm, sizeof(mem_region_manager));
}

[[gnu::optimize("-O3")]]
#if INSTRUMENT_LOGGING
static void
record_write(uintptr_t addr, uint size, uintptr_t rsp, uintptr_t pc)
#else
static void
record_write(uintptr_t addr, uint size, uintptr_t rsp)
#endif
{
#if MOCK_OUT_RECORD_WRITE
    return;
#endif

#if PRINT_TRACE
    dr_fprintf(STDERR, "%p,%u\n", addr, size);
#endif

    if (unlikely(addr >= rsp)) {
        // This happens infrequently---most writes to the stack should have been
        // filtered out because the destination address is an offset from %rsp.
        return;
    }
#ifdef DEBUG
    DR_ASSERT(!mrm->does_manage(addr));
#endif

    bool should_commit =
#if INSTRUMENT_LOGGING
        ul::undo_log_record(addr, size, pc);
#else
        ul::undo_log_record(addr, size);
#endif
    if (should_commit) {
        // This flag is not flipped until commit time.
        instrument_args.should_commit = true;
    }

#if PRINT_TRACE
    // Commit after each consume so that the trace indicates the writes made by each consume.
    instrument_args.should_commit = true;
#endif
}

// Called through `drwrap_replace_native`.
// I just copied the implementation of `dr_fprintf`.
DR_EXPORT void instrument_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    dr_vfprintf(STDERR, fmt, ap);
    va_end(ap);
    drwrap_replace_native_fini(dr_get_current_drcontext());
}

DR_EXPORT int instrument_init() {
    if (!instrument_args.recovered) {
        int res = take_initial_chkpt(instrument_args.recovery_point);
        if (res != 0) {
            return res;
        }
    }

    int res;
    if ((res = dr_app_setup()) != 0)
        return res;
    dr_app_start();

    return 0;
}

// Called through `drwrap_replace_native`.
DR_EXPORT void instrument_commit(int tail) {
#if PRINT_TRACE
    dr_fprintf(STDERR, "0,0\n");
#endif

#if !MOCK_OUT_RECORD_WRITE
    assert_not_instrumented();
    ul::undo_log_commit(tail);
#endif
    drwrap_replace_native_fini(dr_get_current_drcontext());
}

// Called through `drwrap_replace_native`.
// TODO(zhangwen): is this efficient?!
DR_EXPORT void instrument_cleanup() {
#if !MOCK_OUT_RECORD_WRITE
    assert_not_instrumented();
    ul::undo_log_post_commit_cleanup();
#endif
    drwrap_replace_native_fini(dr_get_current_drcontext());
}

static dr_emit_flags_t event_bb_app2app(void *drcontext, void *tag, instrlist_t *bb, bool for_trace,
                                        bool translating) {
    /* Transform string loops into regular loops to monitor all memory accesses. */
    if (!drutil_expand_rep_string(drcontext, bb)) {
        DR_ASSERT(false);
    }
    return DR_EMIT_DEFAULT;
}

#if OPTIMIZE_SKIP_RECORD
static void insert_instrumentation(void *drcontext, instrlist_t *bb, instr_t *instr, opnd_t opnd) {
    uint size = drutil_opnd_mem_size_in_bytes(opnd, instr);
    DR_ASSERT(size > 0);
#if INSTRUMENT_LOGGING
    app_pc pc = instr_get_app_pc(instr);
#endif

    instr_t *slow_path_label = INSTR_CREATE_label(drcontext);
    instr_t *skip_label = INSTR_CREATE_label(drcontext);

    /* `reg_dst` always holds the destination address of the write; do not
     * clobber, as the clean call uses it as an argument. */
    reg_id_t reg_dst, reg_t1;
    if (drreg_reserve_register(drcontext, bb, instr, nullptr, &reg_dst) != DRREG_SUCCESS ||
        drreg_reserve_register(drcontext, bb, instr, nullptr, &reg_t1) != DRREG_SUCCESS) {
        DR_ASSERT_MSG(false, "failed to reserve registers");
    }
    /* The `drutil_insert_get_mem_addr` call must come before
     * `drreg_reserve_aflags`, which can clobber %eax. */
    bool ok = drutil_insert_get_mem_addr(drcontext, bb, instr, opnd, reg_dst, reg_t1);
    DR_ASSERT_MSG(ok, "failed to insert get mem addr");
    if (drreg_reserve_aflags(drcontext, bb, instr) != DRREG_SUCCESS) {
        DR_ASSERT_MSG(false, "failed to reserve aflags");
    }
    ul::undo_insert_fast_path(drcontext, bb, instr, size, slow_path_label, skip_label, reg_dst, reg_t1);

    /* The slow path. */
    instrlist_meta_preinsert(bb, instr, slow_path_label);
#if INSTRUMENT_LOGGING
    dr_insert_clean_call(drcontext, bb, instr, reinterpret_cast<void *>(record_write),
                         /* save fp state */ false, /* num_args */ 4, opnd_create_reg(reg_dst),
                         OPND_CREATE_INT32(size), opnd_create_reg(DR_REG_RSP), OPND_CREATE_INTPTR(pc));
#else
    dr_insert_clean_call(drcontext, bb, instr, reinterpret_cast<void *>(record_write),
                         /* save fp state */ false, /* num_args */ 3, opnd_create_reg(reg_dst),
                         OPND_CREATE_INT32(size), opnd_create_reg(DR_REG_RSP));
#endif

    /* The fast path jumps here. */
    instrlist_meta_preinsert(bb, instr, skip_label);
    if (drreg_unreserve_aflags(drcontext, bb, instr) != DRREG_SUCCESS) {
        DR_ASSERT_MSG(false, "failed to unreserve aflags");
    }
    if (drreg_unreserve_register(drcontext, bb, instr, reg_t1) != DRREG_SUCCESS ||
        drreg_unreserve_register(drcontext, bb, instr, reg_dst) != DRREG_SUCCESS) {
        DR_ASSERT_MSG(false, "failed to unreserve registers");
    }
}
#else
static void insert_instrumentation(void *drcontext, instrlist_t *bb, instr_t *instr, opnd_t opnd) {
    app_pc pc = instr_get_app_pc(instr);

    reg_id_t reg_dst, reg_tmp = DR_REG_NULL;
    if (drreg_reserve_register(drcontext, bb, instr, nullptr, &reg_dst) != DRREG_SUCCESS) {
        DR_ASSERT(false);
    }
    DR_ASSERT(reg_dst != DR_REG_NULL);

    // If I understand it correctly, `drutil_insert_get_mem_addr()` does not
    // clobber the eflags since it only inserts LEA and MOV instructions.
    bool ok = drutil_insert_get_mem_addr(drcontext, bb, instr, opnd, reg_dst, DR_REG_NULL);
    if (!ok) {
        // Try again with a temporary register.
        if (drreg_reserve_register(drcontext, bb, instr, nullptr, &reg_tmp) != DRREG_SUCCESS) {
            DR_ASSERT(false);
        }
        DR_ASSERT(reg_tmp != DR_REG_NULL);
        ok = drutil_insert_get_mem_addr(drcontext, bb, instr, opnd, reg_dst, reg_tmp);
    }
    DR_ASSERT(ok);

    uint size = drutil_opnd_mem_size_in_bytes(opnd, instr);
    DR_ASSERT(size > 0);

    // `reinterpret_cast` can convert a function pointer to `void *` on a
    // POSIX-compatible system.
#if INSTRUMENT_LOGGING
    dr_insert_clean_call(drcontext, bb, instr, reinterpret_cast<void *>(record_write),
                         /* save fp state */ false, /* num_args */ 4, opnd_create_reg(reg_dst),
                         OPND_CREATE_INT32(size), opnd_create_reg(DR_REG_RSP), OPND_CREATE_INTPTR(pc));
#else
    dr_insert_clean_call(drcontext, bb, instr, reinterpret_cast<void *>(record_write),
                         /* save fp state */ false, /* num_args */ 3, opnd_create_reg(reg_dst),
                         OPND_CREATE_INT32(size), opnd_create_reg(DR_REG_RSP));
#endif

    if (drreg_unreserve_register(drcontext, bb, instr, reg_dst) != DRREG_SUCCESS ||
        (reg_tmp != DR_REG_NULL && drreg_unreserve_register(drcontext, bb, instr, reg_tmp) != DRREG_SUCCESS)) {
        DR_ASSERT(false);
    }
}
#endif

static dr_emit_flags_t event_app_instruction(void *drcontext, void *tag, instrlist_t *bb, instr_t *instr,
                                             bool for_trace, bool translating, void *user_data) {
    // Start by ignoring instructions that are not subject to instrumentation.
    if (!instr_is_app(instr))
        return DR_EMIT_DEFAULT;

    app_pc pc = instr_get_app_pc(instr);
    if (dr_module_contains_addr(psm_module, pc)) {
        // This instruction is from PSM code (not application code). Ignore!
        return DR_EMIT_DEFAULT;
    }

#if ENABLE_ASSERT_NOT_INSTRUMENTED
    int opcode = instr_get_opcode(instr);
    DR_ASSERT_MSG(opcode != OP_cpuid, "CPUID encountered -- assert_not_instrumented failed?");
#endif

    // This check must come after the CPUID check, because CPUID doesn't write to memory
    // (and would be skipped by this check).
    if (!instr_writes_memory(instr))
        return DR_EMIT_DEFAULT;

        /* insert code to add an entry for each memory reference opnd */
#if PRINT_GENERATED_CODE
    bool inserted = false;
#endif
    for (int i = 0; i < instr_num_dsts(instr); i++) {
        opnd_t opnd = instr_get_dst(instr, i);
        if (opnd_is_memory_reference(opnd)) {
            if (opnd_is_base_disp(opnd) && opnd_get_base(opnd) == DR_REG_XSP) {
                // Assume that this write destination is on the stack.  Ignore!
                // TODO(zhangwen): is this assumption reasonable?
                continue;
            }
            insert_instrumentation(drcontext, bb, instr, opnd);
#if PRINT_GENERATED_CODE
            inserted = true;
#endif
        }
    }

#if PRINT_GENERATED_CODE
    if (inserted)
        instrlist_disassemble(drcontext, (app_pc)tag, bb, STDERR);
#endif

    return DR_EMIT_DEFAULT;
}

static bool event_filter_syscall(void *drcontext, int sysnum) {
    if (sysnum == SYS_mmap || sysnum == SYS_munmap) {
        return true;
    }

    dr_fprintf(STDERR, "*** WARNING: Unsupported syscall: %d\n", sysnum);
    return false;
}

static bool event_pre_syscall(void *drcontext, int sysnum) {
    if (sysnum != SYS_mmap && sysnum != SYS_munmap) {
        dr_fprintf(STDERR, "*** WARNING (pre_syscall): Unsupported syscall: %d\n", sysnum);
        return true;
    }

    auto addr = reinterpret_cast<app_pc>(dr_syscall_get_param(drcontext, 0));
    size_t size = dr_syscall_get_param(drcontext, 1);

    if (sysnum == SYS_munmap) {
#if INSTRUMENT_LOGGING
        dr_fprintf(STDERR, "munmap:\t%p\t%lu\n", addr, size);
#endif
        ul::undo_log_remove_fresh_region(addr, size);
        auto res = mrm->remove_region(addr, size);
        if (res == mem_region_manager::result::NOT_MANAGED) { // We don't care about this munmap call.
            return true;
        }
        dr_syscall_set_result(drcontext, static_cast<int>(res));
        return false;
    }

    DR_ASSERT(last_syscall.sysnum == -1);
    last_syscall.sysnum = sysnum;
    last_syscall.addr = addr;
    last_syscall.size = size;

    DR_ASSERT(sysnum == SYS_mmap);
    // Make sure that we can handle this mmap.
    DR_ASSERT_MSG(addr == nullptr, "not supported: mmap with address hint");
    // TODO(zhangwen): we assume each `mmap`'ed region has read & write permissions.
    //    Here, we only require the pages to be readable.
    int prot = dr_syscall_get_param(drcontext, 2);
    DR_ASSERT_MSG(prot & PROT_READ, "not supported: mmap unreadable pages");
    // TODO(zhangwen): support more flags?
    int flags = dr_syscall_get_param(drcontext, 3);
    DR_ASSERT_MSG(flags == (MAP_PRIVATE | MAP_ANONYMOUS), "not supported: mmap flags");
    // We'll let this `mmap` go through.  Once it succeeds, we'll replace the region.

    return true;
}

static void event_post_syscall(void *drcontext, int sysnum) {
    if (sysnum != SYS_mmap && sysnum != SYS_munmap) {
        dr_fprintf(STDERR, "*** WARNING (post_syscall): Unsupported syscall: %d\n", sysnum);
        return;
    }

    if (last_syscall.sysnum == SYS_mmap) {
        auto mmap_ret = reinterpret_cast<app_pc>(dr_syscall_get_result(drcontext));
        if (mmap_ret != MAP_FAILED) {
            // TODO(zhangwen): we assume each `mmap`'ed region has read & write permissions.
            //    This simplification allows us to not record protection bits in memory region files.
            auto size = last_syscall.size;
#if INSTRUMENT_LOGGING
            dr_fprintf(STDERR, "mmap:\t%p\t%lu\n", mmap_ret, size);
#endif
            auto res = mrm->replace_region(mmap_ret, size, PROT_READ | PROT_WRITE);
            ul::undo_log_record_fresh_region(mmap_ret, size);
            DR_ASSERT(res == mem_region_manager::result::SUCCESS);
        }
    }

    last_syscall.sysnum = -1;
}

static bool should_replace(const dr_mem_info_t *info) {
    app_pc base = info->base_pc;
    if (dr_memory_is_dr_internal(base)) {
#if INSTRUMENT_LOGGING
        dr_fprintf(STDERR, "[should_replace] skipping internal memory:\t%p\n", base);
#endif
        return false;
    }

    if (dr_memory_is_in_client(base)) {
#if INSTRUMENT_LOGGING
        dr_fprintf(STDERR, "[should_replace] skipping client memory:\t%p\n", base);
#endif
        return false;
    }

    if (info->type == DR_MEMTYPE_FREE) {
#if INSTRUMENT_LOGGING
        dr_fprintf(STDERR, "[should_replace] skipping MEMTYPE_FREE:\t%p\n", base);
#endif
        return false;
    }

    // FIXME(zhangwen): assumes that memory protection never changes
    //      (i.e., a non-writeable page will remain that way).
    if (!(info->prot & DR_MEMPROT_WRITE)) {
#if INSTRUMENT_LOGGING
        dr_fprintf(STDERR, "[should_replace] skipping no-write memory:\t%p\n", base);
#endif
        return false;
    }
    if (info->prot & DR_MEMPROT_VDSO) {
#if INSTRUMENT_LOGGING
        dr_fprintf(STDERR, "[should_replace] skipping VDSO:\t%p\n", base);
#endif
        return false;
    }
    if (info->prot & DR_MEMPROT_STACK) {
#if INSTRUMENT_LOGGING
        dr_fprintf(STDERR, "[should_replace] skipping stack:\t%p\n", base);
#endif
        return false;
    }

    // Hopefully this assertion passes, because I don't know how to deal with "pretend write" pages (which should only
    // cover the executable pages?)
    DR_ASSERT(!(info->prot & DR_MEMPROT_PRETEND_WRITE));
    // TODO(zhangwen): handle write-only pages??!
    DR_ASSERT(info->prot & DR_MEMPROT_READ);

    auto psm_log_base = reinterpret_cast<app_pc>(instrument_args.psm_log_base);
    if (base <= psm_log_base && info->size > static_cast<size_t>(psm_log_base - base)) {
        // Skip the PSM log area.
        DR_ASSERT(base == psm_log_base);
#if INSTRUMENT_LOGGING
        dr_fprintf(STDERR, "[should_replace] skipping PSM log:\t%p\n", base);
#endif
        return false;
    }

    return true;
}

static void init_address_space() {
    drvector_t to_replace; // Vector (of `dr_mem_info_t *`) of memory regions to replace.
    // For peace of mind, we avoid replacing memory regions while iterating through them.
    {
        bool success = drvector_init(&to_replace, /* initial_capacity */ 10, /* synch */ false,
                                     [](void *p) { dr_global_free(p, sizeof(dr_mem_info_t)); });
        DR_ASSERT(success);
    }

    dr_mem_info_t info;
    app_pc pc = nullptr;
    while (reinterpret_cast<uintptr_t>(pc) < POINTER_MAX && dr_query_memory_ex(pc, &info)) {
        if (should_replace(&info)) {
            void *info_mem = dr_global_alloc(sizeof(dr_mem_info_t));
            bool success = drvector_append(&to_replace, new (info_mem) dr_mem_info_t(info));
            DR_ASSERT(success);
        }
        pc = info.base_pc + info.size;
    }

    // Now iterate through the memory regions to replace, and replace them.
    for (uint i = 0; i < to_replace.entries; i++) {
        auto infop = static_cast<dr_mem_info_t *>(to_replace.array[i]);

        uint dr_prot = infop->prot;
        int prot = 0;
        if (dr_prot & DR_MEMPROT_READ) {
            prot |= PROT_READ;
        }
        if (dr_prot & DR_MEMPROT_WRITE) {
            prot |= PROT_WRITE;
        }
        if (dr_prot & DR_MEMPROT_EXEC) {
            prot |= PROT_EXEC;
        }

        auto res = mrm->replace_region(infop->base_pc, infop->size, prot);
        DR_ASSERT(res == mem_region_manager::result::SUCCESS);
    }

    {
        bool success = drvector_delete(&to_replace); // This frees all elements.
        DR_ASSERT(success);
    }
}

DR_EXPORT void dr_client_main(client_id_t id, int argc, const char *argv[]) {
#ifdef DEBUG
    dr_fprintf(STDERR, "WARNING: debug might not work...");
#endif
    mrm = new (dr_global_alloc(sizeof(mem_region_manager))) mem_region_manager(instrument_args.pmem_path);
    if (instrument_args.recovered) {
        mrm->recover();
    } else {
        init_address_space();
    }

    ul::undo_log_init(instrument_args.pmem_path, instrument_args.recovered);
    if (instrument_args.recovered) {
        int recovered_tail = ul::undo_log_apply(mrm);
        if (recovered_tail != -1) {
            instrument_args.recovered_tail = recovered_tail;
        }
    }

    // After applying the undo log, ask the foreground to recover memory pages.
    if (instrument_args.recovered) {
        if (my_close(instrument_args.recovery_fds_btf[PIPE_READ_END]) != 0) {
            DR_ASSERT_MSG(false, "closing read end of btf pipe failed");
        }
        if (my_close(instrument_args.recovery_fds_ftb[PIPE_WRITE_END]) != 0) {
            DR_ASSERT_MSG(false, "closing write end of ftb pipe failed");
        }

        int send_fd = instrument_args.recovery_fds_btf[PIPE_WRITE_END];
        mrm->send_regions(send_fd);

        { // Send recovered tail.
            int recovered_tail = instrument_args.recovered_tail;
            DR_ASSERT(recovered_tail >= 0);
            int written = my_write(send_fd, &recovered_tail, sizeof(recovered_tail));
            DR_ASSERT(written >= 0);
            DR_ASSERT(written == sizeof(recovered_tail));
        }
        if (my_close(send_fd) != 0) {
            DR_ASSERT_MSG(false, "closing write end of btf pipe failed");
        }

        { // Wait for foreground to finish recovery.
            int recv_fd = instrument_args.recovery_fds_ftb[PIPE_READ_END];
            char buf;
            if (my_read(recv_fd, &buf, 1) < 1) {
                DR_ASSERT_MSG(false, "waiting for foreground failed");
            }
            if (my_close(recv_fd) != 0) {
                DR_ASSERT_MSG(false, "closing read end of ftb pipe failed");
            }
        }
    }

    if (!drmgr_init())
        DR_ASSERT(false);

    if (!drutil_init())
        DR_ASSERT(false);

    drreg_options_t ops = {sizeof(ops), 3, false, nullptr, false};
    if (drreg_init(&ops) != DRREG_SUCCESS)
        DR_ASSERT(false);

    if (!drwrap_init())
        DR_ASSERT(false);

    for (auto func : {
             reinterpret_cast<app_pc>(instrument_commit),
             reinterpret_cast<app_pc>(instrument_cleanup),
             reinterpret_cast<app_pc>(instrument_log),
         }) {
        if (!drwrap_replace_native(func, func,
                                   /* at_entry */ false,
                                   /* stack_adjust */ 0,
                                   /* use_data */ nullptr,
                                   /* override */ false))
            DR_ASSERT(false);
    }

    if (!drmgr_register_bb_app2app_event(event_bb_app2app, nullptr) ||
        !drmgr_register_bb_instrumentation_event(nullptr, event_app_instruction, nullptr)) {
        DR_ASSERT(false);
    }

    dr_register_filter_syscall_event(event_filter_syscall);
    if (!drmgr_register_pre_syscall_event(event_pre_syscall)) {
        DR_ASSERT(false);
    }
    if (!drmgr_register_post_syscall_event(event_post_syscall)) {
        DR_ASSERT(false);
    }

    psm_module = dr_lookup_module(reinterpret_cast<app_pc>(&dr_client_main));
    DR_ASSERT(psm_module != nullptr);

    dr_register_exit_event(event_exit);
}
