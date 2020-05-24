#ifndef PSM_SRC_UNDO_UNDO_BG_H
#define PSM_SRC_UNDO_UNDO_BG_H

#include <csetjmp>

#include "state.h"

// If enabled, inserts CPUID instructions into control functions
// and checks that they don't pass through instrumentation.
#define ENABLE_ASSERT_NOT_INSTRUMENTED 0

#define INSTRUMENT_LOGGING 0

#define PRINT_TRACE 0
#define PRINT_GENERATED_CODE 0
#define MOCK_OUT_RECORD_WRITE 0

#define OPTIMIZE_SKIP_STACK 1
#define OPTIMIZE_DEDUPLICATE 1
#define OPTIMIZE_SKIP_RECORD 1

static_assert(!OPTIMIZE_SKIP_RECORD || OPTIMIZE_DEDUPLICATE, "OPTIMIZE_SKIP_RECORD requires OPTIMIZE_DEDUPLICATE");

static inline void assert_not_instrumented() {
#if ENABLE_ASSERT_NOT_INSTRUMENTED
    unsigned int eax, ebx, ecx, edx;
    // We don't specify inputs since we don't care.
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
#endif
}

int instrument_init();
void instrument_commit(int tail);
void instrument_cleanup();
void instrument_log(const char *fmt, ...);

int take_initial_chkpt(jmp_buf recovery_point);

#endif // PSM_SRC_UNDO_UNDO_BG_H
