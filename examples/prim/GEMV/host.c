#include "../../support/host.h"

#define DPU_BINARY_PATH "build/examples/GEMV"

/* Row-major unsigned GEMV; products and sums wrap modulo 2^32. */
int main(int argc, char **argv) {
    uint32_t rows = 16, columns = 32, seed = 7;
    if (argc > 4 || (argc > 1 && !app_u32(argv[1], &rows)) ||
        (argc > 2 && !app_u32(argv[2], &columns)) ||
        (argc > 3 && !app_u32(argv[3], &seed))) {
        app_fail("Usage: %s [rows [columns [seed]]]", argv[0]);
        return 2;
    }
    int result = 1;
    pimwasm_t *module = NULL;
    uint8_t *matrix = NULL, *vector = NULL, *out = NULL;
    APP_CHECK_STATUS(pimwasm_open(DPU_BINARY_PATH, &module, NULL));
    const uint32_t base = 4096;
    /* This sum fits uint64 for all uint32 dimensions. Check before multiplying
       by four or narrowing to native allocation sizes/guest offsets. */
    const uint64_t words = (uint64_t)rows * columns + columns + rows;
    const uint32_t available = pimwasm_info(module)->memory_bytes;
    if (base > available || words > (available - base) / 4) {
        app_fail("Input/output buffers exceed module memory");
        goto done;
    }
    const size_t matrix_bytes = (size_t)rows * columns * 4;
    const size_t vector_bytes = (size_t)columns * 4, output_bytes = (size_t)rows * 4;
    const uint32_t vector_at = base + (uint32_t)matrix_bytes;
    const uint32_t output_at = vector_at + (uint32_t)vector_bytes;
    matrix = malloc(matrix_bytes ? matrix_bytes : 1);
    vector = malloc(vector_bytes ? vector_bytes : 1);
    out = malloc(output_bytes ? output_bytes : 1);
    if (!matrix || !vector || !out) { app_fail("malloc: %s", strerror(errno)); goto done; }
    for (uint32_t i = 0; i < rows * columns; ++i)
        app_put_u32(matrix + 4 * i, seed + i * 104729u);
    for (uint32_t i = 0; i < columns; ++i)
        app_put_u32(vector + 4 * i, UINT32_MAX - seed - i * 7919u);
    APP_CHECK_STATUS(pimwasm_write(module, base, matrix, matrix_bytes));
    APP_CHECK_STATUS(pimwasm_write(module, vector_at, vector, vector_bytes));
    struct pimwasm_value args[] = {
        {.type = PIMWASM_I32, .bits = base}, {.type = PIMWASM_I32, .bits = vector_at},
        {.type = PIMWASM_I32, .bits = output_at}, {.type = PIMWASM_I32, .bits = rows},
        {.type = PIMWASM_I32, .bits = columns}
    };
    struct pimwasm_call call = {5, args};
    struct pimwasm_results returned = {0};
    enum pimwasm_status status = pimwasm_invoke(module, &call, &returned);
    if (status != PIMWASM_OK) {
        app_fail("GEMV: %s, trap=%u", pimwasm_status_string(status), (unsigned)returned.trap);
        goto done;
    }
    APP_CHECK(returned.count == 1 && returned.values[0].type == PIMWASM_I32 &&
              returned.values[0].bits == rows);
    APP_CHECK_STATUS(pimwasm_read(module, output_at, out, output_bytes));
    for (uint32_t row = 0; row < rows; ++row) {
        uint32_t expected = 0;
        for (uint32_t col = 0; col < columns; ++col) {
            uint64_t product = (uint64_t)app_get_u32(matrix + 4 * ((size_t)row * columns + col)) * app_get_u32(vector + 4 * col);
            expected += (uint32_t)product;
        }
        if (app_get_u32(out + 4 * row) != expected) {
            app_fail("Mismatch at row %" PRIu32 "", row); goto done;
        }
    }
    app_ok("GEMV: %" PRIu32 " x %" PRIu32 ", seed=%" PRIu32, rows, columns, seed);
    result = 0;
done:
    free(matrix); free(vector); free(out);
    if (module) pimwasm_close(module);
    return result;
}
