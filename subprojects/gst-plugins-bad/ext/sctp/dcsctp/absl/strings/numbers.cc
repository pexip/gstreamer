// Copyright 2017 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// This file contains string processing functions related to
// numeric values.

#include "absl/strings/numbers.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cfloat>  // for DBL_DIG and FLT_DIG
#include <cmath>   // for HUGE_VAL
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <system_error>  // NOLINT(build/c++11)
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/config.h"
#include "absl/base/internal/endian.h"
#include "absl/base/internal/raw_logging.h"
#include "absl/base/nullability.h"
#include "absl/base/optimization.h"
#include "absl/numeric/bits.h"
#include "absl/numeric/int128.h"
#include "absl/strings/ascii.h"
#include "absl/strings/charconv.h"
#include "absl/strings/match.h"
#include "absl/strings/string_view.h"

namespace absl {
ABSL_NAMESPACE_BEGIN

bool SimpleAtof(absl::string_view str, float* absl_nonnull out) {
  *out = 0.0;
  str = StripAsciiWhitespace(str);
  // std::from_chars doesn't accept an initial +, but SimpleAtof does, so if one
  // is present, skip it, while avoiding accepting "+-0" as valid.
  if (!str.empty() && str[0] == '+') {
    str.remove_prefix(1);
    if (!str.empty() && str[0] == '-') {
      return false;
    }
  }
  auto result = absl::from_chars(str.data(), str.data() + str.size(), *out);
  if (result.ec == std::errc::invalid_argument) {
    return false;
  }
  if (result.ptr != str.data() + str.size()) {
    // not all non-whitespace characters consumed
    return false;
  }
  // from_chars() with DR 3081's current wording will return max() on
  // overflow.  SimpleAtof returns infinity instead.
  if (result.ec == std::errc::result_out_of_range) {
    if (*out > 1.0) {
      *out = std::numeric_limits<float>::infinity();
    } else if (*out < -1.0) {
      *out = -std::numeric_limits<float>::infinity();
    }
  }
  return true;
}

bool SimpleAtod(absl::string_view str, double* absl_nonnull out) {
  *out = 0.0;
  str = StripAsciiWhitespace(str);
  // std::from_chars doesn't accept an initial +, but SimpleAtod does, so if one
  // is present, skip it, while avoiding accepting "+-0" as valid.
  if (!str.empty() && str[0] == '+') {
    str.remove_prefix(1);
    if (!str.empty() && str[0] == '-') {
      return false;
    }
  }
  auto result = absl::from_chars(str.data(), str.data() + str.size(), *out);
  if (result.ec == std::errc::invalid_argument) {
    return false;
  }
  if (result.ptr != str.data() + str.size()) {
    // not all non-whitespace characters consumed
    return false;
  }
  // from_chars() with DR 3081's current wording will return max() on
  // overflow.  SimpleAtod returns infinity instead.
  if (result.ec == std::errc::result_out_of_range) {
    if (*out > 1.0) {
      *out = std::numeric_limits<double>::infinity();
    } else if (*out < -1.0) {
      *out = -std::numeric_limits<double>::infinity();
    }
  }
  return true;
}

bool SimpleAtob(absl::string_view str, bool* absl_nonnull out) {
  ABSL_RAW_CHECK(out != nullptr, "Output pointer must not be nullptr.");
  if (EqualsIgnoreCase(str, "true") || EqualsIgnoreCase(str, "t") ||
      EqualsIgnoreCase(str, "yes") || EqualsIgnoreCase(str, "y") ||
      EqualsIgnoreCase(str, "1")) {
    *out = true;
    return true;
  }
  if (EqualsIgnoreCase(str, "false") || EqualsIgnoreCase(str, "f") ||
      EqualsIgnoreCase(str, "no") || EqualsIgnoreCase(str, "n") ||
      EqualsIgnoreCase(str, "0")) {
    *out = false;
    return true;
  }
  return false;
}

// ----------------------------------------------------------------------
// FastIntToBuffer() overloads
//
// Like the Fast*ToBuffer() functions above, these are intended for speed.
// Unlike the Fast*ToBuffer() functions, however, these functions write
// their output to the beginning of the buffer.  The caller is responsible
// for ensuring that the buffer has enough space to hold the output.
//
// Returns a pointer to the end of the string (i.e. the null character
// terminating the string).
// ----------------------------------------------------------------------

namespace {

// Various routines to encode integers to strings.

// We split data encodings into a group of 2 digits, 4 digits, 8 digits as
// it's easier to combine powers of two into scalar arithmetic.

// Previous implementation used a lookup table of 200 bytes for every 2 bytes
// and it was memory bound, any L1 cache miss would result in a much slower
// result. When benchmarking with a cache eviction rate of several percent,
// this implementation proved to be better.

// These constants represent '00', '0000' and '00000000' as ascii strings in
// integers. We can add these numbers if we encode to bytes from 0 to 9. as
// 'i' = '0' + i for 0 <= i <= 9.
constexpr uint32_t kTwoZeroBytes = 0x0101 * '0';
constexpr uint64_t kFourZeroBytes = 0x01010101 * '0';
constexpr uint64_t kEightZeroBytes = 0x0101010101010101ull * '0';

// * 103 / 1024 is a division by 10 for values from 0 to 99. It's also a
// division of a structure [k takes 2 bytes][m takes 2 bytes], then * 103 / 1024
// will be [k / 10][m / 10]. It allows parallel division.
constexpr uint64_t kDivisionBy10Mul = 103u;
constexpr uint64_t kDivisionBy10Div = 1 << 10;

// * 10486 / 1048576 is a division by 100 for values from 0 to 9999.
constexpr uint64_t kDivisionBy100Mul = 10486u;
constexpr uint64_t kDivisionBy100Div = 1 << 20;

// Encode functions write the ASCII output of input `n` to `out_str`.
inline char* EncodeHundred(uint32_t n, char* absl_nonnull out_str) {
  int num_digits = static_cast<int>(n - 10) >> 8;
  uint32_t div10 = (n * kDivisionBy10Mul) / kDivisionBy10Div;
  uint32_t mod10 = n - 10u * div10;
  uint32_t base = kTwoZeroBytes + div10 + (mod10 << 8);
  base >>= num_digits & 8;
  little_endian::Store16(out_str, static_cast<uint16_t>(base));
  return out_str + 2 + num_digits;
}

inline char* EncodeTenThousand(uint32_t n, char* absl_nonnull out_str) {
  // We split lower 2 digits and upper 2 digits of n into 2 byte consecutive
  // blocks. 123 ->  [\0\1][\0\23]. We divide by 10 both blocks
  // (it's 1 division + zeroing upper bits), and compute modulo 10 as well "in
  // parallel". Then we combine both results to have both ASCII digits,
  // strip trailing zeros, add ASCII '0000' and return.
  uint32_t div100 = (n * kDivisionBy100Mul) / kDivisionBy100Div;
  uint32_t mod100 = n - 100ull * div100;
  uint32_t hundreds = (mod100 << 16) + div100;
  uint32_t tens = (hundreds * kDivisionBy10Mul) / kDivisionBy10Div;
  tens &= (0xFull << 16) | 0xFull;
  tens += (hundreds - 10ull * tens) << 8;
  ABSL_ASSUME(tens != 0);
  // The result can contain trailing zero bits, we need to strip them to a first
  // significant byte in a final representation. For example, for n = 123, we
  // have tens to have representation \0\1\2\3. We do `& -8` to round
  // to a multiple to 8 to strip zero bytes, not all zero bits.
  // countr_zero to help.
  // 0 minus 8 to make MSVC happy.
  uint32_t zeroes = static_cast<uint32_t>(absl::countr_zero(tens)) & (0 - 8u);
  tens += kFourZeroBytes;
  tens >>= zeroes;
  little_endian::Store32(out_str, tens);
  return out_str + sizeof(tens) - zeroes / 8;
}

// Helper function to produce an ASCII representation of `i`.
//
// Function returns an 8-byte integer which when summed with `kEightZeroBytes`,
// can be treated as a printable buffer with ascii representation of `i`,
// possibly with leading zeros.
//
// Example:
//
//  uint64_t buffer = PrepareEightDigits(102030) + kEightZeroBytes;
//  char* ascii = reinterpret_cast<char*>(&buffer);
//  // Note two leading zeros:
//  EXPECT_EQ(absl::string_view(ascii, 8), "00102030");
//
// Pre-condition: `i` must be less than 100000000.
inline uint64_t PrepareEightDigits(uint32_t i) {
  ABSL_ASSUME(i < 10000'0000);
  // Prepare 2 blocks of 4 digits "in parallel".
  uint32_t hi = i / 10000;
  uint32_t lo = i % 10000;
  uint64_t merged = hi | (uint64_t{lo} << 32);
  uint64_t div100 = ((merged * kDivisionBy100Mul) / kDivisionBy100Div) &
                    ((0x7Full << 32) | 0x7Full);
  uint64_t mod100 = merged - 100ull * div100;
  uint64_t hundreds = (mod100 << 16) + div100;
  uint64_t tens = (hundreds * kDivisionBy10Mul) / kDivisionBy10Div;
  tens &= (0xFull << 48) | (0xFull << 32) | (0xFull << 16) | 0xFull;
  tens += (hundreds - 10ull * tens) << 8;
  return tens;
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE char* absl_nonnull EncodeFullU32(
    uint32_t n, char* absl_nonnull out_str) {
  if (n < 10) {
    *out_str = static_cast<char>('0' + n);
    return out_str + 1;
  }
  if (n < 100'000'000) {
    uint64_t bottom = PrepareEightDigits(n);
    ABSL_ASSUME(bottom != 0);
    // 0 minus 8 to make MSVC happy.
    uint32_t zeroes =
        static_cast<uint32_t>(absl::countr_zero(bottom)) & (0 - 8u);
    little_endian::Store64(out_str, (bottom + kEightZeroBytes) >> zeroes);
    return out_str + sizeof(bottom) - zeroes / 8;
  }
  uint32_t div08 = n / 100'000'000;
  uint32_t mod08 = n % 100'000'000;
  uint64_t bottom = PrepareEightDigits(mod08) + kEightZeroBytes;
  out_str = EncodeHundred(div08, out_str);
  little_endian::Store64(out_str, bottom);
  return out_str + sizeof(bottom);
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE char* EncodeFullU64(uint64_t i,
                                                        char* buffer) {
  if (i <= std::numeric_limits<uint32_t>::max()) {
    return EncodeFullU32(static_cast<uint32_t>(i), buffer);
  }
  uint32_t mod08;
  if (i < 1'0000'0000'0000'0000ull) {
    uint32_t div08 = static_cast<uint32_t>(i / 100'000'000ull);
    mod08 =  static_cast<uint32_t>(i % 100'000'000ull);
    buffer = EncodeFullU32(div08, buffer);
  } else {
    uint64_t div08 = i / 100'000'000ull;
    mod08 =  static_cast<uint32_t>(i % 100'000'000ull);
    uint32_t div016 = static_cast<uint32_t>(div08 / 100'000'000ull);
    uint32_t div08mod08 = static_cast<uint32_t>(div08 % 100'000'000ull);
    uint64_t mid_result = PrepareEightDigits(div08mod08) + kEightZeroBytes;
    buffer = EncodeTenThousand(div016, buffer);
    little_endian::Store64(buffer, mid_result);
    buffer += sizeof(mid_result);
  }
  uint64_t mod_result = PrepareEightDigits(mod08) + kEightZeroBytes;
  little_endian::Store64(buffer, mod_result);
  return buffer + sizeof(mod_result);
}

}  // namespace

void numbers_internal::PutTwoDigits(uint32_t i, char* absl_nonnull buf) {
  assert(i < 100);
  uint32_t base = kTwoZeroBytes;
  uint32_t div10 = (i * kDivisionBy10Mul) / kDivisionBy10Div;
  uint32_t mod10 = i - 10u * div10;
  base += div10 + (mod10 << 8);
  little_endian::Store16(buf, static_cast<uint16_t>(base));
}

char* absl_nonnull numbers_internal::FastIntToBuffer(
    uint32_t n, char* absl_nonnull out_str) {
  out_str = EncodeFullU32(n, out_str);
  *out_str = '\0';
  return out_str;
}

char* absl_nonnull numbers_internal::FastIntToBuffer(
    int32_t i, char* absl_nonnull buffer) {
  uint32_t u = static_cast<uint32_t>(i);
  if (i < 0) {
    *buffer++ = '-';
    // We need to do the negation in modular (i.e., "unsigned")
    // arithmetic; MSVC++ apparently warns for plain "-u", so
    // we write the equivalent expression "0 - u" instead.
    u = 0 - u;
  }
  buffer = EncodeFullU32(u, buffer);
  *buffer = '\0';
  return buffer;
}

char* absl_nonnull numbers_internal::FastIntToBuffer(
    uint64_t i, char* absl_nonnull buffer) {
  buffer = EncodeFullU64(i, buffer);
  *buffer = '\0';
  return buffer;
}

char* absl_nonnull numbers_internal::FastIntToBuffer(
    int64_t i, char* absl_nonnull buffer) {
  uint64_t u = static_cast<uint64_t>(i);
  if (i < 0) {
    *buffer++ = '-';
    // We need to do the negation in modular (i.e., "unsigned")
    // arithmetic; MSVC++ apparently warns for plain "-u", so
    // we write the equivalent expression "0 - u" instead.
    u = 0 - u;
  }
  buffer = EncodeFullU64(u, buffer);
  *buffer = '\0';
  return buffer;
}

// Given a 128-bit number expressed as a pair of uint64_t, high half first,
// return that number multiplied by the given 32-bit value.  If the result is
// too large to fit in a 128-bit number, divide it by 2 until it fits.
static std::pair<uint64_t, uint64_t> Mul32(std::pair<uint64_t, uint64_t> num,
                                           uint32_t mul) {
  uint64_t bits0_31 = num.second & 0xFFFFFFFF;
  uint64_t bits32_63 = num.second >> 32;
  uint64_t bits64_95 = num.first & 0xFFFFFFFF;
  uint64_t bits96_127 = num.first >> 32;

  // The picture so far: each of these 64-bit values has only the lower 32 bits
  // filled in.
  // bits96_127:          [ 00000000 xxxxxxxx ]
  // bits64_95:                    [ 00000000 xxxxxxxx ]
  // bits32_63:                             [ 00000000 xxxxxxxx ]
  // bits0_31:                                       [ 00000000 xxxxxxxx ]

  bits0_31 *= mul;
  bits32_63 *= mul;
  bits64_95 *= mul;
  bits96_127 *= mul;

  // Now the top halves may also have value, though all 64 of their bits will
  // never be set at the same time, since they are a result of a 32x32 bit
  // multiply.  This makes the carry calculation slightly easier.
  // bits96_127:          [ mmmmmmmm | mmmmmmmm ]
  // bits64_95:                    [ | mmmmmmmm mmmmmmmm | ]
  // bits32_63:                      |        [ mmmmmmmm | mmmmmmmm ]
  // bits0_31:                       |                 [ | mmmmmmmm mmmmmmmm ]
  // eventually:        [ bits128_up | ...bits64_127.... | ..bits0_63... ]

  uint64_t bits0_63 = bits0_31 + (bits32_63 << 32);
  uint64_t bits64_127 = bits64_95 + (bits96_127 << 32) + (bits32_63 >> 32) +
                        (bits0_63 < bits0_31);
  uint64_t bits128_up = (bits96_127 >> 32) + (bits64_127 < bits64_95);
  if (bits128_up == 0) return {bits64_127, bits0_63};

  auto shift = static_cast<unsigned>(bit_width(bits128_up));
  uint64_t lo = (bits0_63 >> shift) + (bits64_127 << (64 - shift));
  uint64_t hi = (bits64_127 >> shift) + (bits128_up << (64 - shift));
  return {hi, lo};
}

// Compute num * 5 ^ expfive, and return the first 128 bits of the result,
// where the first bit is always a one.  So PowFive(1, 0) starts 0b100000,
// PowFive(1, 1) starts 0b101000, PowFive(1, 2) starts 0b110010, etc.
static std::pair<uint64_t, uint64_t> PowFive(uint64_t num, int expfive) {
  std::pair<uint64_t, uint64_t> result = {num, 0};
  while (expfive >= 13) {
    // 5^13 is the highest power of five that will fit in a 32-bit integer.
    result = Mul32(result, 5 * 5 * 5 * 5 * 5 * 5 * 5 * 5 * 5 * 5 * 5 * 5 * 5);
    expfive -= 13;
  }
  constexpr uint32_t powers_of_five[13] = {
      1,
      5,
      5 * 5,
      5 * 5 * 5,
      5 * 5 * 5 * 5,
      5 * 5 * 5 * 5 * 5,
      5 * 5 * 5 * 5 * 5 * 5,
      5 * 5 * 5 * 5 * 5 * 5 * 5,
      5 * 5 * 5 * 5 * 5 * 5 * 5 * 5,
      5 * 5 * 5 * 5 * 5 * 5 * 5 * 5 * 5,
      5 * 5 * 5 * 5 * 5 * 5 * 5 * 5 * 5 * 5,
      5 * 5 * 5 * 5 * 5 * 5 * 5 * 5 * 5 * 5 * 5,
      5 * 5 * 5 * 5 * 5 * 5 * 5 * 5 * 5 * 5 * 5 * 5};
  result = Mul32(result, powers_of_five[expfive & 15]);
  int shift = countl_zero(result.first);
  if (shift != 0) {
    result.first = (result.first << shift) + (result.second >> (64 - shift));
    result.second = (result.second << shift);
  }
  return result;
}

struct ExpDigits {
  int32_t exponent;
  char digits[6];
};

// SplitToSix converts value, a positive double-precision floating-point number,
// into a base-10 exponent and 6 ASCII digits, where the first digit is never
// zero.  For example, SplitToSix(1) returns an exponent of zero and a digits
// array of {'1', '0', '0', '0', '0', '0'}.  If value is exactly halfway between
// two possible representations, e.g. value = 100000.5, then "round to even" is
// performed.
static ExpDigits SplitToSix(const double value) {
  ExpDigits exp_dig;
  int exp = 5;
  double d = value;
  // First step: calculate a close approximation of the output, where the
  // value d will be between 100,000 and 999,999, representing the digits
  // in the output ASCII array, and exp is the base-10 exponent.  It would be
  // faster to use a table here, and to look up the base-2 exponent of value,
  // however value is an IEEE-754 64-bit number, so the table would have 2,000
  // entries, which is not cache-friendly.
  if (d >= 999999.5) {
    if (d >= 1e+261) exp += 256, d *= 1e-256;
    if (d >= 1e+133) exp += 128, d *= 1e-128;
    if (d >= 1e+69) exp += 64, d *= 1e-64;
    if (d >= 1e+37) exp += 32, d *= 1e-32;
    if (d >= 1e+21) exp += 16, d *= 1e-16;
    if (d >= 1e+13) exp += 8, d *= 1e-8;
    if (d >= 1e+9) exp += 4, d *= 1e-4;
    if (d >= 1e+7) exp += 2, d *= 1e-2;
    if (d >= 1e+6) exp += 1, d *= 1e-1;
  } else {
    if (d < 1e-250) exp -= 256, d *= 1e256;
    if (d < 1e-122) exp -= 128, d *= 1e128;
    if (d < 1e-58) exp -= 64, d *= 1e64;
    if (d < 1e-26) exp -= 32, d *= 1e32;
    if (d < 1e-10) exp -= 16, d *= 1e16;
    if (d < 1e-2) exp -= 8, d *= 1e8;
    if (d < 1e+2) exp -= 4, d *= 1e4;
    if (d < 1e+4) exp -= 2, d *= 1e2;
    if (d < 1e+5) exp -= 1, d *= 1e1;
  }
  // At this point, d is in the range [99999.5..999999.5) and exp is in the
  // range [-324..308]. Since we need to round d up, we want to add a half
  // and truncate.
  // However, the technique above may have lost some precision, due to its
  // repeated multiplication by constants that each may be off by half a bit
  // of precision.  This only matters if we're close to the edge though.
  // Since we'd like to know if the fractional part of d is close to a half,
  // we multiply it by 65536 and see if the fractional part is close to 32768.
  // (The number doesn't have to be a power of two,but powers of two are faster)
  uint64_t d64k = static_cast<uint64_t>(d * 65536);
  uint32_t dddddd;  // A 6-digit decimal integer.
  if ((d64k % 65536) == 32767 || (d64k % 65536) == 32768) {
    // OK, it's fairly likely that precision was lost above, which is
    // not a surprise given only 52 mantissa bits are available.  Therefore
    // redo the calculation using 128-bit numbers.  (64 bits are not enough).

    // Start out with digits rounded down; maybe add one below.
    dddddd = static_cast<uint32_t>(d64k / 65536);

    // mantissa is a 64-bit integer representing M.mmm... * 2^63.  The actual
    // value we're representing, of course, is M.mmm... * 2^exp2.
    int exp2;
    double m = std::frexp(value, &exp2);
    uint64_t mantissa =
        static_cast<uint64_t>(m * (32768.0 * 65536.0 * 65536.0 * 65536.0));
    // std::frexp returns an m value in the range [0.5, 1.0), however we
    // can't multiply it by 2^64 and convert to an integer because some FPUs
    // throw an exception when converting an number higher than 2^63 into an
    // integer - even an unsigned 64-bit integer!  Fortunately it doesn't matter
    // since m only has 52 significant bits anyway.
    mantissa <<= 1;
    exp2 -= 64;  // not needed, but nice for debugging

    // OK, we are here to compare:
    //     (dddddd + 0.5) * 10^(exp-5)  vs.  mantissa * 2^exp2
    // so we can round up dddddd if appropriate.  Those values span the full
    // range of 600 orders of magnitude of IEE 64-bit floating-point.
    // Fortunately, we already know they are very close, so we don't need to
    // track the base-2 exponent of both sides.  This greatly simplifies the
    // the math since the 2^exp2 calculation is unnecessary and the power-of-10
    // calculation can become a power-of-5 instead.

    std::pair<uint64_t, uint64_t> edge, val;
    if (exp >= 6) {
      // Compare (dddddd + 0.5) * 5 ^ (exp - 5) to mantissa
      // Since we're tossing powers of two, 2 * dddddd + 1 is the
      // same as dddddd + 0.5
      edge = PowFive(2 * dddddd + 1, exp - 5);

      val.first = mantissa;
      val.second = 0;
    } else {
      // We can't compare (dddddd + 0.5) * 5 ^ (exp - 5) to mantissa as we did
      // above because (exp - 5) is negative.  So we compare (dddddd + 0.5) to
      // mantissa * 5 ^ (5 - exp)
      edge = PowFive(2 * dddddd + 1, 0);

      val = PowFive(mantissa, 5 - exp);
    }
    // printf("exp=%d %016lx %016lx vs %016lx %016lx\n", exp, val.first,
    //        val.second, edge.first, edge.second);
    if (val > edge) {
      dddddd++;
    } else if (val == edge) {
      dddddd += (dddddd & 1);
    }
  } else {
    // Here, we are not close to the edge.
    dddddd = static_cast<uint32_t>((d64k + 32768) / 65536);
  }
  if (dddddd == 1000000) {
    dddddd = 100000;
    exp += 1;
  }
  exp_dig.exponent = exp;

  uint32_t two_digits = dddddd / 10000;
  dddddd -= two_digits * 10000;
  numbers_internal::PutTwoDigits(two_digits, &exp_dig.digits[0]);

  two_digits = dddddd / 100;
  dddddd -= two_digits * 100;
  numbers_internal::PutTwoDigits(two_digits, &exp_dig.digits[2]);

  numbers_internal::PutTwoDigits(dddddd, &exp_dig.digits[4]);
  return exp_dig;
}

// Helper function for fast formatting of floating-point.
// The result is the same as "%g", a.k.a. "%.6g".
size_t numbers_internal::SixDigitsToBuffer(double d,
                                           char* absl_nonnull const buffer) {
  static_assert(std::numeric_limits<float>::is_iec559,
                "IEEE-754/IEC-559 support only");

  char* out = buffer;  // we write data to out, incrementing as we go, but
                       // FloatToBuffer always returns the address of the buffer
                       // passed in.

  if (std::isnan(d)) {
    strcpy(out, "nan");  // NOLINT(runtime/printf)
    return 3;
  }
  if (d == 0) {  // +0 and -0 are handled here
    if (std::signbit(d)) *out++ = '-';
    *out++ = '0';
    *out = 0;
    return static_cast<size_t>(out - buffer);
  }
  if (d < 0) {
    *out++ = '-';
    d = -d;
  }
  if (d > std::numeric_limits<double>::max()) {
    strcpy(out, "inf");  // NOLINT(runtime/printf)
    return static_cast<size_t>(out + 3 - buffer);
  }

  auto exp_dig = SplitToSix(d);
  int exp = exp_dig.exponent;
  const char* digits = exp_dig.digits;
  out[0] = '0';
  out[1] = '.';
  switch (exp) {
    case 5:
      memcpy(out, &digits[0], 6), out += 6;
      *out = 0;
      return static_cast<size_t>(out - buffer);
    case 4:
      memcpy(out, &digits[0], 5), out += 5;
      if (digits[5] != '0') {
        *out++ = '.';
        *out++ = digits[5];
      }
      *out = 0;
      return static_cast<size_t>(out - buffer);
    case 3:
      memcpy(out, &digits[0], 4), out += 4;
      if ((digits[5] | digits[4]) != '0') {
        *out++ = '.';
        *out++ = digits[4];
        if (digits[5] != '0') *out++ = digits[5];
      }
      *out = 0;
      return static_cast<size_t>(out - buffer);
    case 2:
      memcpy(out, &digits[0], 3), out += 3;
      *out++ = '.';
      memcpy(out, &digits[3], 3);
      out += 3;
      while (out[-1] == '0') --out;
      if (out[-1] == '.') --out;
      *out = 0;
      return static_cast<size_t>(out - buffer);
    case 1:
      memcpy(out, &digits[0], 2), out += 2;
      *out++ = '.';
      memcpy(out, &digits[2], 4);
      out += 4;
      while (out[-1] == '0') --out;
      if (out[-1] == '.') --out;
      *out = 0;
      return static_cast<size_t>(out - buffer);
    case 0:
      memcpy(out, &digits[0], 1), out += 1;
      *out++ = '.';
      memcpy(out, &digits[1], 5);
      out += 5;
      while (out[-1] == '0') --out;
      if (out[-1] == '.') --out;
      *out = 0;
      return static_cast<size_t>(out - buffer);
    case -4:
      out[2] = '0';
      ++out;
      ABSL_FALLTHROUGH_INTENDED;
    case -3:
      out[2] = '0';
      ++out;
      ABSL_FALLTHROUGH_INTENDED;
    case -2:
      out[2] = '0';
      ++out;
      ABSL_FALLTHROUGH_INTENDED;
    case -1:
      out += 2;
      memcpy(out, &digits[0], 6);
      out += 6;
      while (out[-1] == '0') --out;
      *out = 0;
      return static_cast<size_t>(out - buffer);
  }
  assert(exp < -4 || exp >= 6);
  out[0] = digits[0];
  assert(out[1] == '.');
  out += 2;
  memcpy(out, &digits[1], 5), out += 5;
  while (out[-1] == '0') --out;
  if (out[-1] == '.') --out;
  *out++ = 'e';
  if (exp > 0) {
    *out++ = '+';
  } else {
    *out++ = '-';
    exp = -exp;
  }
  if (exp > 99) {
    int dig1 = exp / 100;
    exp -= dig1 * 100;
    *out++ = '0' + static_cast<char>(dig1);
  }
  PutTwoDigits(static_cast<uint32_t>(exp), out);
  out += 2;
  *out = 0;
  return static_cast<size_t>(out - buffer);
}

namespace numbers_internal {

// Digit conversion.
ABSL_CONST_INIT ABSL_DLL const char kHexChar[] =
    "0123456789abcdef";

ABSL_CONST_INIT ABSL_DLL const char kHexTable[513] =
    "000102030405060708090a0b0c0d0e0f"
    "101112131415161718191a1b1c1d1e1f"
    "202122232425262728292a2b2c2d2e2f"
    "303132333435363738393a3b3c3d3e3f"
    "404142434445464748494a4b4c4d4e4f"
    "505152535455565758595a5b5c5d5e5f"
    "606162636465666768696a6b6c6d6e6f"
    "707172737475767778797a7b7c7d7e7f"
    "808182838485868788898a8b8c8d8e8f"
    "909192939495969798999a9b9c9d9e9f"
    "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf"
    "b0b1b2b3b4b5b6b7b8b9babbbcbdbebf"
    "c0c1c2c3c4c5c6c7c8c9cacbcccdcecf"
    "d0d1d2d3d4d5d6d7d8d9dadbdcdddedf"
    "e0e1e2e3e4e5e6e7e8e9eaebecedeeef"
    "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff";

}  // namespace numbers_internal
ABSL_NAMESPACE_END
}  // namespace absl
