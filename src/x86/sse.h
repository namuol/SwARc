#pragma once
// SSE/SSE2 for lifted code. The XMM registers and MXCSR live in Ctx and
// are used in place. Floating-point lanes are computed with SoftFloat,
// with the SSE NaN rules applied here (the shared SoftFloat build uses the
// x87 ones), so results and MXCSR flags match the hardware bit for bit on
// any host. Integer and data-movement operations are plain portable C++.
//
// FTZ flushes denormal results to zero, DAZ denormal operands. Not
// modelled, fatal if reached: unmasked SIMD exceptions (#XM).
//
// RSQRTPS and RCPPS have no architectural result, only an error bound
// (|rel. error| <= 1.5 * 2^-12), and CPUs differ: here they are the
// correctly rounded 1/sqrt(x) and 1/x, within the bound and the same on
// every host (and what a native replacement computes).
// tests/x86/sse_rsqrt_test checks them; the bit-for-bit oracle against
// the host skips them.
//
// Only the forms the supported games use are implemented; the lifter
// reports the rest as unimplemented.

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#include "x86/ctx.h"

// RECOMP_X86_MUTATION=<n> plants a known bug in a fast path (sse.cpp) so
// tests/x86/sse_fast_test can show it catches it; never set it elsewhere.
#ifndef RECOMP_X86_MUTATION
#define RECOMP_X86_MUTATION 0
#endif

namespace recomp::sse {

inline constexpr uint32_t kMxcsrDefault = 0x1F80;  // all masked, RN

// Out-of-line slow paths (and CheckAligned's fatal check) keep the
// caller's general registers (clang's preserve_most): hot lifted float
// code, whose XMM lanes live in them, keeps its values in registers across
// each possible call instead of saving them around it. Declarations and
// definitions carry it alike. (preserve_all, which would keep the vector
// registers too, broke sse_fast_test on arm64.)
#if defined(__clang__)
#define RECOMP_SSE_SLOW __attribute__((preserve_most))
#else
#define RECOMP_SSE_SLOW
#endif



template <typename T>
inline T Lane(const Xmm& x, int i) {
  T v;
  std::memcpy(&v, x.b + i * sizeof(T), sizeof(T));
  return v;
}
template <typename T>
inline void SetLane(Xmm& x, int i, T v) {
  std::memcpy(x.b + i * sizeof(T), &v, sizeof(T));
}

// `bytes` of guest memory at a, zero-extended to a register.
inline Xmm Ld(const uint8_t* B, uint64_t a, int bytes) {
  Xmm x{};
  std::memcpy(x.b, B + a, bytes);
  return x;
}
inline void St(uint8_t* B, uint64_t a, const Xmm& x, int bytes) {
  std::memcpy(B + a, x.b, bytes);
}
// Legacy-SSE m128 operands other than movups/movupd/movdqu must be 16-byte
// aligned (#GP otherwise): fatal, naming the instruction.
// The check inline, the fatal error out of line.
[[noreturn]] RECOMP_SSE_SLOW void Misaligned(uint64_t a, uint64_t va, unsigned align);
inline void CheckAligned(Ctx& c, uint64_t a, uint64_t va) {
  (void)c;
  if (a & 15) [[unlikely]] Misaligned(a, va, 16);
}
// The same for VEX moves that require `align` (16 or 32) bytes.
inline void CheckAligned(Ctx& c, uint64_t a, uint64_t va, unsigned align) {
  (void)c;
  if (a & (align - 1)) [[unlikely]] Misaligned(a, va, align);
}

// ---- floating point ---------------------------------------------------
enum Op : uint8_t { kAdd, kSub, kMul, kDiv };

// RECOMP_SSE_FLAGS_UNOBSERVED=1: a game's build may set it for its lifted
// code when it has shown that no guest code's behaviour depends on
// MXCSR's status flags (bits 0-5): every STMXCSR, FXSAVE and XSAVE in the
// binary either masks them off, only stores them to be loaded back
// unchanged, or runs only while handling an unmasked float exception
// (impossible under condition (1) below). The record of that check
// belongs with the game. Then ADD/SUB/MUL/DIV take the fast path for
// every operand under condition (1), but a NaN result: IEEE 754 defines
// all of them as the correctly rounded result, and with the host in its
// default environment (no flush to zero) and MXCSR the default (no FTZ,
// no DAZ) the host's result has the same bits as SSE's, signed zeros,
// infinities and denormals included. Only NaNs differ (x86's default NaN
// is negative, ARM's positive; operand NaN propagation rules differ), so
// a NaN result takes the exact model. The status flags are left as they
// were (wrong, but unobserved by that check).
#ifndef RECOMP_SSE_FLAGS_UNOBSERVED
#define RECOMP_SSE_FLAGS_UNOBSERVED 0
#endif

namespace fast {

constexpr bool Mutated(int n) { return RECOMP_X86_MUTATION == n; }

// ---- host fast paths (inline in lifted code) ------------------------------
//
// ADD/SUB/MUL/DIV (PS/PD/SS/SD): the common case is computed with host float/double arithmetic, which
// gives the same bits as x86 there, and everything else falls back to the
// exact model (ArithExact, sse.cpp). Conditions (checked per instruction and per lane):
//
//   (1) MXCSR is the default but for its flags: every exception masked,
//       round to nearest even, FTZ and DAZ off ((mxcsr & 0xFFC0) == 0x1F80).
//       FastArith also takes FTZ and DAZ on ((mxcsr & 0x7F80) == 0x1F80):
//       (2) and (3) keep denormals out of its operands and results.
//       The host runs in its default environment too (round to nearest
//       even, no flush to zero, no traps: platform::HostFpProblem, checked
//       on every guest thread), and this file is built with
//       RECOMP_X87_FP_FLAGS (no contraction into FMA, no fast-math).
//   (2) Each operand is a zero or "in range": 2^(emin+2p) <= |x| < 2^emax,
//       i.e. 2^-78 <= |x| < 2^127 (float), 2^-916 <= |x| < 2^1023 (double).
//       Excluded: NaN, infinity, denormal and tiny or huge operands.
//   (3) The host result r is in range, or a zero that is exact: any zero of
//       ADD/SUB (x + y rounds to zero only when it is exactly zero: a
//       nonzero exact sum of in-range operands is a multiple of the smaller
//       one's ulp, at least 2^(emin+p+1), far above the denormals), a zero
//       of MUL with a zero operand, of DIV with a zero dividend.
//
// Then the result: IEEE 754 defines +, -, *, / as the correctly rounded
// exact result, and SSE's are IEEE's in round to nearest even; signed
// zeros follow the same rules (x - x = +0, (-0) + (-0) = -0, the sign of a
// product or quotient is the XOR). The host computes the same.
//
// And the flags: no invalid (operands are finite, and 0/0 and inf-inf
// cannot arise: 0/0 gives a NaN result, rejected by (3)); no denormal
// operand (2); no divide-by-zero (x/0 gives inf or NaN, rejected); no
// overflow (r is finite: a rounded result beyond the largest finite value
// is inf in round to nearest); no underflow (r is normal, and x86 detects
// tininess after rounding; the zeros in (3) are exact). Only inexact
// remains, set iff the rounding changed the value, which is computed
// exactly with error-free transformations:
//
//   ADD/SUB: TwoSum (Knuth) gives e = (x + y) - r exactly in round to
//     nearest for any finite operands without overflow; |x|, |y| < 2^emax
//     keeps every intermediate finite. Inexact iff e != 0.
//   MUL: e = fma(x, y, -r) = xy - r exactly when that is representable.
//     xy is a multiple of q = ulp(x) ulp(y) = 2^(Ex+Ey-2p+2); r has
//     exponent Er <= Ex+Ey+1 and |e| <= ulp(r)/2 = 2^(Er-p), so e needs at
//     most p bits; and Er >= emin+2p (3) gives Ex+Ey >= emin+2p-1, so q >=
//     2^(emin+1) is normal and e is representable. Inexact iff e != 0.
//   DIV: e = fma(-r, y, x) = x - ry exactly when representable. x and ry
//     are multiples of 2^(Er+Ey-2p+2) (Ex >= Er+Ey); |e| = |y||x/y - r| <
//     2^(Ey+1) 2^(Er-p) needs at most p-1 bits; |x| >= 2^(emin+2p) (2)
//     gives Er+Ey >= emin+2p-1, so the quantum is normal. Inexact iff e !=
//     0 (e = 0 is x = ry, an exact quotient).
//
// PE is sticky: once set, only LDMXCSR (or XRSTOR/FXRSTOR) clears it, and
// no arithmetic does. So when it is already set the instruction leaves it
// set whatever it computes, and the error term is not computed at all.
//
// tests/x86/sse_fast_test runs random and edge-case operands through both
// paths and compares results and MXCSR bit for bit; each planted bug
// (RECOMP_X86_MUTATION 101-106) must make it fail. Inline here, so each
// call site (its op, format and lane count constants) compiles to a few
// host instructions; lifted code is built with RECOMP_X87_FP_FLAGS.

template <bool D>
struct Host;
template <>
struct Host<false> {
  using F = float;
  static constexpr int kShift = 23;
  // Biased exponents: 2^-78 = emin + 2p (-126 + 48), and below 2^127.
  static constexpr uint32_t kMinExp = 127 - 78, kMaxExp = 127 + 126;
};
template <>
struct Host<true> {
  using F = double;
  static constexpr int kShift = 52;
  // 2^-916 = emin + 2p (-1022 + 106), and below 2^1023.
  static constexpr uint64_t kMinExp = 1023 - 916, kMaxExp = 1023 + 1022;
};

template <bool D>
using Bits = std::conditional_t<D, uint64_t, uint32_t>;

template <bool D>
inline bool IsZeroBits(Bits<D> v) {
  return !(v & ~(Bits<D>(1) << (sizeof(Bits<D>) * 8 - 1)));
}

// Zero, or 2^(emin+2p) <= |v| < 2^emax (condition (2)).
template <bool D>
inline bool InRange(Bits<D> v) {
  using U = Bits<D>;
  if (IsZeroBits<D>(v)) return true;
  U e = (v >> Host<D>::kShift) & (D ? 0x7FF : 0xFF);
  U lo = Mutated(102) ? 1 : Host<D>::kMinExp;
  return e >= lo && e <= Host<D>::kMaxExp;
}

// One lane on the host: false (nothing written) outside the conditions.
// `inexact` null: PE is already set, so the error term is not needed.
template <bool D>
inline bool FastLane(Op op, Bits<D> a, Bits<D> b, Bits<D>* out, bool* inexact) {
  using U = Bits<D>;
  using F = typename Host<D>::F;
  if constexpr (RECOMP_SSE_FLAGS_UNOBSERVED) {
    const F x = std::bit_cast<F>(a), y = std::bit_cast<F>(b);
    // x - y is x + (-y) in IEEE 754, signed zeros included.
    const F r = op == kAdd ? x + y : op == kSub ? x - y : op == kMul ? x * y : x / y;
    if (r != r && !Mutated(111)) return false;  // NaN: the exact model's bits
    *out = std::bit_cast<U>(r);
    (void)inexact;  // flags unobserved: PE is not maintained
    return true;
  }
  if (!InRange<D>(a) || !InRange<D>(b)) return false;
  const F x = std::bit_cast<F>(a);
  F y = std::bit_cast<F>(b), r, e;
  const bool want_e = inexact != nullptr;
  e = 0;
  switch (op) {
    case kAdd:
    case kSub: {
      if (op == kSub) y = -y;  // x - y is x + (-y), signed zeros included
      r = x + y;
      if (want_e) {
        const F yv = r - x, xv = r - yv;
        e = Mutated(104) ? r - x - y : (x - xv) + (y - yv);
      }
      break;
    }
    case kMul:
      r = x * y;
      if (want_e) e = std::fma(x, y, -r);
      break;
    default:
      r = x / y;
      if (want_e) e = std::fma(-r, y, x);
      break;
  }
  const U rb = std::bit_cast<U>(r);
  if (IsZeroBits<D>(rb)) {
    // An exact zero only (3).
    if (op == kMul && !Mutated(103) && !IsZeroBits<D>(a) && !IsZeroBits<D>(b)) return false;
    if (op == kDiv && !IsZeroBits<D>(a)) return false;
  } else if (!InRange<D>(rb)) {
    return false;
  }
  *out = rb;
  if (inexact) *inexact = Mutated(101) ? false : e != 0;
  return true;
}

// The whole instruction on the host, or false (nothing changed) if any
// lane is outside the conditions.
template <bool D>
inline bool FastArith(Ctx& c, Op op, int lanes, Xmm& d, const Xmm& s) {
  using U = Bits<D>;
  // FTZ and DAZ may be set (0x8040 left out): (2) keeps denormal operands
  // out, so DAZ never applies, and (3) keeps tiny results out, so FTZ
  // never flushes and the flags are as without them. Not with the flags
  // unobserved: that path does not check (2) and (3).
  constexpr uint32_t kMask = RECOMP_SSE_FLAGS_UNOBSERVED ? 0xFFC0 : 0x7F80;
  if ((c.mxcsr & kMask) != kMxcsrDefault) return false;
  U r[16 / sizeof(U)];
  bool inexact = false;
  const bool pe_set = Mutated(106) || (c.mxcsr & 0x20) != 0;  // 106: always skipped
  for (int i = 0; i < lanes; i++) {
    bool ix = false;
    if (!FastLane<D>(op, Lane<U>(d, i), Lane<U>(s, i), &r[i], pe_set ? nullptr : &ix)) return false;
    inexact |= ix;
  }
  for (int i = 0; i < lanes; i++) SetLane<U>(d, i, r[i]);
  if (inexact) c.mxcsr |= 0x20;  // PE
  return true;
}

// COMISS/COMISD: neither operand a NaN nor a denormal, so the comparison
// raises no flag and DAZ changes nothing: the result alone, with no MXCSR
// bookkeeping. Exact by construction (integer ordering of the bits, no
// host floats): 0 greater, 1 less, 2 equal; -1 outside the condition.
template <bool D>
inline int FastComi(const Xmm& x, const Xmm& y) {
  using U = Bits<D>;
  constexpr U kSign = U(1) << (sizeof(U) * 8 - 1);
  constexpr U kExp = D ? 0x7FF0000000000000ull : 0x7F800000u;
  const U a = Lane<U>(x, 0), b = Lane<U>(y, 0);
  auto special = [&](U v) {  // NaN, or (unless planted bug 105) denormal
    const U e = v & kExp, f = v & ~(kSign | kExp);
    return (e == kExp && f) || (!Mutated(105) && !e && f);
  };
  if (special(a) || special(b)) return -1;
  if (!((a | b) & ~kSign)) return 2;  // zeros of either sign are equal
  const bool na = a & kSign, nb = b & kSign;
  if (na != nb) return na ? 1 : 0;
  if (a == b) return 2;
  return ((a < b) != na) ? 1 : 0;  // magnitude order flips for negatives
}


// ---- conversions (inline in lifted code) -----------------------------------
//
// Under condition (1) above (MXCSR the default but for its flags, FTZ and
// DAZ: see CvtMxcsr; the host in its default environment), checked per
// instruction, and per lane the conditions below; otherwise the exact
// model (the ...Exact functions).
//
//   int32 -> float (CVTDQ2PS, CVTSI2SS) and int32 -> double (CVTSI2SD): IEEE
//     754's conversion is the correctly rounded value in round to nearest
//     even, as SSE's is; the host's int -> float gives it. To double it is
//     exact. Inexact iff the float is not v: int64_t(r) != v, where r is an
//     integer with |r| <= 2^31, exactly convertible to int64. Nothing else
//     can arise (the result is a normal number or zero).
//   float/double -> int32 (CVT(T)SS2SI, CVT(T)SD2SI, CVT(T)PS2DQ,
//     CVT(T)PD2DQ): when |x| < 2^30 (no NaN or infinity: their exponent is
//     the largest), truncating or rounding to nearest even stays within
//     int32, so there is no invalid operation, and std::trunc and
//     std::nearbyint (the default environment rounds to nearest even) give
//     the result exactly. Inexact iff it differs from x (compared as
//     double: both exact there). Denormal operands give 0 and inexact here
//     too; these conversions raise no DE in the model (DAZ only, off by (1)).
//   float -> double (CVTSS2SD, CVTPS2PD): exact for any operand but NaNs
//     (quieting, IE for an SNaN) and denormals (DE), which are left out.
//   double -> float (CVTSD2SS, CVTPD2PS): for an operand that is a zero (an
//     exact zero of the same sign) or has 2^-126 <= |d| (normal as a float,
//     so not tiny before or after rounding: no underflow) and is finite and
//     not NaN, the host's correctly rounded conversion; taken when the
//     result is finite (in round to nearest a finite result means no
//     overflow). Inexact iff double(r) != d.
//
// tests/x86/sse_fast_test compares them with the exact paths bit for bit
// (results and MXCSR); planted bugs 107-110 and 112 must make it fail.

// The conversions also run under FTZ and DAZ (0x8040 left out of the
// check): int -> float/double has an integer operand and no tiny result;
// float <-> double leaves denormal operands and tiny results out; to int32
// leaves denormal operands out when DAZ is on (DAZ makes them an exact
// zero: no PE), and its result is an integer (FTZ never applies).
// Mutated(112): every conversion requires the default control word but
// for DAZ's denormal check, which is left out.
inline bool CvtMxcsr(const Ctx& c) {
  return (c.mxcsr & 0x7F80) == kMxcsrDefault;
}
inline bool Daz(const Ctx& c) { return (c.mxcsr & 0x40) && !Mutated(112); }

inline bool FastIntToF(Ctx& c, bool dbl, int32_t v, Xmm& d) {
  if (!CvtMxcsr(c)) return false;
  if (dbl) {
    SetLane<uint64_t>(d, 0, std::bit_cast<uint64_t>(double(v)));
    return true;
  }
  const float r = float(v);
  SetLane<uint32_t>(d, 0, std::bit_cast<uint32_t>(r));
  if (int64_t(r) != v && !Mutated(107)) c.mxcsr |= 0x20;  // PE
  return true;
}

inline bool FastCvtdq2ps(Ctx& c, Xmm& d, const Xmm& s) {
  if (!CvtMxcsr(c)) return false;
  Xmm r;
  bool inexact = false;
  for (int i = 0; i < 4; i++) {
    const int32_t v = Lane<int32_t>(s, i);
    const float f = float(v);
    inexact |= int64_t(f) != v;
    SetLane<uint32_t>(r, i, std::bit_cast<uint32_t>(f));
  }
  d = r;
  if (inexact && !Mutated(107)) c.mxcsr |= 0x20;
  return true;
}

// One lane to int32 without touching MXCSR: false outside the conditions.
template <bool D>
inline bool ToI32Lane(Bits<D> v, bool truncate, int32_t* out, bool* inexact, bool daz) {
  using F = typename Host<D>::F;
  // |x| < 2^30: biased exponent below bias + 30 (Mutated(108): 2^31, where
  // rounding can leave int32).
  const Bits<D> e = (v >> Host<D>::kShift) & (D ? 0x7FF : 0xFF);
  const Bits<D> limit = (D ? 1023 : 127) + (Mutated(108) ? 32 : 30);
  if (e >= limit) return false;
  if (daz && e == 0 && (v << 1)) return false;  // a denormal: DAZ's zero
  const F x = std::bit_cast<F>(v);
  const F r = truncate ? std::trunc(x) : std::nearbyint(x);
  *out = int32_t(r);
  *inexact = double(r) != double(x);
  return true;
}

template <bool D>
inline bool FastToI32(Ctx& c, Bits<D> v, bool truncate, int32_t* out) {
  if (!CvtMxcsr(c)) return false;
  bool inexact;
  if (!ToI32Lane<D>(v, truncate, out, &inexact, Daz(c))) return false;
  if (inexact) c.mxcsr |= 0x20;
  return true;
}

inline bool FastToDq(Ctx& c, bool dbl, bool truncate, Xmm& d, const Xmm& s) {
  if (!CvtMxcsr(c)) return false;
  const bool daz = Daz(c);
  Xmm r{};
  bool inexact = false;
  for (int i = 0; i < (dbl ? 2 : 4); i++) {
    int32_t v;
    bool ix;
    if (!(dbl ? ToI32Lane<true>(Lane<uint64_t>(s, i), truncate, &v, &ix, daz)
              : ToI32Lane<false>(Lane<uint32_t>(s, i), truncate, &v, &ix, daz)))
      return false;
    SetLane<int32_t>(r, i, v);
    inexact |= ix;
  }
  d = r;
  if (inexact) c.mxcsr |= 0x20;
  return true;
}

inline bool FastCvtFloat(Ctx& c, bool to_dbl, int lanes, Xmm& d, const Xmm& s) {
  if (!CvtMxcsr(c)) return false;
  Xmm r = d;
  bool inexact = false;
  if (to_dbl) {
    for (int i = 0; i < lanes; i++) {
      const uint32_t a = Lane<uint32_t>(s, i);
      const uint32_t e = (a >> 23) & 0xFF, f = a & 0x7FFFFF;
      if (e == 0xFF && f) return false;                    // NaN
      if (e == 0 && f && !Mutated(110)) return false;      // denormal: DE
      SetLane<uint64_t>(r, i, std::bit_cast<uint64_t>(double(std::bit_cast<float>(a))));
    }
  } else {
    for (int i = 0; i < lanes; i++) {
      const uint64_t a = Lane<uint64_t>(s, i);
      const uint64_t e = (a >> 52) & 0x7FF;
      float out;
      if (!(a & ~(uint64_t(1) << 63))) {
        out = (a >> 63) ? -0.0f : 0.0f;  // a zero: the zero of its sign, exact
      } else {
        // 2^-126 <= |d| (Mutated(109): any normal double, tiny ones too),
        // finite, not NaN.
        if (e == 0x7FF || e < (Mutated(109) ? 1 : 1023 - 126)) return false;
        const double x = std::bit_cast<double>(a);
        out = float(x);
        if (!std::isfinite(out)) return false;  // overflow
        inexact |= double(out) != x;
      }
      SetLane<uint32_t>(r, i, std::bit_cast<uint32_t>(out));
    }
    if (lanes == 2) SetLane<uint64_t>(r, 1, 0);  // CVTPD2PS zeroes the top
  }
  d = r;
  if (inexact) c.mxcsr |= 0x20;
  return true;
}

}  // namespace fast

// d = d op s, lane-wise: `lanes` lanes of double (dbl) or float. Scalar
// forms pass lanes = 1 and keep the upper lanes of d. The fast path
// (above) inline; ArithExact the exact model for everything else.
RECOMP_SSE_SLOW void ArithExact(Ctx& c, Op op, bool dbl, int lanes, Xmm& d, const Xmm& s, uint64_t va);
inline void Arith(Ctx& c, Op op, bool dbl, int lanes, Xmm& d, const Xmm& s, uint64_t va) {
  if (dbl ? fast::FastArith<true>(c, op, lanes, d, s) : fast::FastArith<false>(c, op, lanes, d, s))
    return;
  // Copies, here and in the wrappers below: lifted code keeps XMM
  // registers in locals, and an address passed out of line would make the
  // compiler keep them in memory.
  Xmm t = d;
  const Xmm u = s;
  ArithExact(c, op, dbl, lanes, t, u, va);
  d = t;
}

// CMPPS/PD/SS/SD: each lane of d becomes all ones if `d pred s` holds,
// else zero. pred is AVX's 5-bit predicate (0 EQ_OQ, 1 LT_OS, 2 LE_OS,
// 3 UNORD_Q, 4 NEQ_UQ, 5 NLT_US, 6 NLE_US, 7 ORD_Q, 8 EQ_UQ, ...
// 15 TRUE_UQ; 16-31 the same with signalling flipped); legacy SSE passes
// imm8 & 7.
RECOMP_SSE_SLOW void Cmp(Ctx& c, bool dbl, int lanes, Xmm& d, const Xmm& s, uint8_t pred,
         uint64_t va);

// (U)COMISS/SD of the low lanes: 0 greater, 1 less, 2 equal, 3 unordered
// (the caller sets ZF/PF/CF). `ordered` = COMIS (invalid on QNaN too).
RECOMP_SSE_SLOW int ComiExact(Ctx& c, bool dbl, bool ordered, const Xmm& a, const Xmm& b, uint64_t va);
inline int Comi(Ctx& c, bool dbl, bool ordered, const Xmm& a, const Xmm& b, uint64_t va) {
  int r = dbl ? fast::FastComi<true>(a, b) : fast::FastComi<false>(a, b);
  if (r >= 0) return r;
  const Xmm x = a, y = b;
  return ComiExact(c, dbl, ordered, x, y, va);
}

// SQRTSD/SQRTSS/SQRTPD/SQRTPS: d = sqrt(s), lane-wise; scalar forms keep
// the upper lanes of d.
RECOMP_SSE_SLOW void SqrtExact(Ctx& c, bool dbl, int lanes, Xmm& d, const Xmm& s, uint64_t va);
inline void Sqrt(Ctx& c, bool dbl, int lanes, Xmm& d, const Xmm& s, uint64_t va) {
  Xmm t = d;
  const Xmm u = s;
  SqrtExact(c, dbl, lanes, t, u, va);
  d = t;
}

// RSQRTPS: d = 1/sqrt(s) in all four float lanes (see above). No flags:
// ±0 and denormals give ±inf, negative values the default NaN, +inf +0,
// NaNs themselves quieted.
void Rsqrt(Xmm& d, const Xmm& s);
uint32_t RsqrtLane(uint32_t x);

// CVTSI2SD: the low lane of d = (double)v (always exact).
void Cvtsi2sd(Xmm& d, int32_t v);

// CVT(T)SS2SI / CVT(T)SD2SI of the low lane of s.
RECOMP_SSE_SLOW uint32_t CvtToI32Exact(Ctx& c, bool dbl, const Xmm& s, bool truncate, uint64_t va);
inline uint32_t CvtToI32(Ctx& c, bool dbl, const Xmm& s, bool truncate, uint64_t va) {
  int32_t r;
  if (dbl ? fast::FastToI32<true>(c, Lane<uint64_t>(s, 0), truncate, &r)
          : fast::FastToI32<false>(c, Lane<uint32_t>(s, 0), truncate, &r))
    return uint32_t(r);
  return CvtToI32Exact(c, dbl, s, truncate, va);
}

// CVT(T)SS2SI / CVT(T)SD2SI to a 64-bit register.
uint64_t CvtToI64(Ctx& c, bool dbl, const Xmm& s, bool truncate, uint64_t va);

// MINSS/SD/PS/PD (max = false) and MAXSS/SD/PS/PD, lane-wise: d op s ? d : s.
// A NaN in either operand (invalid, quiet ones too) or equal values,
// zeros of either sign included, give s. Scalar forms keep d's upper lanes.
RECOMP_SSE_SLOW void MinMaxExact(Ctx& c, bool max, bool dbl, int lanes, Xmm& d, const Xmm& s,
                 uint64_t va);
inline void MinMax(Ctx& c, bool max, bool dbl, int lanes, Xmm& d, const Xmm& s,
                   uint64_t va) {
  Xmm t = d;
  const Xmm u = s;
  MinMaxExact(c, max, dbl, lanes, t, u, va);
  d = t;
}

// CVTSS2SD / CVTPS2PD (to_dbl, 1 or 2 lanes) and CVTSD2SS / CVTPD2PS.
// Scalar forms keep d's other lanes; CVTPD2PS zeroes its upper half.
RECOMP_SSE_SLOW void CvtFloatExact(Ctx& c, bool to_dbl, int lanes, Xmm& d, const Xmm& s, uint64_t va);
inline void CvtFloat(Ctx& c, bool to_dbl, int lanes, Xmm& d, const Xmm& s, uint64_t va) {
  if (fast::FastCvtFloat(c, to_dbl, lanes, d, s)) return;
  Xmm t = d;
  const Xmm u = s;
  CvtFloatExact(c, to_dbl, lanes, t, u, va);
  d = t;
}

// CVTSI2SS / CVTSI2SD of a 32-bit or 64-bit (src64) integer into the low
// lane, rounded by MXCSR (only int64 -> double and int -> float can be
// inexact).
RECOMP_SSE_SLOW void CvtIntToFExact(Ctx& c, bool dbl, bool src64, uint64_t v, Xmm& d, uint64_t va);
inline void CvtIntToF(Ctx& c, bool dbl, bool src64, uint64_t v, Xmm& d, uint64_t va) {
  if (src64 || !fast::FastIntToF(c, dbl, int32_t(v), d)) CvtIntToFExact(c, dbl, src64, v, d, va);
}

// CVTTPS2DQ / CVTPS2DQ (truncate false) and CVTTPD2DQ / CVTPD2DQ (dbl,
// two lanes into the low half, the upper one zeroed): float lanes to
// int32, 0x80000000 for NaN and out of range (invalid).
RECOMP_SSE_SLOW void CvtToDqExact(Ctx& c, bool dbl, bool truncate, Xmm& d, const Xmm& s, uint64_t va);
inline void CvtToDq(Ctx& c, bool dbl, bool truncate, Xmm& d, const Xmm& s, uint64_t va) {
  if (!fast::FastToDq(c, dbl, truncate, d, s)) CvtToDqExact(c, dbl, truncate, d, s, va);
}

// F16C: VCVTPH2PS, four half lanes of s from 16-bit lane `from` (0 or 4)
// into d's four floats; VCVTPS2PH, s's four floats into d's 16-bit lanes
// from `to`, the others kept, rounded by imm.
void Cvtph2ps(Ctx& c, Xmm& d, const Xmm& s, int from, uint64_t va);
void Cvtps2ph(Ctx& c, Xmm& d, int to, const Xmm& s, uint8_t imm, uint64_t va);

// HADDPS/PD, HSUBPS/PD (SSE3): pairs of adjacent lanes of d, then of s,
// added (subtracted: the higher lane from the lower). ADDSUBPS/PD: even
// lanes d - s, odd ones d + s.
void HorizOp(Ctx& c, bool dbl, bool sub, Xmm& d, const Xmm& s, uint64_t va);
void AddSub(Ctx& c, bool dbl, Xmm& d, const Xmm& s, uint64_t va);

// ROUNDPS/PD/SS/SD (SSE4.1): lanes rounded to integers, imm bits 1:0 the
// mode (bit 2: MXCSR's instead), bit 3 suppressing PE. Scalar forms
// (lanes = 1) keep d's upper lanes.
void Round(Ctx& c, bool dbl, int lanes, Xmm& d, const Xmm& s, uint8_t imm, uint64_t va);

// DPPS/DPPD (SSE4.1): the products of the lanes imm selects (bits 7:4,
// 5:4 for DPPD; others +0) summed pairwise, written to the lanes imm
// bits 3:0 (1:0) select, the others zeroed.
void Dp(Ctx& c, bool dbl, Xmm& d, const Xmm& s, uint8_t imm, uint64_t va);

// RCPPS: d = 1/s in all four float lanes, correctly rounded (see above).
// No flags: ±0 and denormals give ±inf, results below the normal range
// ±0, ±inf ±0, NaNs themselves quieted.
void Rcp(Xmm& d, const Xmm& s);
uint32_t RcpLane(uint32_t x);
inline void RcpSs(Xmm& d, const Xmm& s) {
  SetLane<uint32_t>(d, 0, RcpLane(Lane<uint32_t>(s, 0)));
}

// RSQRTSS: the low lane of d = 1/sqrt(the low lane of s), as RsqrtLane.
inline void RsqrtSs(Xmm& d, const Xmm& s) {
  SetLane<uint32_t>(d, 0, RsqrtLane(Lane<uint32_t>(s, 0)));
}

// CVTDQ2PS: four int32 lanes to float, rounded by MXCSR.
RECOMP_SSE_SLOW void Cvtdq2psExact(Ctx& c, Xmm& d, const Xmm& s, uint64_t va);
inline void Cvtdq2ps(Ctx& c, Xmm& d, const Xmm& s, uint64_t va) {
  if (!fast::FastCvtdq2ps(c, d, s)) Cvtdq2psExact(c, d, s, va);
}

// CVTDQ2PD: the two low int32 lanes of s as doubles (always exact).
inline Xmm Cvtdq2pd(const Xmm& s) {
  Xmm r;
  for (int i = 0; i < 2; i++) {
    double v = double(Lane<int32_t>(s, i));
    SetLane<double>(r, i, v);
  }
  return r;
}

// FXSAVE / FXRSTOR (both forms) of the 512-byte area at p (16-byte
// aligned): the x87 part (x87::FxSave), MXCSR and MXCSR_MASK (0xFFFF:
// the bits LDMXCSR takes), XMM0-15 from byte 160; bytes 416-511 are left
// alone. 32-bit guests have xmm_regs 8: XMM0-7, and bytes 288-511 are
// left alone, as the CPU does outside 64-bit mode. FXRSTOR with reserved
// MXCSR bits set would #GP: fatal.
void Fxsave(Ctx& c, uint8_t* p, int xmm_regs = 16);
void Fxrstor(Ctx& c, const uint8_t* p, uint64_t va, int xmm_regs = 16);

// LDMXCSR: reserved bits set would #GP: fatal.
void Ldmxcsr(Ctx& c, uint32_t v, uint64_t va);

// ---- bitwise and integer ----------------------------------------------
// Bitwise operations in two 64-bit halves: the same bits as byte by byte,
// and a register kept in a local stays two host registers (byte loops make
// the compiler split it into 16).
inline void And(Xmm& d, const Xmm& s) {
  for (int i = 0; i < 2; i++) SetLane<uint64_t>(d, i, Lane<uint64_t>(d, i) & Lane<uint64_t>(s, i));
}
inline void AndNot(Xmm& d, const Xmm& s) {
  for (int i = 0; i < 2; i++) SetLane<uint64_t>(d, i, ~Lane<uint64_t>(d, i) & Lane<uint64_t>(s, i));
}
inline void Or(Xmm& d, const Xmm& s) {
  for (int i = 0; i < 2; i++) SetLane<uint64_t>(d, i, Lane<uint64_t>(d, i) | Lane<uint64_t>(s, i));
}
inline void Xor(Xmm& d, const Xmm& s) {
  for (int i = 0; i < 2; i++) SetLane<uint64_t>(d, i, Lane<uint64_t>(d, i) ^ Lane<uint64_t>(s, i));
}

// PCMPEQB/W/D: lanes of type T equal -> all ones.
template <typename T>
inline void CmpEq(Xmm& d, const Xmm& s) {
  for (int i = 0; i < int(16 / sizeof(T)); i++)
    SetLane<T>(d, i, Lane<T>(d, i) == Lane<T>(s, i) ? T(~T(0)) : T(0));
}
// PADDB/W/D/Q (wrapping).
template <typename T>
inline void Add(Xmm& d, const Xmm& s) {
  for (int i = 0; i < int(16 / sizeof(T)); i++)
    SetLane<T>(d, i, T(Lane<T>(d, i) + Lane<T>(s, i)));
}
// PSUBB/W/D/Q (wrapping).
template <typename T>
inline void Sub(Xmm& d, const Xmm& s) {
  for (int i = 0; i < int(16 / sizeof(T)); i++)
    SetLane<T>(d, i, T(Lane<T>(d, i) - Lane<T>(s, i)));
}
// PCMPGTB/W/D (signed lanes of type T).
template <typename T>
inline void CmpGt(Xmm& d, const Xmm& s) {
  using U = std::make_unsigned_t<T>;
  for (int i = 0; i < int(16 / sizeof(T)); i++)
    SetLane<U>(d, i, Lane<T>(d, i) > Lane<T>(s, i) ? U(~U(0)) : U(0));
}
// PMAXUB/UW/UD, PMAXSB/SW/SD, PMINxx: lane-wise, by T's order.
template <typename T>
inline void Max(Xmm& d, const Xmm& s) {
  for (int i = 0; i < int(16 / sizeof(T)); i++) {
    T a = Lane<T>(d, i), b = Lane<T>(s, i);
    SetLane<T>(d, i, a > b ? a : b);
  }
}
template <typename T>
inline void Min(Xmm& d, const Xmm& s) {
  for (int i = 0; i < int(16 / sizeof(T)); i++) {
    T a = Lane<T>(d, i), b = Lane<T>(s, i);
    SetLane<T>(d, i, a < b ? a : b);
  }
}
// PABSB/W/D (SSSE3): the minimum value stays itself (as unsigned, its
// absolute value).
template <typename T>
inline void Abs(Xmm& d, const Xmm& s) {
  using U = std::make_unsigned_t<T>;
  for (int i = 0; i < int(16 / sizeof(T)); i++) {
    T v = Lane<T>(s, i);
    SetLane<U>(d, i, v < 0 ? U(-U(v)) : U(v));
  }
}
// PSLLW/D/Q, PSRLW/D/Q, PSRAW/D: every lane of type T by `count` (the
// immediate, or the low quadword of the register/memory operand). Counts
// past the lane width clear the lane (logical) or fill it with its sign.
enum ShiftKind : uint8_t { kShl, kShr, kSar };
template <typename T>
inline void Shift(Xmm& d, uint64_t count, ShiftKind k) {
  using S = std::make_signed_t<T>;
  constexpr uint64_t kBits = sizeof(T) * 8;
  for (int i = 0; i < int(16 / sizeof(T)); i++) {
    T v = Lane<T>(d, i);
    if (k == kSar)
      v = T(S(v) >> (count >= kBits ? kBits - 1 : count));
    else if (count >= kBits)
      v = 0;
    else
      v = k == kShl ? T(v << count) : T(v >> count);
    SetLane<T>(d, i, v);
  }
}
// PUNPCKLBW/WD/DQ/QDQ (high: PUNPCKH..): the low (high) halves of d and s
// interleaved, d's lane first.
template <typename T>
inline void Unpck(Xmm& d, const Xmm& s, bool high) {
  constexpr int n = int(16 / sizeof(T));
  Xmm r;
  int b = high ? n / 2 : 0;
  for (int i = 0; i < n / 2; i++) {
    SetLane<T>(r, 2 * i, Lane<T>(d, b + i));
    SetLane<T>(r, 2 * i + 1, Lane<T>(s, b + i));
  }
  d = r;
}
// PSHUFLW / PSHUFHW: the four words of the low (high) quadword of s
// picked by imm; the other quadword copied.
inline void ShufW(Xmm& d, const Xmm& s, uint8_t imm, bool high) {
  Xmm r = s;
  int b = high ? 4 : 0;
  for (int i = 0; i < 4; i++)
    SetLane<uint16_t>(r, b + i, Lane<uint16_t>(s, b + ((imm >> (2 * i)) & 3)));
  d = r;
}
// PMULUDQ / PMULDQ (SSE4.1): the even 32-bit lanes multiplied into
// 64-bit products.
inline void MulUDq(Xmm& d, const Xmm& s) {
  for (int i = 0; i < 2; i++)
    SetLane<uint64_t>(d, i, uint64_t(Lane<uint32_t>(d, 2 * i)) * Lane<uint32_t>(s, 2 * i));
}
inline void MulDq(Xmm& d, const Xmm& s) {
  for (int i = 0; i < 2; i++)
    SetLane<int64_t>(d, i, int64_t(Lane<int32_t>(d, 2 * i)) * Lane<int32_t>(s, 2 * i));
}
// PMULLW / PMULLD (SSE4.1): the low half of each lane's product.
template <typename T>
inline void MulLo(Xmm& d, const Xmm& s) {
  for (int i = 0; i < int(16 / sizeof(T)); i++)
    SetLane<T>(d, i, T(uint64_t(Lane<T>(d, i)) * Lane<T>(s, i)));
}
// PACKSSDW / PACKSSWB / PACKUSWB / PACKUSDW: d's lanes then s's,
// narrowed with saturation (signed From to To).
template <typename From, typename To>
inline void Pack(Xmm& d, const Xmm& s) {
  constexpr int n = int(16 / sizeof(From));
  auto sat = [](From v) {
    constexpr From lo = From(std::numeric_limits<To>::min()),
                   hi = From(std::numeric_limits<To>::max());
    return To(v < lo ? lo : v > hi ? hi : v);
  };
  Xmm r;
  for (int i = 0; i < n; i++) SetLane<To>(r, i, sat(Lane<From>(d, i)));
  for (int i = 0; i < n; i++) SetLane<To>(r, n + i, sat(Lane<From>(s, i)));
  d = r;
}
// PMOVSX/PMOVZX (SSE4.1): the low lanes of s widened from From to To.
template <typename From, typename To>
inline void Extend(Xmm& d, const Xmm& s) {
  Xmm r;
  for (int i = 0; i < int(16 / sizeof(To)); i++) SetLane<To>(r, i, To(Lane<From>(s, i)));
  d = r;
}
// PALIGNR (SSSE3): bytes n.. of the 32-byte d:s (d high), zero past it.
inline void Alignr(Xmm& d, const Xmm& s, uint8_t n) {
  uint8_t t[32];
  std::memcpy(t, s.b, 16);
  std::memcpy(t + 16, d.b, 16);
  Xmm r;
  for (int i = 0; i < 16; i++) r.b[i] = n + i < 32 ? t[n + i] : 0;
  d = r;
}
// MOVMSKPS / MOVMSKPD: the sign bits of the float (double) lanes.
inline uint32_t MovMskPs(const Xmm& s) {
  uint32_t m = 0;
  for (int i = 0; i < 4; i++) m |= (Lane<uint32_t>(s, i) >> 31) << i;
  return m;
}
inline uint32_t MovMskPd(const Xmm& s) {
  return uint32_t(Lane<uint64_t>(s, 0) >> 63) | uint32_t(Lane<uint64_t>(s, 1) >> 63) << 1;
}
// BLENDVPS/PD (SSE4.1): lanes of s where the mask lane's sign bit is set.
template <typename T>
inline void BlendV(Xmm& d, const Xmm& s, const Xmm& mask) {
  constexpr int kTop = int(sizeof(T) * 8 - 1);
  for (int i = 0; i < int(16 / sizeof(T)); i++)
    if (Lane<T>(mask, i) >> kTop) SetLane<T>(d, i, Lane<T>(s, i));
}

// PADDSB/SW, PADDUSB/USW, PSUBSB/SW, PSUBUSB/USW: lanes of T, saturated.
template <typename T>
inline void AddSat(Xmm& d, const Xmm& s, bool sub) {
  for (int i = 0; i < int(16 / sizeof(T)); i++) {
    int32_t v = sub ? int32_t(Lane<T>(d, i)) - int32_t(Lane<T>(s, i))
                    : int32_t(Lane<T>(d, i)) + int32_t(Lane<T>(s, i));
    constexpr int32_t lo = std::numeric_limits<T>::min(), hi = std::numeric_limits<T>::max();
    SetLane<T>(d, i, T(v < lo ? lo : v > hi ? hi : v));
  }
}
// PMULHW / PMULHUW: the high half of each word product (unsigned words
// multiply as uint32_t: 0xFFFF * 0xFFFF overflows int32_t).
template <typename T>
inline void MulHi16(Xmm& d, const Xmm& s) {
  using P = std::conditional_t<std::is_signed_v<T>, int32_t, uint32_t>;
  for (int i = 0; i < 8; i++)
    SetLane<uint16_t>(d, i, uint16_t((P(Lane<T>(d, i)) * P(Lane<T>(s, i))) >> 16));
}
// PMULHRSW (SSSE3): each word product scaled by 2^-15 with rounding.
inline void MulHrs(Xmm& d, const Xmm& s) {
  for (int i = 0; i < 8; i++) {
    int32_t p = int32_t(Lane<int16_t>(d, i)) * int32_t(Lane<int16_t>(s, i));
    SetLane<uint16_t>(d, i, uint16_t(((p >> 14) + 1) >> 1));
  }
}
// PMADDWD: adjacent signed word products summed into dwords (wrapping).
inline void MAddWd(Xmm& d, const Xmm& s) {
  Xmm r;
  for (int i = 0; i < 4; i++) {
    uint32_t a = uint32_t(int32_t(Lane<int16_t>(d, 2 * i)) * Lane<int16_t>(s, 2 * i));
    uint32_t b = uint32_t(int32_t(Lane<int16_t>(d, 2 * i + 1)) * Lane<int16_t>(s, 2 * i + 1));
    SetLane<uint32_t>(r, i, a + b);
  }
  d = r;
}
// PMADDUBSW (SSSE3): unsigned bytes of d times signed bytes of s, adjacent
// products summed into words with signed saturation.
inline void MAddUbsw(Xmm& d, const Xmm& s) {
  Xmm r;
  for (int i = 0; i < 8; i++) {
    int32_t v = int32_t(d.b[2 * i]) * int8_t(s.b[2 * i]) +
                int32_t(d.b[2 * i + 1]) * int8_t(s.b[2 * i + 1]);
    SetLane<int16_t>(r, i, int16_t(v < -32768 ? -32768 : v > 32767 ? 32767 : v));
  }
  d = r;
}
// PHADDW/D, PHSUBW/D (SSSE3): adjacent lanes of d, then of s, added
// (subtracted: the higher from the lower), wrapping.
template <typename T>
inline void HorizInt(Xmm& d, const Xmm& s, bool sub) {
  constexpr int n = int(16 / sizeof(T));
  Xmm r;
  for (int i = 0; i < n; i++) {
    const Xmm& x = i < n / 2 ? d : s;
    int k = 2 * (i % (n / 2));
    T a = Lane<T>(x, k), b = Lane<T>(x, k + 1);
    SetLane<T>(r, i, T(sub ? a - b : a + b));
  }
  d = r;
}
// PHADDSW / PHSUBSW (SSSE3): HorizInt on words with signed saturation.
inline void HorizSatW(Xmm& d, const Xmm& s, bool sub) {
  Xmm r;
  for (int i = 0; i < 8; i++) {
    const Xmm& x = i < 4 ? d : s;
    int k = 2 * (i % 4);
    int32_t a = Lane<int16_t>(x, k), b = Lane<int16_t>(x, k + 1);
    int32_t v = sub ? a - b : a + b;
    SetLane<int16_t>(r, i, int16_t(v < -32768 ? -32768 : v > 32767 ? 32767 : v));
  }
  d = r;
}
// PSIGNB/W/D (SSSE3): each lane of d negated (wrapping), zeroed or kept
// as the same lane of s is negative, zero or positive.
template <typename T>
inline void Sign(Xmm& d, const Xmm& s) {
  using U = std::make_unsigned_t<T>;
  for (int i = 0; i < int(16 / sizeof(T)); i++) {
    T v = Lane<T>(s, i);
    U x = Lane<U>(d, i);
    SetLane<U>(d, i, v < 0 ? U(0 - x) : v == 0 ? U(0) : x);
  }
}
// MPSADBW (SSE4.1): word i = the sum of |d byte (o1 + i + k) - s byte
// (o2 + k)| over k < 4, with o1 = imm[2] * 4 and o2 = imm[1:0] * 4.
inline void MpSadBw(Xmm& d, const Xmm& s, uint8_t imm) {
  const int o1 = (imm >> 2 & 1) * 4, o2 = (imm & 3) * 4;
  Xmm r;
  for (int i = 0; i < 8; i++) {
    uint32_t sum = 0;
    for (int k = 0; k < 4; k++) {
      int a = d.b[o1 + i + k], b = s.b[o2 + k];
      sum += uint32_t(a > b ? a - b : b - a);
    }
    SetLane<uint16_t>(r, i, uint16_t(sum));
  }
  d = r;
}
// CRC32 (SSE4.2): crc updated with the low `bytes` of v (least significant
// first), CRC-32C (Castagnoli), bit-reflected, no inversion.
inline uint32_t Crc32c(uint32_t crc, uint64_t v, int bytes) {
  for (int i = 0; i < bytes; i++) {
    crc ^= uint8_t(v >> (8 * i));
    for (int k = 0; k < 8; k++) crc = crc >> 1 ^ (crc & 1 ? 0x82F63B78u : 0u);
  }
  return crc;
}

// PAVGB/W: unsigned (a + b + 1) / 2.
template <typename T>
inline void Avg(Xmm& d, const Xmm& s) {
  for (int i = 0; i < int(16 / sizeof(T)); i++)
    SetLane<T>(d, i, T((uint32_t(Lane<T>(d, i)) + Lane<T>(s, i) + 1) >> 1));
}
// PSADBW: per quadword, the sum of the absolute byte differences in its
// low word, the rest zero.
inline void SadBw(Xmm& d, const Xmm& s) {
  Xmm r{};
  for (int q = 0; q < 2; q++) {
    uint32_t sum = 0;
    for (int i = 0; i < 8; i++) {
      int a = d.b[8 * q + i], b = s.b[8 * q + i];
      sum += uint32_t(a > b ? a - b : b - a);
    }
    SetLane<uint64_t>(r, q, sum);
  }
  d = r;
}
// PSHUFB (SSSE3): byte i of d = the byte of d that s's byte i indexes (low
// four bits), or zero if its top bit is set.
inline void ShufB(Xmm& d, const Xmm& s) {
  Xmm r;
  for (int i = 0; i < 16; i++) r.b[i] = (s.b[i] & 0x80) ? 0 : d.b[s.b[i] & 15];
  d = r;
}
// PBLENDW (SSE4.1): words of s where imm's bit is set.
inline void BlendW(Xmm& d, const Xmm& s, uint8_t imm) {
  for (int i = 0; i < 8; i++)
    if (imm >> i & 1) SetLane<uint16_t>(d, i, Lane<uint16_t>(s, i));
}
// BLENDPS/PD (SSE4.1): lanes of s where imm's bit is set.
template <typename T>
inline void BlendImm(Xmm& d, const Xmm& s, uint8_t imm) {
  for (int i = 0; i < int(16 / sizeof(T)); i++)
    if (imm >> i & 1) SetLane<T>(d, i, Lane<T>(s, i));
}
// INSERTPS (SSE4.1): float lane imm[7:6] of s (a register; a memory
// operand passes its m32 as lane 0 with imm[7:6] ignored) into lane
// imm[5:4] of d, then the lanes in imm[3:0] zeroed.
inline void InsertPs(Xmm& d, const Xmm& s, uint8_t imm, bool mem) {
  SetLane<uint32_t>(d, (imm >> 4) & 3, Lane<uint32_t>(s, mem ? 0 : imm >> 6));
  for (int i = 0; i < 4; i++)
    if (imm >> i & 1) SetLane<uint32_t>(d, i, 0);
}
// PTEST (SSE4.1): ZF = (d & s) == 0, CF = (~d & s) == 0; the other flags
// cleared. Returns ZF | CF << 1.
inline uint32_t Ptest(const Xmm& d, const Xmm& s) {
  bool z = true, cf = true;
  for (int i = 0; i < 16; i++) {
    if (d.b[i] & s.b[i]) z = false;
    if (~d.b[i] & s.b[i]) cf = false;
  }
  return uint32_t(z) | uint32_t(cf) << 1;
}
// PHMINPOSUW (SSE4.1): the least unsigned word of s (the first of equal
// ones) and its index, in the low two words; the rest zero.
inline void MinPosUw(Xmm& d, const Xmm& s) {
  int k = 0;
  for (int i = 1; i < 8; i++)
    if (Lane<uint16_t>(s, i) < Lane<uint16_t>(s, k)) k = i;
  Xmm r{};
  SetLane<uint16_t>(r, 0, Lane<uint16_t>(s, k));
  SetLane<uint16_t>(r, 1, uint16_t(k));
  d = r;
}
// MOVSLDUP / MOVSHDUP (SSE3): the even (odd) float lanes of s duplicated.
// MOVDDUP: the low quadword of s twice.
inline void MovSDup(Xmm& d, const Xmm& s, bool high) {
  Xmm r;
  for (int i = 0; i < 4; i++) SetLane<uint32_t>(r, i, Lane<uint32_t>(s, (i & ~1) + high));
  d = r;
}
inline void MovDDup(Xmm& d, const Xmm& s) {
  uint64_t q = Lane<uint64_t>(s, 0);
  SetLane<uint64_t>(d, 0, q);
  SetLane<uint64_t>(d, 1, q);
}

// ---- AES-NI, PCLMULQDQ, SSE4.2 string compares (sse_ext.cpp) -----------
// AESENC/AESENCLAST (last: no MixColumns), AESDEC/AESDECLAST: one round of
// d with round key s. AESIMC: InvMixColumns of s. AESKEYGENASSIST: the
// key expansion's SubWord/RotWord of s's dwords 1 and 3 with rcon.
void AesEnc(Xmm& d, const Xmm& s, bool last);
void AesDec(Xmm& d, const Xmm& s, bool last);
void AesImc(Xmm& d, const Xmm& s);
void AesKeygenAssist(Xmm& d, const Xmm& s, uint8_t rcon);
// PCLMULQDQ: the carry-less product of quadword imm[0] of d and
// quadword imm[4] of s.
void Clmul(Xmm& d, const Xmm& s, uint8_t imm);
// PCMPESTRI (explicit lengths la, lb: rax/eax, rdx/edx, signed) and
// PCMPISTRI (implicit: up to the first zero element): the index into ECX;
// flags CF, ZF, SF, OF as the SDM defines them (AF, PF cleared), returned
// as CF | ZF << 1 | SF << 2 | OF << 3.
uint32_t PcmpStrI(const Xmm& a, const Xmm& b, uint8_t imm, bool expl, int64_t la, int64_t lb,
                  uint32_t* index);
// PCMPESTRM / PCMPISTRM: the same, the result into *mask (XMM0): bits
// (imm[6] clear) or whole elements (set).
uint32_t PcmpStrM(const Xmm& a, const Xmm& b, uint8_t imm, bool expl, int64_t la, int64_t lb,
                  Xmm* mask);

// PMAXSW.
inline void MaxS16(Xmm& d, const Xmm& s) {
  for (int i = 0; i < 8; i++) {
    int16_t a = Lane<int16_t>(d, i), b = Lane<int16_t>(s, i);
    SetLane<int16_t>(d, i, a > b ? a : b);
  }
}
// PMOVMSKB: the sign bit of every byte.
inline uint32_t MovMskB(const Xmm& s) {
  uint32_t m = 0;
  for (int i = 0; i < 16; i++) m |= uint32_t(s.b[i] >> 7) << i;
  return m;
}
// PSLLQ/PSRLQ: each quadword shifted by `count` (>= 64 clears it). The
// register/memory form passes the low quadword of its operand.
inline void ShlQ(Xmm& d, uint64_t count) {
  for (int i = 0; i < 2; i++)
    SetLane<uint64_t>(d, i, count > 63 ? 0 : Lane<uint64_t>(d, i) << count);
}
inline void ShrQ(Xmm& d, uint64_t count) {
  for (int i = 0; i < 2; i++)
    SetLane<uint64_t>(d, i, count > 63 ? 0 : Lane<uint64_t>(d, i) >> count);
}
// PSRLDQ / PSLLDQ: the whole register shifted by `n` bytes (more than 15
// clears it).
inline void ShrDq(Xmm& d, uint8_t n) {
  Xmm r{};
  for (int i = 0; i + n < 16; i++) r.b[i] = d.b[i + n];
  d = r;
}
inline void ShlDq(Xmm& d, uint8_t n) {
  Xmm r{};
  for (int i = n; i < 16; i++) r.b[i] = d.b[i - n];
  d = r;
}
// PSHUFD.
inline void ShufD(Xmm& d, const Xmm& s, uint8_t imm) {
  Xmm r;
  for (int i = 0; i < 4; i++)
    SetLane<uint32_t>(r, i, Lane<uint32_t>(s, (imm >> (2 * i)) & 3));
  d = r;
}
// SHUFPS: low two lanes from d, high two from s.
inline void ShufPs(Xmm& d, const Xmm& s, uint8_t imm) {
  Xmm r;
  SetLane<uint32_t>(r, 0, Lane<uint32_t>(d, imm & 3));
  SetLane<uint32_t>(r, 1, Lane<uint32_t>(d, (imm >> 2) & 3));
  SetLane<uint32_t>(r, 2, Lane<uint32_t>(s, (imm >> 4) & 3));
  SetLane<uint32_t>(r, 3, Lane<uint32_t>(s, (imm >> 6) & 3));
  d = r;
}
// UNPCKLPD: high quadword of d = low quadword of s.
inline void UnpckLPd(Xmm& d, const Xmm& s) {
  SetLane<uint64_t>(d, 1, Lane<uint64_t>(s, 0));
}
// UNPCKHPD: low quadword of d = its high one, high quadword = s's high one.
inline void UnpckHPd(Xmm& d, const Xmm& s) {
  SetLane<uint64_t>(d, 0, Lane<uint64_t>(d, 1));
  SetLane<uint64_t>(d, 1, Lane<uint64_t>(s, 1));
}
// UNPCKLPS / UNPCKHPS: the low / high two lanes of d and s interleaved.
inline void UnpckPs(Xmm& d, const Xmm& s, bool high) {
  Xmm r;
  int b = high ? 2 : 0;
  SetLane<uint32_t>(r, 0, Lane<uint32_t>(d, b));
  SetLane<uint32_t>(r, 1, Lane<uint32_t>(s, b));
  SetLane<uint32_t>(r, 2, Lane<uint32_t>(d, b + 1));
  SetLane<uint32_t>(r, 3, Lane<uint32_t>(s, b + 1));
  d = r;
}
// MOVLHPS: high quadword of d = low quadword of s. MOVHLPS: low quadword
// of d = high quadword of s.
inline void MovLHPs(Xmm& d, const Xmm& s) {
  SetLane<uint64_t>(d, 1, Lane<uint64_t>(s, 0));
}
inline void MovHLPs(Xmm& d, const Xmm& s) {
  SetLane<uint64_t>(d, 0, Lane<uint64_t>(s, 1));
}
// SHUFPD: low quadword from d, high from s, picked by imm bits 0 and 1.
inline void ShufPd(Xmm& d, const Xmm& s, uint8_t imm) {
  Xmm r;
  SetLane<uint64_t>(r, 0, Lane<uint64_t>(d, imm & 1));
  SetLane<uint64_t>(r, 1, Lane<uint64_t>(s, (imm >> 1) & 1));
  d = r;
}

}  // namespace recomp::sse
