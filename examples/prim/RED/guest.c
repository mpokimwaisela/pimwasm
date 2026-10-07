#include <stdint.h>

/* PrIM RED, sequential u64 sum modulo 2^64. n is an element count; input must
 * contain n complete elements. Empty input returns zero without accessing it. */
uint64_t RED(const uint64_t *input, uint32_t n) {
    uint64_t sum = 0;
    for (uint32_t i = 0; i < n; ++i)
        sum += input[i];
    return sum;
}
