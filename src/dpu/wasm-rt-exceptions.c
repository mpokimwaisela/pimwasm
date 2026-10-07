#include "wasm-rt-exceptions.h"
#include <string.h>
#define WASM_RT_EXCEPTION_CAPACITY 256u
#if PIMWASM_EXCEPTION_ENABLED
static wasm_rt_tag_t active_tag;
static uint32_t active_size;
static uint8_t payload[WASM_RT_EXCEPTION_CAPACITY];
static bool pending;
void wasm_rt_load_exception(wasm_rt_tag_t tag, uint32_t size,
                            const void *values) {
  if (size > sizeof(payload))
    wasm_rt_trap(WASM_RT_TRAP_EXHAUSTION);
  active_tag = tag;
  active_size = size;
  if (size)
    memcpy(payload, values, size);
}
void wasm_rt_throw(void) { pending = true; }
wasm_rt_tag_t wasm_rt_exception_tag(void) { return active_tag; }
uint32_t wasm_rt_exception_size(void) { return active_size; }
void *wasm_rt_exception(void) { return payload; }
bool wasm_rt_exception_pending(void) { return pending; }
void wasm_rt_exception_clear(void) { pending = false; }
void wasm_rt_exception_reset(void) {
  pending = false;
  active_tag = NULL;
  active_size = 0;
  memset(payload, 0, sizeof(payload));
}
#else
void wasm_rt_exception_reset(void) {}
bool wasm_rt_exception_pending(void) { return false; }
#endif
