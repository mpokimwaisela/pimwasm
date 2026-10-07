#include <stdint.h>

uint32_t count(const uint32_t *values, uint32_t length, uint32_t threshold) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < length; ++i)
        count += values[i] > threshold;
    return count;
}
