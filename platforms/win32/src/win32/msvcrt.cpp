// msvcrt.dll for 32-bit guests: the Visual C++ 6 runtime DLL that older
// DLLs import (rather than linking a CRT statically), and that MinGW
// builds of libraries import (FreeType, HarfBuzz, fontconfig and libxml2
// in Source's 2024 engine): the heap, strings, ctype in the C locale,
// math, stdio, the low-level I/O and file system functions, errno,
// threads, the CRT locks, _initterm, _ftol and the _adjust_fdiv flag.
// printf and its relatives are in ucrt_stdio.cpp (the UCRT's formatting,
// with 32-bit varargs).
//
// The heap is the process heap. A FILE* is a guest pointer to a small
// opaque block, the standard streams those of _iob; code that reads the
// FILE fields directly (VC6's getc macro) would need the real layout,
// which nothing seen does. Low-level descriptors (_open) are host file
// descriptors. Paths are guest paths, through HostPath: reads go to the
// install, writes to the overlay.

#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "win32/msvcrt.h"
#include "win32/win32.h"

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

HLE(kDll, strrchr, kCdecl) {
  gaddr s = Arg(c, 0), last = 0;
  uint8_t ch = uint8_t(Arg(c, 1));
  for (;; s++) {
    uint8_t v = Rd8(s);
    if (v == ch) last = s;
    if (!v) return last;
  }
}

// operator new(size_t) and operator delete(void*), the VC6 runtime's
// (it returns NULL on failure rather than throwing).
HLE_NAMED(kDll, "??2@YAPAXI@Z", OperatorNew, kCdecl) {
  return HeapAllocProcess(Arg(c, 0) ? Arg(c, 0) : 1, false);
}
HLE_NAMED(kDll, "??3@YAXPAX@Z", OperatorDelete, kCdecl) {
  if (Arg(c, 0)) HeapFreeProcess(Arg(c, 0));
  return 0;
}

// exit(code): the process ends as with ExitProcess.
HLE(kDll, exit, kCdecl) {
  std::fprintf(stderr, "msvcrt exit(%d)\n", int32_t(Arg(c, 0)));
  std::fflush(nullptr);
  std::_Exit(int(Arg(c, 0)));
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

// FILE _iob[3]: stdin, stdout and stderr, 32 bytes each (VC6's FILE).
gaddr g_iob = 0;
DataExport g_iob_export(kDll, "_iob", 3 * 32, [](gaddr a) { g_iob = a; });

std::FILE* File(gaddr fp, const char* api) {
  if (g_iob && fp >= g_iob && fp < g_iob + 3 * 32 && (fp - g_iob) % 32 == 0) {
    switch ((fp - g_iob) / 32) {
      case 0: return stdin;
      case 1: return stdout;
      default: return stderr;
    }
  }
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


// fp's host stream, for ucrt_stdio.cpp's fprintf.
}  // namespace
std::FILE* MsvcrtFile(gaddr fp, const char* api) { return File(fp, api); }
namespace {

// ---- errno -----------------------------------------------------------------------
// Each thread's errno is a guest int (_errno returns its address). Host
// errno values for what these functions report (ENOENT, EACCES, EEXIST,
// EINVAL, ERANGE, ...) are MSVC's.

gaddr ErrnoSlot() {
  static thread_local gaddr slot = 0;
  if (!slot) slot = HeapAllocProcess(4, true);
  return slot;
}
void SetErrno(int e) { Wr32(ErrnoSlot(), uint32_t(e)); }
// A host call's failure: -1 and its errno.
uint32_t Fail() {
  SetErrno(errno);
  return uint32_t(-1);
}

HLE(kDll, _errno, kCdecl) { return ErrnoSlot(); }

// strerror(e): the message, in a buffer per thread (as MSVC's).
HLE(kDll, strerror, kCdecl) {
  static thread_local gaddr buf = 0;
  if (!buf) buf = HeapAllocProcess(96, false);
  std::string m = std::strerror(int(Arg(c, 0)));
  PutStr(buf, m.substr(0, 95));
  return buf;
}
HLE(kDll, perror, kCdecl) {
  int e = int32_t(Rd32(ErrnoSlot()));
  std::string s = Arg(c, 0) ? GStr(Arg(c, 0)) : "";
  std::fprintf(stderr, "%s%s%s\n", s.c_str(), s.empty() ? "" : ": ", std::strerror(e));
  return 0;
}

// ---- process ------------------------------------------------------------------

HLE(kDll, abort, kCdecl) { FatalF("msvcrt abort() called (return address %08X)", Rd32(c.esp)); }
HLE(kDll, _amsg_exit, kCdecl) { FatalF("msvcrt _amsg_exit(%u): runtime error", uint32_t(Arg(c, 0))); }
HLE(kDll, _assert, kCdecl) {
  FatalF("assertion failed: %s, %s:%u", GStr(Arg(c, 0)).c_str(), GStr(Arg(c, 1)).c_str(),
         uint32_t(Arg(c, 2)));
}
// __setusermatherr(handler): the math functions here never call one.
HLE(kDll, __setusermatherr, kCdecl) { return 0; }

// getenv(name): the guest environment's value, in a copy kept per name
// (as long as the variable is not set again: MSVC's points into its
// environment copy, which the next _putenv may replace).
HLE(kDll, getenv, kCdecl) {
  std::string name = GStr(Arg(c, 0));
  std::string key = win32::Lower(name) + "=";
  for (const std::string& kv : EnvironmentVars()) {
    if (win32::Lower(kv.substr(0, key.size())) != key) continue;
    static std::mutex mu;
    static std::map<std::string, std::pair<std::string, gaddr>> kept;
    std::lock_guard<std::mutex> l(mu);
    std::string v = kv.substr(key.size());
    auto& [value, g] = kept[win32::Lower(name)];
    if (!g || value != v) {
      g = HeapAllocProcess(v.size() + 1, false);
      PutStr(g, v);
      value = v;
    }
    return g;
  }
  return 0;
}

// _beginthreadex(security, stack, start, arg, flags, unsigned* tid): a
// thread as CreateThread's (the start routine is __stdcall, as there).
HLE(kDll, _beginthreadex, kCdecl) {
  uint32_t tid;
  uint32_t h = NewThread(Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4) & 4, &tid);
  if (Arg(c, 5)) Wr32(Arg(c, 5), tid);
  return h;
}
HLE(kDll, _endthreadex, kCdecl) {
  GuestSyncFence();
  throw GuestThreadExit{uint32_t(Arg(c, 0))};
}

// _lock(n) / _unlock(n): the CRT's internal locks (recursive, as its
// critical sections are).
std::recursive_mutex& CrtLock(uint32_t n) {
  static std::recursive_mutex locks[64];
  if (n >= 64) FatalF("msvcrt _lock(%u): no such lock", n);
  return locks[n];
}
HLE(kDll, _lock, kCdecl) {
  CrtLock(Arg(c, 0)).lock();
  return 0;
}
HLE(kDll, _unlock, kCdecl) {
  CrtLock(Arg(c, 0)).unlock();
  return 0;
}

// setjmp/longjmp through the DLL: _setjmp3 returns 0, as a setjmp does
// when called; longjmp, which would return from it again, is not
// supported for an imported setjmp (the lifter resumes only at the
// returns of a setjmp it lifted, ARCHITECTURE.md): fatal, naming it.
// FreeType's rasteriser calls setjmp for every glyph and longjmps only
// when its cell pool overflows.
HLE(kDll, _setjmp3, kCdecl) { return 0; }
HLE(kDll, longjmp, kCdecl) {
  FatalF("msvcrt longjmp(jmp_buf %08X, %d) to an imported setjmp: not supported (return "
         "address %08X)",
         uint32_t(Arg(c, 0)), int32_t(Arg(c, 1)), Rd32(c.esp));
}

// ---- strings and memory ----------------------------------------------------------

HLE(kDll, strlen, kCdecl) { return uint32_t(GStr(Arg(c, 0)).size()); }
HLE(kDll, wcslen, kCdecl) {
  gaddr p = Arg(c, 0);
  uint32_t n = 0;
  while (Rd16(p + 2 * n)) n++;
  return n;
}
HLE(kDll, strcmp, kCdecl) {
  return uint32_t(std::strcmp(GStr(Arg(c, 0)).c_str(), GStr(Arg(c, 1)).c_str()));
}
HLE(kDll, strncmp, kCdecl) {
  return uint32_t(std::strncmp(g_proc->mem.ptr<const char>(Arg(c, 0)), g_proc->mem.ptr<const char>(Arg(c, 1)),
                               Arg(c, 2)));
}
HLE(kDll, strcpy, kCdecl) {
  std::string s = GStr(Arg(c, 1));
  std::memmove(GPtr(Arg(c, 0)), s.c_str(), s.size() + 1);
  return Arg(c, 0);
}
HLE(kDll, strcat, kCdecl) {
  std::string d = GStr(Arg(c, 0)), s = GStr(Arg(c, 1));
  std::memmove(GPtr(Arg(c, 0) + uint32_t(d.size())), s.c_str(), s.size() + 1);
  return Arg(c, 0);
}
// strncat(dst, src, n): at most n characters of src, then a NUL.
HLE(kDll, strncat, kCdecl) {
  std::string d = GStr(Arg(c, 0)), s = GStr(Arg(c, 1)).substr(0, Arg(c, 2));
  std::memmove(GPtr(Arg(c, 0) + uint32_t(d.size())), s.c_str(), s.size() + 1);
  return Arg(c, 0);
}
HLE(kDll, strstr, kCdecl) {
  std::string h = GStr(Arg(c, 0)), n = GStr(Arg(c, 1));
  size_t i = h.find(n);
  return i == std::string::npos ? 0 : Arg(c, 0) + uint32_t(i);
}
HLE(kDll, strpbrk, kCdecl) {
  std::string h = GStr(Arg(c, 0)), set = GStr(Arg(c, 1));
  size_t i = h.find_first_of(set);
  return i == std::string::npos ? 0 : Arg(c, 0) + uint32_t(i);
}
HLE(kDll, _strdup, kCdecl) {
  if (!Arg(c, 0)) return 0;
  std::string s = GStr(Arg(c, 0));
  gaddr d = HeapAllocProcess(s.size() + 1, false);
  PutStr(d, s);
  return d;
}
// _mbsrchr(str, ch): in the C locale (single-byte), strrchr.
HLE(kDll, _mbsrchr, kCdecl) {
  std::string s = GStr(Arg(c, 0));
  uint32_t ch = Arg(c, 1);
  if (ch == 0) return Arg(c, 0) + uint32_t(s.size());
  size_t i = s.rfind(char(ch));
  return i == std::string::npos ? 0 : Arg(c, 0) + uint32_t(i);
}
HLE(kDll, memcpy, kCdecl) {
  if (Arg(c, 2)) std::memmove(GPtr(Arg(c, 0)), GPtr(Arg(c, 1)), Arg(c, 2));
  return Arg(c, 0);
}
HLE(kDll, memmove, kCdecl) {
  if (Arg(c, 2)) std::memmove(GPtr(Arg(c, 0)), GPtr(Arg(c, 1)), Arg(c, 2));
  return Arg(c, 0);
}
HLE(kDll, memset, kCdecl) {
  if (Arg(c, 2)) std::memset(GPtr(Arg(c, 0)), int(Arg(c, 1)), Arg(c, 2));
  return Arg(c, 0);
}
HLE(kDll, memcmp, kCdecl) {
  if (!Arg(c, 2)) return 0;
  return uint32_t(std::memcmp(GPtr(Arg(c, 0)), GPtr(Arg(c, 1)), Arg(c, 2)));
}
HLE(kDll, memchr, kCdecl) {
  if (!Arg(c, 2)) return 0;
  const uint8_t* p = g_proc->mem.ptr<const uint8_t>(Arg(c, 0));
  const void* r = std::memchr(p, int(Arg(c, 1)), Arg(c, 2));
  return r ? Arg(c, 0) + uint32_t(static_cast<const uint8_t*>(r) - p) : 0;
}

// The C locale: ASCII classes and case.
HLE(kDll, isalpha, kCdecl) { return std::isalpha(int(Arg(c, 0)) & 0xFF) ? 0x103 : 0; }
HLE(kDll, isspace, kCdecl) { return std::isspace(int(Arg(c, 0)) & 0xFF) ? 0x8 : 0; }
HLE(kDll, isxdigit, kCdecl) { return std::isxdigit(int(Arg(c, 0)) & 0xFF) ? 0x80 : 0; }
HLE(kDll, tolower, kCdecl) {
  int ch = int32_t(Arg(c, 0));
  return uint32_t(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
}
HLE(kDll, toupper, kCdecl) {
  int ch = int32_t(Arg(c, 0));
  return uint32_t(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
}

// int __mb_cur_max: 1, the C locale's.
DataExport g_mb_cur_max(kDll, "__mb_cur_max", 4, [](gaddr a) { Wr32(a, 1); });

// setlocale(category, locale): only the C locale ("C", or "" for the
// user's, which is taken to be it); the name returned stays valid.
HLE(kDll, setlocale, kCdecl) {
  static gaddr name = 0;
  if (!name) {
    name = HeapAllocProcess(2, false);
    PutStr(name, "C");
  }
  if (!Arg(c, 1)) return name;
  std::string l = GStr(Arg(c, 1));
  return l.empty() || l == "C" ? name : 0;
}

// localeconv(): the C locale's struct lconv (ten char* fields, then the
// char ones, CHAR_MAX: not available).
HLE(kDll, localeconv, kCdecl) {
  static gaddr lc = 0;
  if (!lc) {
    lc = HeapAllocProcess(64, true);
    gaddr dot = HeapAllocProcess(2, false), empty = HeapAllocProcess(1, true);
    PutStr(dot, ".");
    for (int i = 0; i < 10; i++) Wr32(lc + 4 * i, uint32_t(i == 0 ? dot : empty));
    for (int i = 0; i < 8; i++) Wr8(lc + 40 + i, 127);
  }
  return lc;
}

// mbstowcs / wcstombs in the C locale: bytes are their code points.
HLE(kDll, mbstowcs, kCdecl) {
  std::string s = GStr(Arg(c, 1));
  uint32_t n = Arg(c, 2);
  if (!Arg(c, 0)) return uint32_t(s.size());
  uint32_t i = 0;
  for (; i < n && i < s.size(); i++) Wr16(Arg(c, 0) + 2 * i, uint8_t(s[i]));
  if (i < n) Wr16(Arg(c, 0) + 2 * i, 0);
  return i;
}
HLE(kDll, wcstombs, kCdecl) {
  gaddr w = Arg(c, 1);
  uint32_t n = Arg(c, 2), len = 0;
  while (Rd16(w + 2 * len)) {
    if (Rd16(w + 2 * len) > 0xFF) {
      SetErrno(EILSEQ);
      return uint32_t(-1);
    }
    len++;
  }
  if (!Arg(c, 0)) return len;
  uint32_t i = 0;
  for (; i < n && i < len; i++) Wr8(Arg(c, 0) + i, uint8_t(Rd16(w + 2 * i)));
  if (i < n) Wr8(Arg(c, 0) + i, 0);
  return i;
}

// ---- conversions ---------------------------------------------------------------

HLE(kDll, atoi, kCdecl) { return uint32_t(std::atoi(GStr(Arg(c, 0)).c_str())); }
// atof(str): the double in ST0.
HLE(kDll, atof, kCdecl) {
  double d = std::strtod(GStr(Arg(c, 0)).c_str(), nullptr);
  uint64_t b;
  std::memcpy(&b, &d, 8);
  x87::LoadF64(c.fpu, b);
  return 0;
}
// strtol and its relatives: *end (if asked) is the guest address the
// parse stopped at; ERANGE in errno on overflow.
template <typename T, typename F>
T StrTo(Ctx& c, F f) {
  std::string s = GStr(Arg(c, 0));
  char* end;
  errno = 0;
  T v = f(s.c_str(), &end, int(Arg(c, 2)));
  if (errno) SetErrno(errno);
  if (Arg(c, 1)) Wr32(Arg(c, 1), Arg(c, 0) + uint32_t(end - s.c_str()));
  return v;
}
HLE(kDll, strtol, kCdecl) {
  long v = StrTo<long>(c, [](const char* p, char** e, int b) { return std::strtol(p, e, b); });
  // 32-bit long: clamp as MSVC's does.
  if (v > INT32_MAX || v < INT32_MIN) {
    SetErrno(ERANGE);
    v = v > 0 ? INT32_MAX : INT32_MIN;
  }
  return uint32_t(int32_t(v));
}
HLE(kDll, strtoul, kCdecl) {
  std::string s = GStr(Arg(c, 0));
  bool neg = s.find_first_not_of(" \t\n\v\f\r") != std::string::npos &&
             s[s.find_first_not_of(" \t\n\v\f\r")] == '-';
  unsigned long v =
      StrTo<unsigned long>(c, [](const char* p, char** e, int b) { return std::strtoul(p, e, b); });
  if (!neg && v > UINT32_MAX) {
    SetErrno(ERANGE);
    v = UINT32_MAX;
  }
  return uint32_t(v);
}
HLE(kDll, _strtoi64, kCdecl) {
  long long v =
      StrTo<long long>(c, [](const char* p, char** e, int b) { return std::strtoll(p, e, b); });
  c.edx = uint32_t(uint64_t(v) >> 32);
  return uint32_t(v);
}
HLE(kDll, _strtoui64, kCdecl) {
  unsigned long long v = StrTo<unsigned long long>(
      c, [](const char* p, char** e, int b) { return std::strtoull(p, e, b); });
  c.edx = uint32_t(v >> 32);
  return uint32_t(v);
}
// _ultoa(value, buf, radix)
HLE(kDll, _ultoa, kCdecl) {
  uint32_t v = Arg(c, 0), radix = Arg(c, 2);
  if (radix < 2 || radix > 36) FatalF("msvcrt _ultoa: radix %u", radix);
  std::string s;
  do {
    uint32_t d = v % radix;
    s.insert(s.begin(), char(d < 10 ? '0' + d : 'a' + d - 10));
    v /= radix;
  } while (v);
  PutStr(Arg(c, 1), s);
  return Arg(c, 1);
}

// ---- math ----------------------------------------------------------------------
// Doubles take two argument slots; the result is in ST0.

double ArgDouble(Ctx& c, int i) {
  uint64_t b = uint64_t(Arg(c, i)) | uint64_t(Arg(c, i + 1)) << 32;
  double d;
  std::memcpy(&d, &b, 8);
  return d;
}
uint64_t RetDouble(Ctx& c, double d) {
  uint64_t b;
  std::memcpy(&b, &d, 8);
  x87::LoadF64(c.fpu, b);
  return 0;
}
HLE(kDll, tan, kCdecl) { return RetDouble(c, std::tan(ArgDouble(c, 0))); }
HLE(kDll, log10, kCdecl) { return RetDouble(c, std::log10(ArgDouble(c, 0))); }
HLE(kDll, _hypot, kCdecl) { return RetDouble(c, std::hypot(ArgDouble(c, 0), ArgDouble(c, 2))); }

// rand/srand: MSVC's generator (holdrand * 214013 + 2531011), per thread.
uint32_t& RandState() {
  static thread_local uint32_t s = 1;
  return s;
}
HLE(kDll, srand, kCdecl) {
  RandState() = Arg(c, 0);
  return 0;
}
HLE(kDll, rand, kCdecl) {
  uint32_t& s = RandState();
  s = s * 214013u + 2531011u;
  return (s >> 16) & 0x7FFF;
}

// qsort(base, n, size, cmp) and bsearch(key, base, n, size, cmp), calling
// the guest's comparison (cdecl).
HLE(kDll, qsort, kCdecl) {
  gaddr base = Arg(c, 0), cmp = Arg(c, 3);
  uint32_t n = Arg(c, 1), size = Arg(c, 2);
  if (n < 2 || !size) return 0;
  // Sort the indices, then permute: the guest compares elements in place.
  std::vector<uint32_t> idx(n);
  for (uint32_t i = 0; i < n; i++) idx[i] = i;
  std::stable_sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b) {
    return int32_t(CallGuest(c, cmp, {base + a * size, base + b * size})) < 0;
  });
  std::vector<uint8_t> copy(g_proc->mem.ptr<uint8_t>(base), g_proc->mem.ptr<uint8_t>(base) + uint64_t(n) * size);
  for (uint32_t i = 0; i < n; i++)
    std::memcpy(GPtr(base + i * size), copy.data() + uint64_t(idx[i]) * size, size);
  return 0;
}
HLE(kDll, bsearch, kCdecl) {
  gaddr key = Arg(c, 0), base = Arg(c, 1), cmp = Arg(c, 4);
  uint32_t lo = 0, hi = Arg(c, 2), size = Arg(c, 3);
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    int32_t r = int32_t(CallGuest(c, cmp, {key, base + mid * size}));
    if (r == 0) return base + mid * size;
    if (r < 0) hi = mid;
    else lo = mid + 1;
  }
  return 0;
}

// gmtime(const time_t*): msvcrt's time_t is 32-bit; the struct tm (nine
// ints) in a buffer per thread.
HLE(kDll, gmtime, kCdecl) {
  std::time_t t = int32_t(Rd32(Arg(c, 0)));
  std::tm tm;
  if (!gmtime_r(&t, &tm)) return 0;
  static thread_local gaddr buf = 0;
  if (!buf) buf = HeapAllocProcess(36, false);
  const int f[9] = {tm.tm_sec, tm.tm_min, tm.tm_hour, tm.tm_mday, tm.tm_mon,
                    tm.tm_year, tm.tm_wday, tm.tm_yday, tm.tm_isdst};
  for (int i = 0; i < 9; i++) Wr32(buf + 4 * i, uint32_t(f[i]));
  return buf;
}

// ---- stdio, more -----------------------------------------------------------------

HLE(kDll, fwrite, kCdecl) {
  uint32_t size = Arg(c, 1), count = Arg(c, 2);
  if (!size || !count) return 0;
  std::FILE* f = File(Arg(c, 3), "fwrite");
  return uint32_t(std::fwrite(GPtr(Arg(c, 0)), size, count, f));
}
HLE(kDll, fputc, kCdecl) {
  return uint32_t(std::fputc(int(Arg(c, 0)), File(Arg(c, 1), "fputc")));
}
HLE(kDll, getc, kCdecl) { return uint32_t(std::getc(File(Arg(c, 0), "getc"))); }
HLE(kDll, ungetc, kCdecl) {
  return uint32_t(std::ungetc(int(Arg(c, 0)), File(Arg(c, 1), "ungetc")));
}
HLE(kDll, feof, kCdecl) { return std::feof(File(Arg(c, 0), "feof")) ? 0x10 : 0; }
HLE(kDll, ferror, kCdecl) { return std::ferror(File(Arg(c, 0), "ferror")) ? 0x20 : 0; }
HLE(kDll, fflush, kCdecl) {
  if (!Arg(c, 0)) return uint32_t(std::fflush(nullptr));
  return uint32_t(std::fflush(File(Arg(c, 0), "fflush")));
}
HLE(kDll, _fileno, kCdecl) { return uint32_t(fileno(File(Arg(c, 0), "_fileno"))); }

// _wfopen(path, mode) as fopen, the strings wide.
gaddr OpenStream(const std::string& path, const std::string& mode) {
  bool write = mode.find_first_of("wa+") != std::string::npos;
  std::string host = HostPath(path, write ? (mode[0] == 'r' ? PathAccess::kUpdate
                                                            : PathAccess::kWrite)
                                          : PathAccess::kRead);
  std::string m;
  for (char ch : mode)
    if (ch == 'r' || ch == 'w' || ch == 'a' || ch == '+' || ch == 'b') m += ch;
  if (m.find('b') == std::string::npos) m += 'b';
  std::FILE* f = std::fopen(host.c_str(), m.c_str());
  if (!f) {
    SetErrno(errno);
    return 0;
  }
  gaddr fp = HeapAllocProcess(32, true);
  std::lock_guard<std::mutex> l(g_mu);
  g_files[fp] = f;
  return fp;
}
HLE(kDll, _wfopen, kCdecl) {
  return OpenStream(NarrowPath(GWStr(Arg(c, 0))), NarrowPath(GWStr(Arg(c, 1))));
}
// _fdopen(fd, mode): a stream over a descriptor from _open.
HLE(kDll, _fdopen, kCdecl) {
  std::string mode = GStr(Arg(c, 1)), m;
  for (char ch : mode)
    if (ch == 'r' || ch == 'w' || ch == 'a' || ch == '+') m += ch;
  std::FILE* f = fdopen(int(Arg(c, 0)), m.c_str());
  if (!f) {
    SetErrno(errno);
    return 0;
  }
  gaddr fp = HeapAllocProcess(32, true);
  std::lock_guard<std::mutex> l(g_mu);
  g_files[fp] = f;
  return fp;
}

// ---- low-level I/O -------------------------------------------------------------
// _open's oflag: _O_RDONLY 0, _O_WRONLY 1, _O_RDWR 2, _O_APPEND 8,
// _O_CREAT 0x100, _O_TRUNC 0x200, _O_EXCL 0x400; text/binary, inheritance
// and the access hints change nothing here.

uint32_t Open(const std::string& path, uint32_t oflag) {
  int acc = oflag & 3;
  bool write = acc != 0 || (oflag & 0x300);
  int flags = acc == 1 ? O_WRONLY : acc == 2 ? O_RDWR : O_RDONLY;
  if (oflag & 0x8) flags |= O_APPEND;
  if (oflag & 0x100) flags |= O_CREAT;
  if (oflag & 0x200) flags |= O_TRUNC;
  if (oflag & 0x400) flags |= O_EXCL;
  std::string host = HostPath(path, !write ? PathAccess::kRead
                                    : (oflag & 0x200) ? PathAccess::kWrite
                                                      : PathAccess::kUpdate);
  int fd = open(host.c_str(), flags | O_CLOEXEC, 0644);
  if (fd < 0) return Fail();
  return uint32_t(fd);
}
HLE(kDll, _open, kCdecl) { return Open(GStr(Arg(c, 0)), Arg(c, 1)); }
HLE(kDll, _wopen, kCdecl) { return Open(NarrowPath(GWStr(Arg(c, 0))), Arg(c, 1)); }
// _sopen(path, oflag, shflag, pmode): sharing is between processes.
HLE(kDll, _sopen, kCdecl) { return Open(GStr(Arg(c, 0)), Arg(c, 1)); }
HLE(kDll, _close, kCdecl) { return close(int(Arg(c, 0))) == 0 ? 0 : Fail(); }
HLE(kDll, _dup, kCdecl) {
  int fd = dup(int(Arg(c, 0)));
  return fd < 0 ? Fail() : uint32_t(fd);
}
HLE(kDll, _read, kCdecl) {
  uint32_t n = Arg(c, 2);
  if (!n) return 0;
  if (!EnsureReadable(c, Arg(c, 1), 0)) return uint32_t(-1);
  ssize_t r = read(int(Arg(c, 0)), GPtr(Arg(c, 1)), n);
  return r < 0 ? Fail() : uint32_t(r);
}
HLE(kDll, _write, kCdecl) {
  uint32_t n = Arg(c, 2);
  if (!n) return 0;
  ssize_t r = write(int(Arg(c, 0)), GPtr(Arg(c, 1)), n);
  return r < 0 ? Fail() : uint32_t(r);
}
// _lseeki64(fd, int64 offset, origin) -> int64 in EDX:EAX.
HLE(kDll, _lseeki64, kCdecl) {
  int64_t off = int64_t(uint64_t(Arg(c, 1)) | uint64_t(Arg(c, 2)) << 32);
  off_t r = lseek(int(Arg(c, 0)), off, int(Arg(c, 3)));
  if (r < 0) {
    Fail();
    c.edx = 0xFFFFFFFF;
    return 0xFFFFFFFF;
  }
  c.edx = uint32_t(uint64_t(r) >> 32);
  return uint32_t(r);
}
// _locking(fd, mode, bytes): locks guard against other processes, of
// which there are none.
HLE(kDll, _locking, kCdecl) { return 0; }
// _get_osfhandle(fd): a descriptor here is not a kernel32 handle; no
// caller seen uses the result as one beyond checking it.
HLE(kDll, _get_osfhandle, kCdecl) {
  if (fcntl(int(Arg(c, 0)), F_GETFD) < 0) {
    SetErrno(EBADF);
    return 0xFFFFFFFF;
  }
  return Arg(c, 0);
}

// ---- the file system ---------------------------------------------------------

// struct _stat (msvcrt, 32-bit times) and _stati64 (64-bit size):
// st_dev(4) st_ino(2) st_mode(2) st_nlink(2) st_uid(2) st_gid(2) pad(2)
// st_rdev(4) then st_size (4, or 8 at offset 24 for _stati64) and
// st_atime, st_mtime, st_ctime (4 each). Mode bits are MSVC's: _S_IFDIR
// 0x4000, _S_IFREG 0x8000, read 0x100, write 0x80, execute 0x40.
uint32_t Stat(const std::string& path, gaddr out, bool size64) {
  struct stat st;
  std::string host = HostPath(path);
  if (stat(host.c_str(), &st) != 0) return Fail();
  uint16_t mode = (S_ISDIR(st.st_mode) ? 0x4000 : 0x8000) | 0x100;
  if (st.st_mode & S_IWUSR) mode |= 0x80;
  if (S_ISDIR(st.st_mode) || (st.st_mode & S_IXUSR)) mode |= 0x40;
  mode |= (mode & 0x1C0) >> 3 | (mode & 0x1C0) >> 6;
  std::memset(GPtr(out), 0, size64 ? 48 : 36);
  Wr32(out + 0, 2);  // drive C:
  Wr16(out + 6, mode);
  Wr16(out + 8, 1);
  Wr32(out + 16, 2);
  uint32_t t = size64 ? 32 : 24;
  if (size64) Wr64(out + 24, uint64_t(st.st_size));
  else Wr32(out + 20, uint32_t(st.st_size));
  Wr32(out + t, uint32_t(st.st_atime));
  Wr32(out + t + 4, uint32_t(st.st_mtime));
  Wr32(out + t + 8, uint32_t(st.st_ctime));
  return 0;
}
HLE(kDll, _stat, kCdecl) { return Stat(GStr(Arg(c, 0)), Arg(c, 1), false); }
HLE(kDll, _stati64, kCdecl) { return Stat(GStr(Arg(c, 0)), Arg(c, 1), true); }
HLE(kDll, _wstat, kCdecl) { return Stat(NarrowPath(GWStr(Arg(c, 0))), Arg(c, 1), false); }

// _access(path, mode): 0 existence, 2 write, 4 read, 6 both. The install
// is writable through the overlay.
HLE(kDll, _access, kCdecl) {
  struct stat st;
  if (stat(HostPath(GStr(Arg(c, 0))).c_str(), &st) != 0) return Fail();
  return 0;
}
HLE(kDll, _chmod, kCdecl) {
  struct stat st;
  if (stat(HostPath(GStr(Arg(c, 0))).c_str(), &st) != 0) return Fail();
  return 0;
}
HLE(kDll, _mkdir, kCdecl) {
  std::string host = HostPath(GStr(Arg(c, 0)), PathAccess::kWrite);
  return mkdir(host.c_str(), 0755) == 0 ? 0 : Fail();
}
HLE(kDll, _rmdir, kCdecl) {
  std::string host = HostPath(GStr(Arg(c, 0)), PathAccess::kWrite);
  return rmdir(host.c_str()) == 0 ? 0 : Fail();
}
HLE(kDll, _unlink, kCdecl) {
  std::string host = HostPath(GStr(Arg(c, 0)), PathAccess::kWrite);
  return unlink(host.c_str()) == 0 ? 0 : Fail();
}
HLE(kDll, remove, kCdecl) {
  std::string host = HostPath(GStr(Arg(c, 0)), PathAccess::kWrite);
  return std::remove(host.c_str()) == 0 ? 0 : Fail();
}
HLE(kDll, rename, kCdecl) {
  std::string from = HostPath(GStr(Arg(c, 0)), PathAccess::kUpdate);
  std::string to = HostPath(GStr(Arg(c, 1)), PathAccess::kWrite);
  return std::rename(from.c_str(), to.c_str()) == 0 ? 0 : Fail();
}

// _getcwd(buf, size) and _fullpath(buf, path, size): kernel32's current
// directory and path rules (GetFullPathNameA). A null buf is allocated.
gaddr PutPath(gaddr buf, uint32_t size, const std::string& p) {
  if (!buf) {
    size = std::max<uint32_t>(size, uint32_t(p.size() + 1));
    buf = HeapAllocProcess(size, false);
  }
  if (p.size() + 1 > size) {
    SetErrno(ERANGE);
    return 0;
  }
  PutStr(buf, p);
  return buf;
}
HLE(kDll, _getcwd, kCdecl) { return PutPath(Arg(c, 0), Arg(c, 1), GuestCurrentDir()); }
HLE(kDll, _fullpath, kCdecl) {
  return PutPath(Arg(c, 0), Arg(c, 2) ? Arg(c, 2) : 260, GuestFullPath(GStr(Arg(c, 1))));
}

// _findfirst(spec, struct _finddata_t*) / _findnext(handle, data) /
// _findclose(handle). _finddata_t (32-bit times): attrib, time_create,
// time_access, time_write, size (4 each), name[260].
struct Find {
  std::vector<std::string> names;  // as on disk
  std::vector<std::string> dirs;   // the host directory of each
  size_t next = 0;
};
std::mutex g_find_mu;
std::map<uint32_t, Find> g_finds;
uint32_t g_next_find = 1;

bool PutFind(Find& f, gaddr data) {
  while (f.next < f.names.size()) {
    const std::string& n = f.names[f.next];
    const std::string& dir = f.dirs[f.next++];
    struct stat st;
    if (stat((dir + "/" + n).c_str(), &st) != 0) continue;
    std::memset(GPtr(data), 0, 20 + 260);
    uint32_t attr = S_ISDIR(st.st_mode) ? 0x10 : 0x20;  // _A_SUBDIR, _A_ARCH
    Wr32(data, attr);
    Wr32(data + 4, uint32_t(st.st_ctime));
    Wr32(data + 8, uint32_t(st.st_atime));
    Wr32(data + 12, uint32_t(st.st_mtime));
    Wr32(data + 16, uint32_t(st.st_size));
    PutStr(data + 20, n.substr(0, 259));
    return true;
  }
  return false;
}

HLE(kDll, _findfirst, kCdecl) {
  std::string spec = GStr(Arg(c, 0));
  size_t slash = spec.find_last_of("\\/");
  std::string dir = slash == std::string::npos ? "." : spec.substr(0, slash);
  std::string pat = slash == std::string::npos ? spec : spec.substr(slash + 1);
  // The overlay's entries, then the install's not already listed.
  Find f;
  std::vector<std::string> dirs = HostDirs(GuestFullPath(dir));
  for (const std::string& hd : dirs) {
    DIR* d = opendir(hd.c_str());
    if (!d) continue;
    while (dirent* e = readdir(d)) {
      std::string n = e->d_name;
      bool match = fnmatch(pat.c_str(), n.c_str(), FNM_CASEFOLD) == 0 ||
                   (pat == "*.*" && n.find('.') == std::string::npos);
      if (!match || std::count(f.names.begin(), f.names.end(), n)) continue;
      f.names.push_back(n);
      f.dirs.push_back(hd);
    }
    closedir(d);
  }
  if (!PutFind(f, Arg(c, 1))) {
    SetErrno(ENOENT);
    return uint32_t(-1);
  }
  std::lock_guard<std::mutex> l(g_find_mu);
  uint32_t h = g_next_find++;
  g_finds[h] = std::move(f);
  return h;
}
HLE(kDll, _findnext, kCdecl) {
  std::lock_guard<std::mutex> l(g_find_mu);
  auto it = g_finds.find(Arg(c, 0));
  if (it == g_finds.end() || !PutFind(it->second, Arg(c, 1))) {
    SetErrno(ENOENT);
    return uint32_t(-1);
  }
  return 0;
}
HLE(kDll, _findclose, kCdecl) {
  std::lock_guard<std::mutex> l(g_find_mu);
  return g_finds.erase(Arg(c, 0)) ? 0 : uint32_t(-1);
}

}  // namespace
}  // namespace recomp::hle
