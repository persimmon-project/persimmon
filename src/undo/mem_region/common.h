#ifndef PSM_SRC_UNDO_MEM_REGION_COMMON_H
#define PSM_SRC_UNDO_MEM_REGION_COMMON_H

#include <cstddef>
#include <cstdint>

#define MEM_REGION_LOGGING 0

constexpr const char *FILE_NAME_FORMAT = "mem_%lx";
constexpr size_t FILE_NAME_BUF_LEN = 21;

struct region {
    char *base;
    size_t size;

    [[nodiscard]] char *end() const { return base + size; }

    region(void *_base, size_t _size) : base(reinterpret_cast<char *>(_base)), size(_size) {}

    [[nodiscard]] bool does_include(void *_addr) const {
        char *addr = reinterpret_cast<char *>(_addr);
        return base <= addr && addr < end();
    }

    [[nodiscard]] bool does_include(const region &other) const {
        return does_include(other.base) && does_include(other.end() - 1);
    }

    [[nodiscard]] bool does_include(const region *other) const {
        return does_include(other->base) && does_include(other->end() - 1);
    }

    [[nodiscard]] bool does_overlap_with(const region &other) const {
        return does_include(other.base) || other.does_include(base);
    }
};

// Format: mem_%lx
template <size_t N> void make_file_name(char (&buf)[N], void *base) {
    static_assert(N >= FILE_NAME_BUF_LEN, "buffer too small");

    buf[0] = 'm';
    buf[1] = 'e';
    buf[2] = 'm';
    buf[3] = '_';

    int hex_len = 1;
    auto n_base = reinterpret_cast<uintptr_t>(base);
    for (uintptr_t x = 16; n_base >= x; x <<= 4u, ++hex_len)
        ;

    for (int i = hex_len; i > 0; --i, n_base >>= 4u) {
        auto d = n_base & 0xfu;
        auto digit = d < 10 ? '0' + d : 'a' + (d - 10);
        buf[3 + i] = digit;
    }
    buf[4 + hex_len] = '\0';
}

#endif // PSM_SRC_UNDO_MEM_REGION_COMMON_H
