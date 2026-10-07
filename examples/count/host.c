#include "../support/host.h"

#define DPU_BINARY_PATH "build/examples/count"

int main(int argc, char **argv) {
  uint32_t length = 32, threshold = 21, stride = 1;

  if (argc > 3 || (argc > 1 && !app_u32(argv[1], &length)) ||
      (argc > 2 && !app_u32(argv[2], &threshold))) {
    app_fail("Usage: %s [length [threshold]]", argv[0]);
    return 2;
  }

  pimwasm_t *module = NULL;
  uint64_t *values = NULL;
  int status = 1;

  APP_CHECK(pimwasm_open(DPU_BINARY_PATH, &module, NULL) == PIMWASM_OK);
  const uint32_t offset = 4096;
  uint64_t words = length ? (uint64_t)(length - 1) * stride + 1 : 0;

  APP_CHECK(pimwasm_info(module)->memory_bytes >= offset);
  APP_CHECK(words <= (pimwasm_info(module)->memory_bytes - offset) / 4);
  values = malloc(words ? (size_t)words * sizeof(*values) : 1);
  APP_CHECK(values);

  for (uint32_t i = 0; i < words; ++i)
    values[i] = i * 104729u + 17u;

  uint32_t expected = 0;

  for (uint32_t i = 0; i < length; ++i)
    expected += values[(size_t)i * stride] > threshold;

  APP_CHECK(app_upload(module, offset, values, (size_t)words, 4) == PIMWASM_OK);

  const struct pimwasm_value args[] = {{.type = PIMWASM_I32, .bits = offset},
                                   {.type = PIMWASM_I32, .bits = length},
                                   {.type = PIMWASM_I32, .bits = threshold}};
  struct pimwasm_call call = {3, args};
  struct pimwasm_results results;
  enum pimwasm_status invoke_status = pimwasm_invoke(module, &call, &results);

  if (invoke_status != PIMWASM_OK) {
    app_fail("count: %s, trap=%u",
            pimwasm_status_string(invoke_status), (unsigned)results.trap);
    goto done;
  }

  APP_CHECK(results.count == 1 && results.values[0].type == PIMWASM_I32 &&
            results.values[0].bits == expected);

  app_ok("count: result=%" PRIu32, expected);
  status = 0;

done:
  free(values);
  if (module)
    pimwasm_close(module);
  return status;
}
