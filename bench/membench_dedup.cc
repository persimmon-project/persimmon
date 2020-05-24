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

template <bool persistent, size_t locs, size_t stride>[[gnu::noinline]] size_t run_op(size_t offset) {
    static_assert(WRITES_PER_OP % locs == 0, "WRITES_PER_OP must be divisible by locs");
    auto p = static_cast<volatile uint64_t *>(mem) + offset;
    for (size_t i = 0; i < WRITES_PER_OP / locs; i++) {
        for (size_t j = 0; j < locs; j++) {
            volatile uint64_t *this_p = &p[j * stride];

            if constexpr (LARGE_ACCESS) {
                static_assert(stride >= 4, "stride too small for large access");
                __m256i val = _mm256_load_si256((const __m256i *)this_p);
                val = _mm256_add_epi64(val, val);
                _mm256_store_si256((__m256i *)this_p, val);
            } else {
                uint64_t val = *this_p;
                val *= val;
                *this_p = val;
            }

            if constexpr (persistent) {
                _mm_clwb((void *)this_p);
                _mm_sfence();
            }
            asm volatile("": : :"memory");
        }
    }
    return (offset + locs * stride) % TOTAL_ELEMS;
}

namespace baseline {

// Returns number of seconds.
template<bool persistent, size_t locs, size_t stride>
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

    auto constexpr f = run_op<persistent, locs, stride>;

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

template<size_t locs, size_t stride>
int consume(const void *p) {
    op_t op;
    memcpy(&op, p, sizeof(op));
    run_op</* persistent = */ false, locs, stride>(op.offset);
    return sizeof(op);
}

template<size_t locs, size_t stride>
size_t run_loop(size_t num_ops, size_t offset = 0) {
    op_t op = {.unused = 1, .offset = 0};
    for (size_t i = 0; i < num_ops; i++) {
        op.offset = offset;
        void *p = psm_reserve(sizeof(op));
        memcpy(p, &op, sizeof(op));
        psm_commit(/* push_only = */ false);
        offset = (offset + locs * stride) % TOTAL_ELEMS;
    }
    return offset;
}

// Returns number of seconds.
template<size_t locs, size_t stride>
double run(size_t num_ops) {
    mem = mmap(nullptr, MEM_SIZE_B, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE, -1, 0);
    if (mem == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }

    psm_config_t config = {
        .use_sga = false,
        .pin_core = 27,
        .consume_func = consume<locs, stride>,
        .mode = PSM_MODE_UNDO,
        .pmem_path = "/mnt/pmem1/bench",
        .undo = {.criu_service_path = "/tmp/criu_service.socket"},
    };
    if (psm_init(&config) != 0) {
        fprintf(stderr, "psm init failed\n");
        exit(1);
    }

    // Warmup.
    size_t offset = run_loop<locs, stride>(WARMUP_OPS);

    auto start = std::chrono::steady_clock::now();
    run_loop<locs, stride>(num_ops, offset);
    auto end = std::chrono::steady_clock::now();
    auto time_span = std::chrono::duration_cast<std::chrono::duration<double>>(end - start);
    return time_span.count();
}
};

template<bool persistent, size_t locs, size_t stride>
int actual_main(const std::string &mode, size_t rounds) {
    static_assert(TOTAL_ELEMS % (locs * stride) == 0, "TOTAL_ELEMS must be divisible by (locs * stride)");
    size_t num_ops = (rounds + WRITES_PER_OP - 1) / WRITES_PER_OP;

    double dur_sec;
    if (mode == "psm") {
        dur_sec = psm_bench::run<locs, stride>(num_ops);
    } else {
        dur_sec = baseline::run<persistent, locs, stride>(mode, num_ops);
    }

    printf("%lu,%lf,%lg\n", num_ops, dur_sec, num_ops / dur_sec);
    return 0;
}

int main(int argc, char *argv[]) {
    pin_thread_to_core(26);

    if (argc != 5) {
        fprintf(stderr, "Usage: %s mode rounds locs stride\n", argv[0]);
        exit(1);
    }

    auto mode = std::string(argv[1]);
    auto rounds = static_cast<size_t>(std::stoul(argv[2]));
    auto locs = static_cast<size_t>(std::stoul(argv[3]));
    auto stride = static_cast<size_t>(std::stoul(argv[4]));

    if (mode == "pm") {
        if (locs == 1 && stride == 4) return actual_main<true, 1, 4>(mode, rounds);
        if (locs == 2 && stride == 4) return actual_main<true, 2, 4>(mode, rounds);
        if (locs == 4 && stride == 4) return actual_main<true, 4, 4>(mode, rounds);
        if (locs == 8 && stride == 4) return actual_main<true, 8, 4>(mode, rounds);
        if (locs == 16 && stride == 4) return actual_main<true, 16, 4>(mode, rounds);
        if (locs == 32 && stride == 4) return actual_main<true, 32, 4>(mode, rounds);
        if (locs == 64 && stride == 4) return actual_main<true, 64, 4>(mode, rounds);
        if (locs == 128 && stride == 4) return actual_main<true, 128, 4>(mode, rounds);
        if (locs == 1 && stride == 8) return actual_main<true, 1, 8>(mode, rounds);
        if (locs == 2 && stride == 8) return actual_main<true, 2, 8>(mode, rounds);
        if (locs == 4 && stride == 8) return actual_main<true, 4, 8>(mode, rounds);
        if (locs == 8 && stride == 8) return actual_main<true, 8, 8>(mode, rounds);
        if (locs == 16 && stride == 8) return actual_main<true, 16, 8>(mode, rounds);
        if (locs == 32 && stride == 8) return actual_main<true, 32, 8>(mode, rounds);
        if (locs == 64 && stride == 8) return actual_main<true, 64, 8>(mode, rounds);
        if (locs == 128 && stride == 8) return actual_main<true, 128, 8>(mode, rounds);
        if (locs == 1 && stride == 16) return actual_main<true, 1, 16>(mode, rounds);
        if (locs == 2 && stride == 16) return actual_main<true, 2, 16>(mode, rounds);
        if (locs == 4 && stride == 16) return actual_main<true, 4, 16>(mode, rounds);
        if (locs == 8 && stride == 16) return actual_main<true, 8, 16>(mode, rounds);
        if (locs == 16 && stride == 16) return actual_main<true, 16, 16>(mode, rounds);
        if (locs == 32 && stride == 16) return actual_main<true, 32, 16>(mode, rounds);
        if (locs == 64 && stride == 16) return actual_main<true, 64, 16>(mode, rounds);
        if (locs == 128 && stride == 16) return actual_main<true, 128, 16>(mode, rounds);
        if (locs == 1 && stride == 32) return actual_main<true, 1, 32>(mode, rounds);
        if (locs == 2 && stride == 32) return actual_main<true, 2, 32>(mode, rounds);
        if (locs == 4 && stride == 32) return actual_main<true, 4, 32>(mode, rounds);
        if (locs == 8 && stride == 32) return actual_main<true, 8, 32>(mode, rounds);
        if (locs == 16 && stride == 32) return actual_main<true, 16, 32>(mode, rounds);
        if (locs == 32 && stride == 32) return actual_main<true, 32, 32>(mode, rounds);
        if (locs == 64 && stride == 32) return actual_main<true, 64, 32>(mode, rounds);
        if (locs == 128 && stride == 32) return actual_main<true, 128, 32>(mode, rounds);
        if (locs == 1 && stride == 64) return actual_main<true, 1, 64>(mode, rounds);
        if (locs == 2 && stride == 64) return actual_main<true, 2, 64>(mode, rounds);
        if (locs == 4 && stride == 64) return actual_main<true, 4, 64>(mode, rounds);
        if (locs == 8 && stride == 64) return actual_main<true, 8, 64>(mode, rounds);
        if (locs == 16 && stride == 64) return actual_main<true, 16, 64>(mode, rounds);
        if (locs == 32 && stride == 64) return actual_main<true, 32, 64>(mode, rounds);
        if (locs == 64 && stride == 64) return actual_main<true, 64, 64>(mode, rounds);
        if (locs == 128 && stride == 64) return actual_main<true, 128, 64>(mode, rounds);
        if (locs == 1 && stride == 128) return actual_main<true, 1, 128>(mode, rounds);
        if (locs == 2 && stride == 128) return actual_main<true, 2, 128>(mode, rounds);
        if (locs == 4 && stride == 128) return actual_main<true, 4, 128>(mode, rounds);
        if (locs == 8 && stride == 128) return actual_main<true, 8, 128>(mode, rounds);
        if (locs == 16 && stride == 128) return actual_main<true, 16, 128>(mode, rounds);
        if (locs == 32 && stride == 128) return actual_main<true, 32, 128>(mode, rounds);
        if (locs == 64 && stride == 128) return actual_main<true, 64, 128>(mode, rounds);
        if (locs == 128 && stride == 128) return actual_main<true, 128, 128>(mode, rounds);
    } else {
        if (locs == 1 && stride == 4) return actual_main<false, 1, 4>(mode, rounds);
        if (locs == 2 && stride == 4) return actual_main<false, 2, 4>(mode, rounds);
        if (locs == 4 && stride == 4) return actual_main<false, 4, 4>(mode, rounds);
        if (locs == 8 && stride == 4) return actual_main<false, 8, 4>(mode, rounds);
        if (locs == 16 && stride == 4) return actual_main<false, 16, 4>(mode, rounds);
        if (locs == 32 && stride == 4) return actual_main<false, 32, 4>(mode, rounds);
        if (locs == 64 && stride == 4) return actual_main<false, 64, 4>(mode, rounds);
        if (locs == 128 && stride == 4) return actual_main<false, 128, 4>(mode, rounds);
        if (locs == 1 && stride == 8) return actual_main<false, 1, 8>(mode, rounds);
        if (locs == 2 && stride == 8) return actual_main<false, 2, 8>(mode, rounds);
        if (locs == 4 && stride == 8) return actual_main<false, 4, 8>(mode, rounds);
        if (locs == 8 && stride == 8) return actual_main<false, 8, 8>(mode, rounds);
        if (locs == 16 && stride == 8) return actual_main<false, 16, 8>(mode, rounds);
        if (locs == 32 && stride == 8) return actual_main<false, 32, 8>(mode, rounds);
        if (locs == 64 && stride == 8) return actual_main<false, 64, 8>(mode, rounds);
        if (locs == 128 && stride == 8) return actual_main<false, 128, 8>(mode, rounds);
        if (locs == 1 && stride == 16) return actual_main<false, 1, 16>(mode, rounds);
        if (locs == 2 && stride == 16) return actual_main<false, 2, 16>(mode, rounds);
        if (locs == 4 && stride == 16) return actual_main<false, 4, 16>(mode, rounds);
        if (locs == 8 && stride == 16) return actual_main<false, 8, 16>(mode, rounds);
        if (locs == 16 && stride == 16) return actual_main<false, 16, 16>(mode, rounds);
        if (locs == 32 && stride == 16) return actual_main<false, 32, 16>(mode, rounds);
        if (locs == 64 && stride == 16) return actual_main<false, 64, 16>(mode, rounds);
        if (locs == 128 && stride == 16) return actual_main<false, 128, 16>(mode, rounds);
        if (locs == 1 && stride == 32) return actual_main<false, 1, 32>(mode, rounds);
        if (locs == 2 && stride == 32) return actual_main<false, 2, 32>(mode, rounds);
        if (locs == 4 && stride == 32) return actual_main<false, 4, 32>(mode, rounds);
        if (locs == 8 && stride == 32) return actual_main<false, 8, 32>(mode, rounds);
        if (locs == 16 && stride == 32) return actual_main<false, 16, 32>(mode, rounds);
        if (locs == 32 && stride == 32) return actual_main<false, 32, 32>(mode, rounds);
        if (locs == 64 && stride == 32) return actual_main<false, 64, 32>(mode, rounds);
        if (locs == 128 && stride == 32) return actual_main<false, 128, 32>(mode, rounds);
        if (locs == 1 && stride == 64) return actual_main<false, 1, 64>(mode, rounds);
        if (locs == 2 && stride == 64) return actual_main<false, 2, 64>(mode, rounds);
        if (locs == 4 && stride == 64) return actual_main<false, 4, 64>(mode, rounds);
        if (locs == 8 && stride == 64) return actual_main<false, 8, 64>(mode, rounds);
        if (locs == 16 && stride == 64) return actual_main<false, 16, 64>(mode, rounds);
        if (locs == 32 && stride == 64) return actual_main<false, 32, 64>(mode, rounds);
        if (locs == 64 && stride == 64) return actual_main<false, 64, 64>(mode, rounds);
        if (locs == 128 && stride == 64) return actual_main<false, 128, 64>(mode, rounds);
        if (locs == 1 && stride == 128) return actual_main<false, 1, 128>(mode, rounds);
        if (locs == 2 && stride == 128) return actual_main<false, 2, 128>(mode, rounds);
        if (locs == 4 && stride == 128) return actual_main<false, 4, 128>(mode, rounds);
        if (locs == 8 && stride == 128) return actual_main<false, 8, 128>(mode, rounds);
        if (locs == 16 && stride == 128) return actual_main<false, 16, 128>(mode, rounds);
        if (locs == 32 && stride == 128) return actual_main<false, 32, 128>(mode, rounds);
        if (locs == 64 && stride == 128) return actual_main<false, 64, 128>(mode, rounds);
        if (locs == 128 && stride == 128) return actual_main<false, 128, 128>(mode, rounds);
    }

    fprintf(stderr, "invalid arguments\n");
    exit(1);
}
