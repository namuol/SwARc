// advapi32 (registry, user name) and the small ole32/oleaut32 surface the
// game uses.
//
// The registry is an in-memory tree persisted to
// $XDG_DATA_HOME/<id>/registry.txt. It is seeded with what the retail
// installer would have written (GameConfig::registry).

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <vector>

#include "runtime/log.h"
#include "win32/com.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "win32/win32.h"

namespace recomp::hle {
namespace {

using namespace win32;

constexpr uint32_t kHkeyClassesRoot = 0x80000000, kHkeyCurrentUser = 0x80000001,
                   kHkeyLocalMachine = 0x80000002, kHkeyUsers = 0x80000003;
constexpr uint32_t kRegSz = 1;

struct RegValue {
  uint32_t type;
  std::vector<uint8_t> data;
};

std::mutex g_reg_mu;
// Full key path (lower case, e.g. "hklm\software\...") -> values (lower
// case name -> value). Key existence = presence in the map.
std::map<std::string, std::map<std::string, RegValue>> g_reg;
std::map<uint32_t, std::string> g_open;  // HKEY -> key path
uint32_t g_next_hkey = 0x1000;
bool g_reg_loaded = false;

std::string RegFile() { return DataDir() + "/registry.txt"; }

RegValue Str(const std::string& s) {
  RegValue v{kRegSz, {}};
  v.data.assign(s.begin(), s.end());
  v.data.push_back(0);
  return v;
}

void Seed() {
  for (const auto& r : Game().registry) {
    std::string k = Lower(r.key);
    // Parent keys below the root exist too.
    for (size_t i = k.find('\\', k.find('\\') + 1); i != std::string::npos;
         i = k.find('\\', i + 1))
      g_reg[k.substr(0, i)];
    g_reg[k][Lower(r.name)] = Str(r.value);
  }
}

void Save() {
  MakeDirs(DataDir());
  std::ofstream f(RegFile());
  for (const auto& [key, vals] : g_reg) {
    f << "[" << key << "]\n";
    for (const auto& [name, v] : vals) {
      f << name << "\t" << v.type << "\t";
      for (uint8_t b : v.data) {
        char buf[3];
        std::snprintf(buf, sizeof buf, "%02x", b);
        f << buf;
      }
      f << "\n";
    }
  }
}

void LoadOnce() {
  if (g_reg_loaded) return;
  g_reg_loaded = true;
  std::ifstream f(RegFile());
  if (!f) {
    Seed();
    return;
  }
  std::string line, key;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    if (line[0] == '[') {
      key = line.substr(1, line.size() - 2);
      g_reg[key];
      continue;
    }
    std::istringstream in(line);
    std::string name, type, hex;
    std::getline(in, name, '\t');
    std::getline(in, type, '\t');
    std::getline(in, hex);
    RegValue v{uint32_t(std::stoul(type)), {}};
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
      v.data.push_back(uint8_t(std::stoul(hex.substr(i, 2), nullptr, 16)));
    g_reg[key][name] = v;
  }
}

std::string RootName(uint32_t h) {
  switch (h) {
    case kHkeyClassesRoot: return "hkcr";
    case kHkeyCurrentUser: return "hkcu";
    case kHkeyLocalMachine: return "hklm";
    case kHkeyUsers: return "hku";
    default: {
      auto it = g_open.find(h);
      return it == g_open.end() ? std::string() : it->second;
    }
  }
}

std::string Join(const std::string& base, const std::string& sub) {
  std::string s = Lower(sub);
  while (!s.empty() && s.back() == '\\') s.pop_back();
  while (!s.empty() && s.front() == '\\') s.erase(0, 1);
  return s.empty() ? base : base + "\\" + s;
}

HLE("advapi32.dll", RegOpenKeyExA, 5) {
  std::lock_guard<std::mutex> l(g_reg_mu);
  LoadOnce();
  std::string base = RootName(Arg(c, 0));
  if (base.empty()) return kErrorInvalidHandle;
  std::string path = Join(base, GStr(Arg(c, 1)));
  if (!g_reg.count(path)) return kErrorFileNotFound;
  uint32_t h = g_next_hkey += 4;
  g_open[h] = path;
  Wr32(Arg(c, 4), h);
  return kErrorSuccess;
}

// RegOpenKeyA(hkey, subkey, HKEY* out): RegOpenKeyExA with full access; a
// NULL or empty subkey opens `hkey` itself again.
HLE("advapi32.dll", RegOpenKeyA, 3) {
  std::lock_guard<std::mutex> l(g_reg_mu);
  LoadOnce();
  std::string base = RootName(Arg(c, 0));
  if (base.empty()) return kErrorInvalidHandle;
  std::string path = Join(base, Arg(c, 1) ? GStr(Arg(c, 1)) : std::string());
  if (!g_reg.count(path)) return kErrorFileNotFound;
  uint32_t h = g_next_hkey += 4;
  g_open[h] = path;
  Wr32(Arg(c, 2), h);
  return kErrorSuccess;
}

HLE("advapi32.dll", RegCreateKeyExA, 9) {
  std::lock_guard<std::mutex> l(g_reg_mu);
  LoadOnce();
  std::string base = RootName(Arg(c, 0));
  if (base.empty()) return kErrorInvalidHandle;
  std::string path = Join(base, GStr(Arg(c, 1)));
  bool existed = g_reg.count(path);
  // Create the key and all its parents.
  for (size_t i = 0; i <= path.size(); i++)
    if (i == path.size() || path[i] == '\\') g_reg[path.substr(0, i)];
  uint32_t h = g_next_hkey += 4;
  g_open[h] = path;
  Wr32(Arg(c, 7), h);
  if (Arg(c, 8)) Wr32(Arg(c, 8), existed ? 2 : 1);  // REG_OPENED / CREATED
  if (!existed) Save();
  return kErrorSuccess;
}

HLE("advapi32.dll", RegCloseKey, 1) {
  std::lock_guard<std::mutex> l(g_reg_mu);
  g_open.erase(Arg(c, 0));
  return kErrorSuccess;
}

HLE("advapi32.dll", RegQueryValueExA, 6) {
  std::lock_guard<std::mutex> l(g_reg_mu);
  LoadOnce();
  std::string key = RootName(Arg(c, 0));
  uint32_t type_out = Arg(c, 3), data = Arg(c, 4), size_ptr = Arg(c, 5);
  auto k = g_reg.find(key);
  if (k == g_reg.end()) return kErrorInvalidHandle;
  auto v = k->second.find(Lower(GStr(Arg(c, 1))));
  if (v == k->second.end()) {
    RECOMP_TRACE("  RegQueryValueExA(%s, %s): not found\n", key.c_str(),
              GStr(Arg(c, 1)).c_str());
    return kErrorFileNotFound;
  }
  if (type_out) Wr32(type_out, v->second.type);
  uint32_t n = uint32_t(v->second.data.size());
  if (!size_ptr) return data ? kErrorInvalidHandle : kErrorSuccess;
  uint32_t cap = Rd32(size_ptr);
  Wr32(size_ptr, n);
  if (!data) return kErrorSuccess;
  if (cap < n) return kErrorMoreData;
  std::memcpy(GPtr(data), v->second.data.data(), n);
  return kErrorSuccess;
}

HLE("advapi32.dll", RegSetValueExA, 6) {
  std::lock_guard<std::mutex> l(g_reg_mu);
  LoadOnce();
  std::string key = RootName(Arg(c, 0));
  if (!g_reg.count(key)) return kErrorInvalidHandle;
  RegValue v{uint32_t(Arg(c, 3)), {}};
  uint8_t* p = GPtr(Arg(c, 4));
  v.data.assign(p, p + Arg(c, 5));
  g_reg[key][Lower(GStr(Arg(c, 1)))] = v;
  Save();
  return kErrorSuccess;
}

// Direct subkeys of `key`, in order (key names as stored: lower case).
std::vector<std::string> Subkeys(const std::string& key) {
  std::vector<std::string> out;
  std::string pre = key + "\\";
  for (auto it = g_reg.lower_bound(pre); it != g_reg.end() && it->first.compare(0, pre.size(), pre) == 0; ++it)
    if (it->first.find('\\', pre.size()) == std::string::npos) out.push_back(it->first.substr(pre.size()));
  return out;
}

// RegDeleteKeyA(hkey, subkey): only keys without subkeys, as on NT.
HLE("advapi32.dll", RegDeleteKeyA, 2) {
  std::lock_guard<std::mutex> l(g_reg_mu);
  LoadOnce();
  std::string base = RootName(Arg(c, 0));
  if (base.empty()) return kErrorInvalidHandle;
  std::string path = Join(base, GStr(Arg(c, 1)));
  if (!g_reg.count(path)) return kErrorFileNotFound;
  if (!Subkeys(path).empty()) return 5;  // ERROR_ACCESS_DENIED
  g_reg.erase(path);
  Save();
  return kErrorSuccess;
}

// RegEnumKeyExA(hkey, index, name, &name_len, reserved, class, &class_len, &time)
HLE("advapi32.dll", RegEnumKeyExA, 8) {
  std::lock_guard<std::mutex> l(g_reg_mu);
  LoadOnce();
  std::string key = RootName(Arg(c, 0));
  if (!g_reg.count(key)) return kErrorInvalidHandle;
  std::vector<std::string> subs = Subkeys(key);
  uint32_t i = Arg(c, 1), name = Arg(c, 2), len_p = Arg(c, 3);
  if (i >= subs.size()) return 259;  // ERROR_NO_MORE_ITEMS
  const std::string& n = subs[i];
  if (Rd32(len_p) < n.size() + 1) return kErrorMoreData;
  PutStr(name, n);
  Wr32(len_p, uint32_t(n.size()));
  if (Arg(c, 5) && Arg(c, 6) && Rd32(Arg(c, 6))) Wr8(Arg(c, 5), 0);
  if (Arg(c, 6)) Wr32(Arg(c, 6), 0);
  if (Arg(c, 7)) { Wr32(Arg(c, 7), 0); Wr32(Arg(c, 7) + 4, 0); }
  return kErrorSuccess;
}

// RegQueryInfoKeyA(hkey, class, &class_len, reserved, &subkeys,
// &max_subkey_len, &max_class_len, &values, &max_value_name_len,
// &max_value_len, &security_descriptor_len, &last_write_time)
HLE("advapi32.dll", RegQueryInfoKeyA, 12) {
  std::lock_guard<std::mutex> l(g_reg_mu);
  LoadOnce();
  std::string key = RootName(Arg(c, 0));
  auto k = g_reg.find(key);
  if (k == g_reg.end()) return kErrorInvalidHandle;
  std::vector<std::string> subs = Subkeys(key);
  uint32_t max_sub = 0, max_name = 0, max_val = 0;
  for (const auto& s : subs) max_sub = std::max(max_sub, uint32_t(s.size()));
  for (const auto& [n, v] : k->second) {
    max_name = std::max(max_name, uint32_t(n.size()));
    max_val = std::max(max_val, uint32_t(v.data.size()));
  }
  auto put = [&](int i, uint32_t v) { if (Arg(c, i)) Wr32(Arg(c, i), v); };
  if (Arg(c, 1) && Arg(c, 2) && Rd32(Arg(c, 2))) Wr8(Arg(c, 1), 0);
  put(2, 0);
  put(4, uint32_t(subs.size()));
  put(5, max_sub);
  put(6, 0);
  put(7, uint32_t(k->second.size()));
  put(8, max_name);
  put(9, max_val);
  put(10, 0);
  if (Arg(c, 11)) { Wr32(Arg(c, 11), 0); Wr32(Arg(c, 11) + 4, 0); }
  return kErrorSuccess;
}

HLE("advapi32.dll", RegDeleteValueA, 2) {
  std::lock_guard<std::mutex> l(g_reg_mu);
  LoadOnce();
  auto k = g_reg.find(RootName(Arg(c, 0)));
  if (k == g_reg.end()) return kErrorInvalidHandle;
  if (!k->second.erase(Lower(Arg(c, 1) ? GStr(Arg(c, 1)) : std::string()))) return kErrorFileNotFound;
  Save();
  return kErrorSuccess;
}

// GetUserNameA/W(buf, size*): the size is in characters, with the null.
uint32_t UserName(Ctx& c, bool wide) {
  std::string n = "Player";
  uint32_t buf = Arg(c, 0), psize = Arg(c, 1);
  if (Rd32(psize) < n.size() + 1) {
    Wr32(psize, uint32_t(n.size() + 1));
    SetLastError(kErrorInsufficientBuffer);
    return 0;
  }
  if (wide) PutWStr(buf, n);
  else PutStr(buf, n);
  Wr32(psize, uint32_t(n.size() + 1));
  return 1;
}
HLE("advapi32.dll", GetUserNameA, 2) { return UserName(c, false); }
HLE("advapi32.dll", GetUserNameW, 2) { return UserName(c, true); }

// ---- ole32 ------------------------------------------------------------------

// SystemFunction036 is RtlGenRandom(buf, len): BOOLEAN, len random bytes
// (the CRT's rand_s). From the host's random device.
HLE("advapi32.dll", SystemFunction036, 2) {
  static std::mutex mu;
  static std::random_device rd;
  std::lock_guard<std::mutex> l(mu);
  gaddr buf = Arg(c, 0);
  for (uint32_t i = 0, n = Arg(c, 1); i < n; i += 4) {
    uint32_t r = rd();
    for (uint32_t k = 0; k < 4 && i + k < n; k++) Wr8(buf + i + k, uint8_t(r >> (8 * k)));
  }
  return 1;
}

HLE("ole32.dll", CoInitialize, 1) { return 0; }  // S_OK
HLE("ole32.dll", CoInitializeEx, 2) { return 0; }
// CoInitializeSecurity(desc, cAuth, auth, reserved, authn level, imp level,
// auth list, caps, reserved): process-wide COM security for remote calls,
// which never happen here.
HLE("ole32.dll", CoInitializeSecurity, 9) { return 0; }  // S_OK
HLE("ole32.dll", CoUninitialize, 0) { return 0; }

// The COM task allocator: the process heap (a NULL free is a no-op).
HLE64("ole32.dll", CoTaskMemAlloc, 1) { return HeapAllocProcess(Arg(c, 0), false); }
HLE64("ole32.dll", CoTaskMemFree, 1) {
  if (Arg(c, 0)) HeapFreeProcess(Arg(c, 0));
  return 0;
}

// (rclsid, pUnkOuter, dwClsContext, riid, ppv): the classes HLE modules
// register (com.h). The others games create are optional and
// REGDB_E_CLASSNOTREG reads as "not installed", as on a system without
// them: DirectPlay8 (multiplayer), the Zone update object and the Game
// Explorer (VerifyAccess: access is then not denied).
// Class objects the process registered itself (CoRegisterClassObject):
// CoCreateInstance asks their IClassFactory first. Games implement COM
// classes in their own EXE this way (Tiberian Sun's unit locomotors).
struct ClassObject {
  uint8_t clsid[16];
  uint32_t factory;  // IClassFactory*, AddRef'd
  uint32_t cookie;
};
std::vector<ClassObject> g_class_objects;
uint32_t g_next_cookie = 0x100;

// (rclsid, pUnk, dwClsContext, flags, lpdwRegister)
HLE("ole32.dll", CoRegisterClassObject, 5) {
  uint32_t clsid = Arg(c, 0), unk = Arg(c, 1);
  ClassObject o;
  std::memcpy(o.clsid, GPtr(clsid), 16);
  o.factory = unk;
  o.cookie = g_next_cookie++;
  CallGuest(c, Rd32(Rd32(unk) + 4), {unk});  // AddRef
  g_class_objects.push_back(o);
  if (Arg(c, 4)) Wr32(Arg(c, 4), o.cookie);
  return 0;
}

HLE("ole32.dll", CoRevokeClassObject, 1) {
  for (auto it = g_class_objects.begin(); it != g_class_objects.end(); ++it)
    if (it->cookie == Arg(c, 0)) {
      uint32_t f = it->factory;
      g_class_objects.erase(it);
      CallGuest(c, Rd32(Rd32(f) + 8), {f});  // Release
      return 0;
    }
  return 0x80040016;  // CO_E_OBJNOTREG
}

// CoCreateInstance through a registered class object; false if none.
bool RegisteredClass(Ctx& c, uint32_t clsid, uint32_t iid, uint32_t out, uint32_t* hr) {
  for (const ClassObject& o : g_class_objects) {
    if (std::memcmp(o.clsid, GPtr(clsid), 16)) continue;
    uint32_t f = o.factory;
    *hr = uint32_t(CallGuest(c, Rd32(Rd32(f) + 12), {f, 0, iid, out}));  // CreateInstance
    return true;
  }
  return false;
}

// An in-process server the game's installer registered: the DLL named by
// HKCR\CLSID\{clsid}\InprocServer32 (a lifted DLL), loaded with
// LoadLibrary, asked for its class factory with DllGetClassObject, whose
// CreateInstance makes the object, as ole32 does.
bool InprocServer(Ctx& c, uint32_t clsid, uint32_t iid, uint32_t out, uint32_t* hr) {
  char k[96];
  std::snprintf(k, sizeof k,
                "hkcr\\clsid\\{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}\\inprocserver32",
                Rd32(clsid), Rd16(clsid + 4), Rd16(clsid + 6), Rd8(clsid + 8), Rd8(clsid + 9),
                Rd8(clsid + 10), Rd8(clsid + 11), Rd8(clsid + 12), Rd8(clsid + 13),
                Rd8(clsid + 14), Rd8(clsid + 15));
  std::string dll;
  {
    std::lock_guard<std::mutex> l(g_reg_mu);
    LoadOnce();
    auto key = g_reg.find(k);
    if (key == g_reg.end()) return false;
    auto v = key->second.find("");
    if (v == key->second.end() || v->second.data.empty()) return false;
    dll.assign(v->second.data.begin(), v->second.data.end());
    while (!dll.empty() && !dll.back()) dll.pop_back();
  }
  uint32_t tmp = HeapAllocProcess(uint32_t(dll.size()) + 64, true);
  PutStr(tmp, dll);
  uint32_t mod = uint32_t(CallGuest(c, ThunkFor("kernel32.dll", "LoadLibraryA"), {tmp}));
  uint32_t get = 0;
  if (mod) {
    PutStr(tmp, "DllGetClassObject");
    get = uint32_t(CallGuest(c, ThunkFor("kernel32.dll", "GetProcAddress"), {mod, tmp}));
  }
  if (!get) {
    WarnF("CoCreateInstance: in-process server %s not available\n", dll.c_str());
    HeapFreeProcess(tmp);
    return false;
  }
  // IID_IClassFactory {00000001-0000-0000-C000-000000000046}, then the factory.
  const uint8_t cf_iid[16] = {1, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46};
  std::memcpy(GPtr(tmp), cf_iid, 16);
  Wr32(tmp + 16, 0);
  *hr = uint32_t(CallGuest(c, get, {clsid, tmp, tmp + 16}));
  uint32_t cf = Rd32(tmp + 16);
  HeapFreeProcess(tmp);
  if (*hr || !cf) {
    if (out) Wr32(out, 0);
    return true;
  }
  uint32_t vt = Rd32(cf);
  *hr = uint32_t(CallGuest(c, Rd32(vt + 12), {cf, 0, iid, out}));  // CreateInstance
  CallGuest(c, Rd32(vt + 8), {cf});                                // Release
  return true;
}

HLE("ole32.dll", CoCreateInstance, 5) {
  uint32_t clsid = Arg(c, 0), out = Arg(c, 4), hr_inproc = 0;
  if (com::CreateFn create = com::FindCoClass(clsid)) {
    uint32_t obj = 0;
    uint32_t hr = create(Arg(c, 3), &obj);
    if (out) Wr32(out, obj);
    return hr;
  }
  if (RegisteredClass(c, clsid, Arg(c, 3), out, &hr_inproc)) return hr_inproc;
  if (InprocServer(c, clsid, Arg(c, 3), out, &hr_inproc)) return hr_inproc;
  WarnF("CoCreateInstance({%08X-...}): class not available\n", Rd32(clsid));
  if (out) Wr32(out, 0);
  return 0x80040154;
}

// (pProxy, dwAuthnSvc, dwAuthzSvc, pServerPrincName, dwAuthnLevel,
// dwImpLevel, pAuthInfo, dwCapabilities): the objects are in-process, so
// there is no proxy security to set.
HLE("ole32.dll", CoSetProxyBlanket, 8) { return 0; }

HLE("ole32.dll", CoCreateGuid, 1) {
  static std::mt19937_64 rng{std::random_device{}()};
  uint32_t g = Arg(c, 0);
  for (int i = 0; i < 4; i++) Wr32(g + 4 * i, uint32_t(rng()));
  Wr8(g + 7, uint8_t((g_proc->mem.read<uint8_t>(g + 7) & 0x0F) | 0x40));  // v4
  return 0;
}

HLE("ole32.dll", StringFromGUID2, 3) {
  uint32_t g = Arg(c, 0), out = Arg(c, 1), cap = Arg(c, 2);
  char buf[64];
  const uint8_t* b = GPtr(g + 8);
  std::snprintf(buf, sizeof buf,
                "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", Rd32(g),
                Rd16(g + 4), Rd16(g + 6), b[0], b[1], b[2], b[3], b[4], b[5],
                b[6], b[7]);
  size_t n = std::strlen(buf) + 1;
  if (cap < n) return 0;
  PutWStr(out, buf);
  return uint32_t(n);
}

// Parses "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" into the GUID at `g`.
bool ParseGuid(gaddr str, gaddr g) {
  std::string s = Narrow(GWStr(str));
  unsigned int d1, d2, d3, b[8];
  if (std::sscanf(s.c_str(), "{%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}", &d1,
                  &d2, &d3, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6],
                  &b[7]) != 11)
    return false;
  Wr32(g, d1);
  Wr16(g + 4, uint16_t(d2));
  Wr16(g + 6, uint16_t(d3));
  for (int i = 0; i < 8; i++) Wr8(g + 8 + i, uint8_t(b[i]));
  return true;
}

HLE("ole32.dll", IIDFromString, 2) {
  return ParseGuid(Arg(c, 0), Arg(c, 1)) ? 0 : kEInvalidArg;
}

// CLSIDFromString: the braced form only. A ProgID ("Lib.Class") would be
// looked up in HKCR, where no COM class is registered here, so it fails
// as an unknown one does: CO_E_CLASSSTRING.
HLE("ole32.dll", CLSIDFromString, 2) {
  if (ParseGuid(Arg(c, 0), Arg(c, 1))) return 0;
  std::memset(GPtr(Arg(c, 1)), 0, 16);
  return 0x800401F3u;  // CO_E_CLASSSTRING
}

// ---- oleaut32 (imported by ordinal) -----------------------------------------

HLE_NAMED("oleaut32.dll", "#2", SysAllocString, 1) {
  return Arg(c, 0) ? com::AllocBstr(GWStr(Arg(c, 0))) : 0;
}

HLE_NAMED("oleaut32.dll", "#6", SysFreeString, 1) {
  com::FreeBstr(Arg(c, 0));
  return 0;
}

// VARIANT (16 bytes): vt (u16) at 0, the value at 8.
HLE_NAMED("oleaut32.dll", "#8", VariantInit, 1) {  // void
  std::memset(GPtr(Arg(c, 0)), 0, 8);  // vt = VT_EMPTY, reserved words
  return 0;
}

// VariantClear(v): frees what the variant owns (a BSTR, a reference on an
// interface) and leaves it VT_EMPTY. By-reference values own nothing.
HLE_NAMED("oleaut32.dll", "#9", VariantClear, 1) {
  gaddr v = Arg(c, 0);
  const uint16_t vt = Rd16(v);
  if (!(vt & 0x4000)) {  // not VT_BYREF
    switch (vt) {
      case 8:  // VT_BSTR
        com::FreeBstr(Rd32(v + 8));
        break;
      case 9:    // VT_DISPATCH
      case 13: {  // VT_UNKNOWN: IUnknown::Release
        if (gaddr p = Rd32(v + 8)) CallGuest(c, Rd32(Rd32(p) + 8), {p});
        break;
      }
      default:
        if (vt & 0x2000) {  // VT_ARRAY
          WarnF("VariantClear: SAFEARRAY variant (vt %X) not freed\n", vt);
          return 0x80020008u;  // DISP_E_BADVARTYPE
        }
        break;  // scalars own nothing
    }
  }
  Wr16(v, 0);
  return 0;  // S_OK
}

// Error objects (IErrorInfo) are never set here: GetErrorInfo has none to
// return (S_FALSE), SetErrorInfo keeps nothing.
HLE_NAMED("oleaut32.dll", "#200", GetErrorInfo, 2) {
  if (Arg(c, 1)) Wr32(Arg(c, 1), 0);
  return 1;  // S_FALSE
}
HLE_NAMED("oleaut32.dll", "#201", SetErrorInfo, 2) { return 0; }

}  // namespace
}  // namespace recomp::hle
