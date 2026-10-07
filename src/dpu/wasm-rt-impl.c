#include "wasm-rt-exceptions.h"
#include "wasm-rt.h"

static bool initialized;
void wasm_rt_init(void) {
  wasm_rt_call_stack_depth = 0;
  wasm_rt_exception_reset();
  initialized = true;
}

bool wasm_rt_is_initialized(void) { return initialized; }

void wasm_rt_free(void) {
  initialized = false;
  wasm_rt_call_stack_depth = 0;
  wasm_rt_exception_reset();
}
