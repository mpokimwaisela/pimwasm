#ifndef PIMWASM_H
#define PIMWASM_H
#include <stddef.h>
#include <stdint.h>
#define PIMWASM_ABI_VERSION 13u
/* Logical memory32 can grow within bounded backing on one DPU. */
#define PIMWASM_MEMORY_PAGE_BYTES 65536u
#define PIMWASM_MAX_MEMORY_PAGES 1024u
#define PIMWASM_MAX_MEMORY_BYTES (PIMWASM_MEMORY_PAGE_BYTES * PIMWASM_MAX_MEMORY_PAGES)
#define PIMWASM_MAX_PARAMETERS 32u
#define PIMWASM_MAX_RESULTS 4u
#define PIMWASM_MAX_EXPORTS 32u
#define PIMWASM_MAX_GLOBAL_EXPORTS 16u
#define PIMWASM_MAX_TABLE_EXPORTS 16u
typedef struct pimwasm pimwasm_t;
enum pimwasm_status {
  PIMWASM_OK = 0,
  PIMWASM_INVALID = 1,
  PIMWASM_ABI_MISMATCH = 2,
  PIMWASM_BACKEND_ERROR = 3,
  PIMWASM_TRAPPED = 4
};
enum pimwasm_trap {
  PIMWASM_TRAP_NONE = 0,
  PIMWASM_TRAP_BOUNDS = 1,
  PIMWASM_TRAP_UNREACHABLE = 2,
  PIMWASM_TRAP_EXHAUSTION = 3,
  PIMWASM_TRAP_DIV_BY_ZERO = 4,
  PIMWASM_TRAP_INT_OVERFLOW = 5,
  PIMWASM_TRAP_INVALID_CONVERSION = 6,
  PIMWASM_TRAP_INDIRECT_CALL = 7,
  PIMWASM_TRAP_UNCAUGHT_EXCEPTION = 8
};
/* VOID denotes no function result; it is never a parameter/global value type.
 */
enum pimwasm_type {
  PIMWASM_VOID = 0,
  PIMWASM_I32 = 1,
  PIMWASM_I64 = 2,
  PIMWASM_F32 = 3,
  PIMWASM_F64 = 4
};
struct pimwasm_module_info {
  uint32_t abi_version, memory_bytes;
  uint32_t initial_memory_bytes, memory_capacity_bytes;
  uint8_t
      memory_count; /* 0: absent; 1: declared, even if current size is zero. */
  uint8_t table_count;
  uint8_t reserved[6]; /* Zero; keep native transfers eight-byte aligned. */
};
struct pimwasm_export_info {
  uint32_t argc;
  char name[64];
  uint32_t name_length;
  uint32_t parameter_types[PIMWASM_MAX_PARAMETERS], result_type;
  uint32_t result_count, result_types[PIMWASM_MAX_RESULTS];
};
struct pimwasm_global_info {
  uint32_t type, mutable;
  char name[64];
  uint32_t name_length;
};
struct pimwasm_table_info {
  char name[64];
  uint32_t name_length, initial, maximum, capacity, size;
};
/* Integer bit patterns or IEEE-754 binary32/binary64 bits, without
   host-language signedness or numeric conversion. i32/f32 use the low 32 bits
   only. Reserved fields must be zero. A Wasm signature determines
   signed/unsigned operations. */
struct pimwasm_value {
  uint64_t bits;
  uint32_t type, reserved;
};
/* Counted results retain Wasm order and exact scalar bit patterns.
   On a trap count is zero: no partial result tuple is published. */
struct pimwasm_results {
  uint32_t count;
  enum pimwasm_trap trap;
  struct pimwasm_value values[PIMWASM_MAX_RESULTS];
};
/* args points to exactly argc values; NULL is permitted only for argc zero. */
struct pimwasm_call {
  uint32_t argc;
  const struct pimwasm_value *args;
};
/* Open on one physical UPMEM DPU. Artifacts are accepted/AOT-compiled before
   opening; native bundles are trusted. Use all instances serially from one
   host thread. There is no simulation or host-execution fallback.
   Initialization executes the Wasm start function when present. A trapping
   start returns PIMWASM_TRAPPED and leaves *out NULL. initialization_trap may be
   NULL; otherwise it receives the exact start trap (NONE on other outcomes). */
enum pimwasm_status pimwasm_open(const char *bundle_directory, pimwasm_t **out,
                         enum pimwasm_trap *initialization_trap);
/* Borrowed metadata: memory_bytes is current and updates after every completed
   invocation (including traps) and reset. Initial size/capacity are fixed. */
const struct pimwasm_module_info *pimwasm_info(const pimwasm_t *);
/* Export order follows the Wasm export section. Descriptors are borrowed. */
uint32_t pimwasm_export_count(const pimwasm_t *);
const struct pimwasm_export_info *pimwasm_export(const pimwasm_t *, uint32_t index);
/* Global exports have their own index space. Aliases share the same guest
   global. Descriptors are borrowed; reads/writes use exact scalar bits. */
uint32_t pimwasm_global_count(const pimwasm_t *);
const struct pimwasm_global_info *pimwasm_global(const pimwasm_t *, uint32_t index);
uint32_t pimwasm_table_export_count(const pimwasm_t *);
const struct pimwasm_table_info *pimwasm_table_export(const pimwasm_t *, uint32_t index);
enum pimwasm_status pimwasm_table_size(const pimwasm_t *, uint32_t *);
enum pimwasm_status pimwasm_global_get(pimwasm_t *, const char *name, struct pimwasm_value *);
enum pimwasm_status pimwasm_global_set(pimwasm_t *, const char *name,
                               const struct pimwasm_value *);
/* Reinstantiate including data and start; a failed reset leaves the instance
   unavailable for calls/transfers until a later reset succeeds. */
enum pimwasm_status pimwasm_reset(pimwasm_t *);
enum pimwasm_status pimwasm_write(pimwasm_t *, uint32_t offset, const void *, size_t bytes);
enum pimwasm_status pimwasm_read(pimwasm_t *, uint32_t offset, void *, size_t bytes);
/* Invoke the sole function export. Zero or multiple function exports return
   PIMWASM_INVALID before guest execution, even if aliases share one function. */
enum pimwasm_status pimwasm_invoke(pimwasm_t *, const struct pimwasm_call *,
                           struct pimwasm_results *);
/* Invoke an explicitly named export; supports zero to four scalar results. */
enum pimwasm_status pimwasm_invoke_named(pimwasm_t *, const char *, const struct pimwasm_call *,
                                 struct pimwasm_results *);
/* UTF-8 names are byte strings and may contain embedded NUL. Ordinary
   C-string APIs delegate to these length-aware operations. */
enum pimwasm_status pimwasm_invoke_named_bytes(pimwasm_t *, const void *, size_t,
                                       const struct pimwasm_call *,
                                       struct pimwasm_results *);
enum pimwasm_status pimwasm_global_get_bytes(pimwasm_t *, const void *, size_t,
                                     struct pimwasm_value *);
enum pimwasm_status pimwasm_global_set_bytes(pimwasm_t *, const void *, size_t,
                                     const struct pimwasm_value *);
void pimwasm_close(pimwasm_t *);
const char *pimwasm_status_string(enum pimwasm_status);
#endif
