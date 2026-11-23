// VCRUNTIME140.dll for x64 guests: the memory and string primitives, the
// type_info and std::exception helpers. Its exception handling
// (_CxxThrowException, __CxxFrameHandler4, __C_specific_handler) belongs
// to the x64 unwinder (ARCHITECTURE.md, "64-bit guests").

#include <cstring>

#include "runtime/log.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "x86/runtime.h"

namespace recomp::hle {
namespace {

const char* S(gaddr p) { return g_proc->mem.ptr<const char>(p); }

}  // namespace

// ---- memory ---------------------------------------------------------------
// Guest memory is one host mapping, so these are the host's on it.

HLE64("vcruntime140.dll", memcpy, kCdecl) {
  std::memmove(GPtr(Arg(c, 0)), GPtr(Arg(c, 1)), Arg(c, 2));
  return Arg(c, 0);
}
HLE64("vcruntime140.dll", memmove, kCdecl) {
  std::memmove(GPtr(Arg(c, 0)), GPtr(Arg(c, 1)), Arg(c, 2));
  return Arg(c, 0);
}
HLE64("vcruntime140.dll", memset, kCdecl) {
  std::memset(GPtr(Arg(c, 0)), int(Arg(c, 1)), Arg(c, 2));
  return Arg(c, 0);
}
HLE64("vcruntime140.dll", memcmp, kCdecl) {
  int r = std::memcmp(GPtr(Arg(c, 0)), GPtr(Arg(c, 1)), Arg(c, 2));
  return uint32_t(r < 0 ? -1 : r > 0 ? 1 : 0);
}
HLE64("vcruntime140.dll", memchr, kCdecl) {
  const uint8_t* p = GPtr(Arg(c, 0));
  const void* r = std::memchr(p, int(uint8_t(Arg(c, 1))), Arg(c, 2));
  return r ? Arg(c, 0) + gaddr(static_cast<const uint8_t*>(r) - p) : 0;
}

// ---- strings ----------------------------------------------------------------

HLE64("vcruntime140.dll", strchr, kCdecl) {
  const char* p = S(Arg(c, 0));
  const char* r = std::strchr(p, int(char(Arg(c, 1))));
  return r ? Arg(c, 0) + gaddr(r - p) : 0;
}
HLE64("vcruntime140.dll", strrchr, kCdecl) {
  const char* p = S(Arg(c, 0));
  const char* r = std::strrchr(p, int(char(Arg(c, 1))));
  return r ? Arg(c, 0) + gaddr(r - p) : 0;
}
HLE64("vcruntime140.dll", strstr, kCdecl) {
  const char* p = S(Arg(c, 0));
  const char* r = std::strstr(p, S(Arg(c, 1)));
  return r ? Arg(c, 0) + gaddr(r - p) : 0;
}

namespace {
// Wide strings: wchar_t is 16 bits on Windows.
uint64_t WcsLen(gaddr s) {
  uint64_t n = 0;
  while (Rd16(s + 2 * n)) n++;
  return n;
}
}  // namespace

HLE64("vcruntime140.dll", wcsrchr, kCdecl) {
  const gaddr s = Arg(c, 0);
  const uint16_t ch = uint16_t(Arg(c, 1));
  const uint64_t n = WcsLen(s);
  for (uint64_t i = n + 1; i-- > 0;)  // the terminator matches a 0 search
    if (Rd16(s + 2 * i) == ch) return s + 2 * i;
  return 0;
}
HLE64("vcruntime140.dll", wcsstr, kCdecl) {
  const gaddr s = Arg(c, 0), sub = Arg(c, 1);
  const uint64_t n = WcsLen(s), m = WcsLen(sub);
  if (!m) return s;
  for (uint64_t i = 0; i + m <= n; i++) {
    uint64_t j = 0;
    while (j < m && Rd16(s + 2 * (i + j)) == Rd16(sub + 2 * j)) j++;
    if (j == m) return s + 2 * i;
  }
  return 0;
}

// ---- setjmp / longjmp -------------------------------------------------------
// An imported setjmp returns 0, as a setjmp does when it is called. A
// longjmp back to it is not supported: the lifter resumes only at the
// returns of a setjmp it lifted itself (ARCHITECTURE.md). Libraries that
// longjmp only on errors (FreeType's rasteriser, for one) then run as long
// as nothing fails; a longjmp is fatal and names its target.
// The jmp_buf (_JUMP_BUFFER) gets the caller's Rip (0x50) and Rsp (0x10),
// so the fatal longjmp can say where it was going.
HLE64("vcruntime140.dll", __intrinsic_setjmp, kCdecl) {
  Wr64(Arg(c, 0) + 0x10, c.rsp + 8);
  Wr64(Arg(c, 0) + 0x50, Rd64(c.rsp));
  return 0;
}
HLE64("vcruntime140.dll", longjmp, kCdecl) {
  FatalF("longjmp(jmp_buf %llX, %d) to an imported setjmp: not supported (its return "
         "address is %llX)",
         (unsigned long long)Arg(c, 0), int32_t(Arg(c, 1)),
         (unsigned long long)Rd64(Arg(c, 0) + 0x50));
}

// ---- type_info ----------------------------------------------------------------
// __std_type_info_data {const char* _UndecoratedName; char _DecoratedName[];}
// (after type_info's vtable pointer). Types are equal when their decorated
// names are, past the leading '.'.

HLE64("vcruntime140.dll", __std_type_info_compare, kCdecl) {
  gaddr a = Arg(c, 0), b = Arg(c, 1);
  if (a == b) return 0;
  int r = std::strcmp(S(a + 8 + 1), S(b + 8 + 1));
  return uint32_t(r < 0 ? -1 : r > 0 ? 1 : 0);
}

// The list of undecorated names type_info::name() allocated: never built
// (name() is not provided), so nothing to free.
HLE64("vcruntime140.dll", __std_type_info_destroy_list, kCdecl) { return 0; }

// ---- std::exception -------------------------------------------------------------
// __std_exception_data {const char* _What; bool _DoFree;}: _DoFree means
// _What was allocated (copied) and is owned.

HLE64("vcruntime140.dll", __std_exception_copy, kCdecl) {
  gaddr from = Arg(c, 0), to = Arg(c, 1);
  gaddr what = Rd64(from);
  if (Rd8(from + 8) && what) {
    std::string s = S(what);
    gaddr copy = HeapAllocProcess(s.size() + 1, false);
    PutStr(copy, s);
    Wr64(to, copy);
    Wr8(to + 8, 1);
  } else {
    Wr64(to, what);
    Wr8(to + 8, 0);
  }
  return 0;
}

HLE64("vcruntime140.dll", __std_exception_destroy, kCdecl) {
  gaddr data = Arg(c, 0);
  if (Rd8(data + 8)) HeapFreeProcess(Rd64(data));
  Wr64(data, 0);
  Wr8(data + 8, 0);
  return 0;
}

HLE64("vcruntime140.dll", __std_terminate, kCdecl) { FatalF("guest called std::terminate()"); }
HLE64("vcruntime140.dll", _purecall, kCdecl) {
  FatalF("guest called a pure virtual function (_purecall)");
}

}  // namespace recomp::hle
