#include <stdint.h>

/* CSR float32 y = A*x. Pointer arguments are Wasm byte offsets. row_offsets
   contains rows+1 u32 entries, column_indices/values contain nnz entries, vector
   contains columns floats, and output contains rows floats. Output must not
   overlap any input. The host must supply complete valid byte ranges.

   Zero rows return without accessing memory or validating unused arguments.
   Otherwise CSR starts at zero, has monotonic offsets, ends at nnz, and every
   column index is below columns. Malformed shape/CSR traps with unreachable;
   actual out-of-memory accesses trap with bounds. Validation is incremental:
   already completed output rows remain visible after a later trap. No extra
   nonzero is read. Multiplication and addition are separate float32 operations;
   build without contraction or reassociation. No NaN payload is promised. */
uint32_t SpMV(const uint32_t *row_offsets,
                  const uint32_t *column_indices, const float *values,
                  const float *vector, float *output,
                  uint32_t rows, uint32_t columns, uint32_t nnz) {
    if (rows == 0)
        return 0;
    /* Keep element-to-byte indexing representable, including rows+1 offsets.
       These are logical shape checks, not eager memory-range validation. */
    if (rows >= UINT32_MAX / 4 || columns > UINT32_MAX / 4 ||
        nnz > UINT32_MAX / 4)
        __builtin_trap();
    uint32_t begin = row_offsets[0];
    if (begin != 0)
        __builtin_trap();
    for (uint32_t row = 0; row < rows; ++row) {
        uint32_t end = row_offsets[row + 1];
        if (end < begin || end > nnz || (row + 1 == rows && end != nnz))
            __builtin_trap();
        float sum = 0.0f;
        for (uint32_t index = begin; index < end; ++index) {
            uint32_t column = column_indices[index];
            if (column >= columns)
                __builtin_trap();
            sum += values[index] * vector[column];
        }
        output[row] = sum;
        begin = end;
    }
    return rows;
}
