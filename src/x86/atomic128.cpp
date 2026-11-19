// The 16-byte compare-exchange of CMPXCHG16B (lifted.h). The compiler's
// own atomic on a 16-byte integer, which every supported host does in one
// instruction (CMPXCHG16B with -mcx16 on x86-64; CASP or LDAXP/STLXP on
// arm64), so it is lock-free and atomic against the guest's other threads.

#include <cstdint>
#include <cstring>
#include <mutex>

#include "x86/lifted.h"

namespace recomp::lift {

bool Cas128(uint8_t* p, uint64_t* lo, uint64_t* hi, uint64_t new_lo, uint64_t new_hi) {
  using U128 = unsigned __int128;
  U128 e = U128(*hi) << 64 | *lo;
  const U128 n = U128(new_hi) << 64 | new_lo;
  const bool ok = __atomic_compare_exchange_n(reinterpret_cast<U128*>(p), &e, n, false,
                                              __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  *lo = uint64_t(e);
  *hi = uint64_t(e >> 64);
  return ok;
}

// CMPXCHG8B on an operand that is not 8-byte aligned: host atomics need
// alignment, so such operands are serialised against each other here.
bool Cas64Unaligned(uint8_t* p, uint32_t* eax, uint32_t* edx, uint32_t ebx, uint32_t ecx) {
  static std::mutex mu;
  std::lock_guard<std::mutex> l(mu);
  uint64_t v;
  std::memcpy(&v, p, 8);
  if (v == (uint64_t(*edx) << 32 | *eax)) {
    const uint64_t n = uint64_t(ecx) << 32 | ebx;
    std::memcpy(p, &n, 8);
    return true;
  }
  *eax = uint32_t(v);
  *edx = uint32_t(v >> 32);
  return false;
}

}  // namespace recomp::lift
