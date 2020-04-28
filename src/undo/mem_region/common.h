#ifndef PSM_SRC_UNDO_MEM_REGION_COMMON_H
#define PSM_SRC_UNDO_MEM_REGION_COMMON_H

#include <cstddef>
#include <cstdint>
#include <type_traits>

#define MEM_REGION_LOGGING 0

// mem_ADDR_FILEID
constexpr size_t FILE_NAME_BUF_LEN = 4 + 16 + 1 + 8 + 1;

struct region {
    char *base;
    size_t size;
    uint file_id;

    [[nodiscard]] char *end() const { return base + size; }

    region(void *_base, size_t _size, uint _file_id)
        : base(reinterpret_cast<char *>(_base)), size(_size), file_id(_file_id) {}

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

    template <size_t N> void make_file_name(char (&buf)[N]) {
        static_assert(N >= FILE_NAME_BUF_LEN, "buffer too small");

        char *ptr = buf;
        *ptr++ = 'm';
        *ptr++ = 'e';
        *ptr++ = 'm';
        *ptr++ = '_';
        ptr = write_hex(ptr, reinterpret_cast<uintptr_t>(base));
        *ptr++ = '_';
        ptr = write_hex(ptr, file_id);
        *ptr = '\0';
    }

  private:
    template <typename T>
    static char *write_hex(char *p, T number) {
        static_assert(std::is_unsigned<T>::value, "unsigned type required");
        int hex_len = 0;
        for (T copy = number; copy > 0; copy >>= 4u, ++hex_len)
            ;
        if (hex_len == 0) {
            hex_len = 1;
        }

        for (int i = hex_len; i > 0; --i, number >>= 4u) {
            auto d = number & 0xfu;
            auto digit = d < 10 ? '0' + d : 'a' + (d - 10);
            p[i - 1] = digit;
        }
        return p + hex_len;
    }
};
#endif // PSM_SRC_UNDO_MEM_REGION_COMMON_H
