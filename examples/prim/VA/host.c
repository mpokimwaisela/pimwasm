#include "../../support/host.h"

#define DPU_BINARY_PATH "build/examples/VA"

/* Run from the repository root; argv supplies runtime input dimensions/seed. */
int main(int argc, char **argv) {
  uint32_t length = 1024, seed = 7;
  if (argc > 3 || (argc > 1 && !app_u32(argv[1], &length)) ||
      (argc > 2 && !app_u32(argv[2], &seed))) {
    app_fail("Usage: %s [length [seed]]", argv[0]);
    return 2;
  }
  int result = 1;
  pimwasm_t *module = NULL;
  uint8_t *a = NULL, *b = NULL, *out = NULL;
  APP_CHECK_STATUS(pimwasm_open(DPU_BINARY_PATH, &module, NULL));
  const uint32_t base = 4096;
  const uint64_t total = (uint64_t)length * 12;
  const uint32_t available = pimwasm_info(module)->memory_bytes;
  if (base > available || total > available - base) {
    app_fail("Input/output buffers exceed module memory");
    goto done;
  }
  const size_t bytes = (size_t)length * 4;
  const uint32_t b_at = base + (uint32_t)bytes;
  const uint32_t out_at = b_at + (uint32_t)bytes;
  a = malloc(bytes ? bytes : 1);
  b = malloc(bytes ? bytes : 1);
  out = malloc(bytes ? bytes : 1);
  if (!a || !b || !out) {
    app_fail("malloc: %s", strerror(errno));
    goto done;
  }
  for (uint32_t i = 0; i < length; ++i) {
    app_put_u32(a + 4 * i, i ? seed + i * 1664525u : UINT32_MAX);
    app_put_u32(b + 4 * i, i ? seed ^ (i * 1013904223u) : 1);
  }
  APP_CHECK_STATUS(pimwasm_write(module, base, a, bytes));
  APP_CHECK_STATUS(pimwasm_write(module, b_at, b, bytes));
  /* Pointer arguments are byte offsets, never native host pointers. */
  struct pimwasm_value args[] = {{.type = PIMWASM_I32, .bits = base},
                             {.type = PIMWASM_I32, .bits = b_at},
                             {.type = PIMWASM_I32, .bits = out_at},
                             {.type = PIMWASM_I32, .bits = length}};
  struct pimwasm_call call = {4, args};
  struct pimwasm_results returned = {0};
  enum pimwasm_status status = pimwasm_invoke(module, &call, &returned);
  if (status != PIMWASM_OK) {
    app_fail("VA: %s, trap=%u", pimwasm_status_string(status),
            (unsigned)returned.trap);
    goto done;
  }
  APP_CHECK(returned.count == 1 && returned.values[0].type == PIMWASM_I32 &&
            returned.values[0].bits == length);
  APP_CHECK_STATUS(pimwasm_read(module, out_at, out, bytes));
  for (uint32_t i = 0; i < length; ++i) {
    uint32_t expected =
        (uint32_t)((uint64_t)app_get_u32(a + 4 * i) + app_get_u32(b + 4 * i));
    if (app_get_u32(out + 4 * i) != expected) {
      app_fail("Mismatch at element %" PRIu32 "", i);
      goto done;
    }
  }
  app_ok("VA: %" PRIu32 " elements, seed=%" PRIu32, length, seed);
  result = 0;
done:
  free(a);
  free(b);
  free(out);
  if (module)
    pimwasm_close(module);
  return result;
}