/* Private native transport, never guest-addressable and not the Wasm ABI. */
#ifndef PIMWASM_WIRE_H
#define PIMWASM_WIRE_H
#include "pimwasm.h"
#define PIMWASM_NATIVE_ABI_VERSION 13u
#define PIMWASM_WIRE_DONE 0x55444631u
#define PIMWASM_WIRE_INIT 1u
#define PIMWASM_WIRE_CALL 2u
#define PIMWASM_WIRE_GLOBAL_GET 3u
#define PIMWASM_WIRE_GLOBAL_SET 4u
struct pimwasm_wire_request {
  uint32_t abi_version, operation, argc, export_index;
  uint64_t args[PIMWASM_MAX_PARAMETERS];
};
struct pimwasm_export_table {
  uint32_t native_abi, count;
  struct pimwasm_export_info entries[PIMWASM_MAX_EXPORTS];
};
struct pimwasm_global_table {
  uint32_t native_abi, count;
  struct pimwasm_global_info entries[PIMWASM_MAX_GLOBAL_EXPORTS];
};
struct pimwasm_table_exports {
  uint32_t native_abi, count;
  struct pimwasm_table_info entries[PIMWASM_MAX_TABLE_EXPORTS];
};
struct pimwasm_wire_response {
  uint32_t completed, status, trap, reserved;
  uint64_t value;
  uint64_t extra[PIMWASM_MAX_RESULTS - 1];
  uint32_t memory_bytes, table_size;
};
struct pimwasm_raw_result {
  uint64_t value, extra[PIMWASM_MAX_RESULTS - 1];
  enum pimwasm_trap trap;
};
_Static_assert(sizeof(struct pimwasm_module_info) == 24,
               "metadata transport size");
_Static_assert(sizeof(struct pimwasm_wire_request) == 272,
               "request transport size");
_Static_assert(sizeof(struct pimwasm_wire_response) == 56,
               "response transport size");
_Static_assert(sizeof(struct pimwasm_export_table) == 7176,
               "export table transport size");
_Static_assert(sizeof(struct pimwasm_global_table) == 1224,
               "global table transport size");
_Static_assert(sizeof(struct pimwasm_table_exports) == 1352,
               "table export transport size");
#endif