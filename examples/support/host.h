#ifndef PIMWASM_APP_HOST_H
#define PIMWASM_APP_HOST_H

/* Shared example-host helpers; not part of the PIMWASM API. */

#include "pimwasm.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static inline void app_fail(const char *format, ...) {
  va_list args;
  fprintf(stderr, "\033[31m[FAIL]\033[0m ");
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fputc('\n', stderr);
}

#define APP_CHECK(expr)                                                        \
  do {                                                                         \
    if (!(expr)) {                                                             \
      app_fail("%s:%d: %s failed", __FILE__, __LINE__, #expr);        \
      goto done;                                                               \
    }                                                                          \
  } while (0)

/* Status-returning API calls need a different check from boolean conditions. */
#define APP_CHECK_STATUS(expr)                                                 \
  do {                                                                         \
    enum pimwasm_status app_status_ = (expr);                                    \
    if (app_status_ != PIMWASM_OK) {                                            \
      app_fail("%s: %s", #expr, pimwasm_status_string(app_status_));    \
      goto done;                                                               \
    }                                                                          \
  } while (0)

static inline void app_ok(const char *format, ...) {
  va_list args;
  printf("\033[32m[OK]\033[0m ");
  va_start(args, format);
  vprintf(format, args);
  va_end(args);
  putchar('\n');
}

static inline int app_u32(const char *text, uint32_t *out) {
  char *end;
  errno = 0;
  unsigned long long n = strtoull(text, &end, 0);
  if (text[0] < '0' || text[0] > '9' || errno || *end || n > UINT32_MAX)
    return 0;
  *out = (uint32_t)n;
  return 1;
}

static inline void app_encode(uint8_t *bytes, uint64_t value, unsigned width) {
  for (unsigned i = 0; i < width; ++i)
    bytes[i] = (uint8_t)(value >> (8 * i));
}

static inline uint64_t app_decode(const uint8_t *bytes, unsigned width) {
  uint64_t value = 0;
  for (unsigned i = 0; i < width; ++i)
    value |= (uint64_t)bytes[i] << (8 * i);
  return value;
}

static inline void app_put_u32(uint8_t *bytes, uint32_t value) {
  app_encode(bytes, value, 4);
}

static inline uint32_t app_get_u32(const uint8_t *bytes) {
  return (uint32_t)app_decode(bytes, 4);
}

/* Host arrays are explicitly encoded; native endianness is never assumed. */
static inline enum pimwasm_status app_upload(pimwasm_t *module, uint32_t offset,
                                         const uint64_t *values, size_t count,
                                         unsigned width) {
  if ((width != 4 && width != 8) || count > SIZE_MAX / width)
    return PIMWASM_INVALID;
  uint8_t *bytes = malloc(count ? count * width : 1);
  if (!bytes)
    return PIMWASM_BACKEND_ERROR;
  for (size_t i = 0; i < count; ++i)
    app_encode(bytes + i * width, values[i], width);
  enum pimwasm_status status = pimwasm_write(module, offset, bytes, count * width);
  free(bytes);
  return status;
}

static inline int app_check_output(pimwasm_t *module, uint32_t offset,
                                   const uint64_t *expected, size_t count,
                                   unsigned width) {
  if ((width != 4 && width != 8) || count > SIZE_MAX / width)
    return 0;
  uint8_t *bytes = malloc(count ? count * width : 1);
  if (!bytes)
    return 0;
  int valid = pimwasm_read(module, offset, bytes, count * width) == PIMWASM_OK;
  for (size_t i = 0; valid && i < count; ++i)
    valid = app_decode(bytes + i * width, width) == expected[i];
  free(bytes);
  return valid;
}

#endif
