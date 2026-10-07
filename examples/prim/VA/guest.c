#include <stdint.h>

/* Pointer arguments become byte offsets in Wasm linear memory. Inputs and
   output should be disjoint, or output may exactly alias either input. */
uint32_t VA(const uint32_t *a, const uint32_t *b, uint32_t *out,
                    uint32_t length) {
  for (uint32_t i = 0; i < length; ++i)
    out[i] = a[i] + b[i];
  return length;
}
