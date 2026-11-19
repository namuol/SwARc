// kernel32: resources, time conversion/formatting, strings, and the odds
// and ends (console title, FormatMessage, CreateProcess); version.dll
// (version resources).

#include <algorithm>
#include <cstring>
#include <mutex>
#include <ctime>
#include <string>

#include "runtime/log.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "win32/win32.h"

namespace recomp::hle {
namespace {

using namespace win32;

// ---- resources ------------------------------------------------------------------
// The EXE's .rsrc and those of loaded lifted DLLs (which include
// resource-only DLLs such as a game's language DLL: they are mapped, have
// no code, and LoadLibrary returns their base). Resource "handles" (HRSRC) are guest
// addresses of IMAGE_RESOURCE_DATA_ENTRY records; LoadResource returns the
// data pointer and LockResource is the identity, as on Win32.

uint32_t RsrcBase() {
  for (const auto& s : g_win32->img.sections)
    if (s.name == ".rsrc") return s.va;
  return 0;
}

bool IsOwnModule(uint32_t h) { return h == 0 || h == g_win32->img.image_base; }

// The image of module handle `h` (0: the EXE), or null.
const PeImage* ResImage(uint32_t h) {
  if (IsOwnModule(h)) return &g_win32->img;
  // The module list is fixed after startup; a handle names a loaded one.
  for (const Module& m : g_win32->modules)
    if (m.refs && m.img.image_base == h) return &m.img;
  return nullptr;
}

uint32_t RsrcBaseOf(const PeImage& img) {
  for (const auto& s : img.sections)
    if (s.name == ".rsrc") return uint32_t(s.va);
  return 0;
}

// Name-or-id argument as a comparable key: "#123" for integer ids. A
// string "#..." is an id too, read as RtlCharToInteger reads it in base 10
// (blanks, a sign, then digits up to the first other character, wrapping
// at 32 bits); false (ERROR_INVALID_PARAMETER) if it is not a 16-bit one.
bool ResKey(uint32_t v, std::string* key) {
  if (v < 0x10000) {
    *key = "#" + std::to_string(v);
    return true;
  }
  std::string s = GStr(v);
  if (!s.empty() && s[0] == '#') {
    size_t i = 1;
    while (i < s.size() && uint8_t(s[i]) <= ' ') i++;
    const bool minus = i < s.size() && s[i] == '-';
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) i++;
    uint32_t id = 0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; i++) id = id * 10 + uint32_t(s[i] - '0');
    if (minus) id = 0 - id;
    if (id > 0xFFFF) return false;
    *key = "#" + std::to_string(id);
    return true;
  }
  for (auto& ch : s) ch = char(std::toupper(uint8_t(ch)));
  *key = s;
  return true;
}

// Key of a directory entry (names are UTF-16, upper-cased like Windows).
std::string EntryKey(uint32_t rsrc, uint32_t entry) {
  uint32_t id = Rd32(entry);
  if (!(id & 0x80000000)) return "#" + std::to_string(id & 0xFFFF);
  uint32_t str = rsrc + (id & 0x7FFFFFFF);
  uint16_t len = Rd16(str);
  std::string s;
  for (uint32_t i = 0; i < len; i++)
    s.push_back(char(std::toupper(uint8_t(Rd16(str + 2 + 2 * i)))));
  return s;
}

// Finds the subdirectory/data offset for `key` in directory `dir`; with an
// empty key, the first entry. Returns 0 if absent.
uint32_t FindEntry(uint32_t rsrc, uint32_t dir, const std::string& key) {
  uint32_t n = uint32_t(Rd16(dir + 12)) + Rd16(dir + 14);
  for (uint32_t i = 0; i < n; i++) {
    uint32_t e = dir + 16 + 8 * i;
    if (key.empty() || EntryKey(rsrc, e) == key) return Rd32(e + 4);
  }
  return 0;
}

// The IMAGE_RESOURCE_DATA_ENTRY for type/name keys (an empty name: the
// first one) in language `lang` (0: the first one), or 0.
uint32_t FindResourceIn(const PeImage& img, const std::string& type, const std::string& name,
                        uint32_t lang) {
  uint32_t rsrc = RsrcBaseOf(img);
  if (!rsrc) return 0;
  uint32_t t = FindEntry(rsrc, rsrc, type);
  if (!(t & 0x80000000)) return 0;
  uint32_t n = FindEntry(rsrc, rsrc + (t & 0x7FFFFFFF), name);
  if (!(n & 0x80000000)) return 0;
  uint32_t ldir = rsrc + (n & 0x7FFFFFFF);
  uint32_t l = lang ? FindEntry(rsrc, ldir, "#" + std::to_string(lang)) : 0;
  if (!l) l = FindEntry(rsrc, ldir, "");
  if (!l || (l & 0x80000000)) return 0;
  return rsrc + l;
}
uint32_t FindResourceImpl(const std::string& type, const std::string& name, uint32_t lang) {
  return FindResourceIn(g_win32->img, type, name, lang);
}

HLE("kernel32.dll", FindResourceA, 3) {
  const PeImage* img = ResImage(Arg(c, 0));
  if (!img) return 0;
  std::string type, name;
  if (!ResKey(Arg(c, 2), &type) || !ResKey(Arg(c, 1), &name)) {
    SetLastError(kErrorInvalidParameter);
    return 0;
  }
  uint32_t r = FindResourceIn(*img, type, name, 0);
  if (!r) SetLastError(kErrorResourceNameNotFound);
  return r;
}

HLE("kernel32.dll", FindResourceExA, 4) {
  const PeImage* img = ResImage(Arg(c, 0));
  if (!img) return 0;
  std::string type, name;
  if (!ResKey(Arg(c, 1), &type) || !ResKey(Arg(c, 2), &name)) {
    SetLastError(kErrorInvalidParameter);
    return 0;
  }
  uint32_t r = FindResourceIn(*img, type, name, Arg(c, 3) & 0xFFFF);
  if (!r) SetLastError(kErrorResourceNameNotFound);
  return r;
}

HLE("kernel32.dll", LoadResource, 2) {
  uint32_t hrsrc = Arg(c, 1);
  const PeImage* img = ResImage(Arg(c, 0));
  return hrsrc && img ? uint32_t(img->image_base) + Rd32(hrsrc) : 0;
}

// LoadAcceleratorsA(module, name): the RT_ACCELERATOR (9) table's data is
// the handle (ACCEL entries, as LoadResource gives them); TranslateAccelerator
// here does not translate, so nothing reads it yet. 0 if there is none.
HLE("user32.dll", LoadAcceleratorsA, 2) {
  const PeImage* img = ResImage(Arg(c, 0));
  std::string name;
  if (!img || !ResKey(Arg(c, 1), &name)) return 0;
  uint32_t r = FindResourceIn(*img, "#9", name, 0);
  if (!r) {
    SetLastError(kErrorResourceNameNotFound);
    return 0;
  }
  return uint32_t(img->image_base) + Rd32(r);
}
// DestroyAcceleratorTable(haccel): a loaded table is the module's data.
HLE("user32.dll", DestroyAcceleratorTable, 1) { return 1; }

HLE("kernel32.dll", SizeofResource, 2) {
  uint32_t hrsrc = Arg(c, 1);
  return hrsrc ? Rd32(hrsrc + 4) : 0;
}

// String `id` of module `h`: RT_STRING bundle (id / 16) + 1 holds 16
// counted UTF-16 strings. Returns its address and length (0 if absent).
uint32_t StringResource(uint32_t h, uint32_t id, uint32_t* len) {
  *len = 0;
  const PeImage* img = ResImage(h);
  if (!img) return 0;
  uint32_t e = FindResourceIn(*img, "#6", "#" + std::to_string((id >> 4) + 1), 0);
  if (!e) return 0;
  uint32_t p = uint32_t(img->image_base) + Rd32(e);
  for (uint32_t i = 0; i < (id & 15); i++) p += 2 + 2 * uint32_t(Rd16(p));
  *len = Rd16(p);
  return p + 2;
}

// LoadStringA(hInstance, id, buffer, max): the string as ANSI (Latin-1
// for the code points below 256), cut to max - 1 characters.
HLE("user32.dll", LoadStringA, 4) {
  uint32_t len = 0, s = StringResource(Arg(c, 0), Arg(c, 1), &len);
  uint32_t buf = Arg(c, 2), max = Arg(c, 3);
  if (!buf || !max) return 0;
  uint32_t n = std::min(len, max - 1);
  for (uint32_t i = 0; i < n; i++) {
    uint16_t ch = Rd16(s + 2 * i);
    Wr8(buf + i, uint8_t(ch < 256 ? ch : '?'));
  }
  Wr8(buf + n, 0);
  if (!s) SetLastError(kErrorResourceNameNotFound);
  return n;
}

// LoadStringW: max 0 returns a read-only pointer to the string itself.
HLE("user32.dll", LoadStringW, 4) {
  uint32_t len = 0, s = StringResource(Arg(c, 0), Arg(c, 1), &len);
  uint32_t buf = Arg(c, 2), max = Arg(c, 3);
  if (!buf) return 0;
  if (!max) {
    Wr32(buf, s);
    return len;
  }
  uint32_t n = std::min(len, max - 1);
  for (uint32_t i = 0; i < n; i++) Wr16(buf + 2 * i, Rd16(s + 2 * i));
  Wr16(buf + 2 * n, 0);
  return n;
}

HLE("kernel32.dll", LockResource, 1) { return Arg(c, 0); }
// FreeResource: obsolete on Win32, a resource lives as long as its module;
// FALSE, as Windows returns.
HLE("kernel32.dll", FreeResource, 1) { return 0; }

// Enumeration callbacks get either an integer id or a guest string.
uint32_t KeyArg(const std::string& key, uint32_t* tmp) {
  if (key.size() > 1 && key[0] == '#' &&
      key.find_first_not_of("0123456789", 1) == std::string::npos)
    return uint32_t(std::stoul(key.substr(1)));
  *tmp = HeapAllocProcess(uint32_t(key.size() + 1), false);
  PutStr(*tmp, key);
  return *tmp;
}

HLE("kernel32.dll", EnumResourceTypesA, 3) {
  uint32_t rsrc = RsrcBase(), fn = Arg(c, 1), param = Arg(c, 2);
  if (!rsrc || !IsOwnModule(Arg(c, 0))) return 0;
  uint32_t n = uint32_t(Rd16(rsrc + 12)) + Rd16(rsrc + 14);
  for (uint32_t i = 0; i < n; i++) {
    uint32_t tmp = 0;
    uint32_t key = KeyArg(EntryKey(rsrc, rsrc + 16 + 8 * i), &tmp);
    bool go = CallGuest(c, fn, {Arg(c, 0), key, param});
    if (tmp) HeapFreeProcess(tmp);
    if (!go) break;
  }
  return 1;
}

HLE("kernel32.dll", EnumResourceNamesA, 4) {
  uint32_t rsrc = RsrcBase(), type = Arg(c, 1), fn = Arg(c, 2), param = Arg(c, 3);
  std::string tkey;
  if (!rsrc || !IsOwnModule(Arg(c, 0))) return 0;
  if (!ResKey(type, &tkey)) {
    SetLastError(kErrorInvalidParameter);
    return 0;
  }
  uint32_t t = FindEntry(rsrc, rsrc, tkey);
  if (!(t & 0x80000000)) return 0;
  uint32_t dir = rsrc + (t & 0x7FFFFFFF);
  uint32_t n = uint32_t(Rd16(dir + 12)) + Rd16(dir + 14);
  for (uint32_t i = 0; i < n; i++) {
    uint32_t tmp = 0;
    uint32_t key = KeyArg(EntryKey(rsrc, dir + 16 + 8 * i), &tmp);
    bool go = CallGuest(c, fn, {Arg(c, 0), type, key, param});
    if (tmp) HeapFreeProcess(tmp);
    if (!go) break;
  }
  return 1;
}

HLE("kernel32.dll", EnumResourceLanguagesA, 5) {
  uint32_t rsrc = RsrcBase(), type = Arg(c, 1), name = Arg(c, 2), fn = Arg(c, 3),
           param = Arg(c, 4);
  std::string tkey, nkey;
  if (!rsrc || !IsOwnModule(Arg(c, 0))) return 0;
  if (!ResKey(type, &tkey) || !ResKey(name, &nkey)) {
    SetLastError(kErrorInvalidParameter);
    return 0;
  }
  uint32_t t = FindEntry(rsrc, rsrc, tkey);
  if (!(t & 0x80000000)) return 0;
  uint32_t nd = FindEntry(rsrc, rsrc + (t & 0x7FFFFFFF), nkey);
  if (!(nd & 0x80000000)) return 0;
  uint32_t dir = rsrc + (nd & 0x7FFFFFFF);
  uint32_t n = uint32_t(Rd16(dir + 12)) + Rd16(dir + 14);
  for (uint32_t i = 0; i < n; i++) {
    uint32_t lang = Rd32(dir + 16 + 8 * i) & 0xFFFF;
    if (!CallGuest(c, fn, {Arg(c, 0), type, name, lang, param})) break;
  }
  return 1;
}

// ---- time --------------------------------------------------------------------------

// SYSTEMTIME at st as a broken-down time (milliseconds dropped).
std::tm ReadSystemTime(uint32_t st) {
  std::tm t{};
  t.tm_year = Rd16(st) - 1900;
  t.tm_mon = Rd16(st + 2) - 1;
  t.tm_wday = Rd16(st + 4);
  t.tm_mday = Rd16(st + 6);
  t.tm_hour = Rd16(st + 8);
  t.tm_min = Rd16(st + 10);
  t.tm_sec = Rd16(st + 12);
  return t;
}

HLE("kernel32.dll", CompareFileTime, 2) {
  uint64_t a = Rd64(Arg(c, 0)), b = Rd64(Arg(c, 1));
  return a < b ? 0xFFFFFFFF : a > b ? 1 : 0;
}

HLE64("kernel32.dll", FileTimeToLocalFileTime, 2) {
  time_t now = time(nullptr);
  std::tm lt{};
  localtime_r(&now, &lt);
  int64_t off = int64_t(lt.tm_gmtoff) * 10000000;
  Wr64(Arg(c, 1), uint64_t(int64_t(Rd64(Arg(c, 0))) + off));
  return 1;
}

// FileTimeToDosDateTime(ft, &date, &time): the FILETIME's fields as
// they are (no time zone conversion), DOS-packed; years 1980-2107 only.
HLE64("kernel32.dll", FileTimeToDosDateTime, 3) {
  uint64_t ft = Rd64(Arg(c, 0));
  time_t secs = time_t(int64_t(ft - kFileTimeUnixEpoch) / 10000000);
  std::tm t{};
  if (ft < kFileTimeUnixEpoch || !gmtime_r(&secs, &t) || t.tm_year < 80 || t.tm_year > 207) {
    SetLastError(kErrorInvalidParameter);
    return 0;
  }
  Wr16(Arg(c, 1), uint16_t((t.tm_year - 80) << 9 | (t.tm_mon + 1) << 5 | t.tm_mday));
  Wr16(Arg(c, 2), uint16_t(t.tm_hour << 11 | t.tm_min << 5 | t.tm_sec / 2));
  return 1;
}

HLE("kernel32.dll", FileTimeToSystemTime, 2) {
  uint64_t ft = Rd64(Arg(c, 0));
  if (ft < kFileTimeUnixEpoch) return 0;
  time_t secs = time_t((ft - kFileTimeUnixEpoch) / 10000000);
  std::tm t{};
  gmtime_r(&secs, &t);
  WriteSystemTime(Arg(c, 1), t, int((ft / 10000) % 1000));
  return 1;
}

HLE("kernel32.dll", SystemTimeToFileTime, 2) {
  uint32_t st = Arg(c, 0);
  std::tm t = ReadSystemTime(st);
  time_t secs = timegm(&t);
  Wr64(Arg(c, 1),
       uint64_t(secs) * 10000000 + uint64_t(Rd16(st + 14)) * 10000 + kFileTimeUnixEpoch);
  return 1;
}

// SystemTimeToTzSpecificLocalTime(TIME_ZONE_INFORMATION* tz, utc, local).
// tz NULL: the host's zone, with the daylight saving in force at that
// moment (the host's rules for its date). A given tz: its Bias only; its
// StandardDate/DaylightDate rules are not applied.
HLE("kernel32.dll", SystemTimeToTzSpecificLocalTime, 3) {
  uint32_t tz = Arg(c, 0), utc = Arg(c, 1), out = Arg(c, 2);
  std::tm t = ReadSystemTime(utc);
  int ms = Rd16(utc + 14);
  time_t secs = timegm(&t);
  std::tm lt{};
  if (tz) {
    time_t local = secs - time_t(int32_t(Rd32(tz))) * 60;  // Bias: UTC = local + bias
    gmtime_r(&local, &lt);
  } else {
    localtime_r(&secs, &lt);
  }
  WriteSystemTime(out, lt, ms);
  return 1;
}

// The SYSTEMTIME a Get*Format call formats; null means now (local).
std::tm TmFromSystemTime(uint32_t st) {
  if (st) return ReadSystemTime(st);
  std::tm t{};
  time_t now = time(nullptr);
  localtime_r(&now, &t);
  return t;
}

// en-US defaults: M/d/yyyy and h:mm:ss tt. Explicit pictures are not
// translated (the game passes NULL for the format).
HLE("kernel32.dll", GetDateFormatA, 6) {
  std::tm t = TmFromSystemTime(Arg(c, 2));
  char out[64];
  if (Arg(c, 1) & 2)  // DATE_LONGDATE
    std::strftime(out, sizeof out, "%A, %B %d, %Y", &t);
  else
    std::snprintf(out, sizeof out, "%d/%d/%d", t.tm_mon + 1, t.tm_mday, t.tm_year + 1900);
  if (Arg(c, 3)) WarnF("GetDateFormatA: custom picture '%s' ignored\n", GStr(Arg(c, 3)).c_str());
  return PutCounted(Arg(c, 4), Arg(c, 5), out, false);
}

HLE("kernel32.dll", GetTimeFormatA, 6) {
  std::tm t = TmFromSystemTime(Arg(c, 2));
  char out[64];
  int h = t.tm_hour % 12;
  if (Arg(c, 1) & 2)  // TIME_NOSECONDS
    std::snprintf(out, sizeof out, "%d:%02d %s", h ? h : 12, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
  else
    std::snprintf(out, sizeof out, "%d:%02d:%02d %s", h ? h : 12, t.tm_min, t.tm_sec,
                  t.tm_hour < 12 ? "AM" : "PM");
  if (Arg(c, 3)) WarnF("GetTimeFormatA: custom picture '%s' ignored\n", GStr(Arg(c, 3)).c_str());
  return PutCounted(Arg(c, 4), Arg(c, 5), out, false);
}

// ---- strings -------------------------------------------------------------------------

HLE64("kernel32.dll", lstrlenA, 1) {
  return Arg(c, 0) ? std::strlen(g_proc->mem.ptr<const char>(Arg(c, 0))) : 0;
}

HLE64("kernel32.dll", lstrcpyA, 2) {
  PutStr(Arg(c, 0), GStr(Arg(c, 1)));
  return Arg(c, 0);
}

// lstrcpynA(dst, src, n): at most n-1 characters, always terminated
// (when n > 0).
HLE("kernel32.dll", lstrcpynA, 3) {
  gaddr d = Arg(c, 0), src = Arg(c, 1);
  uint32_t n = Arg(c, 2), i = 0;
  if (!d || !n) return d;
  for (; i + 1 < n && Rd8(src + i); i++) Wr8(d + i, Rd8(src + i));
  Wr8(d + i, 0);
  return d;
}

HLE("kernel32.dll", IsDBCSLeadByte, 1) { return 0; }    // CP1252: no DBCS
HLE("kernel32.dll", IsDBCSLeadByteEx, 2) { return 0; }

HLE("kernel32.dll", VerLanguageNameA, 3) {
  std::string n = (Arg(c, 0) & 0xFFFF) == 0x0409 ? "English (United States)"
                                                 : "Language Neutral";
  uint32_t buf = Arg(c, 1), cap = Arg(c, 2);
  if (!cap) return uint32_t(n.size());
  n = n.substr(0, cap - 1);
  PutStr(buf, n);
  return uint32_t(n.size());
}

HLE("kernel32.dll", FormatMessageA, 7) {
  uint32_t flags = Arg(c, 0), id = Arg(c, 2), buf = Arg(c, 4), cap = Arg(c, 5);
  if (!(flags & 0x1000)) {  // only FORMAT_MESSAGE_FROM_SYSTEM
    WarnF("FormatMessageA(flags %X): only FROM_SYSTEM is supported\n", flags);
    return 0;
  }
  std::string s = "Error " + std::to_string(id) + ".\r\n";
  if (flags & 0x100) {  // FORMAT_MESSAGE_ALLOCATE_BUFFER
    uint32_t p = HeapAllocProcess(uint32_t(s.size() + 1), false);
    PutStr(p, s);
    Wr32(buf, p);
    return uint32_t(s.size());
  }
  if (s.size() + 1 > cap) return 0;
  PutStr(buf, s);
  return uint32_t(s.size());
}

// The same in WCHARs; `cap` counts WCHARs.
HLE("kernel32.dll", FormatMessageW, 7) {
  uint32_t flags = Arg(c, 0), id = Arg(c, 2), buf = Arg(c, 4), cap = Arg(c, 5);
  if (!(flags & 0x1000)) {  // only FORMAT_MESSAGE_FROM_SYSTEM
    WarnF("FormatMessageW(flags %X): only FROM_SYSTEM is supported\n", flags);
    return 0;
  }
  std::string s = "Error " + std::to_string(id) + ".\r\n";
  if (flags & 0x100) {  // FORMAT_MESSAGE_ALLOCATE_BUFFER
    uint32_t p = HeapAllocProcess(uint32_t(2 * (s.size() + 1)), false);
    PutWStr(p, s);
    Wr32(buf, p);
    return uint32_t(s.size());
  }
  if (s.size() + 1 > cap) return 0;
  PutWStr(buf, s);
  return uint32_t(s.size());
}

// ---- console, processes, debugging ------------------------------------------------------

HLE("kernel32.dll", GetConsoleTitleA, 2) {
  SetLastError(kErrorInvalidHandle);  // no console attached
  return 0;
}
HLE("kernel32.dll", SetConsoleTitleA, 1) { return 1; }

HLE("kernel32.dll", CreateProcessA, 10) {
  WarnF("CreateProcessA(%s, %s): not supported\n", GStr(Arg(c, 0)).c_str(),
        GStr(Arg(c, 1)).c_str());
  SetLastError(kErrorFileNotFound);
  return 0;
}

HLE("kernel32.dll", FlushInstructionCache, 3) { return 1; }

HLE64("kernel32.dll", DebugBreak, 0) {
  FatalF("DebugBreak() called from %08llX", (unsigned long long)RdPtr(c.rsp));
}

// ---- version.dll ------------------------------------------------------------
// Only the game's own EXE has version info we can serve; it comes straight
// from the mapped .rsrc section.

// Finds the first RT_VERSION resource in the mapped image. Returns its
// guest VA and size.
bool IsOwnExe(const std::string& path) {
  std::string l = Lower(path), exe = Lower(Game().exe_name);
  size_t slash = l.find_last_of("\\/");
  return l.substr(slash == std::string::npos ? 0 : slash + 1) == exe;
}

// The image a version query names: the game's EXE or one of the DLLs
// lifted with it (by file name, as those live beside the EXE); else null.
const PeImage* ImageForPath(const std::string& path) {
  if (IsOwnExe(path)) return &g_win32->img;
  std::string l = Lower(path);
  l = l.substr(l.find_last_of("\\/") + 1);
  for (const Module& m : g_win32->modules)
    if (m.name == l && m.img.image_base) return &m.img;
  return nullptr;
}

// The RT_VERSION resource (first name and language) of the image at path.
bool FindVersionResource(const std::string& path, uint32_t* va, uint32_t* size) {
  const PeImage* img = ImageForPath(path);
  if (!img) return false;
  uint32_t e = FindResourceIn(*img, "#16", "", 0);
  if (!e) return false;
  *va = img->image_base + Rd32(e);
  *size = Rd32(e + 4);
  return true;
}

HLE("version.dll", GetFileVersionInfoSizeA, 2) {
  uint32_t va, size;
  if (Arg(c, 1)) Wr32(Arg(c, 1), 0);
  if (!FindVersionResource(GStr(Arg(c, 0)), &va, &size)) {
    SetLastError(kErrorFileNotFound);
    return 0;
  }
  // As Windows: the resource, a 4-byte signature, then as much again for
  // the ANSI copies VerQueryValueA hands out.
  return size * 2 + 4;
}

HLE("version.dll", GetFileVersionInfoA, 4) {
  uint32_t va, size;
  uint32_t len = Arg(c, 2), buf = Arg(c, 3);
  if (!FindVersionResource(GStr(Arg(c, 0)), &va, &size)) {
    SetLastError(kErrorFileNotFound);
    return 0;
  }
  std::memcpy(GPtr(buf), GPtr(va), std::min(len, size));
  // The signature Windows puts after the block when the buffer has room.
  if (len >= size + 4) std::memcpy(GPtr(buf + size), "FE2X", 4);
  return 1;
}

// Walks a VS_VERSIONINFO block tree (UTF-16 keys) along a backslash path.
bool VersionLookup(uint32_t block, const std::vector<std::string>& path,
                   size_t depth, uint32_t* value, uint32_t* vlen, bool* text) {
  uint16_t length = Rd16(block), value_len = Rd16(block + 2), type = Rd16(block + 4);
  std::u16string key = GWStr(block + 6);
  uint32_t p = block + 6 + uint32_t(key.size() + 1) * 2;
  p = (p + 3) & ~3u;
  if (depth == path.size()) {
    *value = p;
    *vlen = value_len;
    *text = type == 1;
    return true;
  }
  uint32_t child = (p + (type == 1 ? value_len * 2u : value_len) + 3) & ~3u;
  uint32_t end = block + length;
  while (child < end) {
    uint16_t clen = Rd16(child);
    if (!clen) break;
    if (Lower(Narrow(GWStr(child + 6))) == Lower(path[depth]))
      return VersionLookup(child, path, depth + 1, value, vlen, text);
    child = (child + clen + 3) & ~3u;
  }
  return false;
}

HLE("version.dll", VerQueryValueA, 4) {
  uint32_t buf = Arg(c, 0), out_ptr = Arg(c, 2), out_len = Arg(c, 3);
  std::string sub = GStr(Arg(c, 1));
  std::vector<std::string> path;
  std::string cur;
  for (char ch : sub) {
    if (ch == '\\') {
      if (!cur.empty()) path.push_back(cur);
      cur.clear();
    } else {
      cur += ch;
    }
  }
  if (!cur.empty()) path.push_back(cur);
  uint32_t value, vlen;
  bool text;
  if (!VersionLookup(buf, path, 0, &value, &vlen, &text)) return 0;
  if (text) {
    // An ANSI copy of the value's characters (the count includes the NUL
    // the resource stores) at the same offset in the area after the block
    // and its signature, as Windows places it: in bounds, and the same
    // place for every query of it. At most what is left of the block from
    // there, as WideCharToMultiByte is bounded there.
    const uint32_t size = Rd16(buf), pos = value - buf, room = pos < size ? size - pos : 0;
    std::u16string w;
    for (uint32_t i = 0; i < vlen && 2 * i + 2 <= room; i++) w.push_back(Rd16(value + 2 * i));
    std::string s = Narrow(w);
    const uint32_t dst = buf + size + 4 + pos;
    std::memcpy(GPtr(dst), s.data(), s.size());
    Wr32(out_ptr, dst);
    Wr32(out_len, uint32_t(s.size()));
  } else {
    Wr32(out_ptr, value);
    Wr32(out_len, vlen);
  }
  return 1;
}


// ---- W forms -----------------------------------------------------------------------

HLE("kernel32.dll", GetDateFormatW, 6) {
  std::tm t = TmFromSystemTime(Arg(c, 2));
  char out[64];
  if (Arg(c, 1) & 2)  // DATE_LONGDATE
    std::strftime(out, sizeof out, "%A, %B %d, %Y", &t);
  else
    std::snprintf(out, sizeof out, "%d/%d/%d", t.tm_mon + 1, t.tm_mday, t.tm_year + 1900);
  if (Arg(c, 3)) WarnF("GetDateFormatW: custom picture ignored\n");
  return PutCounted(Arg(c, 4), Arg(c, 5), out, true);
}

HLE("kernel32.dll", GetTimeFormatW, 6) {
  std::tm t = TmFromSystemTime(Arg(c, 2));
  char out[64];
  int h = t.tm_hour % 12;
  if (Arg(c, 1) & 2)  // TIME_NOSECONDS
    std::snprintf(out, sizeof out, "%d:%02d %s", h ? h : 12, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
  else
    std::snprintf(out, sizeof out, "%d:%02d:%02d %s", h ? h : 12, t.tm_min, t.tm_sec,
                  t.tm_hour < 12 ? "AM" : "PM");
  if (Arg(c, 3)) WarnF("GetTimeFormatW: custom picture ignored\n");
  return PutCounted(Arg(c, 4), Arg(c, 5), out, true);
}

// The version resource of the game's EXE or lifted DLLs, as the A forms
// (W paths).
HLE("version.dll", GetFileVersionInfoSizeW, 2) {
  uint32_t va, size;
  if (Arg(c, 1)) Wr32(Arg(c, 1), 0);
  if (!FindVersionResource(NarrowPath(GWStr(Arg(c, 0))), &va, &size)) {
    SetLastError(kErrorFileNotFound);
    return 0;
  }
  return size * 2 + 4;
}

HLE("version.dll", GetFileVersionInfoW, 4) {
  uint32_t va, size;
  uint32_t len = Arg(c, 2), buf = Arg(c, 3);
  if (!FindVersionResource(NarrowPath(GWStr(Arg(c, 0))), &va, &size)) {
    SetLastError(kErrorFileNotFound);
    return 0;
  }
  std::memcpy(GPtr(buf), GPtr(va), std::min(len, size));
  if (len >= size + 4) std::memcpy(GPtr(buf + size), "FE2X", 4);
  return 1;
}

// VerQueryValueW(block, subblock, out ptr, out len): values point into the
// block itself (UTF-16 text needs no copy).
HLE("version.dll", VerQueryValueW, 4) {
  uint32_t buf = Arg(c, 0), out_ptr = Arg(c, 2), out_len = Arg(c, 3);
  std::string sub = NarrowPath(GWStr(Arg(c, 1)));
  std::vector<std::string> path;
  std::string cur;
  for (char ch : sub) {
    if (ch == '\\') {
      if (!cur.empty()) path.push_back(cur);
      cur.clear();
    } else {
      cur += ch;
    }
  }
  if (!cur.empty()) path.push_back(cur);
  uint32_t value, vlen;
  bool text;
  if (!VersionLookup(buf, path, 0, &value, &vlen, &text)) return 0;
  Wr32(out_ptr, value);
  Wr32(out_len, vlen);
  return 1;
}

// ---- odds and ends ---------------------------------------------------------------

// File APIs take ANSI (code page) strings, not OEM ones.
HLE("kernel32.dll", AreFileApisANSI, 0) { return 1; }

HLE("kernel32.dll", OutputDebugStringW, 1) {
  std::fprintf(stderr, "[guest] %s", Narrow(GWStr(Arg(c, 0))).c_str());
  return 0;
}

// No named semaphore is ever shared with another process here; opening
// one that this process did not create fails as for a missing name.
HLE("kernel32.dll", OpenSemaphoreA, 3) {
  SetLastError(2);  // ERROR_FILE_NOT_FOUND
  return 0;
}

// Child processes and pipes to them are not supported: games use them for
// launching tools and browsers. Fails as for a missing program.
HLE("kernel32.dll", CreateProcessW, 10) {
  WarnF("CreateProcessW(%s): not supported\n",
        Arg(c, 1) ? Narrow(GWStr(Arg(c, 1))).c_str()
                  : (Arg(c, 0) ? Narrow(GWStr(Arg(c, 0))).c_str() : ""));
  SetLastError(2);  // ERROR_FILE_NOT_FOUND
  return 0;
}
HLE("kernel32.dll", CreatePipe, 4) {
  WarnF("CreatePipe: not supported\n");
  SetLastError(50);  // ERROR_NOT_SUPPORTED
  return 0;
}

// CreateNamedPipeW(name, open mode, pipe mode, max, out, in, timeout, sa):
// a pipe server is for another process to connect to (crash reporters
// do this), and there is none.
HLE("kernel32.dll", CreateNamedPipeW, 8) {
  WarnF("CreateNamedPipeW(%s): not supported\n", Narrow(GWStr(Arg(c, 0))).c_str());
  SetLastError(50);  // ERROR_NOT_SUPPORTED
  return 0xFFFFFFFF;  // INVALID_HANDLE_VALUE
}

// Device control (volume and disk queries): no devices to talk to.
HLE("kernel32.dll", DeviceIoControl, 8) {
  SetLastError(50);  // ERROR_NOT_SUPPORTED
  return 0;
}

}  // namespace
}  // namespace recomp::hle
