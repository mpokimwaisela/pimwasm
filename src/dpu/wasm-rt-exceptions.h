#ifndef PIMWASM_WASM_RT_EXCEPTIONS_H
#define PIMWASM_WASM_RT_EXCEPTIONS_H
#include "wasm-rt.h"
#include <stddef.h>

typedef const void *wasm_rt_tag_t;

/* DPU exception propagation is lowered to checked returns/gotos, not longjmp.
   The adapter admits only the documented pinned legacy try/catch schema. */
void wasm_rt_load_exception(wasm_rt_tag_t, uint32_t, const void *);
void wasm_rt_throw(void);
wasm_rt_tag_t wasm_rt_exception_tag(void);
uint32_t wasm_rt_exception_size(void);
void *wasm_rt_exception(void);
bool wasm_rt_exception_pending(void);
void wasm_rt_exception_clear(void);
void wasm_rt_exception_reset(void);
#endif
