#include "wasm-rt-memory.h"
#include "metadata.h"
#include "pimwasm.h"
#include "wasm-rt.h"
#include <assert.h>
#include <defs.h>
#include <mram.h>
#include <string.h>

#define PIMWASM_BUFFER_BYTES 1024u
_Static_assert(PIMWASM_MEMORY_BYTES <= PIMWASM_MAX_MEMORY_BYTES &&
                   PIMWASM_MEMORY_BYTES % PIMWASM_MEMORY_PAGE_BYTES == 0,
               "PIMWASM memory must fit MRAM and contain whole Wasm pages");
_Static_assert(PIMWASM_MEMORY_CAPACITY_BYTES >= PIMWASM_MEMORY_BYTES &&
                   PIMWASM_MEMORY_CAPACITY_BYTES <= PIMWASM_MAX_MEMORY_BYTES &&
                   PIMWASM_MEMORY_CAPACITY_BYTES % PIMWASM_MEMORY_PAGE_BYTES == 0,
               "Reserved linear memory must contain whole pages and fit MRAM");
_Static_assert(PIMWASM_MEMORY_CAPACITY_BYTES % PIMWASM_BUFFER_BYTES == 0,
               "Enclosing DMA blocks must remain within declared memory");
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "The aligned word-load path requires a little-endian DPU target"
#endif

/* Reserve the admitted growth capacity, but only mem->size bytes are currently
   guest-addressable. Cache and transport state remain separate WRAM objects. */
#if PIMWASM_MEMORY_CAPACITY_BYTES
__mram_noinit uint8_t guest_memory[PIMWASM_MEMORY_CAPACITY_BYTES];
static __dma_aligned uint8_t cache[PIMWASM_BUFFER_BYTES];
static __dma_aligned uint8_t stage[8];
static __dma_aligned uint8_t zeros[256];
static uint32_t cache_base = UINT32_MAX;
#endif
void pimwasm_memory_invalidate(void) {
#if PIMWASM_MEMORY_CAPACITY_BYTES
  cache_base = UINT32_MAX;
#endif
}

#if PIMWASM_MEMORY_CAPACITY_BYTES
static void check_range(const wasm_rt_memory_t *mem, uint64_t address,
                        uint64_t size) {
  /* Check the complete effective address before any truncation or DMA. */
  if (address > mem->size || size > mem->size - address)
    wasm_rt_trap(WASM_RT_TRAP_OOB);
}

void pimwasm_memory_zero_range(uint32_t offset, uint32_t bytes) {
  /* This is a private allocation primitive, not a guest memory operation.
     Page-sized ranges also satisfy every DMA alignment/length constraint. */
  assert(offset % PIMWASM_MEMORY_PAGE_BYTES == 0 &&
         bytes % PIMWASM_MEMORY_PAGE_BYTES == 0 &&
         offset <= PIMWASM_MEMORY_CAPACITY_BYTES &&
         bytes <= PIMWASM_MEMORY_CAPACITY_BYTES - offset);
  pimwasm_memory_invalidate();
  memset(zeros, 0, sizeof(zeros));
  uint32_t end = offset + bytes;
  for (; offset < end; offset += sizeof(zeros))
    mram_write(zeros, guest_memory + offset, sizeof(zeros));
}

static const uint8_t *block(uint32_t address) {
  uint32_t base = address & ~(PIMWASM_BUFFER_BYTES - 1u);
  if (cache_base != base) {
    /* Fixed page sizes guarantee that this enclosing block is legal. */
    mram_read(guest_memory + base, cache, sizeof(cache));
    cache_base = base;
  }
  return cache + (address - base);
}

uint32_t i32_load_default32(wasm_rt_memory_t *mem, uint64_t address) {
  check_range(mem, address, 4);
  uint32_t offset = (uint32_t)address;
  if ((offset & (PIMWASM_BUFFER_BYTES - 1u)) <= PIMWASM_BUFFER_BYTES - 4u) {
    const uint8_t *p = block(offset);
    if (!(offset & 3u)) {
      uint32_t result;
      /* memcpy avoids aliasing a declared byte array as a uint32_t. */
      __builtin_memcpy(&result, __builtin_assume_aligned(p, 4), sizeof(result));
      return result;
    }
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
  }
  uint32_t result = 0, consumed = 0;
  while (consumed < 4) {
    uint32_t take = PIMWASM_BUFFER_BYTES - (offset & (PIMWASM_BUFFER_BYTES - 1u));
    if (take > 4 - consumed)
      take = 4 - consumed;
    const uint8_t *p = block(offset);
    /* Consume this fragment before replacing the single cached block. */
    for (uint32_t i = 0; i < take; ++i)
      result |= (uint32_t)p[i] << (8 * (consumed + i));
    offset += take;
    consumed += take;
  }
  return result;
}

void i32_store_default32(wasm_rt_memory_t *mem, uint64_t address,
                         uint32_t value) {
  /* A failing Wasm store must not write an in-bounds prefix. */
  check_range(mem, address, 4);
  pimwasm_memory_invalidate();
  uint32_t offset = (uint32_t)address, consumed = 0;
  while (consumed < 4) {
    uint32_t base = offset & ~UINT32_C(7), within = offset & 7u;
    uint32_t take = 8 - within;
    if (take > 4 - consumed)
      take = 4 - consumed;
    /* Preserve adjacent bytes for unaligned stores and DMA boundaries. */
    mram_read(guest_memory + base, stage, sizeof(stage));
    for (uint32_t i = 0; i < take; ++i)
      stage[within + i] = (uint8_t)(value >> (8 * (consumed + i)));
    mram_write(stage, guest_memory + base, sizeof(stage));
    offset += take;
    consumed += take;
  }
}

/* All scalar widths use the same cache and full effective-address check.
   Consume bytes before a refill; an unaligned 8-byte access may span blocks. */
static uint64_t load_bits(wasm_rt_memory_t *mem, uint64_t address,
                          uint32_t width) {
  check_range(mem, address, width);
  uint32_t offset = (uint32_t)address, consumed = 0;
  uint64_t value = 0;
  while (consumed < width) {
    uint32_t take = PIMWASM_BUFFER_BYTES - (offset & (PIMWASM_BUFFER_BYTES - 1u));
    if (take > width - consumed)
      take = width - consumed;
    const uint8_t *p = block(offset);
    for (uint32_t i = 0; i < take; ++i)
      value |= (uint64_t)p[i] << (8 * (consumed + i));
    consumed += take;
    offset += take;
  }
  return value;
}

static void store_bits(wasm_rt_memory_t *mem, uint64_t address, uint64_t value,
                       uint32_t width) {
  check_range(mem, address, width); /* No partial store on failure. */
  pimwasm_memory_invalidate();
  uint32_t offset = (uint32_t)address, consumed = 0;
  while (consumed < width) {
    uint32_t base = offset & ~UINT32_C(7), within = offset & 7u;
    uint32_t take = 8 - within;
    if (take > width - consumed)
      take = width - consumed;
    mram_read(guest_memory + base, stage, sizeof(stage));
    for (uint32_t i = 0; i < take; ++i)
      stage[within + i] = (uint8_t)(value >> (8 * (consumed + i)));
    mram_write(stage, guest_memory + base, sizeof(stage));
    consumed += take;
    offset += take;
  }
}

/* Sign extension in unsigned arithmetic avoids implementation-defined signed
   narrowing. Wasm integer results are bit containers, not C signed values. */
#define PIMWASM_LOAD(name, type, width, sign)                                      \
  type name##_default32(wasm_rt_memory_t *mem, uint64_t address) {             \
    uint64_t bits = load_bits(mem, address, width);                            \
    uint64_t high = sign ? UINT64_C(1) << ((width)*8 - 1) : 0;                 \
    return (type)((bits ^ high) - high);                                       \
  }
PIMWASM_LOAD(i64_load, uint64_t, 8, 0)
PIMWASM_LOAD(i32_load8_s, uint32_t, 1, 1)
PIMWASM_LOAD(i32_load8_u, uint32_t, 1, 0)
PIMWASM_LOAD(i32_load16_s, uint32_t, 2, 1)
PIMWASM_LOAD(i32_load16_u, uint32_t, 2, 0)
PIMWASM_LOAD(i64_load8_s, uint64_t, 1, 1)
PIMWASM_LOAD(i64_load8_u, uint64_t, 1, 0)
PIMWASM_LOAD(i64_load16_s, uint64_t, 2, 1)
PIMWASM_LOAD(i64_load16_u, uint64_t, 2, 0)
PIMWASM_LOAD(i64_load32_s, uint64_t, 4, 1)
PIMWASM_LOAD(i64_load32_u, uint64_t, 4, 0)
#undef PIMWASM_LOAD
#define PIMWASM_STORE(name, type, width)                                           \
  void name##_default32(wasm_rt_memory_t *mem, uint64_t address, type value) { \
    store_bits(mem, address, value, width);                                    \
  }
PIMWASM_STORE(i64_store, uint64_t, 8)
PIMWASM_STORE(i32_store8, uint32_t, 1)
PIMWASM_STORE(i32_store16, uint32_t, 2)
PIMWASM_STORE(i64_store8, uint64_t, 1)
PIMWASM_STORE(i64_store16, uint64_t, 2)
PIMWASM_STORE(i64_store32, uint64_t, 4)
#undef PIMWASM_STORE
float f32_load_default32(wasm_rt_memory_t *mem, uint64_t address) {
  uint32_t bits = i32_load_default32(mem, address);
  float value;
  memcpy(&value, &bits, 4);
  return value;
}
double f64_load_default32(wasm_rt_memory_t *mem, uint64_t address) {
  uint64_t bits = load_bits(mem, address, 8);
  double value;
  memcpy(&value, &bits, 8);
  return value;
}
void f32_store_default32(wasm_rt_memory_t *mem, uint64_t address, float value) {
  uint32_t bits;
  memcpy(&bits, &value, 4);
  i32_store_default32(mem, address, bits);
}
void f64_store_default32(wasm_rt_memory_t *mem, uint64_t address,
                         double value) {
  uint64_t bits;
  memcpy(&bits, &value, 8);
  store_bits(mem, address, bits, 8);
}

/* Bulk operations borrow the invalidated read cache as DMA scratch. Leave an
   eight-byte margin so either unaligned endpoint's enclosing DMA fits in it.
   No additional WRAM buffer and no simultaneous DMA operations are needed. */
#define PIMWASM_BULK_BYTES (PIMWASM_BUFFER_BYTES - 8u)

/* The payload already occupies cache[within .. within+size). Preserve bytes
   outside the guest write at both DMA edges before the single block write.
   Callers validated the whole instruction range; fixed page sizes make each
   enclosing eight-byte MRAM word legal, including the final word of memory. */
static void write_bulk_chunk(uint32_t address, uint32_t size) {
  uint32_t within = address & 7u;
  uint32_t base = address & ~UINT32_C(7);
  uint32_t end = within + size;
  uint32_t dma_size = (end + 7u) & ~UINT32_C(7);
  if (within) {
    mram_read(guest_memory + base, stage, sizeof(stage));
    memcpy(cache, stage, within);
  }
  if (end & 7u) {
    mram_read(guest_memory + base + dma_size - 8u, stage, sizeof(stage));
    memcpy(cache + end, stage + (end & 7u), 8u - (end & 7u));
  }
  mram_write(cache, guest_memory + base, dma_size);
}

void memory_copy(wasm_rt_memory_t *dest, const wasm_rt_memory_t *src,
                 uint64_t dest_addr, uint64_t src_addr, uint64_t size) {
  /* Validate both ranges even for zero length or equal addresses. A trap
     must not leave a partially copied prefix. Subtraction avoids overflow. */
  check_range(dest, dest_addr, size);
  check_range(src, src_addr, size);
  if (!size || dest_addr == src_addr)
    return;
  /* Admission permits one memory only; both descriptors refer to its MRAM.
     Range checks above prove all subsequent offsets/sums fit in uint32_t. */
  uint32_t to = (uint32_t)dest_addr, from = (uint32_t)src_addr;
  uint32_t remaining = (uint32_t)size;
  bool backward = to > from && to - from < remaining;
  pimwasm_memory_invalidate();
  while (remaining) {
    uint32_t take = remaining < PIMWASM_BULK_BYTES ? remaining : PIMWASM_BULK_BYTES;
    uint32_t source = backward ? from + remaining - take : from;
    uint32_t target = backward ? to + remaining - take : to;
    uint32_t source_within = source & 7u, target_within = target & 7u;
    uint32_t dma_size = (source_within + take + 7u) & ~UINT32_C(7);
    /* Read the entire chunk before any write, then align its payload for
       the destination DMA. WRAM memmove handles overlapping scratch bytes. */
    mram_read(guest_memory + (source & ~UINT32_C(7)), cache, dma_size);
    memmove(cache + target_within, cache + source_within, take);
    write_bulk_chunk(target, take);
    remaining -= take;
    if (!backward) {
      from += take;
      to += take;
    }
  }
  /* cache_base stays invalid: scratch bytes are not a cached MRAM block. */
}

void memory_fill(wasm_rt_memory_t *mem, uint64_t address, uint32_t value,
                 uint64_t size) {
  check_range(mem, address, size);
  if (!size)
    return;
  uint32_t offset = (uint32_t)address, remaining = (uint32_t)size;
  pimwasm_memory_invalidate();
  while (remaining) {
    uint32_t take = remaining < PIMWASM_BULK_BYTES ? remaining : PIMWASM_BULK_BYTES;
    /* Wasm fill uses only the low eight bits of its i32 value. */
    memset(cache + (offset & 7u), (uint8_t)value, take);
    write_bulk_chunk(offset, take);
    remaining -= take;
    offset += take;
  }
}

/* Pinned wasm2c supplies immutable WRAM data and the segment's effective
   length (zero after drop, or for an active/empty segment). Validate BOTH
   ranges before pointer arithmetic, cache invalidation, or writes. */
void memory_init(wasm_rt_memory_t *mem, const uint8_t *data, uint32_t data_size,
                 uint64_t address, uint32_t source, uint32_t size) {
  if (source > data_size || size > data_size - source)
    wasm_rt_trap(WASM_RT_TRAP_OOB);
  check_range(mem, address, size);
  if (!size)
    return; /* Also avoids arithmetic on an empty segment's NULL. */
  pimwasm_memory_invalidate();
  uint32_t offset = (uint32_t)address;
  while (size) {
    uint32_t take = size < PIMWASM_BULK_BYTES ? size : PIMWASM_BULK_BYTES;
    memcpy(cache + (offset & 7u), data + source, take);
    write_bulk_chunk(offset, take);
    source += take;
    offset += take;
    size -= take;
  }
}

/* Each generated active segment is applied in module order, after the complete
   initial memory was zeroed. Validate its entire range before changing bytes.
 */
void pimwasm_load_data(wasm_rt_memory_t *mem, uint64_t address, const uint8_t *data,
                   uint32_t size) {
  check_range(mem, address, size);
  pimwasm_memory_invalidate();
  uint32_t offset = (uint32_t)address;
  while (size) {
    uint32_t base = offset & ~UINT32_C(7), within = offset & 7u;
    uint32_t take = 8 - within;
    if (take > size)
      take = size;
    mram_read(guest_memory + base, stage, sizeof(stage));
    memcpy(stage + within, data, take);
    mram_write(stage, guest_memory + base, sizeof(stage));
    offset += take;
    data += take;
    size -= take;
  }
}
#elif PIMWASM_MEMORY_COUNT
/* A declared memory with zero reserved capacity has a real descriptor but
   no backing array or DMA buffers. Scalar accesses always trap. Empty bulk
   operations still validate all offsets, then succeed without native access. */
void pimwasm_memory_zero_range(uint32_t offset, uint32_t bytes) {
  assert(offset == 0 && bytes == 0);
}
#define ZERO_LOAD(name, type)                                                  \
  type name##_default32(wasm_rt_memory_t *mem, uint64_t address) {             \
    (void)mem;                                                                 \
    (void)address;                                                             \
    wasm_rt_trap(WASM_RT_TRAP_OOB);                                            \
  }
#define ZERO_STORE(name, type)                                                 \
  void name##_default32(wasm_rt_memory_t *mem, uint64_t address, type value) { \
    (void)mem;                                                                 \
    (void)address;                                                             \
    (void)value;                                                               \
    wasm_rt_trap(WASM_RT_TRAP_OOB);                                            \
  }
ZERO_LOAD(i32_load, uint32_t)
ZERO_LOAD(i64_load, uint64_t)
ZERO_LOAD(f32_load, float)
ZERO_LOAD(f64_load, double)
ZERO_LOAD(i32_load8_s, uint32_t)
ZERO_LOAD(i32_load8_u, uint32_t)
ZERO_LOAD(i32_load16_s, uint32_t)
ZERO_LOAD(i32_load16_u, uint32_t)
ZERO_LOAD(i64_load8_s, uint64_t)
ZERO_LOAD(i64_load8_u, uint64_t)
ZERO_LOAD(i64_load16_s, uint64_t)
ZERO_LOAD(i64_load16_u, uint64_t)
ZERO_LOAD(i64_load32_s, uint64_t)
ZERO_LOAD(i64_load32_u, uint64_t)
ZERO_STORE(i32_store, uint32_t)
ZERO_STORE(i64_store, uint64_t)
ZERO_STORE(f32_store, float)
ZERO_STORE(f64_store, double)
ZERO_STORE(i32_store8, uint32_t)
ZERO_STORE(i32_store16, uint32_t)
ZERO_STORE(i64_store8, uint64_t)
ZERO_STORE(i64_store16, uint64_t)
ZERO_STORE(i64_store32, uint64_t)
#undef ZERO_LOAD
#undef ZERO_STORE
static void empty_range(uint64_t address, uint64_t size) {
  if (address || size)
    wasm_rt_trap(WASM_RT_TRAP_OOB);
}
void memory_copy(wasm_rt_memory_t *dest, const wasm_rt_memory_t *src,
                 uint64_t address, uint64_t source, uint64_t size) {
  (void)dest;
  (void)src;
  empty_range(address, size);
  empty_range(source, size);
}
void memory_fill(wasm_rt_memory_t *mem, uint64_t address, uint32_t value,
                 uint64_t size) {
  (void)mem;
  (void)value;
  empty_range(address, size);
}
void memory_init(wasm_rt_memory_t *mem, const uint8_t *data, uint32_t data_size,
                 uint64_t address, uint32_t source, uint32_t size) {
  (void)mem;
  (void)data;
  if (source > data_size || size > data_size - source)
    wasm_rt_trap(WASM_RT_TRAP_OOB);
  empty_range(address, size);
}
void pimwasm_load_data(wasm_rt_memory_t *mem, uint64_t address, const uint8_t *data,
                   uint32_t size) {
  (void)mem;
  (void)data;
  empty_range(address, size);
}
#endif /* Capacity-zero memories allocate no MRAM/cache; absent memory has no  \
          helpers. */

uint32_t wasm_rt_memory_size_bytes;
#if PIMWASM_MEMORY_COUNT
void wasm_rt_allocate_memory(wasm_rt_memory_t *mem, uint64_t initial,
                             uint64_t maximum, bool is64) {
  /* MRAM capacity is reserved at link time. Preserve the declared maximum,
     but expose/initialize only the initial pages until growth succeeds. */
  assert(initial == PIMWASM_MEMORY_BYTES / PIMWASM_MEMORY_PAGE_BYTES &&
         maximum == PIMWASM_MEMORY_MAX_PAGES && maximum >= initial && !is64);
  pimwasm_memory_zero_range(0, PIMWASM_MEMORY_BYTES);
  mem->pages = initial;
  mem->max_pages = maximum;
  mem->size = PIMWASM_MEMORY_BYTES;
  mem->is64 = false;
  wasm_rt_memory_size_bytes = PIMWASM_MEMORY_BYTES;
}

uint64_t wasm_rt_grow_memory(wasm_rt_memory_t *mem, uint64_t pages) {
  uint64_t old_pages = mem->pages;
  uint64_t capacity_pages = PIMWASM_MEMORY_CAPACITY_BYTES / PIMWASM_MEMORY_PAGE_BYTES;
  /* The pinned wasm2c ABI returns u64; its memory32 caller keeps the low
     32 bits of the -1 failure result. Validate with subtraction so a huge
     unsigned request cannot wrap around either the maximum or capacity. */
  if (old_pages > mem->max_pages || old_pages > capacity_pages ||
      pages > mem->max_pages - old_pages || pages > capacity_pages - old_pages)
    return UINT64_MAX;
  if (!pages)
    return old_pages;
  uint32_t old_size = (uint32_t)mem->size;
  uint32_t added_size = (uint32_t)(pages * PIMWASM_MEMORY_PAGE_BYTES);
  /* Zero before publishing: stale capacity from a prior instance/reset is
     inaccessible, and becomes visible only after all new bytes are zero. */
  pimwasm_memory_zero_range(old_size, added_size);
  mem->pages = old_pages + pages;
  mem->size = old_size + added_size;
  wasm_rt_memory_size_bytes = (uint32_t)mem->size;
  return old_pages;
}

void wasm_rt_free_memory(wasm_rt_memory_t *mem) {
  /* Static MRAM has no heap allocation to release. Retire the instance view. */
  pimwasm_memory_invalidate();
  mem->size = 0;
  mem->pages = 0;
  wasm_rt_memory_size_bytes = 0;
}
#endif
