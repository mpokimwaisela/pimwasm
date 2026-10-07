/* Scalar numeric support for the generated WABT helpers.
 *
 * UPMEM's C runtime supplies scalar arithmetic but not libm. This header
 * supplies corrected binary32/binary64 compiler-ABI division helpers.  These
 * routines operate on the representation; they do not depend on a host
 * header/library or a floating-point rounding mode.  nearest and sqrt round to
 * nearest, ties to even as Wasm requires. This is scalar software support, not
 * a claim of a complete C math library.
 */
#ifndef PIMWASM_DPU_NUMERIC_H
#define PIMWASM_DPU_NUMERIC_H

#include <float.h>
#include <stdint.h>
#include <string.h>

_Static_assert(sizeof(float) == 4 && sizeof(double) == 8 && FLT_RADIX == 2 &&
                   FLT_MANT_DIG == 24 && DBL_MANT_DIG == 53 &&
                   FLT_MAX_EXP == 128 && DBL_MAX_EXP == 1024,
               "The scalar runtime requires IEEE binary32 and binary64");

#ifndef LIKELY
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif
#define wasm_rt_memcpy memcpy

#define I32_CLZ(x) ((x) ? (uint32_t)__builtin_clz((uint32_t)(x)) : 32u)
#define I64_CLZ(x) ((x) ? (uint64_t)__builtin_clzll((uint64_t)(x)) : 64u)
#define I32_CTZ(x) ((x) ? (uint32_t)__builtin_ctz((uint32_t)(x)) : 32u)
#define I64_CTZ(x) ((x) ? (uint64_t)__builtin_ctzll((uint64_t)(x)) : 64u)
#define I32_POPCNT(x) ((uint32_t)__builtin_popcount((uint32_t)(x)))
#define I64_POPCNT(x) ((uint64_t)__builtin_popcountll((uint64_t)(x)))

static inline uint32_t pimwasm_num_bits32(float x) {
  uint32_t bits;
  memcpy(&bits, &x, sizeof(bits));
  return bits;
}
static inline uint64_t pimwasm_num_bits64(double x) {
  uint64_t bits;
  memcpy(&bits, &x, sizeof(bits));
  return bits;
}
static inline float pimwasm_num_from32(uint32_t bits) {
  float x;
  memcpy(&x, &bits, sizeof(x));
  return x;
}
static inline double pimwasm_num_from64(uint64_t bits) {
  double x;
  memcpy(&x, &bits, sizeof(x));
  return x;
}
static inline int pimwasm_num_isnan32(float x) {
  return (pimwasm_num_bits32(x) & UINT32_C(0x7fffffff)) > UINT32_C(0x7f800000);
}
static inline int pimwasm_num_isnan64(double x) {
  return (pimwasm_num_bits64(x) & UINT64_C(0x7fffffffffffffff)) >
         UINT64_C(0x7ff0000000000000);
}
static inline int pimwasm_num_signbit32(float x) { return pimwasm_num_bits32(x) >> 31; }
static inline int pimwasm_num_signbit64(double x) {
  return pimwasm_num_bits64(x) >> 63;
}

enum pimwasm_num_round {
  PIMWASM_NUM_TRUNC,
  PIMWASM_NUM_FLOOR,
  PIMWASM_NUM_CEIL,
  PIMWASM_NUM_NEAREST
};

/* The exponent, hidden bit and discarded fraction completely determine
 * rounding to an integer.  Incrementing the truncated representation by one
 * unit at its integer bit also handles carries into the next exponent. */
static inline uint64_t pimwasm_num_round_bits(uint64_t bits, unsigned fraction,
                                          unsigned exponent_bits,
                                          enum pimwasm_num_round mode) {
  const uint64_t sign_mask = UINT64_C(1) << (fraction + exponent_bits);
  const uint64_t exponent_mask = (UINT64_C(1) << exponent_bits) - 1;
  const uint64_t mantissa_mask = (UINT64_C(1) << fraction) - 1;
  const uint64_t sign = bits & sign_mask;
  const uint64_t magnitude = bits & (sign_mask - 1);
  const unsigned exponent = (unsigned)((bits >> fraction) & exponent_mask);
  const int bias = (1 << (exponent_bits - 1)) - 1;
  const int e = (int)exponent - bias;
  if (exponent == exponent_mask) {
    /* Arithmetic operations quiet a signaling NaN; infinity is unchanged. */
    return (bits & mantissa_mask) ? bits | (UINT64_C(1) << (fraction - 1))
                                  : bits;
  }
  if (!magnitude || e >= (int)fraction)
    return bits;
  if (e < 0) {
    int away =
        (mode == PIMWASM_NUM_FLOOR && sign) || (mode == PIMWASM_NUM_CEIL && !sign);
    if (mode == PIMWASM_NUM_NEAREST)
      away = magnitude > ((uint64_t)(bias - 1) << fraction);
    return away ? sign | ((uint64_t)bias << fraction) : sign;
  }
  const unsigned shift = fraction - (unsigned)e; /* strictly 1..fraction */
  const uint64_t unit = UINT64_C(1) << shift;
  const uint64_t tail = bits & (unit - 1);
  uint64_t result = bits & ~(unit - 1);
  if (!tail)
    return result;
  int increment =
      (mode == PIMWASM_NUM_FLOOR && sign) || (mode == PIMWASM_NUM_CEIL && !sign);
  if (mode == PIMWASM_NUM_NEAREST)
    increment = tail > (unit >> 1) || (tail == (unit >> 1) && (result & unit));
  return result + (increment ? unit : 0);
}

/* Return a bit of a virtual significand << shift.  The radicand for binary64
 * sqrt has up to 106 bits, but restoring square root needs only a 53-bit root
 * and at most 55 bits for intermediate remainders.  No 128-bit integer runtime
 * is required. */
static inline uint64_t pimwasm_num_radical_bit(uint64_t significand, int position) {
  return position >= 0 && position < 64 ? (significand >> position) & 1 : 0;
}
static inline uint64_t pimwasm_num_sqrt_bits(uint64_t bits, unsigned fraction,
                                         unsigned exponent_bits) {
  const uint64_t hidden = UINT64_C(1) << fraction;
  const uint64_t sign_mask = UINT64_C(1) << (fraction + exponent_bits);
  const uint64_t exponent_mask = (UINT64_C(1) << exponent_bits) - 1;
  const uint64_t quiet_nan = (exponent_mask << fraction) | (hidden >> 1);
  const unsigned exponent = (unsigned)((bits >> fraction) & exponent_mask);
  uint64_t significand = bits & (hidden - 1);
  if (exponent == exponent_mask && significand)
    return bits | (hidden >> 1);
  if (!(bits & (sign_mask - 1)))
    return bits; /* including negative zero */
  if (bits & sign_mask)
    return quiet_nan;
  if (exponent == exponent_mask)
    return bits; /* positive infinity */
  const int bias = (1 << (exponent_bits - 1)) - 1;
  int e = (int)exponent - bias;
  if (exponent) {
    significand |= hidden;
  } else {
    e = 1 - bias;
    while (!(significand & hidden)) {
      significand <<= 1;
      --e;
    }
  }
  const int odd = e % 2 != 0;
  const int shift = (int)fraction + odd;
  e -= odd;
  uint64_t root = 0, remainder = 0;
  for (int pair = (int)fraction; pair >= 0; --pair) {
    const uint64_t digit =
        (pimwasm_num_radical_bit(significand, 2 * pair + 1 - shift) << 1) |
        pimwasm_num_radical_bit(significand, 2 * pair - shift);
    remainder = (remainder << 2) | digit;
    const uint64_t trial = (root << 2) | 1;
    root <<= 1;
    if (remainder >= trial) {
      remainder -= trial;
      root |= 1;
    }
  }
  /* sqrt(N) > q+1/2 iff N-q*q > q+1/4.  Both N and the remainder are
   * integers, so remainder > q is exact, and a halfway case is impossible. */
  if (remainder > root)
    ++root;
  if (root == (hidden << 1)) {
    root >>= 1;
    e += 2;
  }
  /* The square root of the smallest subnormal is itself normal. */
  return ((uint64_t)(e / 2 + bias) << fraction) | (root & (hidden - 1));
}

#define PIMWASM_NUM_ROUND_WRAPPER(name, mode)                                      \
  static inline float pimwasm_num_##name##f(float x) {                             \
    return pimwasm_num_from32(                                                     \
        (uint32_t)pimwasm_num_round_bits(pimwasm_num_bits32(x), 23, 8, mode));         \
  }                                                                            \
  static inline double pimwasm_num_##name(double x) {                              \
    return pimwasm_num_from64(                                                     \
        pimwasm_num_round_bits(pimwasm_num_bits64(x), 52, 11, mode));                  \
  }
PIMWASM_NUM_ROUND_WRAPPER(trunc, PIMWASM_NUM_TRUNC)
PIMWASM_NUM_ROUND_WRAPPER(floor, PIMWASM_NUM_FLOOR)
PIMWASM_NUM_ROUND_WRAPPER(ceil, PIMWASM_NUM_CEIL)
PIMWASM_NUM_ROUND_WRAPPER(nearbyint, PIMWASM_NUM_NEAREST)
#undef PIMWASM_NUM_ROUND_WRAPPER

static inline float pimwasm_num_sqrtf(float x) {
  return pimwasm_num_from32((uint32_t)pimwasm_num_sqrt_bits(pimwasm_num_bits32(x), 23, 8));
}
static inline double pimwasm_num_sqrt(double x) {
  return pimwasm_num_from64(pimwasm_num_sqrt_bits(pimwasm_num_bits64(x), 52, 11));
}
static inline float pimwasm_num_fabsf(float x) {
  return pimwasm_num_from32(pimwasm_num_bits32(x) & UINT32_C(0x7fffffff));
}
static inline double pimwasm_num_fabs(double x) {
  return pimwasm_num_from64(pimwasm_num_bits64(x) & UINT64_C(0x7fffffffffffffff));
}
static inline float pimwasm_num_copysignf(float x, float y) {
  return pimwasm_num_from32((pimwasm_num_bits32(x) & UINT32_C(0x7fffffff)) |
                        (pimwasm_num_bits32(y) & UINT32_C(0x80000000)));
}
static inline double pimwasm_num_copysign(double x, double y) {
  return pimwasm_num_from64((pimwasm_num_bits64(x) & UINT64_C(0x7fffffffffffffff)) |
                        (pimwasm_num_bits64(y) & UINT64_C(0x8000000000000000)));
}

/* Emit the compiler-ABI definitions in exactly one translation unit:
 * wasm-rt-numeric.c defines PIMWASM_NUMERIC_IMPLEMENTATION before including this header.
 * Other consumers retain the ordinary inline numeric helpers only.
 * Generated C division still calls __divsf3 / __divdf3; no generated
 * computation is rewritten. Division uses only uint64_t arithmetic and
 * round-to-nearest, ties-to-even, including subnormal results.
 */
#ifdef PIMWASM_NUMERIC_IMPLEMENTATION
static uint64_t divide_bits(uint64_t a, uint64_t b, unsigned fraction,
                            unsigned exponent_bits) {
  const uint64_t hidden = UINT64_C(1) << fraction;
  const uint64_t exp_mask = (UINT64_C(1) << exponent_bits) - 1;
  const uint64_t inf = exp_mask << fraction;
  const uint64_t sign_mask = UINT64_C(1) << (fraction + exponent_bits);
  const uint64_t sign = (a ^ b) & sign_mask;
  const uint64_t nan = inf | (hidden >> 1);
  uint64_t ma = a & (sign_mask - 1), mb = b & (sign_mask - 1);
  if (ma > inf || mb > inf || (ma == inf && mb == inf) || (!ma && !mb))
    return nan;
  if (ma == inf || !mb)
    return sign | inf;
  if (!ma || mb == inf)
    return sign;

  int bias = (1 << (exponent_bits - 1)) - 1;
  int minimum = 1 - bias;
  int ea = (int)(ma >> fraction), eb = (int)(mb >> fraction);
  ma &= hidden - 1;
  mb &= hidden - 1;
  if (ea) {
    ma |= hidden;
    ea -= bias;
  } else {
    ea = minimum;
    while (ma < hidden) {
      ma <<= 1;
      --ea;
    }
  }
  if (eb) {
    mb |= hidden;
    eb -= bias;
  } else {
    eb = minimum;
    while (mb < hidden) {
      mb <<= 1;
      --eb;
    }
  }
  int exponent = ea - eb;
  if (ma < mb) {
    ma <<= 1;
    --exponent;
  }
  /* Ratio is now [1,2). Subnormal results have fewer retained bits.
     Rounding directly at that precision avoids double rounding. */
  if (exponent > bias)
    return sign | inf;
  int places = (int)fraction;
  if (exponent < minimum)
    places += exponent - minimum;
  if (places < -1)
    return sign;
  if (places == -1)
    return sign | (ma > mb); /* midpoint ties to even zero */

  uint64_t quotient = 1, remainder = ma - mb;
  for (int i = 0; i < places; ++i) {
    remainder <<= 1;
    quotient <<= 1;
    if (remainder >= mb) {
      remainder -= mb;
      quotient |= 1;
    }
  }
  /* remainder < mb < 2^53, so doubling fits in uint64_t. */
  uint64_t twice = remainder << 1;
  if (twice > mb || (twice == mb && (quotient & 1)))
    ++quotient;
  if (exponent < minimum)
    return sign | quotient; /* may round to minimum normal */
  if (quotient == (hidden << 1)) {
    quotient >>= 1;
    ++exponent;
  }
  if (exponent > bias)
    return sign | inf;
  return sign | ((uint64_t)(exponent + bias) << fraction) |
         (quotient & (hidden - 1));
}

float __divsf3(float a, float b) {
  uint32_t x, y, z;
  float result;
  memcpy(&x, &a, 4);
  memcpy(&y, &b, 4);
  z = (uint32_t)divide_bits(x, y, 23, 8);
  memcpy(&result, &z, 4);
  return result;
}
double __divdf3(double a, double b) {
  uint64_t x, y, z;
  double result;
  memcpy(&x, &a, 8);
  memcpy(&y, &b, 8);
  z = divide_bits(x, y, 52, 11);
  memcpy(&result, &z, 8);
  return result;
}
#endif /* PIMWASM_NUMERIC_IMPLEMENTATION */

/* Aliases are private to adapted generated translation units.  Tests disable
 * them so the reference calls below can resolve independently to host libm. */
#ifndef PIMWASM_NUMERIC_NO_MATH_ALIASES
#define NAN (pimwasm_num_from32(UINT32_C(0x7fc00000)))
#define INFINITY (pimwasm_num_from32(UINT32_C(0x7f800000)))
#define isnan(x)                                                               \
  _Generic((x), float : pimwasm_num_isnan32, double : pimwasm_num_isnan64)(x)
#define signbit(x)                                                             \
  _Generic((x), float : pimwasm_num_signbit32, double : pimwasm_num_signbit64)(x)
#define floorf pimwasm_num_floorf
#define floor pimwasm_num_floor
#define ceilf pimwasm_num_ceilf
#define ceil pimwasm_num_ceil
#define truncf pimwasm_num_truncf
#define trunc pimwasm_num_trunc
#define nearbyintf pimwasm_num_nearbyintf
#define nearbyint pimwasm_num_nearbyint
#define sqrtf pimwasm_num_sqrtf
#define sqrt pimwasm_num_sqrt
#define fabsf pimwasm_num_fabsf
#define fabs pimwasm_num_fabs
#define copysignf pimwasm_num_copysignf
#define copysign pimwasm_num_copysign
#endif
#endif
