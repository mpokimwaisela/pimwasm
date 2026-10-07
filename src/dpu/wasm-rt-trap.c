#include "pimwasm-module.h"
#include "wasm-rt.h"
uint32_t wasm_rt_call_stack_depth;
void wasm_rt_trap(wasm_rt_trap_t reason) { pimwasm_module_trap(reason); }
const char *wasm_rt_strerror(wasm_rt_trap_t reason) {
  switch (reason) {
  case WASM_RT_TRAP_OOB:
    return "out of bounds";
  case WASM_RT_TRAP_INT_OVERFLOW:
    return "integer overflow";
  case WASM_RT_TRAP_DIV_BY_ZERO:
    return "division by zero";
  case WASM_RT_TRAP_INVALID_CONVERSION:
    return "invalid conversion";
  case WASM_RT_TRAP_UNREACHABLE:
    return "unreachable";
  case WASM_RT_TRAP_CALL_INDIRECT:
    return "indirect call";
  case WASM_RT_TRAP_EXHAUSTION:
    return "exhaustion";
  case WASM_RT_TRAP_UNCAUGHT_EXCEPTION:
    return "uncaught exception";
  default:
    return "unknown trap";
  }
}
