/* Private MRAM support used by the memory runtime; guest accesses use wasm-rt.h. */
#ifndef PIMWASM_DPU_WASM_RT_MEMORY_H
#define PIMWASM_DPU_WASM_RT_MEMORY_H
#include <stdint.h>

void pimwasm_memory_invalidate(void);
/* Clear a page-aligned range of reserved MRAM and invalidate cached data.
   The allocation/growth implementation validates allocation/growth before calling this; the range may
   include newly added pages not yet visible to guest loads or host transfers.
   Only defined for bundles with linear memory. */
void pimwasm_memory_zero_range(uint32_t offset, uint32_t bytes);

#endif
