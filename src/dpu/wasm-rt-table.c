#include "wasm-rt.h"
#include <assert.h>
#include <stddef.h>
#include <string.h>
uint32_t wasm_rt_table_current_size;
#if PIMWASM_TABLE_COUNT && !PIMWASM_TABLE_EXTERNREF
#if PIMWASM_TABLE_CAPACITY
static wasm_rt_funcref_t table_entries[PIMWASM_TABLE_CAPACITY];
#endif
void wasm_rt_allocate_funcref_table(wasm_rt_funcref_table_t *table,
                                    uint32_t initial, uint32_t maximum) {
  assert(initial == PIMWASM_TABLE_ENTRIES && maximum == PIMWASM_TABLE_MAXIMUM);
  table->size = initial;
  table->max_size = maximum;
  wasm_rt_table_current_size = initial;
#if PIMWASM_TABLE_CAPACITY
  memset(table_entries, 0, sizeof(table_entries));
  table->data = table_entries;
#else
  table->data = NULL;
#endif
}
uint32_t wasm_rt_grow_funcref_table(wasm_rt_funcref_table_t *table,
                                    uint32_t delta, wasm_rt_funcref_t value) {
  uint32_t old = table->size;
  if (old > table->max_size || old > PIMWASM_TABLE_CAPACITY ||
      delta > table->max_size - old || delta > PIMWASM_TABLE_CAPACITY - old)
    return UINT32_MAX;
  for (uint32_t i = 0; i < delta; ++i)
    table->data[old + i] = value;
  table->size = old + delta;
  wasm_rt_table_current_size = table->size;
  return old;
}
void wasm_rt_free_funcref_table(wasm_rt_funcref_table_t *table) {
  table->size = 0;
  table->data = NULL;
  wasm_rt_table_current_size = 0;
}
#endif

#if PIMWASM_TABLE_COUNT && PIMWASM_TABLE_EXTERNREF
#if PIMWASM_TABLE_CAPACITY
static wasm_rt_externref_t extern_entries[PIMWASM_TABLE_CAPACITY];
#endif
void wasm_rt_allocate_externref_table(wasm_rt_externref_table_t *table,
                                      uint32_t initial, uint32_t maximum) {
  assert(initial == PIMWASM_TABLE_ENTRIES && maximum == PIMWASM_TABLE_MAXIMUM);
  table->size = initial;
  table->max_size = maximum;
  wasm_rt_table_current_size = initial;
#if PIMWASM_TABLE_CAPACITY
  memset(extern_entries, 0, sizeof(extern_entries));
  table->data = extern_entries;
#else
  table->data = NULL;
#endif
}
uint32_t wasm_rt_grow_externref_table(wasm_rt_externref_table_t *table,
                                      uint32_t delta,
                                      wasm_rt_externref_t value) {
  uint32_t old = table->size;
  if (old > table->max_size || old > PIMWASM_TABLE_CAPACITY ||
      delta > table->max_size - old || delta > PIMWASM_TABLE_CAPACITY - old)
    return UINT32_MAX;
  for (uint32_t i = 0; i < delta; ++i)
    table->data[old + i] = value;
  table->size = old + delta;
  wasm_rt_table_current_size = table->size;
  return old;
}
void wasm_rt_free_externref_table(wasm_rt_externref_table_t *table) {
  table->size = 0;
  table->data = NULL;
  wasm_rt_table_current_size = 0;
}
#endif
