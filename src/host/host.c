/* Native host interface for execution on physical UPMEM DPUs. */
#define _POSIX_C_SOURCE 200809L
#include "pimwasm.h"
#include "wire.h"
#include <assert.h>
#include <dpu.h>
#include <dpu_management.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct pimwasm {
  struct pimwasm_module_info info;
  struct pimwasm_export_table exports;
  struct pimwasm_global_table globals;
  struct pimwasm_table_exports tables;
  uint32_t table_size;
  struct dpu_set_t allocation;
  int allocated, ready;
};

static int valid_name(const char *name, uint32_t length) {
  return length < 64 && name[length] == 0;
}
static int name_equal(const void *a, size_t n, const char *b, uint32_t m) {
  return n == m && (!n || (a && !memcmp(a, b, n)));
}
static int valid_info(const struct pimwasm_module_info *m) {
  return m->abi_version == PIMWASM_ABI_VERSION &&
         m->memory_bytes == m->initial_memory_bytes &&
         m->initial_memory_bytes <= m->memory_capacity_bytes &&
         m->memory_capacity_bytes <= PIMWASM_MAX_MEMORY_BYTES &&
         !(m->initial_memory_bytes % PIMWASM_MEMORY_PAGE_BYTES) &&
         !(m->memory_capacity_bytes % PIMWASM_MEMORY_PAGE_BYTES) &&
         m->memory_count <= 1 &&
         (m->memory_count || !m->memory_capacity_bytes) &&
         m->table_count <= 1 &&
         !memcmp(m->reserved, (const uint8_t[6]){0}, sizeof(m->reserved));
}
static int valid_exports(const pimwasm_t *u) {
  const struct pimwasm_export_table *t = &u->exports;
  if (t->native_abi != PIMWASM_NATIVE_ABI_VERSION || t->count > PIMWASM_MAX_EXPORTS)
    return 0;
  for (uint32_t i = 0; i < t->count; ++i) {
    const struct pimwasm_export_info *e = &t->entries[i];
    if (e->argc > PIMWASM_MAX_PARAMETERS || !valid_name(e->name, e->name_length) ||
        e->result_type > PIMWASM_F64 || e->result_count > PIMWASM_MAX_RESULTS ||
        e->result_type != (e->result_count ? e->result_types[0] : PIMWASM_VOID))
      return 0;
    for (uint32_t j = 0; j < PIMWASM_MAX_RESULTS; ++j)
      if (j < e->result_count
              ? (e->result_types[j] < PIMWASM_I32 || e->result_types[j] > PIMWASM_F64)
              : e->result_types[j] != 0)
        return 0;
    for (uint32_t j = 0; j < PIMWASM_MAX_PARAMETERS; ++j)
      if (j < e->argc ? (e->parameter_types[j] < PIMWASM_I32 ||
                         e->parameter_types[j] > PIMWASM_F64)
                      : e->parameter_types[j] != 0)
        return 0;
    for (uint32_t j = 0; j < i; ++j)
      if (name_equal(e->name, e->name_length, t->entries[j].name,
                     t->entries[j].name_length))
        return 0;
  }
  return 1;
}
static int valid_globals(const pimwasm_t *u) {
  const struct pimwasm_global_table *t = &u->globals;
  if (t->native_abi != PIMWASM_NATIVE_ABI_VERSION ||
      t->count > PIMWASM_MAX_GLOBAL_EXPORTS)
    return 0;
  for (uint32_t i = 0; i < t->count; ++i) {
    const struct pimwasm_global_info *e = &t->entries[i];
    if (e->type < PIMWASM_I32 || e->type > PIMWASM_F64 || e->mutable > 1 ||
        !valid_name(e->name, e->name_length))
      return 0;
    for (uint32_t j = 0; j < i; ++j)
      if (name_equal(e->name, e->name_length, t->entries[j].name,
                     t->entries[j].name_length))
        return 0;
    for (uint32_t j = 0; j < u->exports.count; ++j)
      if (name_equal(e->name, e->name_length, u->exports.entries[j].name,
                     u->exports.entries[j].name_length))
        return 0;
  }
  return 1;
}
static int valid_tables(const pimwasm_t *u) {
  const struct pimwasm_table_exports *t = &u->tables;
  if (t->native_abi != PIMWASM_NATIVE_ABI_VERSION ||
      t->count > PIMWASM_MAX_TABLE_EXPORTS || (t->count && !u->info.table_count))
    return 0;
  for (uint32_t i = 0; i < t->count; ++i) {
    const struct pimwasm_table_info *e = &t->entries[i];
    if (!valid_name(e->name, e->name_length) || e->initial > e->capacity ||
        e->capacity > 64 || e->capacity > e->maximum || e->size != e->initial)
      return 0;
    for (uint32_t j = 0; j < i; ++j)
      if (name_equal(e->name, e->name_length, t->entries[j].name,
                     t->entries[j].name_length))
        return 0;
    for (uint32_t j = 0; j < u->exports.count; ++j)
      if (name_equal(e->name, e->name_length, u->exports.entries[j].name,
                     u->exports.entries[j].name_length))
        return 0;
    for (uint32_t j = 0; j < u->globals.count; ++j)
      if (name_equal(e->name, e->name_length, u->globals.entries[j].name,
                     u->globals.entries[j].name_length))
        return 0;
  }
  return 1;
}
/* PrIM's transfer pattern: prepare each DPU, then submit once on the allocation
   set. pimwasm_open enforces one DPU: a future group API must supply distinct read
   destinations per DPU, not reuse this instance's buffer across a larger set.
   DEFAULT is synchronous and clears prepared pointers. PARALLEL instead means
   unsynchronized WRAM access during execution; it is not an MRAM batch flag. */
static enum pimwasm_status device_xfer(pimwasm_t *u, dpu_xfer_t direction,
                                   const char *symbol, uint32_t offset,
                                   void *buffer, size_t bytes) {
  assert(u && u->allocated && buffer && bytes);
  struct dpu_set_t dpu;
  DPU_FOREACH(u->allocation, dpu) {
    DPU_CHECK(dpu_prepare_xfer(dpu, buffer), goto fail);
  }
  DPU_CHECK(dpu_push_xfer(u->allocation, direction, symbol, offset, bytes,
                          DPU_XFER_DEFAULT),
            goto fail);
  return PIMWASM_OK;
fail:
  /* Early SDK errors can leave pointers prepared. Clear them before stack or
     staging buffers expire, including after a partial prepare failure. */
  DPU_CHECK(dpu_prepare_xfer(u->allocation, NULL), (void)0);
  return PIMWASM_BACKEND_ERROR;
}
static enum pimwasm_status transport(pimwasm_t *u, uint32_t op, uint32_t index,
                                 uint32_t argc, const uint64_t *args,
                                 struct pimwasm_raw_result *result) {
  struct pimwasm_wire_request req = {.abi_version = PIMWASM_NATIVE_ABI_VERSION,
                                 .operation = op,
                                 .export_index = index};
  struct pimwasm_wire_response res = {0};
  req.argc = argc;
  if (args)
    memcpy(req.args, args, sizeof(req.args));
  if (device_xfer(u, DPU_XFER_TO_DPU, "pimwasm_request", 0, &req, sizeof(req)) !=
          PIMWASM_OK ||
      device_xfer(u, DPU_XFER_TO_DPU, "pimwasm_response", 0, &res, sizeof(res)) !=
          PIMWASM_OK)
    return PIMWASM_BACKEND_ERROR;
  DPU_CHECK(dpu_launch(u->allocation, DPU_SYNCHRONOUS),
            return PIMWASM_BACKEND_ERROR);
  if (device_xfer(u, DPU_XFER_FROM_DPU, "pimwasm_response", 0, &res, sizeof(res)) !=
      PIMWASM_OK)
    return PIMWASM_BACKEND_ERROR;
  if (res.completed != PIMWASM_WIRE_DONE || res.reserved || res.table_size > 64 ||
      (!u->info.table_count && res.table_size) ||
      (res.status != PIMWASM_OK && res.status != PIMWASM_TRAPPED) ||
      (res.status == PIMWASM_OK && res.trap != PIMWASM_TRAP_NONE) ||
      (res.status == PIMWASM_TRAPPED &&
       (res.trap < PIMWASM_TRAP_BOUNDS || res.trap > PIMWASM_TRAP_UNCAUGHT_EXCEPTION)))
    return PIMWASM_BACKEND_ERROR;
  /* Growth committed before a subsequent trap remains observable. INIT can
     shrink back to the initial size, then grow while running the start. */
  if (res.memory_bytes < u->info.initial_memory_bytes ||
      res.memory_bytes > u->info.memory_capacity_bytes ||
      res.memory_bytes % PIMWASM_MEMORY_PAGE_BYTES ||
      (op != PIMWASM_WIRE_INIT && res.memory_bytes < u->info.memory_bytes))
    return PIMWASM_BACKEND_ERROR;
  u->info.memory_bytes = res.memory_bytes;
  u->table_size = res.table_size;
  for (uint32_t i = 0; i < u->tables.count; ++i)
    u->tables.entries[i].size = res.table_size;
  if (result) {
    result->value = res.value;
    memcpy(result->extra, res.extra, sizeof(res.extra));
    result->trap = (enum pimwasm_trap)res.trap;
  }
  return (enum pimwasm_status)res.status;
}
enum pimwasm_status pimwasm_open(const char *bundle, pimwasm_t **out,
                         enum pimwasm_trap *initialization_trap) {
  if (initialization_trap)
    *initialization_trap = PIMWASM_TRAP_NONE;
  if (!out)
    return PIMWASM_INVALID;
  *out = NULL;
  if (!bundle)
    return PIMWASM_INVALID;
  size_t length = strlen(bundle) + 16;
  char *path = malloc(length);
  pimwasm_t *u = calloc(1, sizeof(*u));
  if (!path || !u) {
    free(path);
    free(u);
    return PIMWASM_BACKEND_ERROR;
  }
  enum pimwasm_status status = PIMWASM_BACKEND_ERROR;
  snprintf(path, length, "%s/dpu", bundle);
  DPU_CHECK(dpu_alloc(1, NULL, &u->allocation), goto fail);
  u->allocated = 1;
  uint32_t count;
  DPU_CHECK(dpu_get_nr_dpus(u->allocation, &count), goto fail);
  if (count != 1) {
    fprintf(stderr, "PIMWASM requires exactly one DPU, got %u\n", count);
    goto fail;
  }
  struct dpu_set_t dpu;
  uint32_t physical_mram_bytes = 0;
  DPU_FOREACH(u->allocation, dpu) {
    struct dpu_rank_t *rank = dpu_get_rank(dpu.dpu);
    dpu_description_t description = dpu_get_description(rank);
    if (description->type != HW) {
      fprintf(stderr, "Refusing non-HW backend\n");
      goto fail;
    }
    physical_mram_bytes = description->hw.memories.mram_size;
    fprintf(stderr, "PIMWASM DPUs=%u tasklets=1\n", (unsigned)count);
  }
  DPU_CHECK(dpu_load(u->allocation, path, NULL), goto fail);
  if (device_xfer(u, DPU_XFER_FROM_DPU, "pimwasm_metadata", 0, &u->info,
                  sizeof(u->info)) != PIMWASM_OK ||
      device_xfer(u, DPU_XFER_FROM_DPU, "pimwasm_exports", 0, &u->exports,
                  sizeof(u->exports)) != PIMWASM_OK)
    goto fail;
  if (!valid_info(&u->info) || !valid_exports(u)) {
    status = PIMWASM_ABI_MISMATCH;
    goto fail;
  }
  if (u->info.memory_capacity_bytes > physical_mram_bytes) {
    fprintf(stderr, "Declared guest memory exceeds device MRAM capacity\n");
    goto fail;
  }
  if (device_xfer(u, DPU_XFER_FROM_DPU, "pimwasm_globals", 0, &u->globals,
                  sizeof(u->globals)) != PIMWASM_OK)
    goto fail;
  if (!valid_globals(u)) {
    status = PIMWASM_ABI_MISMATCH;
    goto fail;
  }
  if (device_xfer(u, DPU_XFER_FROM_DPU, "pimwasm_tables", 0, &u->tables,
                  sizeof(u->tables)) != PIMWASM_OK)
    goto fail;
  if (!valid_tables(u)) {
    status = PIMWASM_ABI_MISMATCH;
    goto fail;
  }
  struct pimwasm_raw_result init_result = {0};
  status = transport(u, PIMWASM_WIRE_INIT, 0, 0, NULL, &init_result);
  if (status != PIMWASM_OK) {
    if (status == PIMWASM_TRAPPED) {
      if (initialization_trap)
        *initialization_trap = init_result.trap;
      fprintf(stderr, "PIMWASM instantiation trap=%u\n",
              (unsigned)init_result.trap);
    }
    goto fail;
  }
  u->ready = 1;
  free(path);
  *out = u;
  return PIMWASM_OK;
fail:
  free(path);
  pimwasm_close(u);
  return status;
}
const struct pimwasm_module_info *pimwasm_info(const pimwasm_t *u) {
  return u ? &u->info : NULL;
}
uint32_t pimwasm_export_count(const pimwasm_t *u) { return u ? u->exports.count : 0; }
const struct pimwasm_export_info *pimwasm_export(const pimwasm_t *u, uint32_t index) {
  return u && index < u->exports.count ? &u->exports.entries[index] : NULL;
}
uint32_t pimwasm_global_count(const pimwasm_t *u) { return u ? u->globals.count : 0; }
const struct pimwasm_global_info *pimwasm_global(const pimwasm_t *u, uint32_t index) {
  return u && index < u->globals.count ? &u->globals.entries[index] : NULL;
}
uint32_t pimwasm_table_export_count(const pimwasm_t *u) {
  return u ? u->tables.count : 0;
}
const struct pimwasm_table_info *pimwasm_table_export(const pimwasm_t *u, uint32_t index) {
  return u && index < u->tables.count ? &u->tables.entries[index] : NULL;
}
enum pimwasm_status pimwasm_table_size(const pimwasm_t *u, uint32_t *size) {
  if (!u || !size || !u->info.table_count)
    return PIMWASM_INVALID;
  if (!u->ready)
    return PIMWASM_BACKEND_ERROR;
  *size = u->table_size;
  return PIMWASM_OK;
}
enum pimwasm_status pimwasm_global_get(pimwasm_t *u, const char *name,
                               struct pimwasm_value *value) {
  if (!name) {
    if (value)
      memset(value, 0, sizeof(*value));
    return PIMWASM_INVALID;
  }
  return pimwasm_global_get_bytes(u, name, strlen(name), value);
}
enum pimwasm_status pimwasm_global_get_bytes(pimwasm_t *u, const void *name, size_t length,
                                     struct pimwasm_value *value) {
  if (value)
    *value = (struct pimwasm_value){0};
  if (!u || (!name && length) || length > 63 || !value)
    return PIMWASM_INVALID;
  for (uint32_t i = 0; i < u->globals.count; ++i) {
    const struct pimwasm_global_info *g = &u->globals.entries[i];
    if (!name_equal(name, length, g->name, g->name_length))
      continue;
    if (!u->ready)
      return PIMWASM_BACKEND_ERROR;
    struct pimwasm_raw_result raw = {0};
    enum pimwasm_status status =
        transport(u, PIMWASM_WIRE_GLOBAL_GET, i, 0, NULL, &raw);
    if (status != PIMWASM_OK)
      return status;
    if ((g->type == PIMWASM_I32 || g->type == PIMWASM_F32) && raw.value > UINT32_MAX)
      return PIMWASM_BACKEND_ERROR;
    value->type = g->type;
    value->bits = raw.value;
    return PIMWASM_OK;
  }
  return PIMWASM_INVALID;
}
enum pimwasm_status pimwasm_global_set(pimwasm_t *u, const char *name,
                               const struct pimwasm_value *value) {
  return name ? pimwasm_global_set_bytes(u, name, strlen(name), value)
              : PIMWASM_INVALID;
}
enum pimwasm_status pimwasm_global_set_bytes(pimwasm_t *u, const void *name, size_t length,
                                     const struct pimwasm_value *value) {
  if (!u || (!name && length) || length > 63 || !value || value->reserved)
    return PIMWASM_INVALID;
  for (uint32_t i = 0; i < u->globals.count; ++i) {
    const struct pimwasm_global_info *g = &u->globals.entries[i];
    if (!name_equal(name, length, g->name, g->name_length))
      continue;
    if (!g->mutable || value->type != g->type ||
        ((g->type == PIMWASM_I32 || g->type == PIMWASM_F32) &&
         value->bits > UINT32_MAX))
      return PIMWASM_INVALID;
    if (!u->ready)
      return PIMWASM_BACKEND_ERROR;
    uint64_t args[PIMWASM_MAX_PARAMETERS] = {value->bits};
    return transport(u, PIMWASM_WIRE_GLOBAL_SET, i, 1, args, NULL);
  }
  return PIMWASM_INVALID;
}
enum pimwasm_status pimwasm_reset(pimwasm_t *u) {
  if (!u)
    return PIMWASM_INVALID;
  u->ready = 0;
  struct pimwasm_raw_result result = {0};
  enum pimwasm_status status = transport(u, PIMWASM_WIRE_INIT, 0, 0, NULL, &result);
  if (status == PIMWASM_TRAPPED)
    fprintf(stderr, "PIMWASM reset instantiation trap=%u\n", (unsigned)result.trap);
  u->ready = status == PIMWASM_OK;
  return status;
}
static int valid_transfer(pimwasm_t *u, uint32_t offset, const void *p,
                          size_t size) {
  return u && (!size || p) && offset <= u->info.memory_bytes &&
         size <= u->info.memory_bytes - offset;
}
static enum pimwasm_status transfer(pimwasm_t *u, uint32_t offset, void *data, size_t n,
                                int write) {
  if (!valid_transfer(u, offset, data, n))
    return PIMWASM_INVALID;
  if (!u->ready)
    return PIMWASM_BACKEND_ERROR;
  if (!n)
    return PIMWASM_OK;
  /* MRAM offsets and transfer lengths must be multiples of eight. The public
     byte-buffer API permits arbitrary host/guest alignment. */
  assert(u->info.memory_bytes <= PIMWASM_MAX_MEMORY_BYTES &&
         !(u->info.memory_bytes & 7u));
  dpu_xfer_t direction = write ? DPU_XFER_TO_DPU : DPU_XFER_FROM_DPU;
  if (!(offset & 7u) && !(n & 7u) && !((uintptr_t)data & 7u))
    return device_xfer(u, direction, "guest_memory", offset, data, n);
  uint32_t base = offset & ~7u, end = (offset + (uint32_t)n + 7u) & ~7u;
  size_t span = end - base;
  assert(base <= offset && end <= u->info.memory_bytes && span >= n);
  uint8_t *staging = malloc(span);
  if (!staging)
    return PIMWASM_BACKEND_ERROR;
  enum pimwasm_status status = PIMWASM_OK;
  if (write) {
    /* Only partial boundary words need old bytes. Fetch them before any
       write; a large unaligned upload reads at most 16 preservation bytes.
       Coalesce edges in the same or adjacent words into one read. */
    int tail_needed = offset + n != end;
    if (offset != base) {
      size_t head_bytes = span == 16 && tail_needed ? 16 : 8;
      status = device_xfer(u, DPU_XFER_FROM_DPU, "guest_memory", base, staging,
                           head_bytes);
      if (status != PIMWASM_OK)
        goto done;
      if (head_bytes == span)
        tail_needed = 0;
    }
    if (tail_needed) {
      status = device_xfer(u, DPU_XFER_FROM_DPU, "guest_memory", end - 8,
                           staging + span - 8, 8);
      if (status != PIMWASM_OK)
        goto done;
    }
    memcpy(staging + offset - base, data, n);
    status =
        device_xfer(u, DPU_XFER_TO_DPU, "guest_memory", base, staging, span);
  } else {
    status =
        device_xfer(u, DPU_XFER_FROM_DPU, "guest_memory", base, staging, span);
    if (status == PIMWASM_OK)
      memcpy(data, staging + offset - base, n);
  }
done:
  free(staging);
  return status;
}
enum pimwasm_status pimwasm_write(pimwasm_t *u, uint32_t offset, const void *data,
                          size_t n) {
  return transfer(u, offset, (void *)data, n, 1);
}
enum pimwasm_status pimwasm_read(pimwasm_t *u, uint32_t offset, void *data, size_t n) {
  return transfer(u, offset, data, n, 0);
}
static enum pimwasm_status invoke_raw(pimwasm_t *u, uint32_t index, uint32_t argc,
                                  const uint64_t args[PIMWASM_MAX_PARAMETERS],
                                  struct pimwasm_raw_result *result) {
  if (!u->ready)
    return PIMWASM_BACKEND_ERROR;
  return transport(u, PIMWASM_WIRE_CALL, index, argc, args, result);
}
enum pimwasm_status pimwasm_invoke(pimwasm_t *u, const struct pimwasm_call *call,
                           struct pimwasm_results *result) {
  if (!u || u->exports.count != 1) {
    if (result)
      memset(result, 0, sizeof(*result));
    return PIMWASM_INVALID;
  }
  const struct pimwasm_export_info *e = &u->exports.entries[0];
  return pimwasm_invoke_named_bytes(u, e->name, e->name_length, call, result);
}
enum pimwasm_status pimwasm_invoke_named(pimwasm_t *u, const char *name,
                                 const struct pimwasm_call *call,
                                 struct pimwasm_results *result) {
  if (!name) {
    if (result)
      memset(result, 0, sizeof(*result));
    return PIMWASM_INVALID;
  }
  return pimwasm_invoke_named_bytes(u, name, strlen(name), call, result);
}
enum pimwasm_status pimwasm_invoke_named_bytes(pimwasm_t *u, const void *name,
                                       size_t length,
                                       const struct pimwasm_call *call,
                                       struct pimwasm_results *result) {
  if (result)
    *result = (struct pimwasm_results){0};
  if (!u || (!name && length) || length > 63 || !call || !result ||
      call->argc > PIMWASM_MAX_PARAMETERS || (call->argc && !call->args))
    return PIMWASM_INVALID;
  uint32_t index = 0;
  while (index < u->exports.count &&
         !name_equal(name, length, u->exports.entries[index].name,
                     u->exports.entries[index].name_length))
    ++index;
  if (index == u->exports.count)
    return PIMWASM_INVALID;
  const struct pimwasm_export_info *e = &u->exports.entries[index];
  if (call->argc != e->argc)
    return PIMWASM_INVALID;
  uint64_t args[PIMWASM_MAX_PARAMETERS] = {0};
  for (uint32_t j = 0; j < call->argc; ++j) {
    const struct pimwasm_value *v = &call->args[j];
    if (v->reserved || v->type != e->parameter_types[j] ||
        ((v->type == PIMWASM_I32 || v->type == PIMWASM_F32) && v->bits > UINT32_MAX))
      return PIMWASM_INVALID;
    args[j] = v->bits;
  }
  struct pimwasm_raw_result raw = {0};
  enum pimwasm_status status = invoke_raw(u, index, call->argc, args, &raw);
  if (status == PIMWASM_TRAPPED) {
    result->trap = raw.trap;
    return status;
  }
  if (status != PIMWASM_OK)
    return status;
  for (uint32_t j = 0; j < PIMWASM_MAX_RESULTS; ++j) {
    uint64_t bits = j ? raw.extra[j - 1] : raw.value;
    if (j >= e->result_count) {
      if (bits)
        return PIMWASM_BACKEND_ERROR;
      continue;
    }
    if ((e->result_types[j] == PIMWASM_I32 || e->result_types[j] == PIMWASM_F32) &&
        bits > UINT32_MAX)
      return PIMWASM_BACKEND_ERROR;
  }
  result->count = e->result_count;
  for (uint32_t j = 0; j < result->count; ++j)
    result->values[j] = (struct pimwasm_value){
        .type = e->result_types[j], .bits = j ? raw.extra[j - 1] : raw.value};
  return PIMWASM_OK;
}

void pimwasm_close(pimwasm_t *u) {
  if (!u)
    return;
  if (u->allocated)
    DPU_CHECK(dpu_free(u->allocation), (void)0);
  free(u);
}
const char *pimwasm_status_string(enum pimwasm_status status) {
  switch (status) {
  case PIMWASM_OK:
    return "ok";
  case PIMWASM_INVALID:
    return "invalid host input";
  case PIMWASM_ABI_MISMATCH:
    return "incompatible native metadata";
  case PIMWASM_BACKEND_ERROR:
    return "backend failure";
  case PIMWASM_TRAPPED:
    return "guest trap";
  default:
    return "unknown status";
  }
}