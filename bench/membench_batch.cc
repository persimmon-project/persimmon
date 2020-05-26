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

constexpr size_t WRITES_PER_OP = 1024;
constexpr size_t WARMUP_OPS = PSM_LOG_SIZE_B / 64 * 2;

// Each element is a `uint64_t`.
//constexpr size_t TOTAL_ELEMS = 1ull << 24u;

//static_assert(TOTAL_ELEMS > 0, "there must be at least one element");
//static_assert((TOTAL_ELEMS & (TOTAL_ELEMS-1)) == 0, "TOTAL_ELEMS should be power of two");
//constexpr size_t MEM_SIZE_B = TOTAL_ELEMS * sizeof(uint64_t);
constexpr size_t MEM_SIZE_B = 1ull << 30u;

void *mem;

typedef struct {
    char x[32];
} block;
static_assert(MEM_SIZE_B % sizeof(block) == 0);
constexpr size_t NUM_BLOCKS = MEM_SIZE_B / sizeof(block);

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

[[gnu::noinline]] void run_op(size_t offset) {
    auto p = static_cast<volatile block *>(mem);
    for (size_t i = 0; i < WRITES_PER_OP; i++) {
        auto this_p = &p[(offset + i) % NUM_BLOCKS];
        __m256i val = _mm256_load_si256((const __m256i *)this_p);
        val = _mm256_add_epi64(val, val);
        _mm256_store_si256((__m256i *)this_p, val);
        asm volatile("": : :"memory");
    }
}

namespace psm_bench {
typedef struct {
    uint64_t unused;
    uint64_t offset;
} op_t;

int consume(const void *p) {
    size_t offset;
    // op_t op;
    // memcpy(&op, p, sizeof(op));
    memcpy(&offset, (char *) p + offsetof(op_t, offset), sizeof(offset));
    run_op(offset);
    return sizeof(op_t);
}

size_t run_loop(size_t num_ops, size_t advance, size_t offset = 0) {
    op_t op = {.unused = 1, .offset = 0};
    for (size_t i = 0; i < num_ops; i++) {
        op.offset = offset;
        void *p = psm_reserve(sizeof(op));
        memcpy(p, &op, sizeof(op));
        psm_commit(/* push_only = */ false);
        offset = (offset + advance) % NUM_BLOCKS;
    }
    return offset;
}

// Returns number of seconds.
double run(size_t num_ops, size_t advance) {
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
    size_t offset = run_loop(WARMUP_OPS, advance);

    auto start = std::chrono::steady_clock::now();
    run_loop(num_ops, advance, offset);
    auto end = std::chrono::steady_clock::now();
    auto time_span = std::chrono::duration_cast<std::chrono::duration<double>>(end - start);
    return time_span.count();
}
};

int main(int argc, char *argv[]) {
    pin_thread_to_core(26);

    if (argc != 3) {
        fprintf(stderr, "Usage: %s rounds advance\n", argv[0]);
        exit(1);
    }

    auto rounds = static_cast<size_t>(std::stoul(argv[1]));
    auto advance = static_cast<size_t>(std::stoul(argv[2]));
    if (advance > WRITES_PER_OP) {
        fprintf(stderr, "too much advance\n");
        exit(1);
    }
    if ((advance & (advance-1)) != 0) {
        fprintf(stderr, "advance must be power of two\n");
        exit(1);
    }

    size_t num_ops = (rounds + WRITES_PER_OP - 1) / WRITES_PER_OP;

    double dur_sec = psm_bench::run(num_ops, advance);

    printf("%lu,%lf,%lg\n", num_ops, dur_sec, num_ops / dur_sec);
    return 0;
}
