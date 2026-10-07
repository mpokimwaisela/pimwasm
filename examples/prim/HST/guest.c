#include <stdint.h>

/* PrIM HST's 12-bit samples and proportional bin mapping. Include the native
 * host's clamp-to-4095 preprocessing; uint64_t multiplication avoids overflow.
 * Clear all bin_count u32 counters before processing n u32 samples. Input and
 * bins must be complete and disjoint. Return n, except bin_count == 0 returns
 * zero without accessing either buffer. Counters wrap modulo 2^32. */
uint32_t HST(const uint32_t *input, uint32_t *bins, uint32_t n,
                     uint32_t bin_count) {
    if (bin_count == 0)
        return 0;
    for (uint32_t bin = 0; bin < bin_count; ++bin)
        bins[bin] = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t value = input[i];
        if (value > 4095)
            value = 4095;
        uint32_t bin = (uint32_t)(((uint64_t)value * bin_count) >> 12);
        bins[bin] += 1;
    }
    return n;
}
