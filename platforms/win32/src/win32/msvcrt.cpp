// msvcrt.dll for 32-bit guests: the Visual C++ 6 runtime DLL that older
// DLLs import (rather than linking a CRT statically). What has been seen
// imported so far: the heap, a few string functions, stdio reading of
// files, time, _initterm, _ftol and the _adjust_fdiv flag.
//
// The heap is the process heap. A FILE* is a guest pointer to a small
// opaque block; code that reads the VC6 FILE fields directly (the getc
// macro) would need the real layout, which nothing seen does.

#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string>

#include "runtime/log.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "x86/runtime.h"
#include "x86/x87.h"

namespace recomp::hle {
namespace {

constexpr const char* kDll = "msvcrt.dll";

// int _adjust_fdiv: nonzero asks for the Pentium FDIV workaround. No
// host has the bug.
DataExport g_adjust_fdiv(kDll, "_adjust_fdiv", 4);

HLE(kDll, malloc, kCdecl) { return HeapAllocProcess(Arg(c, 0) ? Arg(c, 0) : 1, false); }
HLE(kDll, calloc, kCdecl) {
  uint64_t n = uint64_t(Arg(c, 0)) * Arg(c, 1);
  if (n > 0xFFFFFFFFull) return 0;
  return HeapAllocProcess(n ? n : 1, true);
}
HLE(kDll, realloc, kCdecl) {
  if (!Arg(c, 0)) return HeapAllocProcess(Arg(c, 1) ? Arg(c, 1) : 1, false);
  if (!Arg(c, 1)) {
    HeapFreeProcess(Arg(c, 0));
    return 0;
  }
  return HeapReAllocProcess(Arg(c, 0), Arg(c, 1));
}
HLE(kDll, free, kCdecl) {
  if (Arg(c, 0)) HeapFreeProcess(Arg(c, 0));
  return 0;
}

HLE(kDll, strchr, kCdecl) {
  gaddr s = Arg(c, 0);
  uint8_t ch = uint8_t(Arg(c, 1));
  for (;; s++) {
    uint8_t v = Rd8(s);
    if (v == ch) return s;
    if (!v) return 0;
  }
}
HLE(kDll, strncpy, kCdecl) {
  gaddr d = Arg(c, 0), src = Arg(c, 1);
  uint32_t n = Arg(c, 2), i = 0;
  for (; i < n && Rd8(src + i); i++) Wr8(d + i, Rd8(src + i));
  for (; i < n; i++) Wr8(d + i, 0);
  return d;
}

// time_t time(time_t*): 32-bit time_t in this runtime.
HLE(kDll, time, kCdecl) {
  uint32_t t = uint32_t(std::time(nullptr));
  if (Arg(c, 0)) Wr32(Arg(c, 0), t);
  return t;
}

// _initterm(begin, end): calls each non-null function pointer.
HLE(kDll, _initterm, kCdecl) {
  for (gaddr p = Arg(c, 0), end = Arg(c, 1); p < end; p += 4)
    if (gaddr fn = Rd32(p)) CallGuest(c, fn, {});
  return 0;
}

// _ftol: ST0 to a 64-bit integer, truncated, popped; EDX:EAX, as the
// original's fistp under RC=chop does (the x87 model gives the same
// out-of-range results and flags).
HLE(kDll, _ftol, kCdecl) {
  x87::State& s = c.fpu;
  const uint16_t cw = x87::ControlWord(s);
  x87::SetControlWord(s, cw | 0x0C00);
  const int64_t v = x87::StoreI64(s);
  x87::Pop(s);
  x87::SetControlWord(s, cw);
  c.edx = uint32_t(uint64_t(v) >> 32);
  return uint32_t(v);
}

// ---- stdio ---------------------------------------------------------------------

std::mutex g_mu;
std::map<gaddr, std::FILE*> g_files;

std::FILE* File(gaddr fp, const char* api) {
  std::lock_guard<std::mutex> l(g_mu);
  auto it = g_files.find(fp);
  if (it == g_files.end()) FatalF("msvcrt %s on an unknown FILE* %08X", api, uint32_t(fp));
  return it->second;
}

// fopen(path, mode): reading only goes to the install; writes go to the
// overlay (HostPath).
HLE(kDll, fopen, kCdecl) {
  std::string path = GStr(Arg(c, 0)), mode = GStr(Arg(c, 1));
  bool write = mode.find_first_of("wa+") != std::string::npos;
  std::string host = HostPath(path, write ? (mode[0] == 'r' ? PathAccess::kUpdate
                                                            : PathAccess::kWrite)
                                          : PathAccess::kRead);
  std::string m;
  for (char ch : mode)
    if (ch == 'r' || ch == 'w' || ch == 'a' || ch == '+' || ch == 'b') m += ch;
  if (m.find('b') == std::string::npos) m += 'b';
  std::FILE* f = std::fopen(host.c_str(), m.c_str());
  if (!f) return 0;
  gaddr fp = HeapAllocProcess(32, true);
  std::lock_guard<std::mutex> l(g_mu);
  g_files[fp] = f;
  return fp;
}

HLE(kDll, fclose, kCdecl) {
  std::FILE* f = File(Arg(c, 0), "fclose");
  {
    std::lock_guard<std::mutex> l(g_mu);
    g_files.erase(Arg(c, 0));
  }
  HeapFreeProcess(Arg(c, 0));
  return std::fclose(f) == 0 ? 0 : uint32_t(-1);
}

// fread(buf, size, count, fp)
HLE(kDll, fread, kCdecl) {
  uint32_t size = Arg(c, 1), count = Arg(c, 2);
  if (!size || !count) return 0;
  std::FILE* f = File(Arg(c, 3), "fread");
  uint64_t bytes = uint64_t(size) * count;
  if (!EnsureReadable(c, Arg(c, 0), 0)) return 0;
  return uint32_t(std::fread(GPtr(Arg(c, 0)), size, size_t(bytes / size), f));
}

// fseek(fp, offset, origin)
HLE(kDll, fseek, kCdecl) {
  std::FILE* f = File(Arg(c, 0), "fseek");
  return std::fseek(f, long(int32_t(Arg(c, 1))), int(Arg(c, 2))) == 0 ? 0 : uint32_t(-1);
}

}  // namespace
}  // namespace recomp::hle
