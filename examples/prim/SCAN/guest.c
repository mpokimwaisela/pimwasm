#include <stdint.h>

/* Sequential inclusive form of PrIM SCAN: output[i] = sum(input[0..i]).
 * All arithmetic is modulo 2^64; return the final sum, or zero for n == 0.
 * Both buffers contain n u64 elements. Exact in-place operation is supported;
 * otherwise buffers must be disjoint. */
uint64_t SCAN(const uint64_t *input, uint64_t *output, uint32_t n) {
    uint64_t sum = 0;
    for (uint32_t i = 0; i < n; ++i) {
        sum += input[i];
        output[i] = sum;
    }
    return sum;
}
