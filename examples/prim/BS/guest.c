#include <stdint.h>

/* PrIM BS computation, with one result per query. sorted contains count signed
 * i64 keys in ascending order. indices receives the first matching index, or
 * -1 for a missing key. Input/output buffers must be complete and disjoint.
 * Pointers are Wasm byte offsets; all sizes count elements. */
uint32_t BS(const int64_t *sorted, uint32_t count,
                       const int64_t *queries, int64_t *indices,
                       uint32_t query_count) {
    for (uint32_t q = 0; q < query_count; ++q) {
        int64_t key = queries[q];
        uint32_t low = 0, high = count;
        while (low < high) {
            uint32_t middle = low + (high - low) / 2;
            if (sorted[middle] < key)
                low = middle + 1;
            else
                high = middle;
        }
        indices[q] = low < count && sorted[low] == key ? (int64_t)low : -1;
    }
    return query_count;
}
