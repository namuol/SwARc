// mmx_test: the MMX helpers (x86/mmx.h) against the host CPU's MMX
// instructions, bit for bit (x86-64 hosts).
//
// Values: every helper the lifter emits runs on random operands whose
// lanes are drawn from random bits and the edge values of each lane width
// (0, 1, -1, the signed and unsigned extremes and their neighbours), and
// must give the register (or general register, or stored bytes) the host
// gives. Shifts run every immediate count up to past the lane width and
// random 64-bit counts; PSHUFW, PEXTRW and PINSRW a spread of immediates.
// The conversions between MMX and packed floats (CVTPS2PI, CVTPI2PS, ...),
// lifted onto the SSE ones (x86/sse.h), run on float and double edge
// values under random MXCSR (rounding mode, DAZ, FTZ, sticky flags; all
// exceptions masked): the result and MXCSR must match.
//
// State: from an x87 state with values on the stack (TOP not 0), an MMX
// register write, then EMMS, must leave the same FNSAVE and FXSAVE images
// as the host (the x87 pointers and opcode excepted, as in fxsave_test).
// Rosetta 2 does not keep the x87 state as the hardware does (fxsave_test
// fails there), so this part is skipped under it; the values still run.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#include "x86/ctx.h"
#include "x86/mmx.h"
#include "x86/sse.h"
#include "x86/x87.h"

namespace recomp {
void FatalF(const char* fmt, ...);
}

namespace {

namespace mmx = recomp::mmx;

int g_failures = 0;
long g_checks = 0;

void Check(bool ok, const char* what, uint64_t d, uint64_t s, uint64_t hw, uint64_t model) {
  ++g_checks;
  if (ok) return;
  if (++g_failures <= 40)
    std::printf("%s: d=%016llX s=%016llX hw=%016llX model=%016llX\n", what,
                (unsigned long long)d, (unsigned long long)s, (unsigned long long)hw,
                (unsigned long long)model);
}

// ---- operands -----------------------------------------------------------------

std::mt19937_64 g_rng(0x6D6D78);

uint64_t Edge(int bits) {
  const uint64_t mask = bits == 64 ? ~uint64_t(0) : (uint64_t(1) << bits) - 1;
  const uint64_t smin = uint64_t(1) << (bits - 1);
  const uint64_t v[] = {0, 1, 2, mask, mask - 1, smin, smin - 1, smin + 1,
                        smin >> 1, mask >> 1 ^ 1, 0x55555555'55555555ull, 0xAAAAAAAA'AAAAAAAAull};
  return v[g_rng() % (sizeof v / sizeof v[0])] & mask;
}
// Lanes of a random width, each random bits or an edge value.
uint64_t Operand() {
  static const int kWidths[] = {8, 16, 32, 64};
  const int bits = kWidths[g_rng() % 4];
  if (g_rng() % 4 == 0) return g_rng();
  uint64_t r = 0;
  for (int i = 0; i < 64 / bits; i++) {
    const uint64_t mask = bits == 64 ? ~uint64_t(0) : (uint64_t(1) << bits) - 1;
    uint64_t lane = g_rng() % 3 == 0 ? g_rng() & mask : Edge(bits);
    r |= lane << (bits * i);
  }
  return r;
}

// ---- d = op(d, s) ---------------------------------------------------------------

#define NATIVE_BINARY(name, insn)                                                \
  uint64_t name(uint64_t d, uint64_t s) {                                       \
    uint64_t r;                                                                  \
    asm volatile("movq %1, %%mm0\n movq %2, %%mm1\n " insn " %%mm1, %%mm0\n"    \
                 " movq %%mm0, %0\n emms"                                        \
                 : "=r"(r)                                                       \
                 : "r"(d), "r"(s)                                                \
                 : "mm0", "mm1");                                                \
    return r;                                                                    \
  }

NATIVE_BINARY(HwPaddb, "paddb")
NATIVE_BINARY(HwPaddw, "paddw")
NATIVE_BINARY(HwPaddd, "paddd")
NATIVE_BINARY(HwPaddq, "paddq")
NATIVE_BINARY(HwPsubb, "psubb")
NATIVE_BINARY(HwPsubw, "psubw")
NATIVE_BINARY(HwPsubd, "psubd")
NATIVE_BINARY(HwPsubq, "psubq")
NATIVE_BINARY(HwPaddsb, "paddsb")
NATIVE_BINARY(HwPaddsw, "paddsw")
NATIVE_BINARY(HwPaddusb, "paddusb")
NATIVE_BINARY(HwPaddusw, "paddusw")
NATIVE_BINARY(HwPsubsb, "psubsb")
NATIVE_BINARY(HwPsubsw, "psubsw")
NATIVE_BINARY(HwPsubusb, "psubusb")
NATIVE_BINARY(HwPsubusw, "psubusw")
NATIVE_BINARY(HwPcmpeqb, "pcmpeqb")
NATIVE_BINARY(HwPcmpeqw, "pcmpeqw")
NATIVE_BINARY(HwPcmpeqd, "pcmpeqd")
NATIVE_BINARY(HwPcmpgtb, "pcmpgtb")
NATIVE_BINARY(HwPcmpgtw, "pcmpgtw")
NATIVE_BINARY(HwPcmpgtd, "pcmpgtd")
NATIVE_BINARY(HwPmullw, "pmullw")
NATIVE_BINARY(HwPmulhw, "pmulhw")
NATIVE_BINARY(HwPmulhuw, "pmulhuw")
NATIVE_BINARY(HwPmaddwd, "pmaddwd")
NATIVE_BINARY(HwPmuludq, "pmuludq")
NATIVE_BINARY(HwPavgb, "pavgb")
NATIVE_BINARY(HwPavgw, "pavgw")
NATIVE_BINARY(HwPmaxub, "pmaxub")
NATIVE_BINARY(HwPmaxsw, "pmaxsw")
NATIVE_BINARY(HwPminub, "pminub")
NATIVE_BINARY(HwPminsw, "pminsw")
NATIVE_BINARY(HwPsadbw, "psadbw")
NATIVE_BINARY(HwPacksswb, "packsswb")
NATIVE_BINARY(HwPackssdw, "packssdw")
NATIVE_BINARY(HwPackuswb, "packuswb")
NATIVE_BINARY(HwPunpcklbw, "punpcklbw")
NATIVE_BINARY(HwPunpckhbw, "punpckhbw")
NATIVE_BINARY(HwPunpcklwd, "punpcklwd")
NATIVE_BINARY(HwPunpckhwd, "punpckhwd")
NATIVE_BINARY(HwPunpckldq, "punpckldq")
NATIVE_BINARY(HwPunpckhdq, "punpckhdq")
NATIVE_BINARY(HwPand, "pand")
NATIVE_BINARY(HwPandn, "pandn")
NATIVE_BINARY(HwPor, "por")
NATIVE_BINARY(HwPxor, "pxor")
// Shifts by the register count.
NATIVE_BINARY(HwPsllw, "psllw")
NATIVE_BINARY(HwPslld, "pslld")
NATIVE_BINARY(HwPsllq, "psllq")
NATIVE_BINARY(HwPsrlw, "psrlw")
NATIVE_BINARY(HwPsrld, "psrld")
NATIVE_BINARY(HwPsrlq, "psrlq")
NATIVE_BINARY(HwPsraw, "psraw")
NATIVE_BINARY(HwPsrad, "psrad")

struct Binary {
  const char* name;
  uint64_t (*hw)(uint64_t, uint64_t);
  uint64_t (*model)(uint64_t, uint64_t);
};

const Binary kBinary[] = {
    {"paddb", HwPaddb, mmx::Add<uint8_t>},
    {"paddw", HwPaddw, mmx::Add<uint16_t>},
    {"paddd", HwPaddd, mmx::Add<uint32_t>},
    {"paddq", HwPaddq, mmx::Add<uint64_t>},
    {"psubb", HwPsubb, mmx::Sub<uint8_t>},
    {"psubw", HwPsubw, mmx::Sub<uint16_t>},
    {"psubd", HwPsubd, mmx::Sub<uint32_t>},
    {"psubq", HwPsubq, mmx::Sub<uint64_t>},
    {"paddsb", HwPaddsb, mmx::AddSat<int8_t>},
    {"paddsw", HwPaddsw, mmx::AddSat<int16_t>},
    {"paddusb", HwPaddusb, mmx::AddSat<uint8_t>},
    {"paddusw", HwPaddusw, mmx::AddSat<uint16_t>},
    {"psubsb", HwPsubsb, mmx::SubSat<int8_t>},
    {"psubsw", HwPsubsw, mmx::SubSat<int16_t>},
    {"psubusb", HwPsubusb, mmx::SubSat<uint8_t>},
    {"psubusw", HwPsubusw, mmx::SubSat<uint16_t>},
    {"pcmpeqb", HwPcmpeqb, mmx::CmpEq<uint8_t>},
    {"pcmpeqw", HwPcmpeqw, mmx::CmpEq<uint16_t>},
    {"pcmpeqd", HwPcmpeqd, mmx::CmpEq<uint32_t>},
    {"pcmpgtb", HwPcmpgtb, mmx::CmpGt<int8_t>},
    {"pcmpgtw", HwPcmpgtw, mmx::CmpGt<int16_t>},
    {"pcmpgtd", HwPcmpgtd, mmx::CmpGt<int32_t>},
    {"pmullw", HwPmullw, mmx::MulLo16},
    {"pmulhw", HwPmulhw, mmx::MulHi16<int16_t>},
    {"pmulhuw", HwPmulhuw, mmx::MulHi16<uint16_t>},
    {"pmaddwd", HwPmaddwd, mmx::MAddWd},
    {"pmuludq", HwPmuludq, mmx::MulUDq},
    {"pavgb", HwPavgb, mmx::Avg<uint8_t>},
    {"pavgw", HwPavgw, mmx::Avg<uint16_t>},
    {"pmaxub", HwPmaxub, mmx::Max<uint8_t>},
    {"pmaxsw", HwPmaxsw, mmx::Max<int16_t>},
    {"pminub", HwPminub, mmx::Min<uint8_t>},
    {"pminsw", HwPminsw, mmx::Min<int16_t>},
    {"psadbw", HwPsadbw, mmx::SadBw},
    {"packsswb", HwPacksswb, mmx::Pack<int16_t, int8_t>},
    {"packssdw", HwPackssdw, mmx::Pack<int32_t, int16_t>},
    {"packuswb", HwPackuswb, mmx::Pack<int16_t, uint8_t>},
    {"punpcklbw", HwPunpcklbw, [](uint64_t d, uint64_t s) { return mmx::Unpck<uint8_t>(d, s, false); }},
    {"punpckhbw", HwPunpckhbw, [](uint64_t d, uint64_t s) { return mmx::Unpck<uint8_t>(d, s, true); }},
    {"punpcklwd", HwPunpcklwd, [](uint64_t d, uint64_t s) { return mmx::Unpck<uint16_t>(d, s, false); }},
    {"punpckhwd", HwPunpckhwd, [](uint64_t d, uint64_t s) { return mmx::Unpck<uint16_t>(d, s, true); }},
    {"punpckldq", HwPunpckldq, [](uint64_t d, uint64_t s) { return mmx::Unpck<uint32_t>(d, s, false); }},
    {"punpckhdq", HwPunpckhdq, [](uint64_t d, uint64_t s) { return mmx::Unpck<uint32_t>(d, s, true); }},
    // The lifter emits the bitwise forms inline (EmitMmx); the same
    // expressions here.
    {"pand", HwPand, [](uint64_t d, uint64_t s) { return d & s; }},
    {"pandn", HwPandn, [](uint64_t d, uint64_t s) { return ~d & s; }},
    {"por", HwPor, [](uint64_t d, uint64_t s) { return d | s; }},
    {"pxor", HwPxor, [](uint64_t d, uint64_t s) { return d ^ s; }},
    {"psllw mm", HwPsllw, [](uint64_t d, uint64_t s) { return mmx::Shift<uint16_t>(d, s, mmx::kShl); }},
    {"pslld mm", HwPslld, [](uint64_t d, uint64_t s) { return mmx::Shift<uint32_t>(d, s, mmx::kShl); }},
    {"psllq mm", HwPsllq, [](uint64_t d, uint64_t s) { return mmx::Shift<uint64_t>(d, s, mmx::kShl); }},
    {"psrlw mm", HwPsrlw, [](uint64_t d, uint64_t s) { return mmx::Shift<uint16_t>(d, s, mmx::kShr); }},
    {"psrld mm", HwPsrld, [](uint64_t d, uint64_t s) { return mmx::Shift<uint32_t>(d, s, mmx::kShr); }},
    {"psrlq mm", HwPsrlq, [](uint64_t d, uint64_t s) { return mmx::Shift<uint64_t>(d, s, mmx::kShr); }},
    {"psraw mm", HwPsraw, [](uint64_t d, uint64_t s) { return mmx::Shift<uint16_t>(d, s, mmx::kSar); }},
    {"psrad mm", HwPsrad, [](uint64_t d, uint64_t s) { return mmx::Shift<uint32_t>(d, s, mmx::kSar); }},
};

// A shift count for the register forms: small ones, the lane widths and
// their neighbours, and random 64-bit values.
uint64_t ShiftCount() {
  switch (g_rng() % 3) {
    case 0: return g_rng() % 70;
    case 1: {
      static const uint64_t kEdges[] = {15, 16, 17, 31, 32, 33, 63, 64, 65, 255, 256,
                                        0x100000000ull, ~uint64_t(0)};
      return kEdges[g_rng() % (sizeof kEdges / sizeof kEdges[0])];
    }
    default: return g_rng();
  }
}

void TestBinary() {
  for (const Binary& op : kBinary) {
    const bool shift = std::strchr(op.name, ' ') != nullptr;
    for (int t = 0; t < 200000; t++) {
      uint64_t d = Operand(), s = shift ? ShiftCount() : Operand();
      uint64_t hw = op.hw(d, s), model = op.model(d, s);
      Check(hw == model, op.name, d, s, hw, model);
    }
  }
}

// ---- immediate forms -----------------------------------------------------------

template <int N>
uint64_t HwShiftImm(uint64_t d, int which) {
  uint64_t r = d;
  switch (which) {
#define SHIFT_IMM(k, insn) \
  case k: asm volatile("movq %1, %%mm0\n " insn " %2, %%mm0\n movq %%mm0, %0\n emms" : "=r"(r) : "r"(d), "i"(N) : "mm0"); break;
    SHIFT_IMM(0, "psllw") SHIFT_IMM(1, "pslld") SHIFT_IMM(2, "psllq")
    SHIFT_IMM(3, "psrlw") SHIFT_IMM(4, "psrld") SHIFT_IMM(5, "psrlq")
    SHIFT_IMM(6, "psraw") SHIFT_IMM(7, "psrad")
#undef SHIFT_IMM
  }
  return r;
}
uint64_t ModelShift(uint64_t d, uint64_t n, int which) {
  switch (which) {
    case 0: return mmx::Shift<uint16_t>(d, n, mmx::kShl);
    case 1: return mmx::Shift<uint32_t>(d, n, mmx::kShl);
    case 2: return mmx::Shift<uint64_t>(d, n, mmx::kShl);
    case 3: return mmx::Shift<uint16_t>(d, n, mmx::kShr);
    case 4: return mmx::Shift<uint32_t>(d, n, mmx::kShr);
    case 5: return mmx::Shift<uint64_t>(d, n, mmx::kShr);
    case 6: return mmx::Shift<uint16_t>(d, n, mmx::kSar);
    default: return mmx::Shift<uint32_t>(d, n, mmx::kSar);
  }
}
template <int N>
void ShiftImmCount() {
  static const char* kNames[] = {"psllw imm", "pslld imm", "psllq imm", "psrlw imm",
                                 "psrld imm", "psrlq imm", "psraw imm", "psrad imm"};
  for (int which = 0; which < 8; which++)
    for (int t = 0; t < 2000; t++) {
      uint64_t d = Operand();
      uint64_t hw = HwShiftImm<N>(d, which), model = ModelShift(d, N, which);
      Check(hw == model, kNames[which], d, N, hw, model);
    }
}
template <int... N>
void ShiftImm(std::integer_sequence<int, N...>) {
  (ShiftImmCount<N>(), ...);
}

template <int Imm>
void ImmForms() {
  for (int t = 0; t < 2000; t++) {
    uint64_t d = Operand(), s = Operand();
    uint64_t hw;
    asm volatile("movq %1, %%mm1\n pshufw %2, %%mm1, %%mm0\n movq %%mm0, %0\n emms"
                 : "=r"(hw) : "r"(s), "i"(Imm) : "mm0", "mm1");
    Check(hw == mmx::ShufW(s, Imm), "pshufw", Imm, s, hw, mmx::ShufW(s, Imm));

    uint32_t w;
    asm volatile("movq %1, %%mm0\n pextrw %2, %%mm0, %0\n emms"
                 : "=r"(w) : "r"(s), "i"(Imm) : "mm0");
    Check(w == mmx::ExtrW(s, Imm), "pextrw", Imm, s, w, mmx::ExtrW(s, Imm));

    const uint32_t v = uint32_t(g_rng());
    asm volatile("movq %1, %%mm0\n pinsrw %3, %2, %%mm0\n movq %%mm0, %0\n emms"
                 : "=r"(hw) : "r"(d), "r"(v), "i"(Imm) : "mm0");
    Check(hw == mmx::InsrW(d, v, Imm), "pinsrw", d, v, hw, mmx::InsrW(d, v, Imm));
  }
}
template <int... Imm>
void Imms(std::integer_sequence<int, Imm...>) {
  (ImmForms<Imm * 37 % 256>(), ...);  // 40 immediates spread over 0-255
}

void TestMoves() {
  for (int t = 0; t < 200000; t++) {
    const uint64_t s = Operand(), mask = Operand();
    uint32_t m;
    asm volatile("movq %1, %%mm0\n pmovmskb %%mm0, %0\n emms" : "=r"(m) : "r"(s) : "mm0");
    Check(m == mmx::MovMskB(s), "pmovmskb", 0, s, m, mmx::MovMskB(s));

    // MASKMOVQ over a buffer of random bytes: the same bytes stored.
    uint8_t hw_buf[8], model_buf[8];
    const uint64_t fill = g_rng();
    std::memcpy(hw_buf, &fill, 8);
    std::memcpy(model_buf, &fill, 8);
    asm volatile("movq %1, %%mm0\n movq %2, %%mm1\n maskmovq %%mm1, %%mm0\n emms"
                 : : "D"(hw_buf), "r"(s), "r"(mask) : "mm0", "mm1", "memory");
    mmx::MaskMov(model_buf, uint64_t(0), s, mask);
    uint64_t hw, model;
    std::memcpy(&hw, hw_buf, 8);
    std::memcpy(&model, model_buf, 8);
    Check(hw == model, "maskmovq", s, mask, hw, model);
  }
}

// ---- conversions ---------------------------------------------------------------

uint32_t Float() {
  static const uint32_t kEdges[] = {
      0x00000000, 0x80000000, 0x00000001, 0x807FFFFF, 0x3F800000, 0xBF800000,
      0x3FC00000, 0x40200000, 0xC0200000, 0x3F000000, 0xBF000000, 0x4EFFFFFF,
      0x4F000000, 0xCF000000, 0xCF000001, 0x4E800000, 0x7F800000, 0xFF800000,
      0x7FC00000, 0x7FA00001, 0xFFC12345, 0x7F7FFFFF, 0xFF7FFFFF};
  switch (g_rng() % 3) {
    case 0: return kEdges[g_rng() % (sizeof kEdges / sizeof kEdges[0])];
    case 1: return uint32_t(0x3F800000 + (g_rng() % (32u << 23)) - (8u << 23)) |
                   uint32_t(g_rng() & 1) << 31;  // about 2^-8 .. 2^32
    default: return uint32_t(g_rng());
  }
}
uint64_t Double() {
  static const uint64_t kEdges[] = {
      0, 0x8000000000000000ull, 1, 0x3FF0000000000000ull, 0x3FF8000000000000ull,
      0x4004000000000000ull, 0xC004000000000000ull, 0x41DFFFFFFFC00000ull,
      0x41DFFFFFFFE00000ull, 0x41E0000000000000ull, 0xC1E0000000000000ull,
      0xC1E0000000100000ull, 0xC1E0000000200000ull, 0x7FF0000000000000ull,
      0x7FF8000000000000ull, 0x7FF0000000000001ull, 0x000FFFFFFFFFFFFFull};
  switch (g_rng() % 3) {
    case 0: return kEdges[g_rng() % (sizeof kEdges / sizeof kEdges[0])];
    case 1: return (0x3FF0000000000000ull + (g_rng() % (40ull << 52)) - (8ull << 52)) |
                   (g_rng() & 1) << 63;
    default: return g_rng();
  }
}
// All exceptions masked; random rounding, DAZ, FTZ and sticky flags.
uint32_t Mxcsr() {
  return 0x1F80u | uint32_t(g_rng() & 0x3F) | uint32_t(g_rng() & 3) << 13 |
         (g_rng() % 4 == 0 ? 0x40u : 0) | (g_rng() % 4 == 0 ? 0x8000u : 0);
}

struct Native {
  alignas(16) uint8_t x[16];  // xmm0 in/out
  uint64_t mm;                // mm0 in/out
  uint32_t mxcsr, host;
};

// Each runs the host instruction on n.x / n.mm under n.mxcsr.
#define NATIVE_CVT(name, body)                                                         \
  void name(Native& n) {                                                              \
    asm volatile("stmxcsr %3\n ldmxcsr %2\n movdqa %0, %%xmm0\n movq %1, %%mm0\n"   \
                 " " body "\n movdqa %%xmm0, %0\n movq %%mm0, %1\n stmxcsr %2\n"      \
                 " ldmxcsr %3\n emms"                                                 \
                 : "+m"(n.x), "+m"(n.mm), "+m"(n.mxcsr), "+m"(n.host)                   \
                 :                                                                     \
                 : "xmm0", "mm0");                                                     \
  }
NATIVE_CVT(HwCvtps2pi, "cvtps2pi %%xmm0, %%mm0")
NATIVE_CVT(HwCvttps2pi, "cvttps2pi %%xmm0, %%mm0")
NATIVE_CVT(HwCvtpd2pi, "cvtpd2pi %%xmm0, %%mm0")
NATIVE_CVT(HwCvttpd2pi, "cvttpd2pi %%xmm0, %%mm0")
NATIVE_CVT(HwCvtpi2ps, "cvtpi2ps %%mm0, %%xmm0")
NATIVE_CVT(HwCvtpi2pd, "cvtpi2pd %%mm0, %%xmm0")
#undef NATIVE_CVT

// The lifter's code for each (EmitMmx), on the model.
void ModelCvt(int which, recomp::Ctx& c, recomp::Xmm& x, uint64_t& mm) {
  namespace sse = recomp::sse;
  recomp::Xmm t{};
  switch (which) {
    case 0: case 1:  // cvt(t)ps2pi
      sse::SetLane<uint64_t>(t, 0, sse::Lane<uint64_t>(x, 0));
      sse::CvtToDq(c, false, which == 1, t, t, 0);
      mm = sse::Lane<uint64_t>(t, 0);
      break;
    case 2: case 3:  // cvt(t)pd2pi
      sse::CvtToDq(c, true, which == 3, t, x, 0);
      mm = sse::Lane<uint64_t>(t, 0);
      break;
    case 4:  // cvtpi2ps
      sse::SetLane<uint64_t>(t, 0, mm);
      sse::Cvtdq2ps(c, t, t, 0);
      sse::SetLane<uint64_t>(x, 0, sse::Lane<uint64_t>(t, 0));
      break;
    default:  // cvtpi2pd
      sse::SetLane<uint64_t>(t, 0, mm);
      x = sse::Cvtdq2pd(t);
      break;
  }
}

void TestConversions() {
  static void (*const kHw[])(Native&) = {HwCvtps2pi, HwCvttps2pi, HwCvtpd2pi,
                                         HwCvttpd2pi, HwCvtpi2ps, HwCvtpi2pd};
  static const char* kNames[] = {"cvtps2pi", "cvttps2pi", "cvtpd2pi",
                                 "cvttpd2pi", "cvtpi2ps", "cvtpi2pd"};
  for (int which = 0; which < 6; which++)
    for (int t = 0; t < 300000; t++) {
      Native n{};
      if (which < 2) {
        for (int i = 0; i < 4; i++) {
          uint32_t f = Float();
          std::memcpy(n.x + 4 * i, &f, 4);
        }
      } else {
        for (int i = 0; i < 2; i++) {
          uint64_t d = Double();
          std::memcpy(n.x + 8 * i, &d, 8);
        }
      }
      if (which >= 4) {
        n.mm = Operand();
        for (int i = 0; i < 16; i++) n.x[i] = uint8_t(g_rng());
      } else {
        n.mm = g_rng();
      }
      n.mxcsr = Mxcsr();
      recomp::Ctx c{};
      c.mxcsr = n.mxcsr;
      recomp::Xmm x;
      std::memcpy(x.b, n.x, 16);
      uint64_t mm = n.mm;
      const uint64_t in = which < 4 ? recomp::sse::Lane<uint64_t>(x, 0) : n.mm;
      ModelCvt(which, c, x, mm);
      kHw[which](n);
      const bool same = std::memcmp(n.x, x.b, 16) == 0 && n.mm == mm && n.mxcsr == c.mxcsr;
      Check(same, kNames[which], in, c.mxcsr, n.mm, mm);
      if (!same && g_failures <= 40)
        std::printf("  mxcsr hw %08X model %08X\n", n.mxcsr, c.mxcsr);
    }
}

// ---- x87 state ------------------------------------------------------------------

bool UnderRosetta() {
#if defined(__APPLE__)
  int translated = 0;
  size_t n = sizeof translated;
  if (sysctlbyname("sysctl.proc_translated", &translated, &n, nullptr, 0) == 0)
    return translated == 1;
#endif
  return false;
}

// FNSAVE (32-bit protected-mode layout): FIP, FCS/FOP, FDP and FDS
// (bytes 12-27) are the pointers, which the model zeroes (fxsave_test).
bool SameSave(const uint8_t* a, const uint8_t* b) {
  return std::memcmp(a, b, 12) == 0 && std::memcmp(a + 28, b + 28, 108 - 28) == 0;
}
// FXSAVE, x87 part: FOP (6-7) and the pointers (8-23) skipped likewise;
// MXCSR (24-31) is not the x87 state.
bool SameFxsave(const uint8_t* a, const uint8_t* b) {
  return std::memcmp(a, b, 6) == 0 && std::memcmp(a + 32, b + 32, 160 - 32) == 0;
}

void TestState() {
  for (int t = 0; t < 20000; t++) {
    const uint64_t v = Operand();
    const int reg = int(g_rng() % 8);  // physical register R<reg> = MM<reg>
    const int depth = int(g_rng() % 8);  // values pushed before the MMX code
    alignas(16) uint8_t start[108], hw_save[2][108], hw_fx[2][512];
    // Native: a stack of `depth` values, saved as the start image.
    asm volatile("fninit");
    for (int i = 0; i < depth; i++) asm volatile("fldpi");
    asm volatile("fnsave (%0)" : : "r"(start) : "memory");
    for (int e = 0; e < 2; e++) {
      asm volatile("frstor (%0)" : : "r"(start) : "memory");
      switch (reg) {
#define MOVQ_TO(k) case k: asm volatile("movq %0, %%mm" #k : : "r"(v) : "mm" #k); break;
        MOVQ_TO(0) MOVQ_TO(1) MOVQ_TO(2) MOVQ_TO(3)
        MOVQ_TO(4) MOVQ_TO(5) MOVQ_TO(6) MOVQ_TO(7)
#undef MOVQ_TO
      }
      if (e) asm volatile("emms");
      asm volatile("fxsave (%0)\n fnsave (%1)" : : "r"(hw_fx[e]), "r"(hw_save[e]) : "memory");
    }
    asm volatile("fninit");

    for (int e = 0; e < 2; e++) {
      recomp::Ctx c{};
      recomp::x87::Restore(c.fpu, start);
      mmx::Enter(c);
      mmx::Set(c, reg, v);
      if (e) mmx::Emms(c);
      alignas(16) uint8_t model_fx[512], model_save[108];
      std::memcpy(model_fx, hw_fx[e], 512);
      recomp::x87::FxSave(c.fpu, model_fx);
      recomp::x87::Save(c.fpu, model_save);
      const char* what = e ? "state after emms (fnsave)" : "state after movq (fnsave)";
      Check(SameSave(hw_save[e], model_save), what, depth, v, reg, 0);
      what = e ? "state after emms (fxsave)" : "state after movq (fxsave)";
      Check(SameFxsave(hw_fx[e], model_fx), what, depth, v, reg, 0);
    }
  }
}

}  // namespace

int main() {
  TestBinary();
  ShiftImm(std::make_integer_sequence<int, 70>{});
  Imms(std::make_integer_sequence<int, 40>{});
  TestMoves();
  TestConversions();
  if (UnderRosetta())
    std::printf("x87 state checks skipped: running under Rosetta 2\n");
  else
    TestState();
  std::printf("%ld checks, %d failures\n", g_checks, g_failures);
  std::printf("mmx: %s\n", g_failures ? "FAILED" : "ok");
  return g_failures ? 1 : 0;
}
