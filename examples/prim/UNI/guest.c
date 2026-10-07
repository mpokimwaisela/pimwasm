#include <stdint.h>

/* PrIM UNI removes adjacent duplicates, not duplicates elsewhere in the input.
 * Return the number of i64 values written. Output capacity is n elements.
 * Exact in-place operation is supported; otherwise buffers must be disjoint.
 * Unlike the native tasklet's sentinel setup, all i64 values are valid data. */
uint32_t UNI(const int64_t *input, int64_t *output, uint32_t n) {
    uint32_t count = 0;
    int64_t previous = 0;
    for (uint32_t i = 0; i < n; ++i) {
        int64_t value = input[i];
        if (i == 0 || value != previous)
            output[count++] = value;
        previous = value;
    }
    return count;
}
