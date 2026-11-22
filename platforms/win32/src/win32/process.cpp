// The Win32 process (win32/process.h): the PE image and lifted DLLs, the
// PEB, TEBs and static TLS, on the x86 core's process.

#include "win32/process.h"

#include <sys/resource.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <set>

#include "runtime/log.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "win32/user32_window.h"

namespace recomp {

namespace hle {
bool RaiseAccessViolation(Ctx& c, bool write, uint32_t addr);
}

Win32Process* g_win32 = nullptr;

const win32::GameConfig& Game() { return g_win32->game; }

namespace {

constexpr uint32_t kTlsArraySlots = 64;

// TEBs: a finished thread's is taken back and handed out again before a
// new one is cut below the others.
std::mutex g_teb_mu;
gaddr g_next_teb = 0;  // the next new TEB is below it (kTebTop, kTebTop64)
std::vector<gaddr> g_free_tebs;
std::set<gaddr> g_live_tebs;

// The image's .text, where its lifted functions are.
std::pair<gaddr, uint64_t> TextOf(const PeImage& img) {
  for (const auto& s : img.sections)
    if (s.name == ".text") return {s.va, s.vsize};
  return {0, 0};
}

// Process::thread_init: the thread's TEB and TLS array, in the layout of
// the process's mode, found through fs (32-bit) or gs (x64).
void InitTeb(Thread* t) {
  Process& p = *g_proc;
  const Win32Process& w = *g_win32;
  const uint32_t stride = p.x64 ? kTebStride64 : kTebStride;
  gaddr teb;
  {
    std::lock_guard<std::mutex> l(g_teb_mu);
    if (!g_free_tebs.empty()) {
      teb = g_free_tebs.back();
      g_free_tebs.pop_back();
    } else {
      const gaddr lo = p.x64 ? kTebLo64 : kTebLo;
      if (!g_next_teb) g_next_teb = p.x64 ? kTebTop64 : kTebTop;
      if (g_next_teb - lo < stride)
        FatalF("too many guest threads at once: %llu TEBs in use",
               (unsigned long long)g_live_tebs.size());
      g_next_teb -= stride;
      teb = g_next_teb;
    }
    p.mem.Commit(teb, stride, Prot::RW);
    g_live_tebs.insert(teb);
  }
  if (p.x64)
    t->ctx.gs_base = teb;
  else
    t->ctx.fs_base = teb;
  // Static TLS of the image (__declspec(thread)): a per-thread copy of the
  // template, found through the TEB's ThreadLocalStoragePointer
  // [tls_index].
  const uint32_t ptr = p.x64 ? 8 : 4;
  gaddr tls_array = HeapAllocProcess(kTlsArraySlots * ptr, true);
  if (p.x64) {  // NT_TIB64 and the x64 TEB
    Wr64(teb + 0x08, t->stack_hi);  // StackBase
    Wr64(teb + 0x10, t->stack_lo);  // StackLimit
    Wr64(teb + 0x30, teb);          // Self
    Wr64(teb + 0x40, kGuestPid);    // ClientId
    Wr64(teb + 0x48, t->tid);
    Wr64(teb + 0x58, tls_array);    // ThreadLocalStoragePointer
    Wr64(teb + 0x60, kPebVa64);
  } else {
    Wr32(teb + 0x00, 0xFFFFFFFF);  // SEH chain: empty
    Wr32(teb + 0x04, uint32_t(t->stack_hi));
    Wr32(teb + 0x08, uint32_t(t->stack_lo));
    Wr32(teb + 0x18, uint32_t(teb));  // Self
    Wr32(teb + 0x20, kGuestPid);
    Wr32(teb + 0x24, t->tid);
    Wr32(teb + 0x2C, uint32_t(tls_array));
    Wr32(teb + 0x30, kPebVa);
  }
  auto block = [&](const PeImage& img, int index) {
    const PeTls& tls = img.tls;
    uint64_t raw = tls.raw_end - tls.raw_start;
    gaddr b = HeapAllocProcess(raw + tls.zero_fill + 16, true);
    std::memcpy(GPtr(b), GPtr(tls.raw_start), raw);
    WrPtr(tls_array + ptr * uint32_t(index), b);
  };
  if (w.img.has_tls) block(w.img, 0);
  // Lifted DLLs with static TLS (as on Vista and later): every thread has
  // their blocks, loaded or not, since all are mapped at start.
  for (const Module& m : w.modules)
    if (m.tls_index >= 0) block(m.img, m.tls_index);
}

// Process::thread_exit: whoever waits for the thread may go on, then what
// it had goes back (its guest code has finished): the static TLS blocks
// and their array, and the TEB. The core releases the stack itself after
// this hook.
void FinishThread(Thread* t, uint32_t code) {
  OnGuestThreadFinished(t->tid, code);
  Process& p = *g_proc;
  const Win32Process& w = *g_win32;
  const gaddr teb = Teb(t);
  const uint32_t ptr = p.x64 ? 8 : 4;
  const gaddr tls_array = RdPtr(teb + (p.x64 ? 0x58 : 0x2C));
  if (w.img.has_tls) HeapFreeProcess(RdPtr(tls_array));
  for (const Module& m : w.modules)
    if (m.tls_index >= 0) HeapFreeProcess(RdPtr(tls_array + ptr * uint32_t(m.tls_index)));
  HeapFreeProcess(tls_array);
  {
    std::lock_guard<std::mutex> l(g_teb_mu);
    g_live_tebs.erase(teb);
    p.mem.Decommit(teb, p.x64 ? kTebStride64 : kTebStride);
    g_free_tebs.push_back(teb);
  }
}

// Static TLS callbacks are not run (no supported module has any).
void CheckNoTlsCallbacks(const PeImage& img, const std::string& name) {
  if (img.has_tls && img.tls.callbacks && RdPtr(img.tls.callbacks))
    FatalF("%s has TLS callbacks: not supported", name.c_str());
}

}  // namespace

void ForEachTeb(const std::function<void(gaddr teb)>& f) {
  std::lock_guard<std::mutex> l(g_teb_mu);
  for (gaddr teb : g_live_tebs) f(teb);
}

const PeImage* ImageAt(gaddr va) {
  const Win32Process& w = *g_win32;
  if (va - w.img.image_base < w.img.image_size) return &w.img;
  for (const Module& m : w.modules)
    if (va - m.img.image_base < m.img.image_size) return &m.img;
  return nullptr;
}

// An import of `importer` (the EXE: null) from `dll`: a lifted DLL's export
// (by name, or "#<ordinal>"), as Windows' loader binds static imports, else
// the HLE. A lifted DLL not mapped yet gets the HLE thunk for now
// (RunProcess maps every module twice, so the second pass binds it).
static gaddr BindImport(Module* importer, const std::string& dll, const std::string& name) {
  std::string n = hle::NormaliseDll(dll);
  for (Module& m : g_win32->modules) {
    if (m.name != n) continue;
    if (!m.img.image_size) return hle::ImportThunk(dll, name);
    if (importer) {
      if (std::find(importer->deps.begin(), importer->deps.end(), &m) == importer->deps.end())
        importer->deps.push_back(&m);
    } else {
      m.static_import = true;
    }
    bool by_ordinal = !name.empty() && name[0] == '#';
    uint32_t ordinal = by_ordinal ? uint32_t(std::stoul(name.substr(1))) : 0;
    for (const PeExport& e : m.img.exports)
      if (by_ordinal ? e.ordinal == ordinal : e.name == name) return e.va;
    FatalF("%s imports %s!%s, which %s does not export",
           importer ? importer->name.c_str() : Game().exe_name.c_str(), dll.c_str(), name.c_str(),
           m.name.c_str());
  }
  return hle::ImportThunk(dll, name);
}

void MapModule(Module& m) {
  Process& p = *g_proc;
  if (m.img.image_size) p.mem.Decommit(m.img.image_base, m.img.image_size);
  std::string err;
  m.deps.clear();
  auto bind = [&m](const std::string& dll, const std::string& name) {
    return BindImport(&m, dll, name);
  };
  if (!m.img.Load(m.host_path, p.mem, &err, m.base, bind))
    FatalF("lifted DLL %s: %s", m.name.c_str(), err.c_str());
  // Static TLS of a LoadLibrary'd DLL, as Windows has had it since Vista
  // (XP set it up only for DLLs loaded with the EXE, so 32-bit DLLs of
  // that era have none; later 32-bit ones, such as Source's of 2024, and
  // x64 ones do): a slot of its own after the EXE's (slot 0), kept across
  // remaps.
  if (m.img.has_tls) {
    CheckNoTlsCallbacks(m.img, m.name);
    if (m.tls_index < 0) {
      int next = 1;
      for (const Module& o : g_win32->modules) next = std::max(next, o.tls_index + 1);
      m.tls_index = next;
    }
    Wr32(m.img.tls.index_va, uint32_t(m.tls_index));
  }
  m.stale = false;
}

bool AttachModule(Ctx& c, Module& m) {
  if (m.refs++) return true;
  if (m.stale) MapModule(m);
  for (size_t i = 0; i < m.deps.size(); i++)
    if (!AttachModule(c, *m.deps[i])) {
      while (i--) DetachModule(c, *m.deps[i]);
      m.refs = 0;
      return false;
    }
  if (!m.img.entry) return true;
  m.stale = true;
  RECOMP_TRACE("  %s: DllMain(PROCESS_ATTACH)\n", m.name.c_str());
  if (uint32_t(CallGuest(c, m.img.entry, {m.img.image_base, 1 /* PROCESS_ATTACH */, 0})))
    return true;
  for (Module* d : m.deps) DetachModule(c, *d);
  m.refs = 0;
  return false;
}

void DetachModule(Ctx& c, Module& m) {
  if (m.refs == 0 || --m.refs) return;
  if (m.img.entry) {
    RECOMP_TRACE("  %s: DllMain(PROCESS_DETACH)\n", m.name.c_str());
    CallGuest(c, m.img.entry, {m.img.image_base, 0 /* PROCESS_DETACH */, 0});
  }
  for (auto it = m.deps.rbegin(); it != m.deps.rend(); ++it) DetachModule(c, **it);
}

// Windows has no small cap on open handles, and games keep their data
// files open for the whole session (hundreds of them). A Unix process
// starts with the soft limit it inherits: on macOS 256 for an app started
// from Finder or `open`, where running out fails opens with EMFILE and the
// game reads missing data. Raise the soft limit to the hard one (or, where
// the kernel refuses that, as macOS does above its per-process maximum, to
// OPEN_MAX).
static void RaiseOpenFileLimit() {
  struct rlimit rl;
  if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return;
  const rlim_t before = rl.rlim_cur;
  for (rlim_t want : {rl.rlim_max, rlim_t(10240)}) {
    if (want <= before) break;
    struct rlimit r = rl;
    r.rlim_cur = want;
    if (setrlimit(RLIMIT_NOFILE, &r) == 0) return;
  }
  if (before < 4096)
    WarnF("open files limited to %llu (raising it failed)\n", (unsigned long long)before);
}

int RunProcess(const win32::GameConfig& game, const std::string& exe_path,
               const std::string& game_dir, const std::string& game_args) {
  RaiseOpenFileLimit();
  g_win32 = new Win32Process;
  Win32Process& w = *g_win32;
  w.game = game;
  Process& p = StartProcess(game, game_dir, PeImage::IsX64File(exe_path));
  p.thread_init = InitTeb;
  p.thread_exit = FinishThread;
  p.guest_fault = [](Ctx& c, bool write, gaddr addr) {
    return hle::RaiseAccessViolation(c, write, uint32_t(addr));
  };
  p.pump_events = PumpHostEvents;
  w.game_args = game_args;
  std::string err;
  for (const auto& d : game.lifted_dlls) {
    Module& m = w.modules.emplace_back();
    m.name = hle::NormaliseDll(d.name);
    m.host_path = HostPath(GuestGameDir() + "\\" + d.name);
    m.base = d.base;
  }
  // Twice: the first pass maps every DLL (so all export tables are known),
  // the second binds each one's imports from the others.
  for (int pass = 0; pass < 2; pass++)
    for (Module& m : w.modules) MapModule(m);
  // The EXE's imports from a lifted DLL bind to its exports; the rest go
  // to the HLE.
  auto bind = [](const std::string& dll, const std::string& name) {
    return BindImport(nullptr, dll, name);
  };
  if (!w.img.Load(exe_path, p.mem, &err, 0, bind))
    FatalF("%s", err.c_str());

  if (p.x64) {
    // Everything above 4 GiB, below the TEBs.
    p.vm.Init(&p.mem, kUserLo64, kTebLo64);
  } else {
    p.vm.Init(&p.mem, 0x00010000, kThunkBase);
    p.vm.MarkReserved(hle::kModuleRegion, hle::kModuleRegionSize);
  }
  p.vm.MarkReserved(w.img.image_base, w.img.image_size);
  for (const Module& m : w.modules) p.vm.MarkReserved(m.img.image_base, m.img.image_size);

  // PEB: only the fields Win32 code commonly peeks at. x64 guests see
  // Windows 10 (22H2), 32-bit ones Windows XP. ProcessParameters points
  // at an RTL_USER_PROCESS_PARAMETERS in the same page (its sizes, and
  // Flags = RTL_USER_PROC_PARAMS_NORMALIZED as a running process has it):
  // the C runtime's exit path reads Flags to choose how to end the process.
  constexpr uint32_t kParamsOffset = 0x800;
  if (p.x64) {
    p.mem.Commit(kPebVa64, 0x1000, Prot::RW);
    Wr64(kPebVa64 + 0x10, w.img.image_base);
    const gaddr params = kPebVa64 + kParamsOffset;
    Wr32(params + 0, 0x440);  // MaximumLength
    Wr32(params + 4, 0x440);  // Length
    Wr32(params + 8, 1);      // Flags: NORMALIZED
    Wr64(kPebVa64 + 0x20, params);  // ProcessParameters
    Wr32(kPebVa64 + 0xB8, GuestProcessorCount());  // NumberOfProcessors
    Wr32(kPebVa64 + 0x118, 10);     // OSMajorVersion
    Wr32(kPebVa64 + 0x11C, 0);      // OSMinorVersion
    Wr16(kPebVa64 + 0x120, 19045);  // OSBuildNumber
    Wr32(kPebVa64 + 0x124, 2);      // OSPlatformId: VER_PLATFORM_WIN32_NT
  } else {
    p.mem.Commit(kPebVa, 0x1000, Prot::RW);
    Wr32(kPebVa + 0x08, uint32_t(w.img.image_base));
    const uint32_t params = kPebVa + kParamsOffset;
    Wr32(params + 0, 0x290);  // MaximumLength
    Wr32(params + 4, 0x290);  // Length
    Wr32(params + 8, 1);      // Flags: NORMALIZED
    Wr32(kPebVa + 0x10, params);  // ProcessParameters
    Wr32(kPebVa + 0x64, GuestProcessorCount());  // NumberOfProcessors
    Wr32(kPebVa + 0xA4, 5);      // OSMajorVersion (Windows XP)
    Wr32(kPebVa + 0xA8, 1);      // OSMinorVersion
    Wr16(kPebVa + 0xAC, 2600);   // OSBuildNumber
    Wr32(kPebVa + 0xB0, 2);      // OSPlatformId: VER_PLATFORM_WIN32_NT
  }

  // The image's TLS index: slot 0 (lifted DLLs have theirs, MapModule).
  if (w.img.has_tls) {
    CheckNoTlsCallbacks(w.img, game.exe_name);
    Wr32(w.img.tls.index_va, 0);
  }

  // Dispatch looks lifted code up in each image's .text: the EXE's first.
  p.code.push_back(TextOf(w.img));
  for (const Module& m : w.modules) p.code.push_back(TextOf(m.img));
  InitDispatch();
  for (const Module& m : w.modules) {
    // A DLL lifted at another base, or not at all, has no functions here.
    if (m.img.entry && !IsLifted(m.img.entry))
      FatalF("%s at %08llX: its entry point %08llX is not lifted (lift it with "
             "recomp_lift --module, at the same --base)",
             m.name.c_str(), (unsigned long long)m.img.image_base,
             (unsigned long long)m.img.entry);
  }
  InitKernel32State();

  Thread* main = CreateGuestThread(w.img.stack_reserve);
  RegisterMainThread(main);
  return int(RunMainThread(main, [&](Ctx& c) {
    // DLLs the EXE imports from are loaded (one reference) and initialised
    // first, on the main thread, as Windows' loader does before the EXE's
    // entry point.
    // The DLLs they import from first (AttachModule).
    for (Module& m : w.modules) {
      if (!m.static_import) continue;
      if (!AttachModule(c, m)) FatalF("%s: DllMain(PROCESS_ATTACH) failed", m.name.c_str());
    }
    return CallGuest(c, w.img.entry, {});
  }));
}

}  // namespace recomp
