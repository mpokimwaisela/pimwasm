/* Platform services and module execution for the DPU Wasm embedding. */
#include "pimwasm-module.h"
#include "guest.h"
#include "metadata.h"
#include "wasm-rt-exceptions.h"
#include "wasm-rt-memory.h"
#include "wasm-rt.h"
#include "wire.h"
#include <assert.h>
#include <defs.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(NR_TASKLETS == 1,
               "PIMWASM instance, cache and mailbox require one tasklet");
_Static_assert(PIMWASM_EXPORT_COUNT <= PIMWASM_MAX_EXPORTS &&
                   PIMWASM_GLOBAL_COUNT <= PIMWASM_MAX_GLOBAL_EXPORTS,
               "Unsupported export count");

/* Bind the native resource proof to the depth guard compiled into this product.
 */
__host uint32_t pimwasm_call_depth_limit = PIMWASM_CALL_DEPTH_LIMIT;
__host struct pimwasm_wire_request pimwasm_request;
__host struct pimwasm_wire_response pimwasm_response;
__host struct pimwasm_module_info pimwasm_metadata = {
    .abi_version = PIMWASM_ABI_VERSION,
    .memory_bytes = PIMWASM_MEMORY_BYTES,
    .memory_count = PIMWASM_MEMORY_COUNT,
    .initial_memory_bytes = PIMWASM_MEMORY_BYTES,
    .memory_capacity_bytes = PIMWASM_MEMORY_CAPACITY_BYTES,
    .table_count = PIMWASM_TABLE_COUNT,
};
__host struct pimwasm_export_table pimwasm_exports = {
    .native_abi = PIMWASM_NATIVE_ABI_VERSION,
    .count = PIMWASM_EXPORT_COUNT,
    .entries = {PIMWASM_EXPORT_TABLE},
};
__host struct pimwasm_global_table pimwasm_globals = {
    .native_abi = PIMWASM_NATIVE_ABI_VERSION,
    .count = PIMWASM_GLOBAL_COUNT,
    .entries = {PIMWASM_GLOBAL_TABLE},
};
__host struct pimwasm_table_exports pimwasm_tables = {
    .native_abi = PIMWASM_NATIVE_ABI_VERSION,
    .count = PIMWASM_TABLE_EXPORT_COUNT,
    .entries = {PIMWASM_TABLE_EXPORT_TABLE}};

static pimwasm_guest_t instance;
static bool ready;

/* Default builtins. Other imports require explicit generated bindings;
   no dynamic native symbol lookup is provided. */
void pimwasm_builtin_abort(void) { wasm_rt_trap(WASM_RT_TRAP_UNREACHABLE); }

void pimwasm_builtin_assert(uint32_t condition) {
  if (!condition)
    wasm_rt_trap(WASM_RT_TRAP_UNREACHABLE);
}

uint32_t pimwasm_builtin_tasklet_id(void) { return me(); }

static void begin_launch(void) {
  /* Host writes cannot race this single-tasklet launch. */
  pimwasm_memory_invalidate();
  wasm_rt_init();
}

static void finish(enum pimwasm_status status, enum pimwasm_trap trap, uint64_t value) {
  pimwasm_response.status = status;
  pimwasm_response.trap = trap;
  pimwasm_response.value = value;
  if (status != PIMWASM_OK)
    memset(pimwasm_response.extra, 0, sizeof(pimwasm_response.extra));
  pimwasm_response.reserved = 0;
  pimwasm_metadata.memory_bytes = wasm_rt_memory_size_bytes;
  pimwasm_response.memory_bytes = pimwasm_metadata.memory_bytes;
  pimwasm_response.table_size = wasm_rt_table_current_size;
  pimwasm_response.completed = PIMWASM_WIRE_DONE;
}

void pimwasm_module_trap(wasm_rt_trap_t reason) {
  enum pimwasm_trap trap;
  switch (reason) {
  case WASM_RT_TRAP_UNCAUGHT_EXCEPTION:
    trap = PIMWASM_TRAP_UNCAUGHT_EXCEPTION;
    break;
  case WASM_RT_TRAP_CALL_INDIRECT:
    trap = PIMWASM_TRAP_INDIRECT_CALL;
    break;
  case WASM_RT_TRAP_OOB:
    trap = PIMWASM_TRAP_BOUNDS;
    break;
  case WASM_RT_TRAP_UNREACHABLE:
    trap = PIMWASM_TRAP_UNREACHABLE;
    break;
  case WASM_RT_TRAP_EXHAUSTION:
    trap = PIMWASM_TRAP_EXHAUSTION;
    break;
  case WASM_RT_TRAP_DIV_BY_ZERO:
    trap = PIMWASM_TRAP_DIV_BY_ZERO;
    break;
  case WASM_RT_TRAP_INT_OVERFLOW:
    trap = PIMWASM_TRAP_INT_OVERFLOW;
    break;
  case WASM_RT_TRAP_INVALID_CONVERSION:
    trap = PIMWASM_TRAP_INVALID_CONVERSION;
    break;
  default:
    finish(PIMWASM_BACKEND_ERROR, PIMWASM_TRAP_NONE, 0);
    exit(0);
  }
  finish(PIMWASM_TRAPPED, trap, 0);
  /* UPMEM exit ends this launch, preserving guest MRAM until reset. The next
     launch resets stack depth and invalidates every cached host-written byte.
   */
  exit(0);
}

int pimwasm_module_run(void) {
  memset(&pimwasm_response, 0, sizeof(pimwasm_response));
  begin_launch();
  if (pimwasm_request.abi_version != PIMWASM_NATIVE_ABI_VERSION) {
    finish(PIMWASM_INVALID, PIMWASM_TRAP_NONE, 0);
    return 0;
  }
  if (pimwasm_request.operation == PIMWASM_WIRE_INIT) {
    if (ready)
      pimwasm_guest_free(&instance);
    ready = false;
    pimwasm_guest_init(&instance);
    if (wasm_rt_exception_pending())
      wasm_rt_trap(WASM_RT_TRAP_UNCAUGHT_EXCEPTION);
    ready = true;
    finish(PIMWASM_OK, PIMWASM_TRAP_NONE, 0);
  } else if (pimwasm_request.operation == PIMWASM_WIRE_CALL && ready &&
             pimwasm_request.export_index < PIMWASM_EXPORT_COUNT &&
             pimwasm_request.argc ==
                 pimwasm_exports.entries[pimwasm_request.export_index].argc) {
    const struct pimwasm_export_info *e =
        &pimwasm_exports.entries[pimwasm_request.export_index];
    for (uint32_t i = 0; i < e->argc; ++i) {
      if ((e->parameter_types[i] == PIMWASM_I32 ||
           e->parameter_types[i] == PIMWASM_F32) &&
          pimwasm_request.args[i] > UINT32_MAX) {
        finish(PIMWASM_INVALID, PIMWASM_TRAP_NONE, 0);
        return 0;
      }
    }
    uint64_t result = pimwasm_guest_call_bits(&instance, pimwasm_request.export_index,
                                          pimwasm_request.args, pimwasm_response.extra);
    if (wasm_rt_exception_pending())
      wasm_rt_trap(WASM_RT_TRAP_UNCAUGHT_EXCEPTION);
    finish(PIMWASM_OK, PIMWASM_TRAP_NONE, result);
  } else if (ready && pimwasm_request.export_index < PIMWASM_GLOBAL_COUNT &&
             (pimwasm_request.operation == PIMWASM_WIRE_GLOBAL_GET ||
              pimwasm_request.operation == PIMWASM_WIRE_GLOBAL_SET)) {
    uint32_t index = pimwasm_request.export_index;
    const struct pimwasm_global_info *g = &pimwasm_globals.entries[index];
    if (pimwasm_request.operation == PIMWASM_WIRE_GLOBAL_GET && pimwasm_request.argc == 0) {
      finish(PIMWASM_OK, PIMWASM_TRAP_NONE,
             pimwasm_guest_global_get_bits(&instance, index));
    } else if (pimwasm_request.operation == PIMWASM_WIRE_GLOBAL_SET &&
               pimwasm_request.argc == 1 && g->mutable &&
               ((g->type != PIMWASM_I32 && g->type != PIMWASM_F32) ||
                pimwasm_request.args[0] <= UINT32_MAX)) {
      pimwasm_guest_global_set_bits(&instance, index, pimwasm_request.args[0]);
      finish(PIMWASM_OK, PIMWASM_TRAP_NONE, 0);
    } else {
      finish(PIMWASM_INVALID, PIMWASM_TRAP_NONE, 0);
    }
  } else {
    finish(PIMWASM_INVALID, PIMWASM_TRAP_NONE, 0);
  }
  return 0;
}
