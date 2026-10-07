#include "../support/host.h"

#define DPU_BINARY_PATH "build/examples/add"

int main(int argc, char **argv) {
  int exit_code = 1;
  uint32_t a = 20, b = 22;
  pimwasm_t *module = NULL;

  if (argc != 1 &&
      (argc != 3 || !app_u32(argv[1], &a) || !app_u32(argv[2], &b))) {
    app_fail("Usage: %s [A B]", argv[0]);
    return 2;
  }

  APP_CHECK(pimwasm_open(DPU_BINARY_PATH, &module, NULL) == PIMWASM_OK);

  struct pimwasm_value args[] = {{.type = PIMWASM_I32, .bits = a},
                                 {.type = PIMWASM_I32, .bits = b}};
  struct pimwasm_call call = {2, args};
  struct pimwasm_results results;

  APP_CHECK(pimwasm_invoke(module, &call, &results) == PIMWASM_OK);
  APP_CHECK(results.count == 1 && results.values[0].type == PIMWASM_I32 &&
            results.values[0].bits == (uint32_t)(a + b));

  app_ok("add(%" PRIu32 ", %" PRIu32 ") = %" PRIu64, a, b,
         results.values[0].bits);
  exit_code = 0;
done:
  pimwasm_close(module);
  return exit_code;
}
