// Checks the SSE model's host-arithmetic fast paths (sse.h: ADD/SUB/
// MUL/DIV of PS/PD/SS/SD, COMISS/COMISD, and the int/float and
// float/double conversions) against the exact SoftFloat
// paths they bypass (the *Exact functions). Portable, so it runs on arm64
// too. Every case runs one instruction on identical states both ways and
// compares the result register and MXCSR bit for bit. The exact path is
// checked against the hardware by lift_sse_test.
//
// Operands mix random bit patterns with values chosen to sit on the fast
// path's edges: its range limits (2^(emin+2p), 2^emax), pairs whose
// products and quotients land at the underflow threshold or overflow,
// cancelling sums, zeros, denormals, infinities and NaNs. Each fast path
// must be taken and passed over often enough, or the comparison would
// check the exact model against itself.
//
// Built with RECOMP_X86_MUTATION=<n> (a planted bug in a fast path,
// sse.cpp) the test must catch it: it then exits 0, and 1 when the bug
// went unnoticed.
//
// Built with RECOMP_SSE_FLAGS_UNOBSERVED=1 (sse_fast_noflags_test) it
// checks that variant of ADD/SUB/MUL/DIV: the same result bits, MXCSR's
// flags not compared.
//
// Usage: sse_fast_test [cases] [seed]   (a random seed by default, printed)

#include <bit>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <random>

#include "x86/sse.h"

namespace {

using recomp::Ctx;
using recomp::Xmm;
namespace sse = recomp::sse;

std::mt19937_64 g_rng;
uint64_t Rand() { return g_rng(); }

// An interesting value of the format: random bits, or one built near an
// edge (exponent chosen from the edges and their neighbours).
template <bool D>
uint64_t Value() {
  const int bias = D ? 1023 : 127, mant = D ? 52 : 23, emax_b = D ? 2046 : 254;
  const int p = mant + 1;
  const int lo = bias + (D ? -1022 : -126) + 2 * p;  // the fast path's lower limit
  const uint64_t frac_mask = (uint64_t(1) << mant) - 1;
  uint64_t sign = (Rand() & 1) << (D ? 63 : 31);
  uint64_t frac = Rand() & frac_mask;
  if (Rand() % 4 == 0) frac &= ~((uint64_t(1) << (Rand() % mant)) - 1);  // short significands
  int e;
  switch (Rand() % 12) {
    case 0: return Rand() & (D ? ~uint64_t(0) : 0xFFFFFFFFu);  // any bits
    case 1: return sign;                                        // zero
    case 2: return sign | (frac ? frac : 1);                    // denormal
    case 3: return sign | (uint64_t(emax_b + 1) << mant) | (Rand() % 3 ? frac : 0);  // inf/NaN
    case 4: e = lo + int(Rand() % 5) - 2; break;           // around 2^(emin+2p)
    case 5: e = emax_b - int(Rand() % 4); break;            // around 2^emax
    case 6: e = 1 + int(Rand() % 3); break;                 // smallest normals
    case 7: e = lo / 2 + bias / 2 + int(Rand() % 9) - 4; break;  // squares near lo
    default: e = bias + int(Rand() % 120) - 60; break;      // everyday magnitudes
  }
  if (e < 1) e = 1;
  if (e > emax_b) e = emax_b;
  return sign | (uint64_t(e) << mant) | frac;
}

// A second operand related to the first: the same (x - x, x / x), its
// negation (x + -x), or one that puts the result at an edge.
template <bool D>
uint64_t Partner(uint64_t a) {
  const int mant = D ? 52 : 23;
  const uint64_t sign = uint64_t(1) << (D ? 63 : 31);
  switch (Rand() % 6) {
    case 0: return a;
    case 1: return a ^ sign;
    case 2: return a ^ (Rand() & ((uint64_t(1) << (Rand() % 8)) - 1));  // nearly equal
    default: return Value<D>();
  }
  (void)mant;
}

enum Path { kArithTaken, kArithPassed, kComiTaken, kComiPassed, kCvtTaken, kCvtPassed, kPaths };
const char* const kNames[kPaths] = {"arith fast", "arith exact", "comi fast", "comi exact",
                                    "cvt fast", "cvt exact"};
long g_count[kPaths];
long g_fail;

uint32_t Mxcsr() {
  // Mostly the default with random sticky flags; sometimes another
  // rounding mode, FTZ or DAZ (every exception still masked: the fast
  // path must step aside).
  uint32_t m = sse::kMxcsrDefault | uint32_t(Rand() & 0x3F);
  switch (Rand() % 8) {
    case 0: m |= uint32_t(Rand() % 4) << 13; break;
    case 1: m |= 0x8000; break;
    case 2: m |= 0x40; break;
    case 3: m |= 0x8040; break;  // both, as some games run
    default: break;
  }
  return m;
}

template <bool D>
bool IsNaN(std::conditional_t<D, uint64_t, uint32_t> v) {
  return D ? (v & ~(uint64_t(1) << 63)) > 0x7FF0000000000000ull
           : (v & 0x7FFFFFFFu) > 0x7F800000u;
}

template <bool D>
void ArithCase() {
  using U = std::conditional_t<D, uint64_t, uint32_t>;
  const int per = 16 / int(sizeof(U));
  const int lanes = Rand() % 2 ? 1 : per;
  const auto op = sse::Op(Rand() % 4);
  Xmm d{}, s{};
  for (int i = 0; i < per; i++) {
    U a = U(Value<D>()), b = U(Rand() % 3 ? Partner<D>(a) : Value<D>());
    if (Rand() % 4 == 0) std::swap(a, b);
    sse::SetLane<U>(d, i, a);
    sse::SetLane<U>(s, i, b);
  }
  Ctx fast{}, exact{};
  fast.mxcsr = exact.mxcsr = Mxcsr();
  Xmm df = d, de = d;
  sse::ArithExact(exact, op, D, lanes, de, s, 0);
  const uint32_t before = fast.mxcsr;
  sse::Arith(fast, op, D, lanes, df, s, 0);
  // Coverage: the fast path answers when MXCSR is the default and every
  // lane's operands and (exact) result are zero or in range; the zero
  // rules for MUL and DIV make this an estimate, which is all a count
  // needs.
  {
    auto in = [](U v) {
      const int mant = D ? 52 : 23;
      const U m = U(v & ~(U(1) << (sizeof(U) * 8 - 1)));
      if (!m) return true;
      const U e = U(m >> mant);
      const U lo = D ? 107 : 49, hi = D ? 2045 : 253;
      return e >= lo && e <= hi;
    };
    bool taken = (before & (RECOMP_SSE_FLAGS_UNOBSERVED ? 0xFFC0 : 0x7F80)) == sse::kMxcsrDefault;
    for (int i = 0; i < lanes && taken; i++)
      taken = RECOMP_SSE_FLAGS_UNOBSERVED ? !IsNaN<D>(sse::Lane<U>(de, i))
                                          : in(sse::Lane<U>(d, i)) && in(sse::Lane<U>(s, i)) &&
                                                in(sse::Lane<U>(de, i));
    g_count[taken ? kArithTaken : kArithPassed]++;
  }
  // Flags unobserved (sse.h): the result and MXCSR's control bits only.
  const uint32_t flags = RECOMP_SSE_FLAGS_UNOBSERVED ? 0x3F : 0;
  if (std::memcmp(df.b, de.b, 16) != 0 || (fast.mxcsr & ~flags) != (exact.mxcsr & ~flags)) {
    if (g_fail++ < 10) {
      std::printf("MISMATCH %s%s op %d lanes %d mxcsr %04X\n", D ? "pd" : "ps",
                  lanes == 1 ? " (scalar)" : "", int(op), lanes, before);
      for (int i = 0; i < lanes; i++)
        std::printf("  lane %d: %0*" PRIx64 " op %0*" PRIx64 " -> fast %0*" PRIx64
                    " exact %0*" PRIx64 "\n",
                    i, D ? 16 : 8, uint64_t(sse::Lane<U>(d, i)), D ? 16 : 8,
                    uint64_t(sse::Lane<U>(s, i)), D ? 16 : 8, uint64_t(sse::Lane<U>(df, i)),
                    D ? 16 : 8, uint64_t(sse::Lane<U>(de, i)));
      std::printf("  mxcsr fast %04X exact %04X\n", fast.mxcsr, exact.mxcsr);
    }
  }
}

template <bool D>
void ComiCase() {
  using U = std::conditional_t<D, uint64_t, uint32_t>;
  Xmm a{}, b{};
  U x = U(Value<D>()), y = U(Rand() % 3 ? Partner<D>(x) : Value<D>());
  sse::SetLane<U>(a, 0, x);
  sse::SetLane<U>(b, 0, y);
  const bool ordered = Rand() & 1;
  Ctx fast{}, exact{};
  fast.mxcsr = exact.mxcsr = Mxcsr();
  int re = sse::ComiExact(exact, D, ordered, a, b, 0);
  int rf = sse::Comi(fast, D, ordered, a, b, 0);
  auto special = [](U v) {
    const int mant = D ? 52 : 23;
    const U e = U((v >> mant) & (D ? 0x7FF : 0xFF));
    const U f = U(v & ((U(1) << mant) - 1));
    return (e == (D ? 0x7FF : 0xFF) && f) || (e == 0 && f);  // NaN or denormal
  };
  g_count[special(x) || special(y) ? kComiPassed : kComiTaken]++;
  if (rf != re || fast.mxcsr != exact.mxcsr) {
    if (g_fail++ < 10)
      std::printf("MISMATCH comis%s %s %0*" PRIx64 " vs %0*" PRIx64
                  ": fast %d mxcsr %04X, exact %d mxcsr %04X\n",
                  D ? "d" : "s", ordered ? "ordered" : "unordered", D ? 16 : 8, uint64_t(x),
                  D ? 16 : 8, uint64_t(y), rf, fast.mxcsr, re, exact.mxcsr);
  }
}

using sse::Lane;
using sse::SetLane;

// Conversion operands near their fast paths' edges.
int32_t IntValue() {
  switch (Rand() % 6) {
    case 0: return int32_t(Rand());                                       // any
    case 1: return int32_t((int64_t(1) << 24) + int64_t(Rand() % 9) - 4) * (Rand() & 1 ? 1 : -1);
    case 2: return int32_t(0x7FFFFFFF - Rand() % 300);                    // near 2^31
    case 3: return int32_t(0x80000000u + Rand() % 300);                   // near -2^31
    case 4: return int32_t(Rand() % 2001) - 1000;                         // small
    default: return int32_t(Rand() & ((1u << (Rand() % 31)) - 1)) << (Rand() % 8);
  }
}

// Floats and doubles near the int32 conversion's limits (2^30, 2^31) and
// halves, or anything (Value).
template <bool D>
uint64_t ToIntValue() {
  using F = std::conditional_t<D, double, float>;
  using U = std::conditional_t<D, uint64_t, uint32_t>;
  const double sign = Rand() & 1 ? 1.0 : -1.0;
  F x;
  switch (Rand() % 5) {
    case 0: return Value<D>();
    case 1: x = F(sign * (std::ldexp(1.0, 30 + int(Rand() % 2)) + double(int(Rand() % 5) - 2)));
            break;
    case 2: x = F(sign * (double(Rand() % 100000) + 0.5)); break;   // ties
    case 3: x = F(sign * double(Rand() % 1000) / 7.0); break;
    default: {
      U u = std::bit_cast<U>(F(sign * std::ldexp(1.0, 29 + int(Rand() % 3))));
      return u + U(Rand() % 5) - 2;                                   // around 2^29..2^31
    }
  }
  return std::bit_cast<U>(x);
}

// Doubles near float's limits: 2^-126 (smallest normal), FLT_MAX and
// overflow, values halfway between floats, or anything.
uint64_t ToFloatValue() {
  const double sign = Rand() & 1 ? 1.0 : -1.0;
  switch (Rand() % 6) {
    case 0: return Value<true>();
    case 1: return std::bit_cast<uint64_t>(sign * std::ldexp(1.0, -126)) + (Rand() % 9) - 4;
    case 2: return std::bit_cast<uint64_t>(sign * double(FLT_MAX)) + (Rand() % 2 ? 1 : -1) * int64_t(Rand() % (uint64_t(1) << 30));
    case 3: {  // halfway between two floats (ties) and its neighbours
      const float f = std::bit_cast<float>(uint32_t(Rand() % 0x7F000000u + 0x00800000u));
      const double half = double(std::nextafter(f, INFINITY)) - double(f);
      return std::bit_cast<uint64_t>(sign * (double(f) + half / 2)) + (Rand() % 3) - 1;
    }
    case 4: return std::bit_cast<uint64_t>(sign * std::ldexp(double(Rand() % 1000 + 1), -int(Rand() % 160)));
    default: return (Rand() & 1 ? uint64_t(1) << 63 : 0) | (Rand() % 4 ? 0 : (Rand() & 0xFFFFF));  // zeros, denormals
  }
}

void CvtCase() {
  Ctx fast{}, exact{}, probe{};
  fast.mxcsr = exact.mxcsr = probe.mxcsr = Mxcsr();
  const uint32_t before = fast.mxcsr;
  Xmm s{}, d{};
  for (int i = 0; i < 2; i++) SetLane<uint64_t>(d, i, Rand());
  Xmm df = d, de = d, dp = d;
  uint32_t rf = 0, re = 0;
  bool taken = false;
  const int kind = int(Rand() % 6);
  const bool dbl = Rand() & 1, trunc = Rand() & 1;
  const int lanes = Rand() & 1 ? 1 : 2;
  auto both = [&](auto&& op, auto&& exact_op) {
    exact_op(exact, de, &re);
    op(fast, df, &rf);
  };
  switch (kind) {
    case 0:  // CVTDQ2PS
      for (int i = 0; i < 4; i++) SetLane<int32_t>(s, i, IntValue());
      both([&](Ctx& c, Xmm& x, uint32_t*) { sse::Cvtdq2ps(c, x, s, 0); },
           [&](Ctx& c, Xmm& x, uint32_t*) { sse::Cvtdq2psExact(c, x, s, 0); });
      taken = sse::fast::FastCvtdq2ps(probe, dp, s);
      break;
    case 1: {  // CVTSI2SS / CVTSI2SD
      const int32_t v = IntValue();
      both([&](Ctx& c, Xmm& x, uint32_t*) { sse::CvtIntToF(c, dbl, false, uint32_t(v), x, 0); },
           [&](Ctx& c, Xmm& x, uint32_t*) { sse::CvtIntToFExact(c, dbl, false, uint32_t(v), x, 0); });
      taken = sse::fast::FastIntToF(probe, dbl, v, dp);
      break;
    }
    case 2: {  // CVT(T)SS2SI / CVT(T)SD2SI
      if (dbl) SetLane<uint64_t>(s, 0, ToIntValue<true>());
      else SetLane<uint32_t>(s, 0, uint32_t(ToIntValue<false>()));
      both([&](Ctx& c, Xmm&, uint32_t* r) { *r = sse::CvtToI32(c, dbl, s, trunc, 0); },
           [&](Ctx& c, Xmm&, uint32_t* r) { *r = sse::CvtToI32Exact(c, dbl, s, trunc, 0); });
      int32_t t;
      taken = dbl ? sse::fast::FastToI32<true>(probe, Lane<uint64_t>(s, 0), trunc, &t)
                  : sse::fast::FastToI32<false>(probe, Lane<uint32_t>(s, 0), trunc, &t);
      break;
    }
    case 3:  // CVT(T)PS2DQ / CVT(T)PD2DQ
      for (int i = 0; i < (dbl ? 2 : 4); i++) {
        if (dbl) SetLane<uint64_t>(s, i, ToIntValue<true>());
        else SetLane<uint32_t>(s, i, uint32_t(ToIntValue<false>()));
      }
      both([&](Ctx& c, Xmm& x, uint32_t*) { sse::CvtToDq(c, dbl, trunc, x, s, 0); },
           [&](Ctx& c, Xmm& x, uint32_t*) { sse::CvtToDqExact(c, dbl, trunc, x, s, 0); });
      taken = sse::fast::FastToDq(probe, dbl, trunc, dp, s);
      break;
    case 4:  // CVTSS2SD / CVTPS2PD
      for (int i = 0; i < 2; i++) SetLane<uint32_t>(s, i, uint32_t(Value<false>()));
      both([&](Ctx& c, Xmm& x, uint32_t*) { sse::CvtFloat(c, true, lanes, x, s, 0); },
           [&](Ctx& c, Xmm& x, uint32_t*) { sse::CvtFloatExact(c, true, lanes, x, s, 0); });
      taken = sse::fast::FastCvtFloat(probe, true, lanes, dp, s);
      break;
    default:  // CVTSD2SS / CVTPD2PS
      for (int i = 0; i < 2; i++) SetLane<uint64_t>(s, i, ToFloatValue());
      both([&](Ctx& c, Xmm& x, uint32_t*) { sse::CvtFloat(c, false, lanes, x, s, 0); },
           [&](Ctx& c, Xmm& x, uint32_t*) { sse::CvtFloatExact(c, false, lanes, x, s, 0); });
      taken = sse::fast::FastCvtFloat(probe, false, lanes, dp, s);
      break;
  }
  g_count[taken ? kCvtTaken : kCvtPassed]++;
  if (std::memcmp(df.b, de.b, 16) != 0 || rf != re || fast.mxcsr != exact.mxcsr) {
    if (g_fail++ < 10) {
      std::printf("MISMATCH cvt kind %d dbl %d trunc %d lanes %d mxcsr %04X src %016" PRIx64
                  " %016" PRIx64 "\n", kind, dbl, trunc, lanes, before, Lane<uint64_t>(s, 0),
                  Lane<uint64_t>(s, 1));
      std::printf("  fast %016" PRIx64 " %016" PRIx64 " r %08X mxcsr %04X\n", Lane<uint64_t>(df, 0),
                  Lane<uint64_t>(df, 1), rf, fast.mxcsr);
      std::printf("  exact %016" PRIx64 " %016" PRIx64 " r %08X mxcsr %04X\n", Lane<uint64_t>(de, 0),
                  Lane<uint64_t>(de, 1), re, exact.mxcsr);
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  long cases = argc > 1 ? std::atol(argv[1]) : 2000000;
  uint64_t seed = argc > 2 ? std::strtoull(argv[2], nullptr, 0) : std::random_device{}();
  std::printf("sse_fast_test: %ld cases, seed %" PRIu64 " (mutation %d)\n", cases, seed,
              RECOMP_X86_MUTATION);
  g_rng.seed(seed);
  for (long i = 0; i < cases; i++) {
    switch (i % 6) {
      case 4:
      case 5: CvtCase(); break;
      case 0: ArithCase<false>(); break;
      case 1: ArithCase<true>(); break;
      case 2: ComiCase<false>(); break;
      default: ComiCase<true>(); break;
    }
  }
  bool coverage = true;
  for (int p = 0; p < kPaths; p++) {
    std::printf("  %-12s %ld\n", kNames[p], g_count[p]);
    if (g_count[p] < cases / 100) coverage = false;
  }
  if (!coverage) std::printf("a path was too rarely taken or passed over\n");
  const bool ok = !g_fail && coverage;
  std::printf("%ld mismatches\n", g_fail);
  if (RECOMP_X86_MUTATION) {
    std::printf(ok ? "planted bug NOT caught\n" : "planted bug caught\n");
    return ok ? 1 : 0;
  }
  return ok ? 0 : 1;
}
