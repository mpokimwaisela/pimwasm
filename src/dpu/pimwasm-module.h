/* DPU application entry into the Wasm embedding. Generated code uses wasm-rt.h.
 */
#ifndef PIMWASM_DPU_MODULE_H
#define PIMWASM_DPU_MODULE_H

/* Handle one native mailbox request on the single-tasklet module instance.
   Each launch resets transient runtime state; INIT explicitly resets the guest.
 */
int pimwasm_module_run(void);

/* Builtin native import implementations. Generic generated binding wrappers
   supply the opaque context expected by wasm2c. */
void pimwasm_builtin_abort(void);
void pimwasm_builtin_assert(unsigned int);
unsigned int pimwasm_builtin_tasklet_id(void);

#include "wasm-rt.h"
__attribute__((noreturn)) void pimwasm_module_trap(wasm_rt_trap_t);
#endif
