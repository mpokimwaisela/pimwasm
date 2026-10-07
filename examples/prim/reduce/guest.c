#include <stdint.h>
/* Arguments follow the common raw-i32 invocation interface. The output pointer
   is an ordinary Wasm byte offset, not a native mailbox address. */
uint32_t reduce(const uint32_t *values, uint32_t length, uint32_t *output) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < length; ++i) sum += values[i];
    *output = sum;
    return sum;
}
