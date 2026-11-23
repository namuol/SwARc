// kernel32: process, memory, heaps, TLS, synchronisation basics, locale and
// time. Files, kernel objects, resources and conversions, and SEH live in
// kernel32_file.cpp, kernel32_sync.cpp, kernel32_misc.cpp and
// kernel32_seh.cpp.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "runtime/log.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "win32/win32.h"

namespace recomp {

namespace {

std::mutex g_heaps_mu;
std::map<gaddr, std::unique_ptr<GuestHeap>> g_heaps;
GuestHeap* g_process_heap = nullptr;

const auto g_start = std::chrono::steady_clock::now();

GuestHeap* NewHeap(uint64_t initial, uint64_t max) {
  auto h = std::make_unique<GuestHeap>(g_proc->vm, initial, max);
  GuestHeap* raw = h.get();
  if (!raw->handle()) return nullptr;
  std::lock_guard<std::mutex> l(g_heaps_mu);
  g_heaps[raw->handle()] = std::move(h);
  return raw;
}

}  // namespace

uint64_t PerfCounter() {
  using namespace std::chrono;
  return uint64_t(duration_cast<nanoseconds>(steady_clock::now() - g_start).count() / 100);
}

gaddr HeapAllocProcess(uint64_t size, bool zero) {
  return g_process_heap->Alloc(size, zero);
}

void HeapFreeProcess(gaddr p) { g_process_heap->Free(p); }

gaddr HeapReAllocProcess(gaddr p, uint64_t size) {
  return g_process_heap->ReAlloc(p, size, false, false);
}

uint64_t HeapSizeProcess(gaddr p) { return g_process_heap->Size(p); }

uint32_t TickCount() {
  using namespace std::chrono;
  return uint32_t(duration_cast<milliseconds>(steady_clock::now() - g_start).count());
}

void WriteSystemTime(gaddr st, const std::tm& t, int ms) {
  Wr16(st + 0, uint16_t(t.tm_year + 1900));
  Wr16(st + 2, uint16_t(t.tm_mon + 1));
  Wr16(st + 4, uint16_t(t.tm_wday));
  Wr16(st + 6, uint16_t(t.tm_mday));
  Wr16(st + 8, uint16_t(t.tm_hour));
  Wr16(st + 10, uint16_t(t.tm_min));
  Wr16(st + 12, uint16_t(t.tm_sec));
  Wr16(st + 14, uint16_t(ms));
}

GuestHeap* FindHeap(gaddr handle) {
  std::lock_guard<std::mutex> l(g_heaps_mu);
  auto it = g_heaps.find(handle);
  return it == g_heaps.end() ? nullptr : it->second.get();
}

void SetEnvironmentBlock(const std::vector<std::string>& vars);

void InitKernel32State() {
  Win32Process& p = *g_win32;
  g_process_heap = NewHeap(1u << 20, 0);
  if (!g_process_heap) FatalF("cannot create the process heap");
  p.process_heap = g_process_heap->handle();
  if (g_proc->x64)
    Wr64(kPebVa64 + 0x30, p.process_heap);  // PEB ProcessHeap
  else
    Wr32(kPebVa + 0x18, uint32_t(p.process_heap));

  std::string exe = GuestExePath();
  std::string cmd = "\"" + exe + "\"";
  if (!p.game_args.empty()) cmd += " " + p.game_args;
  p.cmdline_a = HeapAllocProcess(uint32_t(cmd.size() + 1), false);
  PutStr(p.cmdline_a, cmd);
  std::u16string wcmd = win32::Widen(cmd);
  p.cmdline_w = HeapAllocProcess(uint32_t(wcmd.size() * 2 + 2), true);
  std::memcpy(GPtr(p.cmdline_w), wcmd.data(), wcmd.size() * 2);

  // A small, fixed environment; the game only needs it to exist.
  SetEnvironmentBlock({"PATH=C:\\WINDOWS\\system32;C:\\WINDOWS",
                       "SystemRoot=C:\\WINDOWS", "windir=C:\\WINDOWS",
                       "TEMP=C:\\TEMP", "TMP=C:\\TEMP"});
}

std::vector<std::string> EnvironmentVars() {
  std::vector<std::string> vars;
  const char* env = g_proc->mem.ptr<const char>(g_win32->environ_a);
  for (const char* e = env; *e; e += std::strlen(e) + 1) vars.push_back(e);
  return vars;
}

void SetEnvironmentBlock(const std::vector<std::string>& vars) {
  Win32Process& p = *g_win32;
  std::string block;
  for (const std::string& e : vars) block.append(e).push_back('\0');
  block.push_back('\0');
  // The old blocks stay allocated: the game may still hold pointers into
  // them (GetEnvironmentStrings).
  p.environ_a = HeapAllocProcess(uint32_t(block.size()), false);
  std::memcpy(GPtr(p.environ_a), block.data(), block.size());
  std::u16string wblock = win32::Widen(block);
  p.environ_w = HeapAllocProcess(uint32_t(wblock.size() * 2), false);
  std::memcpy(GPtr(p.environ_w), wblock.data(), wblock.size() * 2);
}

namespace hle {
namespace {

using namespace win32;

// ---- process / module ----------------------------------------------------

// The version the PEB gives (process.cpp): Windows XP for 32-bit guests,
// Windows 10 22H2 for x64 ones: build << 16 | minor << 8 | major (an NT
// platform leaves bit 31 clear).
HLE64("kernel32.dll", GetVersion, 0) {
  if (g_proc->x64) return 0x4A65000A;  // 10.0, build 19045
  return 0x0A280105;  // Windows XP: build 2600, 5.1, NT platform
}

// The Windows guests see (as the PEB says): XP SP3 for 32-bit guests,
// Windows 10 22H2 for x64 ones.
struct OsVersion {
  uint32_t major, minor, build;
  uint16_t sp_major;
  const char* csd;
};
OsVersion GuestOsVersion() {
  return g_proc->x64 ? OsVersion{10, 0, 19045, 0, ""} : OsVersion{5, 1, 2600, 3, "Service Pack 3"};
}

// OSVERSIONINFO{,EX}{A,W}: no pointers, the same on x64. The W form's
// szCSDVersion is 128 WCHARs, so the EX fields after it sit 128 bytes on.
uint64_t GetVersionEx(gaddr vi, bool wide) {
  const OsVersion v = GuestOsVersion();
  uint32_t size = Rd32(vi);
  Wr32(vi + 4, v.major);
  Wr32(vi + 8, v.minor);
  Wr32(vi + 12, v.build);
  Wr32(vi + 16, 2);  // VER_PLATFORM_WIN32_NT
  std::memset(GPtr(vi + 20), 0, wide ? 256 : 128);
  if (wide) PutWStr(vi + 20, v.csd);
  else PutStr(vi + 20, v.csd);
  const gaddr ex = vi + (wide ? 276 : 148);
  if (size >= (wide ? 284u : 156u)) {  // OSVERSIONINFOEX
    Wr16(ex + 0, v.sp_major);
    Wr16(ex + 2, 0);
    Wr16(ex + 4, 0x100);  // VER_SUITE_SINGLEUSERTS
    Wr8(ex + 6, 1);       // VER_NT_WORKSTATION
  }
  return 1;
}
HLE64("kernel32.dll", GetVersionExA, 1) { return GetVersionEx(Arg(c, 0), false); }
HLE64("kernel32.dll", GetVersionExW, 1) { return GetVersionEx(Arg(c, 0), true); }

// VerSetConditionMask(mask, type, condition): each VER_* type bit (minor
// 1, major 2, build 4, platform 8, service pack minor 0x10 and major 0x20,
// suite 0x40, product type 0x80) has a 3-bit condition, at 3 * its bit
// index, as Windows packs them.
HLE64("kernel32.dll", VerSetConditionMask, 3) {
  uint64_t mask = Arg(c, 0);
  uint32_t type = uint32_t(Arg(c, 1));
  uint64_t cond = Arg(c, 2) & 7;
  for (int i = 0; i < 8; i++)
    if (type & (1u << i)) mask = (mask & ~(uint64_t(7) << (3 * i))) | cond << (3 * i);
  return mask;
}

// VerifyVersionInfoW(OSVERSIONINFOEXW*, type, condition mask): the major,
// minor and service-pack versions compare as one number under the
// condition of the most significant of them asked for, as Windows does;
// the build number and platform on their own. FALSE with
// ERROR_OLD_WIN_VERSION when the system does not match.
HLE64("kernel32.dll", VerifyVersionInfoW, 3) {
  const gaddr vi = Arg(c, 0);
  const uint32_t type = uint32_t(Arg(c, 1));
  const uint64_t mask = Arg(c, 2);
  const OsVersion v = GuestOsVersion();
  auto cond = [&](int bit) { return uint32_t(mask >> (3 * bit)) & 7; };
  auto test = [](uint32_t cnd, uint64_t have, uint64_t want) {
    switch (cnd) {
      case 1: return have == want;  // VER_EQUAL
      case 2: return have > want;   // VER_GREATER
      case 3: return have >= want;  // VER_GREATER_EQUAL
      case 4: return have < want;   // VER_LESS
      case 5: return have <= want;  // VER_LESS_EQUAL
      default: return false;
    }
  };
  bool ok = true;
  // major (bit 1), minor (bit 0), service pack major (bit 5), minor (bit 4)
  if (type & (0x2 | 0x1 | 0x20 | 0x10)) {
    int top = (type & 0x2) ? 1 : (type & 0x1) ? 0 : (type & 0x20) ? 5 : 4;
    uint64_t have = uint64_t(v.major) << 48 | uint64_t(v.minor) << 32 | uint64_t(v.sp_major) << 16;
    uint64_t want = 0;
    if (type & 0x2) want |= uint64_t(Rd32(vi + 4)) << 48;
    else have &= ~(uint64_t(0xFFFF) << 48);
    if (type & 0x1) want |= uint64_t(Rd32(vi + 8)) << 32;
    else have &= ~(uint64_t(0xFFFF) << 32);
    if (type & 0x20) want |= uint64_t(Rd16(vi + 276)) << 16;
    else have &= ~(uint64_t(0xFFFF) << 16);
    if (type & 0x10) want |= Rd16(vi + 278);
    ok &= test(cond(top), have, want);
  }
  if (type & 0x4) ok &= test(cond(2), v.build, Rd32(vi + 12));
  if (type & 0x8) ok &= test(cond(3), 2, Rd32(vi + 16));
  if (!ok) SetLastError(1150);  // ERROR_OLD_WIN_VERSION
  return ok ? 1 : 0;
}

// Lifted DLLs (GameConfig::lifted_dlls) are mapped at startup; LoadLibrary
// counts references and runs DllMain under the loader lock, as Windows
// does. After the last FreeLibrary the next load maps the file afresh, so
// the DLL sees initial data again as after a real unload.
std::recursive_mutex g_loader_mu;

Module* LiftedDll(const std::string& name) {
  std::string n = NormaliseDll(name);
  for (Module& m : g_win32->modules)
    if (m.name == n) return &m;
  return nullptr;
}

Module* LoadedDllAt(gaddr hmodule) {
  for (Module& m : g_win32->modules)
    if (m.refs && m.img.image_base == hmodule) return &m;
  return nullptr;
}

// A guest string argument, ANSI or UTF-16.
std::string GArg(gaddr p, bool wide) { return wide ? Narrow(GWStr(p)) : GStr(p); }

gaddr ModuleHandleOf(gaddr name, bool wide) {
  if (!name) return g_win32->img.image_base;
  std::string n = GArg(name, wide);
  std::string exe = ExeFileName();
  if (NormaliseDll(n) == exe || Lower(n) == exe.substr(0, exe.rfind('.')))
    return g_win32->img.image_base;
  if (Module* m = LiftedDll(n)) {
    std::lock_guard<std::recursive_mutex> l(g_loader_mu);
    if (m->refs) return m->img.image_base;
    SetLastError(kErrorModNotFound);
    return 0;
  }
  gaddr h = ModuleHandle(n);
  if (!h) SetLastError(kErrorModNotFound);
  return h;
}

HLE64("kernel32.dll", GetModuleHandleA, 1) { return ModuleHandleOf(Arg(c, 0), false); }
HLE64("kernel32.dll", GetModuleHandleW, 1) { return ModuleHandleOf(Arg(c, 0), true); }

// The guest path of the module `mod` (0: the EXE); false (ERROR_MOD_NOT_FOUND
// set) if no module is loaded there.
bool ModulePath(gaddr mod, std::string* path) {
  *path = GuestExePath();
  if (mod && mod != g_win32->img.image_base) {
    std::lock_guard<std::recursive_mutex> l(g_loader_mu);
    Module* m = LoadedDllAt(mod);
    if (!m) {
      SetLastError(kErrorModNotFound);
      return false;
    }
    *path = GuestGameDir() + "\\" +
            Game().lifted_dlls[size_t(m - g_win32->modules.data())].name;
  }
  return true;
}

HLE64("kernel32.dll", GetModuleFileNameA, 3) {
  gaddr buf = Arg(c, 1);
  uint32_t size = uint32_t(Arg(c, 2));
  std::string path;
  if (!ModulePath(Arg(c, 0), &path)) return 0;
  if (!size) return 0;
  std::string s = path.substr(0, size - 1);
  PutStr(buf, s);
  return uint32_t(s.size());
}

// The same, in WCHARs: a path that does not fit is cut to size - 1
// characters and NUL-terminated, like the A version.
HLE64("kernel32.dll", GetModuleFileNameW, 3) {
  gaddr buf = Arg(c, 1);
  uint32_t size = uint32_t(Arg(c, 2));
  std::string path;
  if (!ModulePath(Arg(c, 0), &path)) return 0;
  if (!size) return 0;
  std::u16string w = Widen(path).substr(0, size - 1);
  std::memcpy(GPtr(buf), w.data(), w.size() * 2);
  Wr16(buf + gaddr(w.size()) * 2, 0);
  return uint32_t(w.size());
}

// GetModuleHandleExW(flags, name or address, HMODULE* out). Flags:
// GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS (4) looks the module up by an
// address inside it; PIN (1) and UNCHANGED_REFCOUNT (2) only affect
// reference counts, and modules here are never unloaded under a holder.
HLE64("kernel32.dll", GetModuleHandleExW, 3) {
  uint32_t flags = uint32_t(Arg(c, 0));
  gaddr arg = Arg(c, 1), out = Arg(c, 2);
  if (!out) {
    SetLastError(kErrorInvalidParameter);
    return 0;
  }
  gaddr h = 0;
  if (flags & 4) {
    const auto& img = g_win32->img;
    if (arg >= img.image_base && arg < img.image_base + img.image_size) {
      h = img.image_base;
    } else {
      std::lock_guard<std::recursive_mutex> l(g_loader_mu);
      for (const Module& m : g_win32->modules)
        if (m.refs && arg >= m.img.image_base &&
            arg < m.img.image_base + m.img.image_size)
          h = m.img.image_base;
    }
    if (!h) SetLastError(kErrorModNotFound);
  } else {
    h = ModuleHandleOf(arg, true);
  }
  WrPtr(out, h);
  return h ? 1 : 0;
}

// Encoded pointers are opaque to their users: only DecodePointer(
// EncodePointer(p)) == p is observable, so the identity is a faithful
// encoding.
HLE64("kernel32.dll", EncodePointer, 1) { return Arg(c, 0); }
HLE64("kernel32.dll", DecodePointer, 1) { return Arg(c, 0); }

// LoadLibrary(Ex)A/W: lifted DLLs, and the HLE modules.
gaddr LoadModule(Ctx& c, const char* api, const std::string& n) {
  if (Module* m = LiftedDll(n)) {
    std::lock_guard<std::recursive_mutex> l(g_loader_mu);
    if (!AttachModule(c, *m)) {
      WarnF("%s(%s): DllMain failed\n", api, n.c_str());
      SetLastError(kErrorDllInitFailed);
      return 0;
    }
    return m->img.image_base;
  }
  gaddr h = ModuleHandle(n);
  if (!h) {
    WarnF("%s(%s): not provided\n", api, n.c_str());
    SetLastError(kErrorModNotFound);
  }
  return h;
}

HLE64("kernel32.dll", LoadLibraryA, 1) { return LoadModule(c, "LoadLibraryA", GStr(Arg(c, 0))); }
HLE64("kernel32.dll", LoadLibraryW, 1) {
  return LoadModule(c, "LoadLibraryW", Narrow(GWStr(Arg(c, 0))));
}
HLE64("kernel32.dll", LoadLibraryExA, 3) {
  return LoadModule(c, "LoadLibraryExA", GStr(Arg(c, 0)));
}
HLE64("kernel32.dll", LoadLibraryExW, 3) {
  return LoadModule(c, "LoadLibraryExW", Narrow(GWStr(Arg(c, 0))));
}

HLE64("kernel32.dll", FreeLibrary, 1) {
  std::lock_guard<std::recursive_mutex> l(g_loader_mu);
  if (Module* m = LoadedDllAt(Arg(c, 0))) DetachModule(c, *m);
  return 1;
}

// FreeLibraryAndExitThread(module, code): FreeLibrary, then ExitThread
// (the C runtime's _endthreadex for a thread whose code is in a DLL).
HLE("kernel32.dll", FreeLibraryAndExitThread, 2) {
  {
    std::lock_guard<std::recursive_mutex> l(g_loader_mu);
    if (Module* m = LoadedDllAt(Arg(c, 0))) DetachModule(c, *m);
  }
  GuestSyncFence();
  throw GuestThreadExit{uint32_t(Arg(c, 1))};
}

// DLL_THREAD_ATTACH/DETACH are never sent, so there is nothing to disable.
HLE64("kernel32.dll", DisableThreadLibraryCalls, 1) { return 1; }

HLE64("kernel32.dll", GetProcAddress, 2) {
  gaddr mod = Arg(c, 0), name = Arg(c, 1);
  {
    std::lock_guard<std::recursive_mutex> l(g_loader_mu);
    if (Module* m = LoadedDllAt(mod)) {
      std::string n = name < 0x10000 ? std::string() : GStr(name);
      for (const PeExport& e : m->img.exports)
        if (name < 0x10000 ? e.ordinal == name : e.name == n) return e.va;
      SetLastError(kErrorProcNotFound);
      return 0;
    }
  }
  if (name < 0x10000) {
    WarnF("GetProcAddress by ordinal %u: not supported\n", uint32_t(name));
    return 0;
  }
  std::string n = GStr(name);
  gaddr a = ProcAddress(mod, n);
  if (!a) SetLastError(kErrorProcNotFound);
  return a;
}

HLE64("kernel32.dll", GetCommandLineA, 0) { return g_win32->cmdline_a; }
HLE64("kernel32.dll", GetCommandLineW, 0) { return g_win32->cmdline_w; }

// STARTUPINFOA/W: 68 bytes in 32-bit guests, 104 in x64 ones (its handles
// and string pointers are pointer-sized); nothing set but cb.
uint64_t StartupInfo(gaddr si) {
  const uint32_t size = g_proc->x64 ? 104 : 68;
  std::memset(GPtr(si), 0, size);
  Wr32(si, size);
  return 0;
}
HLE64("kernel32.dll", GetStartupInfoA, 1) { return StartupInfo(Arg(c, 0)); }
HLE64("kernel32.dll", GetStartupInfoW, 1) { return StartupInfo(Arg(c, 0)); }

HLE("kernel32.dll", GetEnvironmentStrings, 0) { return g_win32->environ_a; }
HLE64("kernel32.dll", GetEnvironmentStringsW, 0) { return g_win32->environ_w; }
HLE("kernel32.dll", FreeEnvironmentStringsA, 1) { return 1; }
HLE64("kernel32.dll", FreeEnvironmentStringsW, 1) { return 1; }

HLE("kernel32.dll", GetEnvironmentVariableA, 3) {
  std::string name = GStr(Arg(c, 0));
  uint32_t buf = Arg(c, 1), size = Arg(c, 2);
  const char* env = g_proc->mem.ptr<const char>(g_win32->environ_a);
  std::string key = Lower(name) + "=";
  for (const char* e = env; *e; e += std::strlen(e) + 1) {
    std::string kv = e;
    if (Lower(kv.substr(0, key.size())) == key)
      return CopyOut(buf, size, kv.substr(key.size()));
  }
  SetLastError(kErrorEnvVarNotFound);
  return 0;
}

// value null: delete the variable.
uint32_t SetEnvVar(const std::string& name, const std::string* value) {
  if (name.empty() || name.find('=') != std::string::npos) {
    SetLastError(kErrorInvalidParameter);
    return 0;
  }
  std::vector<std::string> vars = EnvironmentVars();
  std::string key = Lower(name) + "=";
  std::erase_if(vars, [&](const std::string& kv) {
    return Lower(kv.substr(0, key.size())) == key;
  });
  if (value) vars.push_back(name + "=" + *value);
  SetEnvironmentBlock(vars);
  return 1;
}

HLE64("kernel32.dll", SetEnvironmentVariableA, 2) {
  std::string v = Arg(c, 1) ? GStr(Arg(c, 1)) : "";
  return SetEnvVar(GStr(Arg(c, 0)), Arg(c, 1) ? &v : nullptr);
}

HLE64("kernel32.dll", SetEnvironmentVariableW, 2) {
  std::string v = Arg(c, 1) ? Narrow(GWStr(Arg(c, 1))) : "";
  return SetEnvVar(Narrow(GWStr(Arg(c, 0))), Arg(c, 1) ? &v : nullptr);
}

HLE("kernel32.dll", ExpandEnvironmentStringsA, 3) {
  // No variable the game uses needs expanding; copy through.
  std::string s = GStr(Arg(c, 0));
  uint32_t buf = Arg(c, 1), size = Arg(c, 2);
  if (s.size() + 1 <= size) PutStr(buf, s);
  return uint32_t(s.size() + 1);
}

HLE("kernel32.dll", SetErrorMode, 1) {
  static std::atomic<uint32_t> mode{0};
  return mode.exchange(Arg(c, 0));
}

// Power requests (keep the display on) have no effect here; the previous
// state is reported as on Windows.
HLE("kernel32.dll", SetThreadExecutionState, 1) {
  static std::atomic<uint32_t> state{0x80000000};  // ES_CONTINUOUS
  uint32_t v = Arg(c, 0);
  return (v & 0x80000000) ? state.exchange(v) : state.load();
}

// Console control events (Ctrl+C, close) are never delivered: the host
// terminal's signals end the process directly. Registration succeeds.
HLE("kernel32.dll", SetConsoleCtrlHandler, 2) { return 1; }

HLE64("kernel32.dll", GetSystemDirectoryA, 2) {
  return CopyOut(Arg(c, 0), uint32_t(Arg(c, 1)), "C:\\WINDOWS\\system32");
}

// HeapCompact(heap, flags): the size of the largest free block. Heaps
// grow on demand, so a large block is always available.
HLE64("kernel32.dll", HeapCompact, 2) { return FindHeap(Arg(c, 0)) ? 0x00100000 : 0; }

HLE64("kernel32.dll", SwitchToThread, 0) {
  GuestSyncPoint sync;
  std::this_thread::yield();
  return 1;
}

HLE64("kernel32.dll", GetCurrentProcess, 0) { return HandleRet(0xFFFFFFFF); }
HLE64("kernel32.dll", GetCurrentProcessId, 0) { return kGuestPid; }
HLE64("kernel32.dll", GetCurrentThread, 0) { return HandleRet(0xFFFFFFFE); }
HLE64("kernel32.dll", GetCurrentThreadId, 0) { return CurrentThread()->tid; }

HLE64("kernel32.dll", ExitProcess, 1) {
  uint32_t code = uint32_t(Arg(c, 0));
  std::fflush(stdout);
  std::fprintf(stderr, "ExitProcess(%u)\n", code);
  std::fflush(stderr);
  std::_Exit(int(code));
}

HLE64("kernel32.dll", TerminateProcess, 2) {
  auto p = GetHandle(uint32_t(Arg(c, 0)));
  if (p && p->type == KObject::Type::Process) {
    std::fprintf(stderr, "TerminateProcess(self, %u) (return address %08X, guest thread %04X)\n",
                 uint32_t(Arg(c, 1)), uint32_t(Rd32(c.esp)), CurrentThread()->tid);
    std::_Exit(int(Arg(c, 1)));
  }
  return 0;
}

HLE64("kernel32.dll", GetLastError, 0) {
  return Rd32(Teb(CurrentThread()) + (g_proc->x64 ? 0x68 : 0x34));
}
HLE64("kernel32.dll", SetLastError, 1) {
  SetLastError(Arg(c, 0));
  return 0;
}

HLE64("kernel32.dll", SetUnhandledExceptionFilter, 1) {
  static gaddr filter = 0;
  gaddr old = filter;
  filter = Arg(c, 0);
  return old;
}

HLE64("kernel32.dll", IsDebuggerPresent, 0) { return 0; }

HLE64("kernel32.dll", OutputDebugStringA, 1) {
  std::fprintf(stderr, "[guest] %s", GStr(Arg(c, 0)).c_str());
  return 0;
}

HLE("kernel32.dll", SetHandleCount, 1) { return Arg(c, 0); }

// Looked up dynamically by the CRT (Pentium FDIV check). Matches the CPU
// that Cpuid() reports: MMX yes, no SSE/3DNow!, no FDIV erratum.
HLE64("kernel32.dll", IsProcessorFeaturePresent, 1) {
  switch (Arg(c, 0)) {
    case 3: return 1;   // PF_MMX_INSTRUCTIONS_AVAILABLE
    case 6: return 1;   // PF_XMMI_INSTRUCTIONS_AVAILABLE (SSE)
    case 8: return 1;   // PF_RDTSC_INSTRUCTION_AVAILABLE
    case 10: return 1;  // PF_XMMI64_INSTRUCTIONS_AVAILABLE (SSE2)
    // x64 guests: the extensions their CPUID model claims (dispatch.cpp).
    case 13:  // PF_SSE3_INSTRUCTIONS_AVAILABLE
    case 14:  // PF_COMPARE_EXCHANGE128
    case 36:  // PF_SSSE3_INSTRUCTIONS_AVAILABLE
    case 37:  // PF_SSE4_1_INSTRUCTIONS_AVAILABLE
    case 38:  // PF_SSE4_2_INSTRUCTIONS_AVAILABLE
      return g_proc->x64 ? 1 : 0;
    default: return 0;
  }
}

// x64 guests: SYSTEM_INFO's pointer-sized fields, an AMD64 processor (the
// CPUID model's family 16h) and the address range guest allocations use.
HLE64("kernel32.dll", GetSystemInfo, 1) {
  gaddr si = Arg(c, 0);
  uint32_t ncpu = GuestProcessorCount();
  if (g_proc->x64) {
    std::memset(GPtr(si), 0, 48);
    Wr16(si + 0, 9);              // PROCESSOR_ARCHITECTURE_AMD64
    Wr32(si + 4, 0x1000);         // page size
    Wr64(si + 8, kUserLo64);      // lowest application address
    Wr64(si + 16, kTebLo64 - 1);  // highest
    Wr64(si + 24, (1ull << ncpu) - 1);
    Wr32(si + 32, ncpu);
    Wr32(si + 36, 8664);          // PROCESSOR_AMD_X8664
    Wr32(si + 40, 0x10000);       // allocation granularity
    Wr16(si + 44, 0x16);          // level: the family
    Wr16(si + 46, 0x0001);        // revision: model 0, stepping 1
    return 0;
  }
  std::memset(GPtr(si), 0, 36);
  Wr16(si + 0, 0);            // PROCESSOR_ARCHITECTURE_INTEL
  Wr32(si + 4, 0x1000);       // page size
  Wr32(si + 8, 0x00010000);   // lowest application address
  Wr32(si + 12, 0x7FFEFFFF);  // highest
  Wr32(si + 16, (1u << ncpu) - 1);  // active processor mask
  Wr32(si + 20, ncpu);              // number of processors
  Wr32(si + 24, 586);         // PROCESSOR_INTEL_PENTIUM
  Wr32(si + 28, 0x10000);     // allocation granularity
  Wr16(si + 32, 6);           // level
  Wr16(si + 34, 0x0803);      // revision
  return 0;
}

HLE("kernel32.dll", GlobalMemoryStatus, 1) {
  // A roomy 2002-era machine: 1 GiB RAM, 1.5 GiB page file. Every value
  // stays below 2 GiB: the game compares these as signed ints (a 2 GiB
  // page file reads as negative and triggers its "swap file" error).
  uint32_t ms = Arg(c, 0);
  Wr32(ms + 0, 32);
  Wr32(ms + 4, 30);           // memory load %
  Wr32(ms + 8, 0x40000000);   // total phys
  Wr32(ms + 12, 0x30000000);  // avail phys
  Wr32(ms + 16, 0x60000000);  // total page file
  Wr32(ms + 20, 0x50000000);  // avail page file
  Wr32(ms + 24, 0x7FFE0000);  // total virtual
  Wr32(ms + 28, 0x70000000);  // avail virtual
  return 0;
}

// The same machine as GlobalMemoryStatus, through MEMORYSTATUSEX. x64
// guests see a current one instead (16 GiB RAM, 24 GiB commit limit) and
// the virtual address range guest allocations use.
HLE64("kernel32.dll", GlobalMemoryStatusEx, 1) {
  gaddr ms = Arg(c, 0);
  if (Rd32(ms) != 64) {
    SetLastError(kErrorInvalidParameter);
    return 0;
  }
  if (g_proc->x64) {
    Wr32(ms + 4, 25);
    Wr64(ms + 8, 16ull << 30);
    Wr64(ms + 16, 12ull << 30);
    Wr64(ms + 24, 24ull << 30);
    Wr64(ms + 32, 20ull << 30);
    Wr64(ms + 40, kTebLo64 - kUserLo64);
    Wr64(ms + 48, kTebLo64 - kUserLo64);  // not tracked: all of it
    Wr64(ms + 56, 0);
    return 1;
  }
  Wr32(ms + 4, 30);
  Wr64(ms + 8, 0x40000000);
  Wr64(ms + 16, 0x30000000);
  Wr64(ms + 24, 0x60000000);
  Wr64(ms + 32, 0x50000000);
  Wr64(ms + 40, 0x7FFE0000);
  Wr64(ms + 48, 0x70000000);
  Wr64(ms + 56, 0);  // ullAvailExtendedVirtual: reserved
  return 1;
}

// a * b / c in 64 bits, rounded to nearest with halves away from zero;
// -1 if c is 0 or the result does not fit in an int.
HLE("kernel32.dll", MulDiv, 3) {
  int64_t p = int64_t(int32_t(Arg(c, 0))) * int32_t(Arg(c, 1));
  int64_t d = int32_t(Arg(c, 2));
  if (d == 0) return 0xFFFFFFFFu;
  if (d < 0) {
    p = -p;
    d = -d;
  }
  int64_t q = (p >= 0 ? p + d / 2 : p - d / 2) / d;
  if (q > INT32_MAX || q < -INT32_MAX) return 0xFFFFFFFFu;
  return uint32_t(int32_t(q));
}

HLE("kernel32.dll", GetComputerNameA, 2) {
  uint32_t buf = Arg(c, 0), psize = Arg(c, 1);
  std::string n = win32::Upper(Game().id) + "HOST";
  if (Rd32(psize) < n.size() + 1) {
    Wr32(psize, uint32_t(n.size() + 1));
    SetLastError(kErrorBufferOverflow);
    return 0;
  }
  PutStr(buf, n);
  Wr32(psize, uint32_t(n.size()));
  return 1;
}

// ---- heaps ---------------------------------------------------------------

HLE64("kernel32.dll", GetProcessHeap, 0) { return g_win32->process_heap; }

HLE64("kernel32.dll", HeapCreate, 3) {
  GuestHeap* h = NewHeap(Arg(c, 1), Arg(c, 2));
  return h ? h->handle() : 0;
}

HLE64("kernel32.dll", HeapDestroy, 1) {
  std::lock_guard<std::mutex> l(g_heaps_mu);
  return g_heaps.erase(Arg(c, 0)) ? 1 : 0;
}

HLE64("kernel32.dll", HeapAlloc, 3) {
  GuestHeap* h = FindHeap(Arg(c, 0));
  if (!h) FatalF("HeapAlloc on unknown heap %08llX", (unsigned long long)Arg(c, 0));
  return h->Alloc(Arg(c, 2), Arg(c, 1) & kHeapZeroMemory);
}

HLE64("kernel32.dll", HeapFree, 3) {
  gaddr p = Arg(c, 2);
  if (!p) return 1;
  GuestHeap* h = FindHeap(Arg(c, 0));
  if (!h || !h->Free(p)) {
    WarnF("HeapFree(%08llX, %08llX): not a live block\n", (unsigned long long)Arg(c, 0),
          (unsigned long long)p);
    return 0;
  }
  return 1;
}

HLE64("kernel32.dll", HeapReAlloc, 4) {
  GuestHeap* h = FindHeap(Arg(c, 0));
  uint32_t flags = uint32_t(Arg(c, 1));
  if (!h) return 0;
  return h->ReAlloc(Arg(c, 2), Arg(c, 3), flags & kHeapZeroMemory,
                    flags & kHeapReallocInPlaceOnly);
}

HLE64("kernel32.dll", HeapSize, 3) {
  GuestHeap* h = FindHeap(Arg(c, 0));
  // (SIZE_T)-1 on failure.
  return h ? h->Size(Arg(c, 2)) : g_proc->x64 ? ~uint64_t(0) : 0xFFFFFFFF;
}

// HeapSetInformation(heap, class, info, length): the low-fragmentation
// heap and terminate-on-corruption (the CRT's and Source's startup) change
// nothing a guest can see.
HLE64("kernel32.dll", HeapSetInformation, 4) { return 1; }

HLE64("kernel32.dll", GlobalAlloc, 2) {
  // GMEM_MOVEABLE handles are returned as plain pointers; GlobalLock is
  // then the identity. Only used for clipboard data here.
  return HeapAllocProcess(Arg(c, 1), Arg(c, 0) & 0x40);
}
HLE64("kernel32.dll", GlobalLock, 1) { return Arg(c, 0); }
HLE("kernel32.dll", GlobalSize, 1) {
  uint32_t s = g_process_heap->Size(Arg(c, 0));
  return s == 0xFFFFFFFF ? 0 : s;
}
HLE64("kernel32.dll", GlobalUnlock, 1) { return 1; }
HLE("kernel32.dll", GlobalFree, 1) {
  g_process_heap->Free(Arg(c, 0));
  return 0;
}

// LocalAlloc(flags, size): like GlobalAlloc, LMEM_MOVEABLE handles are plain
// pointers (LocalLock would be the identity); LMEM_ZEROINIT is 0x40.
HLE64("kernel32.dll", LocalAlloc, 2) {
  return HeapAllocProcess(Arg(c, 1) ? Arg(c, 1) : 1, Arg(c, 0) & 0x40);
}

HLE64("kernel32.dll", LocalFree, 1) {
  if (Arg(c, 0)) g_process_heap->Free(Arg(c, 0));
  return 0;
}

// Handles are the blocks' pointers (above), so locking is the identity
// and there is no lock count: Unlock reports "no longer locked" (0),
// Flags none.
HLE("kernel32.dll", LocalLock, 1) { return Arg(c, 0); }
HLE("kernel32.dll", LocalUnlock, 1) { return 0; }
HLE("kernel32.dll", LocalSize, 1) {
  uint32_t s = g_process_heap->Size(Arg(c, 0));
  return s == 0xFFFFFFFF ? 0 : s;
}
HLE("kernel32.dll", GlobalFlags, 1) { return 0; }
HLE("kernel32.dll", GlobalHandle, 1) { return Arg(c, 0); }

// LocalReAlloc / GlobalReAlloc(handle, size, flags): MODIFY (0x80) changes
// only attributes (nothing to do); otherwise resized, the growth zeroed
// with ZEROINIT (0x40), moved only if MOVEABLE (2), else NULL when it
// cannot grow in place.
uint32_t ReAllocBlock(uint32_t h, uint32_t size, uint32_t flags) {
  if (flags & 0x80) return h;
  if (!h) return 0;
  return g_process_heap->ReAlloc(h, size ? size : 1, flags & 0x40, !(flags & 2));
}
HLE("kernel32.dll", LocalReAlloc, 3) { return ReAllocBlock(Arg(c, 0), Arg(c, 1), Arg(c, 2)); }
HLE("kernel32.dll", GlobalReAlloc, 3) { return ReAllocBlock(Arg(c, 0), Arg(c, 1), Arg(c, 2)); }

// ---- virtual memory ------------------------------------------------------

Prot ProtFromWin(uint32_t p) {
  switch (p & 0xFF) {
    case kPageNoAccess: return Prot::None;
    case kPageReadOnly: return Prot::R;
    case kPageReadWrite:
    case kPageWriteCopy: return Prot::RW;
    case kPageExecute: return Prot::X;
    case kPageExecuteRead: return Prot::RX;
    default: return Prot::RWX;
  }
}

uint32_t ProtToWin(Prot p) {
  switch (p) {
    case Prot::None: return kPageNoAccess;
    case Prot::R: return kPageReadOnly;
    case Prot::RW: return kPageReadWrite;
    case Prot::X: return kPageExecute;
    case Prot::RX: return kPageExecuteRead;
    default: return kPageExecuteReadWrite;
  }
}

HLE64("kernel32.dll", VirtualAlloc, 4) {
  gaddr addr = Arg(c, 0);
  uint64_t size = Arg(c, 1);
  uint32_t type = uint32_t(Arg(c, 2)), prot = uint32_t(Arg(c, 3));
  GuestAllocator& vm = g_proc->vm;
  // MEM_COMMIT with no address reserves and commits, like MEM_RESERVE|COMMIT.
  if (!addr && (type & kMemCommit)) type |= kMemReserve;
  if (type & kMemReserve) {
    // A reservation starts at the address rounded down to 64 KiB and covers
    // every page of [addr, addr + size); MEM_COMMIT commits all of it, and
    // the result is the reservation's base.
    const uint64_t span = size + (addr & 0xFFFF);
    gaddr base = vm.Reserve(addr, span, type & kMemTopDown);
    if (!base) {
      SetLastError(kErrorNotEnoughMemory);
      return 0;
    }
    if (type & kMemCommit) vm.Commit(base, span, ProtFromWin(prot));
    return base;
  }
  if (type & kMemCommit) {
    if (!vm.Commit(addr, size, ProtFromWin(prot))) {
      SetLastError(kErrorInvalidAddress);
      return 0;
    }
    return addr & ~gaddr(0xFFF);
  }
  SetLastError(kErrorInvalidParameter);
  return 0;
}

HLE64("kernel32.dll", VirtualFree, 3) {
  gaddr addr = Arg(c, 0);
  uint64_t size = Arg(c, 1);
  uint32_t type = uint32_t(Arg(c, 2));
  // MEM_RELEASE takes a size of 0 and frees the whole reservation;
  // MEM_DECOMMIT fails for a range past its reservation (VirtualFree).
  if (type == kMemRelease && size == 0) {
    if (g_proc->vm.Release(addr)) return 1;
  } else if (type == kMemDecommit) {
    if (g_proc->vm.Decommit(addr, size)) return 1;
  } else {
    SetLastError(kErrorInvalidParameter);
    return 0;
  }
  SetLastError(kErrorInvalidAddress);
  return 0;
}

HLE("kernel32.dll", VirtualProtect, 4) {
  uint32_t addr = Arg(c, 0), size = Arg(c, 1), prot = Arg(c, 2),
           old = Arg(c, 3);
  if (old) Wr32(old, ProtToWin(g_proc->mem.QueryProt(addr)));
  return g_proc->mem.Protect(addr, size, ProtFromWin(prot)) ? 1 : 0;
}

HLE("kernel32.dll", VirtualLock, 2) { return 1; }

// MEMORY_BASIC_INFORMATION: {base, allocation base, allocation protect,
// region size, state, protect, type}, 28 bytes with 32-bit pointers and
// sizes, 48 on x64. Returns its size.
uint32_t QueryMemory(gaddr addr, gaddr mbi) {
  GuestMemory& mem = g_proc->mem;
  GuestAllocator::Region r;
  const bool x64 = g_proc->x64;
  const uint32_t size = x64 ? 48 : 28;
  auto put = [&](gaddr base, gaddr alloc_base, uint32_t alloc_prot, uint64_t region,
                 uint32_t state, uint32_t prot, uint32_t type) {
    std::memset(GPtr(mbi), 0, size);
    if (x64) {
      Wr64(mbi + 0, base);
      Wr64(mbi + 8, alloc_base);
      Wr32(mbi + 16, alloc_prot);
      Wr64(mbi + 24, region);
      Wr32(mbi + 32, state);
      Wr32(mbi + 36, prot);
      Wr32(mbi + 40, type);
    } else {
      Wr32(mbi + 0, uint32_t(base));
      Wr32(mbi + 4, uint32_t(alloc_base));
      Wr32(mbi + 8, alloc_prot);
      Wr32(mbi + 12, uint32_t(region));
      Wr32(mbi + 16, state);
      Wr32(mbi + 20, prot);
      Wr32(mbi + 24, type);
    }
  };
  gaddr page = addr & ~gaddr(0xFFF);
  if (!g_proc->vm.Find(addr, &r)) {
    uint64_t base, gap;
    g_proc->vm.FreeGap(addr, &base, &gap);
    put(base, 0, 0, gap, kMemFree, kPageNoAccess, 0);
    return size;
  }
  // Extend over pages with the same state.
  bool committed = mem.IsCommitted(page);
  Prot prot = mem.QueryProt(page);
  gaddr end = page;
  while (end < r.base + r.size && mem.IsCommitted(end) == committed &&
         (!committed || mem.QueryProt(end) == prot))
    end += 0x1000;
  const PeImage* img = ImageAt(r.base);
  put(page, r.base, kPageReadWrite, end - page, committed ? kMemCommit : kMemReserve,
      committed ? ProtToWin(prot) : 0,
      img && img->image_base == r.base ? kMemImage : kMemPrivate);
  return size;
}

// VirtualQuery(addr, mbi, length): 0 (ERROR_BAD_LENGTH) if the buffer is
// smaller than the structure.
HLE64("kernel32.dll", VirtualQuery, 3) {
  if (Arg(c, 2) < (g_proc->x64 ? 48u : 28u)) {
    SetLastError(24);  // ERROR_BAD_LENGTH
    return 0;
  }
  uint32_t r = QueryMemory(Arg(c, 0), Arg(c, 1));
  RECOMP_TRACE("  VirtualQuery(%llX) -> %u\n", (unsigned long long)Arg(c, 0), r);
  return r;
}

HLE("kernel32.dll", VirtualQueryEx, 4) {
  return QueryMemory(Arg(c, 1), Arg(c, 2));
}

bool Readable(uint32_t p, uint32_t n) {
  if (!n) return true;
  for (uint32_t a = p & ~0xFFFu; a < p + n && a >= (p & ~0xFFFu); a += 0x1000)
    if (!g_proc->mem.IsCommitted(a)) return false;
  return true;
}

HLE("kernel32.dll", IsBadReadPtr, 2) {
  return Readable(Arg(c, 0), Arg(c, 1)) ? 0 : 1;
}
HLE("kernel32.dll", IsBadWritePtr, 2) {
  return Readable(Arg(c, 0), Arg(c, 1)) ? 0 : 1;
}
HLE("kernel32.dll", IsBadCodePtr, 1) {
  return ImageAt(Arg(c, 0)) ? 0 : 1;
}

// ---- TLS (TlsAlloc slots live in the TEB, as on Windows) -----------------
// TlsSlots: TEB+0xE10 (4-byte slots) in 32-bit guests, TEB+0x1480 (8-byte)
// in x64 ones. TlsGetValue/TlsSetValue touch only the calling thread's TEB
// and are plain accesses on Windows too; TlsAlloc/TlsFree take a process
// lock there, so they are sync points here.

std::mutex g_tls_mu;
uint64_t g_tls_used = 0;

gaddr TlsSlot(gaddr teb, uint32_t i) {
  return g_proc->x64 ? teb + 0x1480 + 8 * i : teb + 0xE10 + 4 * i;
}

HLE64("kernel32.dll", TlsAlloc, 0) {
  GuestSyncPoint sync;
  std::lock_guard<std::mutex> l(g_tls_mu);
  for (uint32_t i = 0; i < 64; i++)
    if (!(g_tls_used & (1ull << i))) {
      g_tls_used |= 1ull << i;
      return i;
    }
  return 0xFFFFFFFF;
}

// Clears the slot in every thread, as Windows does, so that a TlsAlloc
// handing the index out again finds it null everywhere.
HLE64("kernel32.dll", TlsFree, 1) {
  GuestSyncPoint sync;
  const uint32_t i = uint32_t(Arg(c, 0));
  std::lock_guard<std::mutex> l(g_tls_mu);
  if (i >= 64 || !(g_tls_used & (1ull << i))) {
    SetLastError(kErrorInvalidParameter);
    return 0;
  }
  g_tls_used &= ~(1ull << i);
  ForEachTeb([&](gaddr teb) { WrPtr(TlsSlot(teb, i), 0); });
  return 1;
}

HLE64("kernel32.dll", TlsGetValue, 1) {
  uint32_t i = uint32_t(Arg(c, 0));
  if (i >= 64) return 0;
  SetLastError(0);
  return RdPtr(TlsSlot(Teb(CurrentThread()), i));
}

HLE64("kernel32.dll", TlsSetValue, 2) {
  uint32_t i = uint32_t(Arg(c, 0));
  if (i >= 64) return 0;
  WrPtr(TlsSlot(Teb(CurrentThread()), i), Arg(c, 1));
  return 1;
}

// ---- critical sections ---------------------------------------------------
// Windows XP's algorithm on the guest CRITICAL_SECTION itself, so the
// fields games may read hold what they hold there: LockCount is -1 when
// free and counts the Enters past the first (recursions and waiters),
// RecursionCount, OwningThread the owner's thread id. An uncontended Enter
// or Leave is one atomic on LockCount with no global lock. A thread that
// finds the section owned by another waits on a host semaphore keyed by
// the section's address (Windows' LockSemaphore, also created on first
// contention), signalled by the Leave that sees a waiter in LockCount.
// Enter and Leave are full barriers (ARCHITECTURE.md, memory ordering)
// through their seq_cst read-modify-write of LockCount: a lock-prefixed
// instruction on x86-64, LDADDAL on arm64 (LSE); no separate fence.
//
// Layouts: 32-bit {DebugInfo, LockCount +4, RecursionCount +8,
// OwningThread +12, LockSemaphore, SpinCount +20}, 24 bytes; x64 with
// pointer-sized DebugInfo, handles and SpinCount: LockCount +8,
// RecursionCount +0xC, OwningThread +0x10, SpinCount +0x20, 40 bytes (the
// thread id in OwningThread's low half).

struct CsSemaphore {
  std::mutex m;
  std::condition_variable cv;
  uint32_t count = 0;
};

std::mutex g_cs_mu;
std::map<gaddr, std::shared_ptr<CsSemaphore>> g_cs;  // under g_cs_mu

std::shared_ptr<CsSemaphore> CsSemaphoreFor(gaddr cs) {
  std::lock_guard<std::mutex> l(g_cs_mu);
  auto& s = g_cs[cs];
  if (!s) s = std::make_shared<CsSemaphore>();
  return s;
}

struct CsLayout {
  uint32_t lock_count, recursion, owner, spin, size;
};
const CsLayout& Cs() {
  static const CsLayout k32{4, 8, 12, 20, 24}, k64{8, 0xC, 0x10, 0x20, 40};
  return g_proc->x64 ? k64 : k32;
}

uint32_t* CsField(gaddr cs, uint32_t off) {
  return reinterpret_cast<uint32_t*>(GPtr(cs + off));
}

void InitCs(gaddr cs, uint32_t spin) {
  std::memset(GPtr(cs), 0, Cs().size);
  Wr32(cs + Cs().lock_count, 0xFFFFFFFF);  // -1 = free
  Wr32(cs + Cs().spin, spin);              // no effect here
}

// TryEnterCriticalSection: takes a free section or one this thread owns.
bool TryEnterCs(gaddr cs) {
  uint32_t tid = CurrentThread()->tid;
  uint32_t* lock_count = CsField(cs, Cs().lock_count);
  uint32_t* recursion = CsField(cs, Cs().recursion);
  uint32_t* owner = CsField(cs, Cs().owner);
  uint32_t free = 0xFFFFFFFF;
  if (__atomic_compare_exchange_n(lock_count, &free, 0, false, __ATOMIC_SEQ_CST,
                                  __ATOMIC_SEQ_CST)) {
    __atomic_store_n(owner, tid, __ATOMIC_RELAXED);
    __atomic_store_n(recursion, 1, __ATOMIC_RELAXED);
    return true;
  }
  if (__atomic_load_n(owner, __ATOMIC_RELAXED) != tid) return false;
  __atomic_add_fetch(lock_count, 1, __ATOMIC_SEQ_CST);
  __atomic_store_n(recursion, *recursion + 1, __ATOMIC_RELAXED);
  return true;
}

}  // namespace

void EnterCs(gaddr cs) {
  uint32_t tid = CurrentThread()->tid;
  uint32_t* lock_count = CsField(cs, Cs().lock_count);
  uint32_t* recursion = CsField(cs, Cs().recursion);
  uint32_t* owner = CsField(cs, Cs().owner);
  if (__atomic_add_fetch(lock_count, 1, __ATOMIC_SEQ_CST) != 0) {
    // Only the owner writes OwningThread, so it reads its own tid here
    // exactly when it already holds the section.
    if (__atomic_load_n(owner, __ATOMIC_RELAXED) == tid) {
      __atomic_store_n(recursion, *recursion + 1, __ATOMIC_RELAXED);
      return;
    }
    // Owned by another thread: wait for a Leave to hand it over.
    auto s = CsSemaphoreFor(cs);
    std::unique_lock<std::mutex> l(s->m);
    s->cv.wait(l, [&] { return s->count > 0; });
    s->count--;
  }
  __atomic_store_n(owner, tid, __ATOMIC_RELAXED);
  __atomic_store_n(recursion, 1, __ATOMIC_RELAXED);
}

void LeaveCs(gaddr cs) {
  uint32_t* lock_count = CsField(cs, Cs().lock_count);
  uint32_t* recursion = CsField(cs, Cs().recursion);
  uint32_t* owner = CsField(cs, Cs().owner);
  if (__atomic_load_n(owner, __ATOMIC_RELAXED) != CurrentThread()->tid) {
    // Windows would corrupt the section (and usually hang the next
    // Enter); leave it as it is.
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true))
      WarnF("LeaveCriticalSection(%08llX) by a thread that does not own it; "
            "ignored\n", (unsigned long long)cs);
    return;
  }
  uint32_t rc = *recursion - 1;
  __atomic_store_n(recursion, rc, __ATOMIC_RELAXED);
  if (rc) {
    __atomic_sub_fetch(lock_count, 1, __ATOMIC_SEQ_CST);
    return;
  }
  __atomic_store_n(owner, 0, __ATOMIC_RELAXED);
  if (int32_t(__atomic_sub_fetch(lock_count, 1, __ATOMIC_SEQ_CST)) >= 0) {
    // A waiter counted itself in LockCount: hand the section over.
    auto s = CsSemaphoreFor(cs);
    {
      std::lock_guard<std::mutex> l(s->m);
      s->count++;
    }
    s->cv.notify_one();
  }
}

namespace {

HLE64("kernel32.dll", InitializeCriticalSection, 1) {
  InitCs(Arg(c, 0), 0);
  return 0;
}

// InitializeCriticalSectionEx(cs, spin count, flags): the flags only
// control debug info.
HLE64("kernel32.dll", InitializeCriticalSectionEx, 3) {
  InitCs(Arg(c, 0), uint32_t(Arg(c, 1)));
  return 1;
}

HLE64("kernel32.dll", InitializeCriticalSectionAndSpinCount, 2) {
  InitCs(Arg(c, 0), uint32_t(Arg(c, 1)));
  return 1;
}

HLE64("kernel32.dll", DeleteCriticalSection, 1) {
  std::lock_guard<std::mutex> l(g_cs_mu);
  g_cs.erase(Arg(c, 0));
  return 0;
}

HLE64("kernel32.dll", EnterCriticalSection, 1) {
  EnterCs(Arg(c, 0));
  return 0;
}

HLE64("kernel32.dll", TryEnterCriticalSection, 1) { return TryEnterCs(Arg(c, 0)); }

HLE64("kernel32.dll", LeaveCriticalSection, 1) {
  LeaveCs(Arg(c, 0));
  return 0;
}

// x64 SLIST_HEADER: 16 bytes, empty when zero.
HLE64("kernel32.dll", InitializeSListHead, 1) {
  std::memset(GPtr(Arg(c, 0)), 0, g_proc->x64 ? 16 : 8);
  return 0;
}

// 32-bit SLIST_HEADER: one 64-bit word, Next (the first entry, bits 0-31),
// Depth (32-47) and Sequence (48-63), updated together by one
// compare-exchange as Windows does with cmpxchg8b. An entry's first dword
// is its Next link. Sequence counts every push and pop, so a pop that raced
// a pop and push of the same entry fails its exchange (the ABA case).
namespace {
std::atomic_ref<uint64_t> SListWord(gaddr head) {
  return std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t*>(GPtr(head)));
}
uint64_t SListPack(uint32_t next, uint64_t depth, uint64_t seq) {
  return next | (depth & 0xFFFF) << 32 | (seq & 0xFFFF) << 48;
}
}  // namespace

// x64 SLIST_HEADER: 16 bytes, updated by cmpxchg16b on Windows. The low
// qword holds Depth (bits 0-15) and Sequence (16-63); the high one the
// first entry's address (16-byte aligned, so its low 4 bits are free) with
// HeaderType (bit 0) set, as x64 Windows writes it. An entry's first qword
// is its Next link. The four operations run under one lock: guests reach
// the list only through these calls (RtlFirstEntrySList reads the high
// qword, which a locked update writes whole).
std::mutex g_slist64_mu;
gaddr SList64First(gaddr head) { return Rd64(head + 8) & ~uint64_t(0xF); }
void SList64Set(gaddr head, gaddr first, uint64_t depth, uint64_t seq) {
  Wr64(head, (depth & 0xFFFF) | seq << 16);
  Wr64(head + 8, first | 1);
}

// Returns the previous first entry (0 if the list was empty).
HLE64("kernel32.dll", InterlockedPushEntrySList, 2) {
  gaddr head = Arg(c, 0), entry = Arg(c, 1);
  if (g_proc->x64) {
    std::lock_guard<std::mutex> l(g_slist64_mu);
    const gaddr first = SList64First(head);
    const uint64_t lo = Rd64(head);
    Wr64(entry, first);
    SList64Set(head, entry, (lo & 0xFFFF) + 1, (lo >> 16) + 1);
    return first;
  }
  auto w = SListWord(head);
  uint64_t old = w.load();
  for (;;) {
    uint32_t first = uint32_t(old);
    Wr32(entry, first);
    uint64_t neu = SListPack(uint32_t(entry), (old >> 32) + 1, (old >> 48) + 1);
    if (w.compare_exchange_weak(old, neu)) return first;
  }
}

// Returns the removed first entry, or 0 if the list was empty.
HLE64("kernel32.dll", InterlockedPopEntrySList, 1) {
  if (g_proc->x64) {
    gaddr head = Arg(c, 0);
    std::lock_guard<std::mutex> l(g_slist64_mu);
    const gaddr first = SList64First(head);
    if (!first) return 0;
    const uint64_t lo = Rd64(head);
    SList64Set(head, Rd64(first), (lo & 0xFFFF) - 1, (lo >> 16) + 1);
    return first;
  }
  auto w = SListWord(Arg(c, 0));
  uint64_t old = w.load();
  for (;;) {
    uint32_t first = uint32_t(old);
    if (!first) return 0;
    uint64_t neu = SListPack(Rd32(first), (old >> 32) - 1, (old >> 48) + 1);
    if (w.compare_exchange_weak(old, neu)) return first;
  }
}

// Empties the list; returns the old first entry (the chain stays linked).
HLE64("kernel32.dll", InterlockedFlushSList, 1) {
  if (g_proc->x64) {
    gaddr head = Arg(c, 0);
    std::lock_guard<std::mutex> l(g_slist64_mu);
    const gaddr first = SList64First(head);
    SList64Set(head, 0, 0, (Rd64(head) >> 16) + 1);
    return first;
  }
  auto w = SListWord(Arg(c, 0));
  uint64_t old = w.load();
  while (!w.compare_exchange_weak(old, SListPack(0, 0, old >> 48))) {
  }
  return uint32_t(old);
}

HLE64("kernel32.dll", QueryDepthSList, 1) {
  if (g_proc->x64) {
    std::lock_guard<std::mutex> l(g_slist64_mu);
    return Rd64(Arg(c, 0)) & 0xFFFF;
  }
  return uint32_t(SListWord(Arg(c, 0)).load() >> 32 & 0xFFFF);
}

// ---- interlocked ---------------------------------------------------------
// seq_cst read-modify-writes, as lifted lock-prefixed instructions are.
// C++ orders those only against other seq_cst atomics, not against the
// plain accesses lifted code makes, so the GuestSyncPoint fences make each
// call a full barrier like `lock xadd`/`xchg`.

HLE("kernel32.dll", InterlockedIncrement, 1) {
  GuestSyncPoint sync;
  return __atomic_add_fetch(reinterpret_cast<uint32_t*>(GPtr(Arg(c, 0))), 1,
                            __ATOMIC_SEQ_CST);
}
HLE("kernel32.dll", InterlockedDecrement, 1) {
  GuestSyncPoint sync;
  return __atomic_sub_fetch(reinterpret_cast<uint32_t*>(GPtr(Arg(c, 0))), 1,
                            __ATOMIC_SEQ_CST);
}
HLE("kernel32.dll", InterlockedExchange, 2) {
  GuestSyncPoint sync;
  return __atomic_exchange_n(reinterpret_cast<uint32_t*>(GPtr(Arg(c, 0))),
                             Arg(c, 1), __ATOMIC_SEQ_CST);
}

// ---- time ----------------------------------------------------------------

HLE64("kernel32.dll", GetTickCount, 0) { return TickCount(); }
// ULONGLONG GetTickCount64(): EDX:EAX, milliseconds since start; does not
// wrap after 49.7 days as GetTickCount does.
HLE64("kernel32.dll", GetTickCount64, 0) {
  using namespace std::chrono;
  uint64_t ms = uint64_t(duration_cast<milliseconds>(steady_clock::now() - g_start).count());
  if (g_proc->x64) return ms;
  c.edx = uint32_t(ms >> 32);
  return uint32_t(ms);
}

HLE64("kernel32.dll", QueryPerformanceFrequency, 1) {
  Wr64(Arg(c, 0), 10000000);
  return 1;
}

HLE64("kernel32.dll", QueryPerformanceCounter, 1) {
  Wr64(Arg(c, 0), PerfCounter());
  return 1;
}

HLE64("kernel32.dll", GetSystemTimeAsFileTime, 1) {
  using namespace std::chrono;
  auto ns = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
  Wr64(Arg(c, 0), uint64_t(ns / 100) + kFileTimeUnixEpoch);
  return 0;
}

// GetThreadTimes(thread, creation, exit, kernel, user) and
// GetProcessTimes(process, ...): FILETIMEs. The CPU time is the host's:
// the calling thread's own for the current thread (profilers time their
// own work), the process's for the process and for any other thread
// (host threads are not tracked by guest handle). Creation is the
// process's start, exit 0 (still running); all CPU time counts as user.
uint64_t CpuTime100ns(clockid_t clock) {
  timespec ts;
  clock_gettime(clock, &ts);
  return uint64_t(ts.tv_sec) * 10000000 + uint64_t(ts.tv_nsec) / 100;
}
uint32_t PutTimes(Ctx& c, uint64_t cpu) {
  using namespace std::chrono;
  static const uint64_t start =
      uint64_t(duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() / 100) +
      kFileTimeUnixEpoch;
  if (Arg(c, 1)) Wr64(Arg(c, 1), start);
  if (Arg(c, 2)) Wr64(Arg(c, 2), 0);
  if (Arg(c, 3)) Wr64(Arg(c, 3), 0);
  if (Arg(c, 4)) Wr64(Arg(c, 4), cpu);
  return 1;
}
HLE("kernel32.dll", GetThreadTimes, 5) {
  return PutTimes(c, CpuTime100ns(uint32_t(Arg(c, 0)) == 0xFFFFFFFE ? CLOCK_THREAD_CPUTIME_ID
                                                                       : CLOCK_PROCESS_CPUTIME_ID));
}
HLE("kernel32.dll", GetProcessTimes, 5) { return PutTimes(c, CpuTime100ns(CLOCK_PROCESS_CPUTIME_ID)); }

HLE64("kernel32.dll", GetSystemTime, 1) {
  using namespace std::chrono;
  auto now = system_clock::now();
  std::time_t tt = system_clock::to_time_t(now);
  std::tm t{};
  gmtime_r(&tt, &t);
  int ms = int(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
  WriteSystemTime(Arg(c, 0), t, ms);
  return 0;
}

HLE("kernel32.dll", GetLocalTime, 1) {
  using namespace std::chrono;
  auto now = system_clock::now();
  std::time_t tt = system_clock::to_time_t(now);
  std::tm t{};
  localtime_r(&tt, &t);
  int ms = int(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
  WriteSystemTime(Arg(c, 0), t, ms);
  return 0;
}

// The host's current UTC offset as a zone without DST transitions
// (TIME_ZONE_ID_UNKNOWN): the guest's local time then equals the host's
// now, though not across a DST change while it runs.
HLE64("kernel32.dll", GetTimeZoneInformation, 1) {
  gaddr tzi = Arg(c, 0);
  std::time_t now = std::time(nullptr);
  std::tm t{};
  localtime_r(&now, &t);
  std::memset(GPtr(tzi), 0, 172);
  Wr32(tzi + 0, uint32_t(int32_t(-t.tm_gmtoff / 60)));  // Bias, minutes
  std::u16string name = Widen(t.tm_zone ? t.tm_zone : "UTC").substr(0, 31);
  std::memcpy(GPtr(tzi + 4), name.data(), name.size() * 2);    // StandardName
  std::memcpy(GPtr(tzi + 88), name.data(), name.size() * 2);   // DaylightName
  return 0;
}

HLE64("winmm.dll", timeGetTime, 0) { return TickCount(); }
// The host timer is always fine-grained; nothing to change.
HLE64("winmm.dll", timeBeginPeriod, 1) { return Arg(c, 0) ? 0 : 97; }  // TIMERR_NOCANDO
HLE64("winmm.dll", timeEndPeriod, 1) { return Arg(c, 0) ? 0 : 97; }

// No joystick or MIDI input drivers: games see zero devices.
HLE64("winmm.dll", joyGetNumDevs, 0) { return 0; }
HLE64("winmm.dll", joyGetDevCapsA, 3) { return 6; }   // MMSYSERR_NODRIVER
HLE64("winmm.dll", joyGetDevCapsW, 3) { return 6; }
HLE64("winmm.dll", joyGetPosEx, 2) { return 6; }
HLE("winmm.dll", midiInGetNumDevs, 0) { return 0; }
HLE("winmm.dll", midiInGetDevCapsA, 3) { return 2; }  // MMSYSERR_BADDEVICEID
HLE("winmm.dll", midiInOpen, 5) { return 2; }
HLE("winmm.dll", midiInStart, 1) { return 5; }  // MMSYSERR_INVALHANDLE
HLE("winmm.dll", midiInStop, 1) { return 5; }
HLE("winmm.dll", midiInClose, 1) { return 5; }

// A kernel entry on Windows, so a barrier for guests that poll a flag
// with Sleep in the loop.
HLE64("kernel32.dll", FlushProcessWriteBuffers, 0) {
  GuestSyncPoint sync;
  std::atomic_thread_fence(std::memory_order_seq_cst);
  return 0;
}

HLE64("kernel32.dll", GetCurrentProcessorNumber, 0) { return 0; }

HLE64("kernel32.dll", Sleep, 1) {
  GuestSyncPoint sync;
  std::this_thread::sleep_for(std::chrono::milliseconds(Arg(c, 0)));
  return 0;
}

// ---- code pages and locale -----------------------------------------------

HLE64("kernel32.dll", GetACP, 0) { return 1252; }
HLE64("kernel32.dll", GetOEMCP, 0) { return 437; }
HLE64("kernel32.dll", IsValidCodePage, 1) {
  uint32_t cp = uint32_t(Arg(c, 0));
  return cp == 1252 || cp == 437 || cp == 0 ? 1 : 0;
}

HLE64("kernel32.dll", GetCPInfo, 2) {
  gaddr info = Arg(c, 1);
  std::memset(GPtr(info), 0, 20);
  Wr32(info, 1);       // MaxCharSize
  Wr8(info + 4, '?');  // DefaultChar
  return 1;
}

// CP_UTF8 is UTF-8; every other code page is treated as Windows-1252 (the
// ANSI code page reported by GetACP).
constexpr uint32_t kCpUtf8 = 65001;
constexpr uint32_t kMbErrInvalidChars = 0x8, kWcErrInvalidChars = 0x80;
constexpr uint32_t kErrorNoUnicodeTranslation = 1113;

HLE64("kernel32.dll", MultiByteToWideChar, 6) {
  uint32_t cp = uint32_t(Arg(c, 0)), flags = uint32_t(Arg(c, 1));
  gaddr src = Arg(c, 2), dst = Arg(c, 4);
  int32_t n = int32_t(Arg(c, 3)), cap = int32_t(Arg(c, 5));
  if (n < 0) n = int32_t(std::strlen(g_proc->mem.ptr<const char>(src)) + 1);
  std::u16string w;
  if (cp == kCpUtf8) {
    bool bad;
    w = Utf8ToUtf16(GPtr(src), size_t(n), &bad);
    if (bad && (flags & kMbErrInvalidChars)) {
      SetLastError(kErrorNoUnicodeTranslation);
      return 0;
    }
  } else {
    for (int32_t i = 0; i < n; i++) w.push_back(Cp1252ToUnicode(g_proc->mem.read<uint8_t>(src + i)));
  }
  if (!cap) return w.size();
  if (w.size() > size_t(cap)) {
    SetLastError(kErrorInsufficientBuffer);
    return 0;
  }
  std::memcpy(GPtr(dst), w.data(), 2 * w.size());
  return w.size();
}

HLE64("kernel32.dll", WideCharToMultiByte, 8) {
  uint32_t cp = uint32_t(Arg(c, 0)), flags = uint32_t(Arg(c, 1));
  gaddr src = Arg(c, 2), dst = Arg(c, 4), default_char = Arg(c, 6), used_default = Arg(c, 7);
  int32_t n = int32_t(Arg(c, 3)), cap = int32_t(Arg(c, 5));
  if (n < 0) n = int32_t(GWStr(src).size() + 1);
  std::u16string w(size_t(n), u'\0');
  std::memcpy(w.data(), GPtr(src), 2 * size_t(n));
  std::string s;
  if (cp == kCpUtf8) {
    // UTF-8 has no default character.
    if (default_char || used_default) {
      SetLastError(kErrorInvalidParameter);
      return 0;
    }
    bool bad;
    s = Utf16ToUtf8(w.data(), w.size(), &bad);
    if (bad && (flags & kWcErrInvalidChars)) {
      SetLastError(kErrorNoUnicodeTranslation);
      return 0;
    }
  } else {
    if (used_default) Wr32(used_default, 0);
    for (char16_t ch : w) {
      bool ok = true;
      s.push_back(char(UnicodeToCp1252(ch, &ok)));
      if (!ok && used_default) Wr32(used_default, 1);
    }
  }
  if (!cap) return s.size();
  if (s.size() > size_t(cap)) {
    SetLastError(kErrorInsufficientBuffer);
    return 0;
  }
  std::memcpy(GPtr(dst), s.data(), s.size());
  return s.size();
}

HLE64("kernel32.dll", GetStringTypeW, 4) {
  uint32_t type = uint32_t(Arg(c, 0));
  gaddr src = Arg(c, 1), out = Arg(c, 3);
  int32_t n = int32_t(Arg(c, 2));
  if (type != kCtCtype1) return 0;
  if (n < 0) n = int32_t(GWStr(src).size() + 1);
  for (int32_t i = 0; i < n; i++) Wr16(out + 2 * i, CharType1(Rd16(src + 2 * i)));
  return 1;
}

HLE("kernel32.dll", GetStringTypeA, 5) {
  uint32_t type = Arg(c, 1), src = Arg(c, 2), out = Arg(c, 4);
  int32_t n = int32_t(Arg(c, 3));
  if (type != kCtCtype1) return 0;
  if (n < 0) n = int32_t(std::strlen(g_proc->mem.ptr<const char>(src)) + 1);
  for (int32_t i = 0; i < n; i++)
    Wr16(out + 2 * i,
         CharType1(Cp1252ToUnicode(g_proc->mem.read<uint8_t>(src + i))));
  return 1;
}

uint32_t LcMapW(uint32_t flags, gaddr src, int32_t n, gaddr dst, int32_t cap) {
  if (n < 0) n = int32_t(GWStr(src).size() + 1);
  if (!cap) return uint32_t(n);
  if (n > cap) return 0;
  for (int32_t i = 0; i < n; i++)
    Wr16(dst + 2 * i, MapCase(Rd16(src + 2 * i), flags));
  return uint32_t(n);
}

HLE64("kernel32.dll", LCMapStringW, 6) {
  return LcMapW(uint32_t(Arg(c, 1)), Arg(c, 2), int32_t(Arg(c, 3)), Arg(c, 4), int32_t(Arg(c, 5)));
}

// LCMapStringEx(locale name, flags, src, cch, dst, cap, version, reserved,
// sort handle): the Vista form of LCMapStringW; every locale is en-US.
HLE64("kernel32.dll", LCMapStringEx, 9) {
  return LcMapW(uint32_t(Arg(c, 1)), Arg(c, 2), int32_t(Arg(c, 3)), Arg(c, 4), int32_t(Arg(c, 5)));
}

HLE64("kernel32.dll", LCMapStringA, 6) {
  uint32_t flags = uint32_t(Arg(c, 1));
  gaddr src = Arg(c, 2), dst = Arg(c, 4);
  int32_t n = int32_t(Arg(c, 3)), cap = int32_t(Arg(c, 5));
  if (n < 0) n = int32_t(std::strlen(g_proc->mem.ptr<const char>(src)) + 1);
  if (!cap) return uint32_t(n);
  if (n > cap) return 0;
  for (int32_t i = 0; i < n; i++) {
    bool ok;
    uint16_t u = MapCase(Cp1252ToUnicode(g_proc->mem.read<uint8_t>(src + i)),
                         flags);
    Wr8(dst + i, UnicodeToCp1252(u, &ok));
  }
  return uint32_t(n);
}

HLE64("kernel32.dll", GetUserDefaultLCID, 0) { return 0x0409; }
HLE("kernel32.dll", GetUserDefaultLangID, 0) { return 0x0409; }
HLE("kernel32.dll", IsValidLocale, 2) { return Arg(c, 0) == 0x0409 ? 1 : 0; }

HLE("kernel32.dll", EnumSystemLocalesA, 2) {
  uint32_t buf = HeapAllocProcess(16, true);
  PutStr(buf, "00000409");
  CallGuest(c, Arg(c, 0), {buf});
  g_process_heap->Free(buf);
  return 1;
}

HLE("kernel32.dll", EnumSystemLocalesW, 2) {
  uint32_t buf = HeapAllocProcess(32, true);
  PutWStr(buf, "00000409");
  CallGuest(c, Arg(c, 0), {buf});
  g_process_heap->Free(buf);
  return 1;
}

// ---- collation -----------------------------------------------------------
// CompareString for the one locale here (0x0409), on UTF-16 code units:
// letters compare without case first, then, between strings equal that
// way, lower case sorts before upper case ("a" < "A" < "b"), as the en-US
// table orders them. NORM_IGNORECASE (1) drops the second step. Not
// modelled: the word sort's ignoring of '-' and '\'', and expansions
// such as "ae" = U+00E6.
namespace {
constexpr uint32_t kNormIgnoreCase = 1;

int CollateUnits(const std::u16string& a, const std::u16string& b, uint32_t flags) {
  auto fold = [](uint16_t u) { return MapCase(u, kLcmapLowercase); };
  size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; i++) {
    uint16_t x = fold(a[i]), y = fold(b[i]);
    if (x != y) return x < y ? -1 : 1;
  }
  if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
  if (flags & kNormIgnoreCase) return 0;
  for (size_t i = 0; i < n; i++) {
    if (a[i] == b[i]) continue;
    bool la = CharType1(a[i]) & 2, lb = CharType1(b[i]) & 2;  // C1_LOWER
    if (la != lb) return la ? -1 : 1;
    return a[i] < b[i] ? -1 : 1;
  }
  return 0;
}

// cch < 0: NUL-terminated.
std::u16string WideArg(gaddr s, int32_t cch) {
  if (cch < 0) return GWStr(s);
  std::u16string w(size_t(cch), u'\0');
  std::memcpy(w.data(), GPtr(s), size_t(cch) * 2);
  return w;
}
std::u16string AnsiArg(gaddr s, int32_t cch) {
  std::string a = cch < 0 ? GStr(s) : std::string(reinterpret_cast<const char*>(GPtr(s)), size_t(cch));
  std::u16string w;
  for (char ch : a) w += char16_t(Cp1252ToUnicode(uint8_t(ch)));
  return w;
}
}  // namespace

// CompareStringW(lcid, flags, s1, cch1, s2, cch2): CSTR_LESS_THAN (1),
// CSTR_EQUAL (2) or CSTR_GREATER_THAN (3).
HLE64("kernel32.dll", CompareStringW, 6) {
  uint32_t flags = uint32_t(Arg(c, 1));
  return uint32_t(2 + CollateUnits(WideArg(Arg(c, 2), int32_t(Arg(c, 3))),
                                   WideArg(Arg(c, 4), int32_t(Arg(c, 5))), flags));
}

// CompareStringEx(locale name, flags, s1, cch1, s2, cch2, version,
// reserved, param): CompareStringW for the one locale.
HLE64("kernel32.dll", CompareStringEx, 9) {
  uint32_t flags = uint32_t(Arg(c, 1));
  return uint32_t(2 + CollateUnits(WideArg(Arg(c, 2), int32_t(Arg(c, 3))),
                                   WideArg(Arg(c, 4), int32_t(Arg(c, 5))), flags));
}

HLE("kernel32.dll", CompareStringA, 6) {
  uint32_t flags = uint32_t(Arg(c, 1));
  return uint32_t(2 + CollateUnits(AnsiArg(Arg(c, 2), int32_t(Arg(c, 3))),
                                   AnsiArg(Arg(c, 4), int32_t(Arg(c, 5))), flags));
}

// lstrcmp(i)A: CompareStringA in the user's locale, as -1/0/1.
HLE("kernel32.dll", lstrcmpA, 2) {
  return uint32_t(CollateUnits(AnsiArg(Arg(c, 0), -1), AnsiArg(Arg(c, 1), -1), 0));
}
HLE("kernel32.dll", lstrcmpiA, 2) {
  return uint32_t(
      CollateUnits(AnsiArg(Arg(c, 0), -1), AnsiArg(Arg(c, 1), -1), kNormIgnoreCase));
}

std::string LocaleString(uint32_t type) {
  switch (type & 0xFFFF) {
    case 0x0001: return "0409";            // LOCALE_ILANGUAGE
    case 0x0002: return "English (United States)";
    case 0x0003: return "ENU";             // LOCALE_SABBREVLANGNAME
    case 0x0004: return "English";         // LOCALE_SNATIVELANGNAME
    case 0x0005: return "1";               // LOCALE_ICOUNTRY
    case 0x0006: return "United States";
    case 0x0007: return "USA";             // LOCALE_SABBREVCTRYNAME
    case 0x0009: return "0409";            // LOCALE_IDEFAULTLANGUAGE
    case 0x000A: return "1";               // LOCALE_IDEFAULTCOUNTRY
    case 0x000B: return "437";             // LOCALE_IDEFAULTCODEPAGE
    case 0x000E: return ".";               // LOCALE_SDECIMAL
    case 0x000F: return ",";               // LOCALE_STHOUSAND
    case 0x0010: return "3;0";             // LOCALE_SGROUPING
    case 0x0014: return "$";               // LOCALE_SCURRENCY
    case 0x0015: return "USD";             // LOCALE_SINTLSYMBOL
    case 0x0016: return ".";               // LOCALE_SMONDECIMALSEP
    case 0x0017: return ",";               // LOCALE_SMONTHOUSANDSEP
    case 0x0018: return "3;0";
    case 0x0019: return "2";
    case 0x001A: return "2";
    case 0x001B: return "0";
    case 0x001C: return "0";
    case 0x001F: return "M/d/yyyy";        // LOCALE_SSHORTDATE
    case 0x0020: return "dddd, MMMM d, yyyy";  // LOCALE_SLONGDATE
    case 0x0028: return "AM";              // LOCALE_S1159
    case 0x0029: return "PM";              // LOCALE_S2359
    case 0x0050: return "";                // LOCALE_SPOSITIVESIGN
    case 0x0051: return "-";               // LOCALE_SNEGATIVESIGN
    case 0x0059: return "en";              // LOCALE_SISO639LANGNAME
    case 0x005A: return "US";              // LOCALE_SISO3166CTRYNAME
    case 0x0052: return "3";               // LOCALE_IPOSSIGNPOSN
    case 0x0053: return "0";               // LOCALE_INEGSIGNPOSN
    case 0x0054: return "1";               // LOCALE_IPOSSYMPRECEDES
    case 0x0055: return "0";               // LOCALE_IPOSSEPBYSPACE
    case 0x0056: return "1";               // LOCALE_INEGSYMPRECEDES
    case 0x0057: return "0";               // LOCALE_INEGSEPBYSPACE
    case 0x005C: return "en-US";           // LOCALE_SNAME (Vista and later)
    case 0x1001: return "English";         // LOCALE_SENGLANGUAGE
    case 0x1002: return "United States";   // LOCALE_SENGCOUNTRY
    case 0x1003: return "h:mm:ss tt";      // LOCALE_STIMEFORMAT
    case 0x1004: return "1252";            // LOCALE_IDEFAULTANSICODEPAGE
    case 0x1009: return "1";               // LOCALE_ICALENDARTYPE (Gregorian)
    default: break;
  }
  // LOCALE_SDAYNAME1..7 (0x2A, Monday first), LOCALE_SABBREVDAYNAME1..7
  // (0x31), LOCALE_SMONTHNAME1..12 (0x38), LOCALE_SABBREVMONTHNAME1..12
  // (0x44).
  static const char* const kDays[] = {"Monday", "Tuesday", "Wednesday", "Thursday",
                                      "Friday", "Saturday", "Sunday"};
  static const char* const kMonths[] = {"January", "February", "March",     "April",
                                        "May",     "June",     "July",      "August",
                                        "September", "October", "November", "December"};
  uint32_t t = type & 0xFFFF;
  if (t >= 0x2A && t < 0x31) return kDays[t - 0x2A];
  if (t >= 0x31 && t < 0x38) return std::string(kDays[t - 0x31], 3);
  if (t >= 0x38 && t < 0x44) return kMonths[t - 0x38];
  if (t >= 0x44 && t < 0x50) return std::string(kMonths[t - 0x44], 3);
  return {};
}

// GetLocaleInfoA/W(lcid, type, buf, cap).
uint32_t LocaleInfo(Ctx& c, bool wide) {
  uint32_t type = uint32_t(Arg(c, 1));
  std::string s = LocaleString(type);
  if (s.empty() && (type & 0xFFFF) != 0x0050) {
    WarnF("GetLocaleInfo%c(%X, %X): unknown type\n", wide ? 'W' : 'A', uint32_t(Arg(c, 0)),
          type);
    SetLastError(kErrorInvalidParameter);
    return 0;
  }
  // LOCALE_RETURN_NUMBER: a DWORD in the buffer instead of the digits; the
  // count is its size in characters.
  if (type & 0x20000000) {
    uint32_t chars = wide ? 2 : 4, cap = uint32_t(Arg(c, 3));
    if (!cap) return chars;
    if (cap < chars) {
      SetLastError(kErrorInsufficientBuffer);
      return 0;
    }
    Wr32(Arg(c, 2), uint32_t(std::strtoul(s.c_str(), nullptr, 10)));
    return chars;
  }
  return PutCounted(Arg(c, 2), uint32_t(Arg(c, 3)), s, wide);
}

HLE64("kernel32.dll", GetLocaleInfoA, 4) { return LocaleInfo(c, false); }
HLE64("kernel32.dll", GetLocaleInfoW, 4) { return LocaleInfo(c, true); }
// GetLocaleInfoEx(name, type, buf, cap): the name is ignored (en-US), the
// other arguments are GetLocaleInfoW's.
HLE64("kernel32.dll", GetLocaleInfoEx, 4) { return LocaleInfo(c, true); }

}  // namespace
}  // namespace hle
}  // namespace recomp
