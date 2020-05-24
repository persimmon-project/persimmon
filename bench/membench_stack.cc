#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <sys/mman.h>
#include <sched.h>

#include <libpsm/psm.h>
#include <x86intrin.h>

constexpr bool LARGE_ACCESS = true;

constexpr size_t WRITES_PER_OP = 128;
constexpr size_t WARMUP_OPS = PSM_LOG_SIZE_B / 64 * 4;

// Each element is a `uint64_t`.
constexpr size_t TOTAL_ELEMS = 1ull << 24u;

static_assert(TOTAL_ELEMS > 0, "there must be at least one element");
static_assert((TOTAL_ELEMS & (TOTAL_ELEMS-1)) == 0, "TOTAL_ELEMS should be power of two");
constexpr size_t MEM_SIZE_B = TOTAL_ELEMS * sizeof(uint64_t);

void *mem;

[[gnu::always_inline]] static inline int pin_thread_to_core(int id) {
    // Adapted from https://github.com/PlatformLab/PerfUtils.
    cpu_set_t cpuset;

    CPU_ZERO(&cpuset);
    CPU_SET(id, &cpuset);
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0) {
        return errno;
    }

    return 0;
}

[[gnu::noinline]] size_t run_op(size_t offset, size_t stack_writes) {
    alignas(32) volatile uint64_t local[4 * WRITES_PER_OP];
    for (size_t i = 0; i < stack_writes; i++) {
        volatile uint64_t *this_p = &local[i * 4];

        if constexpr (LARGE_ACCESS) {
            __m256i val = _mm256_load_si256((const __m256i *)this_p);
            val = _mm256_add_epi64(val, val);
            _mm256_store_si256((__m256i *)this_p, val);
//            __m256i val = _mm256_load_si256((const __m256i *)local);
//            val = _mm256_add_epi64(val, val);
//            _mm256_store_si256((__m256i *)local, val);
        } else {
            uint64_t val = *this_p;
            val *= val;
            *this_p = val;
        }
        asm volatile("": : :"memory");
    }

    auto p = static_cast<volatile uint64_t *>(mem) + offset;
    for (size_t i = 0; i < WRITES_PER_OP - stack_writes; i++) {
        volatile uint64_t *this_p = &p[i * 4];

        if constexpr (LARGE_ACCESS) {
            __m256i val = _mm256_load_si256((const __m256i *)this_p);
            val = _mm256_add_epi64(val, val);
            _mm256_store_si256((__m256i *)this_p, val);
        } else {
            uint64_t val = *this_p;
            val *= val;
            *this_p = val;
        }
        asm volatile("": : :"memory");
    }
    return local[0];
}

namespace psm_bench {
typedef struct {
    uint64_t unused;
    uint64_t stack_writes;
    uint64_t offset;
} op_t;

int consume(const void *p) {
    size_t offset;
    size_t stack_writes;
    // op_t op;
    // memcpy(&op, p, sizeof(op));
    memcpy(&offset, (char *) p + offsetof(op_t, offset), sizeof(offset));
    memcpy(&stack_writes, (char *) p + offsetof(op_t, stack_writes), sizeof(stack_writes));
    run_op(offset, stack_writes);
    return sizeof(op_t);
}

size_t run_loop(size_t num_ops, size_t stack_writes, size_t offset = 0) {
    op_t op = {.unused = 1, .stack_writes = stack_writes, .offset = 0};
    for (size_t i = 0; i < num_ops; i++) {
        op.offset = offset;
        void *p = psm_reserve(sizeof(op));
        memcpy(p, &op, sizeof(op));
        psm_commit(/* push_only = */ false);
        offset = (offset + WRITES_PER_OP * 4) % TOTAL_ELEMS;
    }
    return offset;
}

// Returns number of seconds.
double run(size_t num_ops, size_t stack_writes) {
    mem = mmap(nullptr, MEM_SIZE_B, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE, -1, 0);
    if (mem == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }

    psm_config_t config = {
        .use_sga = false,
        .pin_core = 27,
        .consume_func = consume,
        .mode = PSM_MODE_UNDO,
        .pmem_path = "/mnt/pmem1/bench",
        .undo = {.criu_service_path = "/tmp/criu_service.socket"},
    };
    if (psm_init(&config) != 0) {
        fprintf(stderr, "psm init failed\n");
        exit(1);
    }

    // Warmup.
    size_t offset = run_loop(WARMUP_OPS, stack_writes);

    auto start = std::chrono::steady_clock::now();
    run_loop(num_ops, stack_writes, offset);
    auto end = std::chrono::steady_clock::now();
    auto time_span = std::chrono::duration_cast<std::chrono::duration<double>>(end - start);
    return time_span.count();
}
};

int main(int argc, char *argv[]) {
    pin_thread_to_core(26);

    if (argc != 3) {
        fprintf(stderr, "Usage: %s rounds stack_writes\n", argv[0]);
        exit(1);
    }

    auto rounds = static_cast<size_t>(std::stoul(argv[1]));
    auto stack_writes = static_cast<size_t>(std::stoul(argv[2]));
    if (stack_writes > 128) {
        fprintf(stderr, "too many stack writes\n");
        exit(1);
    }

    size_t num_ops = (rounds + WRITES_PER_OP - 1) / WRITES_PER_OP;

    double dur_sec = psm_bench::run(num_ops, stack_writes);

    printf("%lu,%lf,%lg\n", num_ops, dur_sec, num_ops / dur_sec);
    return 0;
}
