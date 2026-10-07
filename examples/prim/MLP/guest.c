#include <stdint.h>

/* One PrIM-style integer MLP layer: output = ReLU(weights * input), with no
   bias. weights is a row-major rows-by-columns array. Pointer arguments are
   Wasm byte offsets; all entries are signed int32 bit patterns. Use uint32
   multiplication and addition to explicitly define wrap modulo 2^32, then
   interpret the final sign bit for ReLU. This matches int32 dot products when
   they do not overflow and defines overflow without signed-C undefined behavior.

   Output must not overlap either input; callers supply complete valid matrix,
   input and output ranges. Zero rows touch no memory; zero columns produce zero
   for each row. A bounds trap can leave earlier rows written. Multiple layers
   are consecutive PIMWASM calls with resident weights and separate activation
   buffers: no new host interface or guest calls are required. */
uint32_t MLP(const uint32_t *weights, const uint32_t *input,
                   uint32_t *output, uint32_t rows, uint32_t columns) {
    for (uint32_t row = 0; row < rows; ++row) {
        uint32_t sum = 0;
        for (uint32_t column = 0; column < columns; ++column)
            sum += weights[row * columns + column] * input[column];
        output[row] = (sum & UINT32_C(0x80000000)) ? 0 : sum;
    }
    return rows;
}
