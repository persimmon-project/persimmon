#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

#include <sys/mman.h>
#include <sched.h>

#include <libpmem.h>
#include <libpsm/psm.h>
#include <x86intrin.h>

constexpr size_t WRITES_PER_OP = 1024;
constexpr size_t WARMUP_OPS = PSM_LOG_SIZE_B / 64 * 4;
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

template <bool persistent, size_t locs>[[gnu::noinline]] size_t run_op(size_t offset) {
    static_assert(WRITES_PER_OP % locs == 0, "WRITES_PER_OP must be divisible by locs");
    auto p = static_cast<volatile block *>(mem) + offset;
    for (size_t i = 0; i < WRITES_PER_OP / locs; i++) {
        for (size_t j = 0; j < locs; j++) {
            volatile block *this_p = &p[j];

            __m256i val = _mm256_load_si256((const __m256i *)this_p);
            val = _mm256_add_epi64(val, val);
            _mm256_store_si256((__m256i *)this_p, val);

            if constexpr (persistent) {
                _mm_clwb((void *)this_p);
                _mm_sfence();
            }
            asm volatile("": : :"memory");
        }
    }
    return (offset + locs) % NUM_BLOCKS;
}

namespace baseline {

// Returns number of seconds.
template<bool persistent, size_t locs>
double run(const std::string &mode, size_t num_ops) {
    if (mode == "dram") {
        mem = mmap(nullptr, MEM_SIZE_B, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE, -1, 0);
        if (mem == MAP_FAILED) {
            perror("mmap");
            exit(1);
        }
    } else if (mode == "pm" || mode == "pm-no-persist") {
        int is_pmem;
        mem = pmem_map_file("/mnt/pmem1/foo", MEM_SIZE_B, PMEM_FILE_CREATE, 0666, nullptr, &is_pmem);
        if (nullptr == mem) {
            perror("pmem_map_file");
            exit(1);
        }
        if (!is_pmem) { // We require the log to be on persistent memory.
            fprintf(stderr, "not pm?\n");
            exit(1);
        }
    } else {
        fprintf(stderr, "invalid mode: %s\n", mode.c_str());
        exit(1);
    }

    auto constexpr f = run_op<persistent, locs>;

    size_t offset = 0;
    { // Warmup.
        for (size_t i = 0; i < WARMUP_OPS; i++) {
            offset = f(offset);
        }
    }

    auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < num_ops; i++) {
        offset = f(offset);
    }
    auto end = std::chrono::steady_clock::now();
    auto time_span = std::chrono::duration_cast<std::chrono::duration<double>>(end - start);
    return time_span.count();
}
};

namespace psm_bench {
typedef struct {
    uint64_t unused;
    uint64_t offset;
} op_t;

template<size_t locs>
int consume(const void *p) {
    op_t op;
    memcpy(&op, p, sizeof(op));
    run_op</* persistent = */ false, locs>(op.offset);
    return sizeof(op);
}

template<size_t locs>
size_t run_loop(size_t num_ops, size_t offset = 0) {
    op_t op = {.unused = 1, .offset = 0};
    for (size_t i = 0; i < num_ops; i++) {
        op.offset = offset;
        void *p = psm_reserve(sizeof(op));
        memcpy(p, &op, sizeof(op));
        psm_commit(/* push_only = */ false);
        offset = (offset + locs) % NUM_BLOCKS;
    }
    return offset;
}

// Returns number of seconds.
template<size_t locs>
double run(size_t num_ops) {
    mem = mmap(nullptr, MEM_SIZE_B, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE, -1, 0);
    if (mem == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }

    psm_config_t config = {
        .use_sga = false,
        .pin_core = 27,
        .consume_func = consume<locs>,
        .mode = PSM_MODE_UNDO,
        .pmem_path = "/mnt/pmem1/bench",
        .undo = {.criu_service_path = "/tmp/criu_service.socket"},
    };
    if (psm_init(&config) != 0) {
        fprintf(stderr, "psm init failed\n");
        exit(1);
    }

    // Warmup.
    size_t offset = run_loop<locs>(WARMUP_OPS);

    auto start = std::chrono::steady_clock::now();
    run_loop<locs>(num_ops, offset);
    auto end = std::chrono::steady_clock::now();
    auto time_span = std::chrono::duration_cast<std::chrono::duration<double>>(end - start);
    return time_span.count();
}
};

template<bool persistent, size_t locs>
int actual_main(const std::string &mode, size_t rounds) {
    static_assert(NUM_BLOCKS % locs == 0, "TOTAL_ELEMS must be divisible by locs");
    size_t num_ops = (rounds + WRITES_PER_OP - 1) / WRITES_PER_OP;

    double dur_sec;
    if (mode == "psm") {
        dur_sec = psm_bench::run<locs>(num_ops);
    } else {
        dur_sec = baseline::run<persistent, locs>(mode, num_ops);
    }

    printf("%lu,%lf,%lg\n", num_ops, dur_sec, num_ops / dur_sec);
    return 0;
}

int main(int argc, char *argv[]) {
    pin_thread_to_core(26);

    if (argc != 4) {
        fprintf(stderr, "Usage: %s mode rounds locs\n", argv[0]);
        exit(1);
    }

    auto mode = std::string(argv[1]);
    auto rounds = static_cast<size_t>(std::stoul(argv[2]));
    auto locs = static_cast<size_t>(std::stoul(argv[3]));

    if (mode == "pm") {
        if (locs == 1) return actual_main<true, 1>(mode, rounds);
        if (locs == 2) return actual_main<true, 2>(mode, rounds);
        if (locs == 4) return actual_main<true, 4>(mode, rounds);
        if (locs == 8) return actual_main<true, 8>(mode, rounds);
        if (locs == 16) return actual_main<true, 16>(mode, rounds);
        if (locs == 32) return actual_main<true, 32>(mode, rounds);
        if (locs == 64) return actual_main<true, 64>(mode, rounds);
        if (locs == 128) return actual_main<true, 128>(mode, rounds);
        if (locs == 256) return actual_main<true, 256>(mode, rounds);
        if (locs == 512) return actual_main<true, 512>(mode, rounds);
        if (locs == 1024) return actual_main<true, 1024>(mode, rounds);
    } else {
        if (locs == 1) return actual_main<false, 1>(mode, rounds);
        if (locs == 2) return actual_main<false, 2>(mode, rounds);
        if (locs == 4) return actual_main<false, 4>(mode, rounds);
        if (locs == 8) return actual_main<false, 8>(mode, rounds);
        if (locs == 16) return actual_main<false, 16>(mode, rounds);
        if (locs == 32) return actual_main<false, 32>(mode, rounds);
        if (locs == 64) return actual_main<false, 64>(mode, rounds);
        if (locs == 128) return actual_main<false, 128>(mode, rounds);
        if (locs == 256) return actual_main<false, 256>(mode, rounds);
        if (locs == 512) return actual_main<false, 512>(mode, rounds);
        if (locs == 1024) return actual_main<false, 1024>(mode, rounds);
    }

    fprintf(stderr, "invalid arguments\n");
    exit(1);
}
