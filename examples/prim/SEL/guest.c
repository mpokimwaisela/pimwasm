#include <stdint.h>

/* PrIM SEL keeps !pred(x), where the native sample pred is x % 2 == 0.
 * Preserve the order of odd u64 values; return the number written. Output
 * capacity is n elements. Exact in-place operation is supported; otherwise
 * input/output must be disjoint. No accesses are made when n is zero. */
uint32_t SEL(const uint64_t *input, uint64_t *output, uint32_t n) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t value = input[i];
        if (value & 1)
            output[count++] = value;
    }
    return count;
}
