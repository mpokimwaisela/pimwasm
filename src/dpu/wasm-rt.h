/* Feature-specific implementations live in wasm-rt-*.c/.h. */
#ifndef PIMWASM_DPU_WASM_RT_H
#define PIMWASM_DPU_WASM_RT_H
#include "metadata.h"
#include <stdbool.h>
#include <stdint.h>
#if NR_TASKLETS != 1
#error "The PIMWASM runtime requires exactly one tasklet"
#endif
#ifdef NDEBUG
#error "Generated module assertions must remain enabled"
#endif
typedef const char *wasm_rt_func_type_t;
typedef enum {
  WASM_RT_I32,
  WASM_RT_I64,
  WASM_RT_F32,
  WASM_RT_F64,
  WASM_RT_V128,
  WASM_RT_FUNCREF,
  WASM_RT_EXTERNREF,
  WASM_RT_EXNREF
} wasm_rt_type_t;
typedef enum {
  WASM_RT_TRAP_OOB = 1,
  WASM_RT_TRAP_INT_OVERFLOW = 2,
  WASM_RT_TRAP_DIV_BY_ZERO = 3,
  WASM_RT_TRAP_INVALID_CONVERSION = 4,
  WASM_RT_TRAP_UNREACHABLE = 5,
  WASM_RT_TRAP_CALL_INDIRECT = 6,
  WASM_RT_TRAP_UNCAUGHT_EXCEPTION = 9,
  WASM_RT_TRAP_EXHAUSTION = 10
} wasm_rt_trap_t;
/* MRAM is a separate address space: there is no fictitious WRAM data pointer.
 */
typedef struct {
  uint64_t pages, max_pages, size;
  bool is64;
} wasm_rt_memory_t;
/* Pinned WABT funcref representation; only bounded funcref tables are admitted.
 */
typedef void *wasm_rt_externref_t;
#define wasm_rt_externref_null_value ((wasm_rt_externref_t)0)
typedef struct {
  wasm_rt_externref_t *data;
  uint32_t max_size, size;
} wasm_rt_externref_table_t;
void wasm_rt_allocate_externref_table(wasm_rt_externref_table_t *, uint32_t,
                                      uint32_t);
uint32_t wasm_rt_grow_externref_table(wasm_rt_externref_table_t *, uint32_t,
                                      wasm_rt_externref_t);
void wasm_rt_free_externref_table(wasm_rt_externref_table_t *);
typedef void (*wasm_rt_function_ptr_t)(void);
typedef struct wasm_rt_tailcallee_t {
  void (*fn)(void **, void *, struct wasm_rt_tailcallee_t *);
} wasm_rt_tailcallee_t;
typedef struct {
  wasm_rt_func_type_t func_type;
  wasm_rt_function_ptr_t func;
  wasm_rt_tailcallee_t func_tailcallee;
  void *module_instance;
} wasm_rt_funcref_t;
#define wasm_rt_funcref_null_value ((wasm_rt_funcref_t){0})
typedef struct {
  wasm_rt_funcref_t *data;
  uint32_t max_size, size;
} wasm_rt_funcref_table_t;
void wasm_rt_allocate_funcref_table(wasm_rt_funcref_table_t *, uint32_t,
                                    uint32_t);
uint32_t wasm_rt_grow_funcref_table(wasm_rt_funcref_table_t *, uint32_t,
                                    wasm_rt_funcref_t);
void wasm_rt_free_funcref_table(wasm_rt_funcref_table_t *);
#define WASM_RT_USE_SEGUE_FOR_THIS_MODULE 0
#define WASM_RT_STACK_DEPTH_COUNT 1
/* A recursive bundle uses a resource-checked limit. One additional native
   frame is reserved for the callee that detects exhaustion in its prologue. */
#define WASM_RT_MAX_CALL_STACK_DEPTH PIMWASM_CALL_DEPTH_LIMIT
extern uint32_t wasm_rt_call_stack_depth;
extern uint32_t wasm_rt_memory_size_bytes, wasm_rt_table_current_size;
void wasm_rt_init(void);
void wasm_rt_free(void);
const char *wasm_rt_strerror(wasm_rt_trap_t);
bool wasm_rt_is_initialized(void);
void wasm_rt_allocate_memory(wasm_rt_memory_t *, uint64_t, uint64_t, bool);
uint64_t wasm_rt_grow_memory(wasm_rt_memory_t *, uint64_t);
void wasm_rt_free_memory(wasm_rt_memory_t *);
__attribute__((noreturn)) void wasm_rt_trap(wasm_rt_trap_t);
uint32_t i32_load_default32(wasm_rt_memory_t *, uint64_t);
void i32_store_default32(wasm_rt_memory_t *, uint64_t, uint32_t);
#define PIMWASM_LOAD_DECL(name, type)                                              \
  type name##_default32(wasm_rt_memory_t *, uint64_t)
#define PIMWASM_STORE_DECL(name, type)                                             \
  void name##_default32(wasm_rt_memory_t *, uint64_t, type)
PIMWASM_LOAD_DECL(i64_load, uint64_t);
PIMWASM_LOAD_DECL(f32_load, float);
PIMWASM_LOAD_DECL(f64_load, double);
PIMWASM_LOAD_DECL(i32_load8_s, uint32_t);
PIMWASM_LOAD_DECL(i32_load8_u, uint32_t);
PIMWASM_LOAD_DECL(i32_load16_s, uint32_t);
PIMWASM_LOAD_DECL(i32_load16_u, uint32_t);
PIMWASM_LOAD_DECL(i64_load8_s, uint64_t);
PIMWASM_LOAD_DECL(i64_load8_u, uint64_t);
PIMWASM_LOAD_DECL(i64_load16_s, uint64_t);
PIMWASM_LOAD_DECL(i64_load16_u, uint64_t);
PIMWASM_LOAD_DECL(i64_load32_s, uint64_t);
PIMWASM_LOAD_DECL(i64_load32_u, uint64_t);
PIMWASM_STORE_DECL(i64_store, uint64_t);
PIMWASM_STORE_DECL(f32_store, float);
PIMWASM_STORE_DECL(f64_store, double);
PIMWASM_STORE_DECL(i32_store8, uint32_t);
PIMWASM_STORE_DECL(i32_store16, uint32_t);
PIMWASM_STORE_DECL(i64_store8, uint64_t);
PIMWASM_STORE_DECL(i64_store16, uint64_t);
PIMWASM_STORE_DECL(i64_store32, uint64_t);
#undef PIMWASM_LOAD_DECL
#undef PIMWASM_STORE_DECL
void pimwasm_load_data(wasm_rt_memory_t *, uint64_t, const uint8_t *, uint32_t);
/* Pinned wasm2c bulk helpers. Admission permits only the same memory at index
 * 0. */
void memory_copy(wasm_rt_memory_t *, const wasm_rt_memory_t *, uint64_t,
                 uint64_t, uint64_t);
void memory_fill(wasm_rt_memory_t *, uint64_t, uint32_t, uint64_t);
void memory_init(wasm_rt_memory_t *, const uint8_t *, uint32_t, uint64_t,
                 uint32_t, uint32_t);
#define LOAD_DATA(mem, offset, data, length)                                   \
  pimwasm_load_data(&(mem), offset, data, length)
#endif
