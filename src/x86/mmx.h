#pragma once
// MMX for lifted code (and the MMX-register forms of SSE and SSE2 integer
// instructions). Plain portable integer code: the same bits on every host.
//
// The eight MMX registers are the x87 physical registers R0-R7: MMi is
// the 64-bit significand of Ctx::fpu.r[i], as on the hardware, so x87
// code, FXSAVE/FXRSTOR and FNSAVE/FRSTOR see what MMX code left there.
// - Every MMX instruction but EMMS sets TOP to 0 and tags every register
//   valid (Enter).
// - Writing MMi also sets R_i's sign and exponent to all ones (Set).
// - EMMS tags every register empty and leaves TOP alone.
// x87::State keeps only "empty" per register and derives the other tags
// from the contents (x87.h), so FNSTENV/FNSAVE after MMX code report a
// written register as special (exponent all ones), as current CPUs do;
// the original Pentium MMX stored tag 00 (valid).
//
// Not modelled: #MF for a pending unmasked x87 exception (masked modes
// are assumed throughout, see x87.h) and CR0.EM/TS (#UD/#NM).
//
// tests/x86/mmx_test checks every helper against the host CPU's MMX
// instructions bit for bit.

#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#include "x86/ctx.h"

namespace recomp::mmx {

// ---- state -----------------------------------------------------------------

inline void Enter(Ctx& c) {
  c.fpu.top = 0;
  c.fpu.empty = 0;
}
inline void Set(Ctx& c, int i, uint64_t v) {
  c.fpu.r[i].sig = v;
  c.fpu.r[i].se = 0xFFFF;
}
inline void Emms(Ctx& c) { c.fpu.empty = 0xFF; }

// ---- lanes -----------------------------------------------------------------
// A register as 8 / sizeof(T) lanes of T, lane 0 in the low bits.

template <typename T>
inline T Lane(uint64_t v, int i) {
  using U = std::make_unsigned_t<T>;
  return T(U(v >> (8 * sizeof(T) * i)));
}
template <typename T>
inline uint64_t Put(uint64_t r, int i, T x) {
  using U = std::make_unsigned_t<T>;
  constexpr int kBits = int(8 * sizeof(T));
  const uint64_t mask = kBits == 64 ? ~uint64_t(0) : (uint64_t(1) << kBits) - 1;
  const int sh = kBits * i;
  return (r & ~(mask << sh)) | (uint64_t(U(x)) & mask) << sh;
}
template <typename T>
inline constexpr int kLanes = int(8 / sizeof(T));

// Saturates a wider value to T's range.
template <typename T>
inline T Sat(int64_t v) {
  constexpr int64_t lo = std::numeric_limits<T>::min(), hi = std::numeric_limits<T>::max();
  return T(v < lo ? lo : v > hi ? hi : v);
}

// ---- arithmetic --------------------------------------------------------------

// PADDB/W/D/Q, PSUBB/W/D/Q (wrapping).
template <typename T>
inline uint64_t Add(uint64_t d, uint64_t s) {
  uint64_t r = 0;
  for (int i = 0; i < kLanes<T>; i++) r = Put<T>(r, i, T(Lane<T>(d, i) + Lane<T>(s, i)));
  return r;
}
template <typename T>
inline uint64_t Sub(uint64_t d, uint64_t s) {
  uint64_t r = 0;
  for (int i = 0; i < kLanes<T>; i++) r = Put<T>(r, i, T(Lane<T>(d, i) - Lane<T>(s, i)));
  return r;
}
// PADDSB/SW, PADDUSB/USW, PSUBSB/SW, PSUBUSB/USW: saturated to T.
template <typename T>
inline uint64_t AddSat(uint64_t d, uint64_t s) {
  uint64_t r = 0;
  for (int i = 0; i < kLanes<T>; i++)
    r = Put<T>(r, i, Sat<T>(int64_t(Lane<T>(d, i)) + int64_t(Lane<T>(s, i))));
  return r;
}
template <typename T>
inline uint64_t SubSat(uint64_t d, uint64_t s) {
  uint64_t r = 0;
  for (int i = 0; i < kLanes<T>; i++)
    r = Put<T>(r, i, Sat<T>(int64_t(Lane<T>(d, i)) - int64_t(Lane<T>(s, i))));
  return r;
}
// PCMPEQB/W/D: equal lanes -> all ones. PCMPGTB/W/D: signed greater.
template <typename T>
inline uint64_t CmpEq(uint64_t d, uint64_t s) {
  uint64_t r = 0;
  for (int i = 0; i < kLanes<T>; i++)
    r = Put<T>(r, i, Lane<T>(d, i) == Lane<T>(s, i) ? T(~T(0)) : T(0));
  return r;
}
template <typename T>
inline uint64_t CmpGt(uint64_t d, uint64_t s) {
  uint64_t r = 0;
  for (int i = 0; i < kLanes<T>; i++)
    r = Put<T>(r, i, Lane<T>(d, i) > Lane<T>(s, i) ? T(-1) : T(0));
  return r;
}
// PMULLW: the low word of each product.
inline uint64_t MulLo16(uint64_t d, uint64_t s) {
  uint64_t r = 0;
  for (int i = 0; i < 4; i++)
    r = Put<uint16_t>(r, i, uint16_t(uint32_t(Lane<uint16_t>(d, i)) * Lane<uint16_t>(s, i)));
  return r;
}
// PMULHW (T = int16_t) / PMULHUW (uint16_t): the high word of each product.
template <typename T>
inline uint64_t MulHi16(uint64_t d, uint64_t s) {
  using P = std::conditional_t<std::is_signed_v<T>, int32_t, uint32_t>;
  uint64_t r = 0;
  for (int i = 0; i < 4; i++)
    r = Put<uint16_t>(r, i, uint16_t(uint32_t(P(Lane<T>(d, i)) * P(Lane<T>(s, i))) >> 16));
  return r;
}
// PMADDWD: adjacent signed word products summed into dwords. The one
// overflow, 0x8000 * 0x8000 twice, wraps to 0x80000000 as on the hardware.
inline uint64_t MAddWd(uint64_t d, uint64_t s) {
  uint64_t r = 0;
  for (int i = 0; i < 2; i++) {
    uint32_t a = uint32_t(int32_t(Lane<int16_t>(d, 2 * i)) * Lane<int16_t>(s, 2 * i));
    uint32_t b = uint32_t(int32_t(Lane<int16_t>(d, 2 * i + 1)) * Lane<int16_t>(s, 2 * i + 1));
    r = Put<uint32_t>(r, i, a + b);
  }
  return r;
}
// PMULUDQ (SSE2): the low dwords multiplied into a quadword.
inline uint64_t MulUDq(uint64_t d, uint64_t s) {
  return uint64_t(uint32_t(d)) * uint32_t(s);
}
// PAVGB/W (SSE): unsigned (a + b + 1) / 2.
template <typename T>
inline uint64_t Avg(uint64_t d, uint64_t s) {
  uint64_t r = 0;
  for (int i = 0; i < kLanes<T>; i++)
    r = Put<T>(r, i, T((uint32_t(Lane<T>(d, i)) + Lane<T>(s, i) + 1) >> 1));
  return r;
}
// PMAXUB/PMAXSW, PMINUB/PMINSW (SSE): by T's order.
template <typename T>
inline uint64_t Max(uint64_t d, uint64_t s) {
  uint64_t r = 0;
  for (int i = 0; i < kLanes<T>; i++) {
    T a = Lane<T>(d, i), b = Lane<T>(s, i);
    r = Put<T>(r, i, a > b ? a : b);
  }
  return r;
}
template <typename T>
inline uint64_t Min(uint64_t d, uint64_t s) {
  uint64_t r = 0;
  for (int i = 0; i < kLanes<T>; i++) {
    T a = Lane<T>(d, i), b = Lane<T>(s, i);
    r = Put<T>(r, i, a < b ? a : b);
  }
  return r;
}
// PSADBW (SSE): the sum of the absolute byte differences in the low word,
// the rest zero.
inline uint64_t SadBw(uint64_t d, uint64_t s) {
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) {
    int a = Lane<uint8_t>(d, i), b = Lane<uint8_t>(s, i);
    sum += uint32_t(a > b ? a - b : b - a);
  }
  return sum;
}

// ---- shifts ----------------------------------------------------------------

// PSLLW/D/Q, PSRLW/D/Q, PSRAW/D: every lane by `count` (the immediate, or
// the whole 64-bit source). Counts past the lane width clear the lane
// (logical) or fill it with its sign.
enum ShiftKind : uint8_t { kShl, kShr, kSar };
template <typename T>
inline uint64_t Shift(uint64_t d, uint64_t count, ShiftKind k) {
  using S = std::make_signed_t<T>;
  constexpr uint64_t kBits = sizeof(T) * 8;
  uint64_t r = 0;
  for (int i = 0; i < kLanes<T>; i++) {
    T v = Lane<T>(d, i);
    if (k == kSar)
      v = T(S(v) >> (count >= kBits ? kBits - 1 : count));
    else if (count >= kBits)
      v = 0;
    else
      v = k == kShl ? T(v << count) : T(v >> count);
    r = Put<T>(r, i, v);
  }
  return r;
}

// ---- data movement -----------------------------------------------------------

// PUNPCKLBW/WD/DQ (high: PUNPCKH..): the low (high) halves of d and s
// interleaved, d's lane first.
template <typename T>
inline uint64_t Unpck(uint64_t d, uint64_t s, bool high) {
  constexpr int n = kLanes<T>;
  const int b = high ? n / 2 : 0;
  uint64_t r = 0;
  for (int i = 0; i < n / 2; i++) {
    r = Put<T>(r, 2 * i, Lane<T>(d, b + i));
    r = Put<T>(r, 2 * i + 1, Lane<T>(s, b + i));
  }
  return r;
}
// PACKSSWB / PACKSSDW / PACKUSWB: d's lanes then s's, narrowed with
// saturation (signed From to To).
template <typename From, typename To>
inline uint64_t Pack(uint64_t d, uint64_t s) {
  constexpr int n = kLanes<From>;
  uint64_t r = 0;
  for (int i = 0; i < n; i++) r = Put<To>(r, i, Sat<To>(Lane<From>(d, i)));
  for (int i = 0; i < n; i++) r = Put<To>(r, n + i, Sat<To>(Lane<From>(s, i)));
  return r;
}
// PSHUFW (SSE): word i = the word of s that imm's bits 2i+1:2i pick.
inline uint64_t ShufW(uint64_t s, uint8_t imm) {
  uint64_t r = 0;
  for (int i = 0; i < 4; i++) r = Put<uint16_t>(r, i, Lane<uint16_t>(s, (imm >> (2 * i)) & 3));
  return r;
}
// PEXTRW / PINSRW (SSE): word imm & 3.
inline uint32_t ExtrW(uint64_t s, uint8_t imm) { return Lane<uint16_t>(s, imm & 3); }
inline uint64_t InsrW(uint64_t d, uint32_t v, uint8_t imm) {
  return Put<uint16_t>(d, imm & 3, uint16_t(v));
}
// PMOVMSKB (SSE): the top bit of each byte.
inline uint32_t MovMskB(uint64_t s) {
  uint32_t m = 0;
  for (int i = 0; i < 8; i++) m |= uint32_t(Lane<uint8_t>(s, i) >> 7) << i;
  return m;
}
// MASKMOVQ (SSE): the bytes of d whose byte in mask has its top bit set,
// stored to guest memory at a (a byte at a time, as the instruction may).
template <typename A>
inline void MaskMov(uint8_t* B, A a, uint64_t d, uint64_t mask) {
  for (int i = 0; i < 8; i++)
    if (Lane<uint8_t>(mask, i) >> 7) B[A(a + A(i))] = Lane<uint8_t>(d, i);
}

}  // namespace recomp::mmx
