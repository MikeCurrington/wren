#ifndef wren_math_h
#define wren_math_h

#include <math.h>
#include <stdint.h>

#include <bit>

#define WREN_DOUBLE_QNAN_POS_MIN_BITS (UINT64_C(0x7FF8000000000000))
#define WREN_DOUBLE_QNAN_POS_MAX_BITS (UINT64_C(0x7FFFFFFFFFFFFFFF))

#define WREN_DOUBLE_NAN (wrenDoubleFromBits(WREN_DOUBLE_QNAN_POS_MIN_BITS))

// Reinterpret a double as raw bits and back, without type punning.
static inline double wrenDoubleFromBits(uint64_t bits)
{
  return std::bit_cast<double>(bits);
}

static inline uint64_t wrenDoubleToBits(double num)
{
  return std::bit_cast<uint64_t>(num);
}

#endif
