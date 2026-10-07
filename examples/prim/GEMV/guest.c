#include <stdint.h>

/* Row-major y = A*x, with uint32 multiplication/addition wrapping modulo 2^32.
   Pointer arguments are Wasm byte offsets. The output must not overlap either
   input; callers provide complete, valid matrix/vector/output ranges. */
uint32_t GEMV(const uint32_t *matrix, const uint32_t *vector,
              uint32_t *output, uint32_t rows, uint32_t columns) {
    for (uint32_t row = 0; row < rows; ++row) {
        uint32_t sum = 0;
        for (uint32_t column = 0; column < columns; ++column)
            sum += matrix[row * columns + column] * vector[column];
        output[row] = sum;
    }
    return rows;
}
