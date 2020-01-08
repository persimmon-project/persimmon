#ifndef PSM_SRC_UNDO_MEM_REGION_RANGES_H
#define PSM_SRC_UNDO_MEM_REGION_RANGES_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "dr_api.h"
#include "drvector.h"

template <typename T> class ranges {
    // Overflow is less problematic for unsigned types.
    static_assert(std::is_unsigned<T>::value, "T must be unsigned");

  public:
    ranges() : v{} {
        bool success = drvector_init(&v, /* initial capacity */ 10, false, free_range);
        DR_ASSERT(success);
    }

    ~ranges() {
        bool success = drvector_delete(&v);
        DR_ASSERT(success);
    }

    void insert(T start, size_t size) {
        // Make sure there's space for one more entry.
        drvector_append(&v, nullptr);

        const range r{start, size};
        // Insert `r` into the sorted array.
        uint i;
        for (i = 0; i < v.entries - 1; i++) {
            if (r < *static_cast<const range *>(v.array[i])) {
                break;
            }
        }
        memmove(&v.array[i + 1], &v.array[i], sizeof(void *) * (v.entries - 1 - i));
        v.array[i] = new_range(r);

        // Coalesce.
        uint l = 0;
        for (uint j = 1; j < v.entries; j++) {
            auto prev = static_cast<range *>(v.array[l]);
            auto curr = static_cast<range *>(v.array[j]);
            if (curr->start - prev->start <= prev->size) {
                prev->size = std::max(prev->size, curr->start + curr->size - prev->start);
            } else {
                v.array[++l] = v.array[j];
            }
        }
        v.entries = l + 1;
    }

    bool find(T point) const {
        for (uint i = 0; i < v.entries; i++) {
            if (static_cast<const range *>(v.array[i])->includes(point)) {
                return true;
            }
        }
        return false;
    }

    bool find(T start, size_t size) const {
        if (size == 0) {
            return true;
        }
        const range to_find{start, size};

        for (uint i = 0; i < v.entries; i++) {
            if (static_cast<const range *>(v.array[i])->includes(to_find)) {
                return true;
            }
        }
        return false;
    }

    void remove(T start, size_t size) {
        if (size == 0) {
            return;
        }
        const range to_remove{start, size};

        drvector_t new_v;
        bool success = drvector_init(&new_v, /* initial capacity */ v.entries, false, free_range);
        DR_ASSERT(success);

        for (uint i = 0; i < v.entries; i++) {
            auto curr = static_cast<const range *>(v.array[i]);
            if (curr->includes(to_remove.start)) {
                const range left{curr->start, to_remove.start - curr->start};
                if (left.size > 0) {
                    DR_ASSERT(curr->includes(left));
                    DR_ASSERT(!left.intersects(to_remove));
                    drvector_append(&new_v, new_range(left));
                }
            }
            if (curr->includes(to_remove.end() - 1)) {
                const range right{to_remove.end(), curr->end() - to_remove.end()};
                if (right.size > 0) {
                    DR_ASSERT(curr->includes(right));
                    DR_ASSERT(!right.intersects(to_remove));
                    drvector_append(&new_v, new_range(right));
                }
            }
        }

        drvector_delete(&v);
        v = new_v;
    }

    void clear() { v.entries = 0; }

    template <typename F> void foreach (F f) const {
        for (uint i = 0; i < v.entries; i++) {
            auto r = static_cast<const range *>(v.array[i]);
            f(r->start, r->size);
        }
    }

  private:
    struct range {
        T start;
        size_t size;

        // Exclusive.
        T end() const { return start + size; }

        bool operator<(const range &other) const {
            return (start < other.start) || (start == other.start && size < other.size);
        }

        bool includes(T point) const { return start <= point && point - start < size; }

        bool includes(const range &other) const {
            return other.size == 0 || (start <= other.start && other.start - start + other.size <= size);
        }

        bool intersects(const range &other) const { return includes(other.start) || other.includes(start); }
    };

    drvector_t v; // Sorted, coalesced.

    static range *new_range(const range &r) { return new (dr_global_alloc(sizeof(range))) range(r); }

    static void free_range(void *r) { dr_global_free(r, sizeof(range)); }
};

#endif // PSM_SRC_UNDO_MEM_REGION_RANGES_H
