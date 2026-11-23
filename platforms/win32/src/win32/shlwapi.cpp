// shlwapi: the path and URL string helpers (ANSI, and a few wide ones). Pure string work on
// guest memory, as shlwapi does it: '\\', '/' and ':' separate path
// components; nothing touches the file system.

#include <cstring>
#include <string>

#include "win32/hle.h"

namespace recomp::hle {
namespace {

constexpr uint32_t kSOk = 0;
constexpr uint32_t kEPointer = 0x80004003;
constexpr uint32_t kEInvalidArg = 0x80070057;

bool IsSep(char ch) { return ch == '\\' || ch == '/' || ch == ':'; }

// PathFindFileNameA(path): the last component (after the last separator
// that is followed by something); the path itself if it has none.
HLE("shlwapi.dll", PathFindFileNameA, 1) {
  uint32_t p = Arg(c, 0);
  if (!p) return 0;
  std::string s = GStr(p);
  size_t last = 0;
  for (size_t i = 0; i + 1 < s.size(); i++)
    if (IsSep(s[i]) && s[i + 1] != '\\' && s[i + 1] != '/') last = i + 1;
  return p + uint32_t(last);
}

// The extension's offset in `s`: its last '.' after the last separator or
// space, or the string's end if there is none.
size_t ExtensionAt(const std::string& s) {
  size_t dot = std::string::npos;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '.') dot = i;
    else if (s[i] == '\\' || s[i] == ' ') dot = std::string::npos;
  }
  return dot == std::string::npos ? s.size() : dot;
}

// PathFindExtensionA(path): its ".ext", or its terminating NUL.
HLE("shlwapi.dll", PathFindExtensionA, 1) {
  uint32_t p = Arg(c, 0);
  if (!p) return 0;
  return p + uint32_t(ExtensionAt(GStr(p)));
}

// PathRemoveExtensionA(path): cuts the extension off in place.
HLE("shlwapi.dll", PathRemoveExtensionA, 1) {
  uint32_t p = Arg(c, 0);
  if (p) Wr8(p + uint32_t(ExtensionAt(GStr(p))), 0);
  return 0;
}

// PathIsUNCA(path): "\\server..." (or "\\?\UNC\..."), not "\\?\C:...".
HLE("shlwapi.dll", PathIsUNCA, 1) {
  uint32_t p = Arg(c, 0);
  if (!p) return 0;
  std::string s = GStr(p);
  if (s.rfind("\\\\?\\UNC\\", 0) == 0) return 1;
  return s.size() >= 2 && s[0] == '\\' && s[1] == '\\' && (s.size() < 3 || s[2] != '?');
}

// PathStripToRootA(path): leaves only its root ("C:\", "C:", "\",
// "\\server\share"); FALSE (unchanged) if it has none.
HLE("shlwapi.dll", PathStripToRootA, 1) {
  uint32_t p = Arg(c, 0);
  if (!p) return 0;
  std::string s = GStr(p);
  size_t keep;
  if (s.size() >= 2 && s[1] == ':') {
    keep = s.size() >= 3 && s[2] == '\\' ? 3 : 2;
  } else if (s.size() >= 2 && s[0] == '\\' && s[1] == '\\') {
    size_t server = s.find('\\', 2);
    size_t share = server == std::string::npos ? std::string::npos : s.find('\\', server + 1);
    keep = share == std::string::npos ? s.size() : share;
  } else if (!s.empty() && s[0] == '\\') {
    keep = 1;
  } else {
    return 0;
  }
  Wr8(p + uint32_t(keep), 0);
  return 1;
}

int HexDigit(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

// UrlUnescapeA(url, out, &out_chars, flags): "%XX" decoded; with
// URL_DONT_UNESCAPE_EXTRA_INFO (0x02000000) not after the first '?' or
// '#'; with URL_UNESCAPE_INPLACE (0x00100000) into url itself. out_chars
// is the buffer's size in, the result's length out (with the NUL needed,
// E_POINTER, if it does not fit).
HLE("shlwapi.dll", UrlUnescapeA, 4) {
  uint32_t url = Arg(c, 0), out = Arg(c, 1), pcch = Arg(c, 2), flags = Arg(c, 3);
  if (!url) return kEInvalidArg;
  std::string s = GStr(url), r;
  bool stop = false;
  for (size_t i = 0; i < s.size(); i++) {
    if ((flags & 0x02000000) && (s[i] == '?' || s[i] == '#')) stop = true;
    int hi, lo;
    if (!stop && s[i] == '%' && i + 2 < s.size() && (hi = HexDigit(s[i + 1])) >= 0 &&
        (lo = HexDigit(s[i + 2])) >= 0) {
      r += char(hi << 4 | lo);
      i += 2;
    } else {
      r += s[i];
    }
  }
  if (flags & 0x00100000) {
    PutStr(url, r);
    return kSOk;
  }
  if (!out || !pcch) return kEInvalidArg;
  if (Rd32(pcch) < r.size() + 1) {
    Wr32(pcch, uint32_t(r.size() + 1));
    return kEPointer;
  }
  PutStr(out, r);
  Wr32(pcch, uint32_t(r.size()));
  return kSOk;
}

// ---- wide versions -------------------------------------------------------

char16_t* WPtr(uint32_t va) { return reinterpret_cast<char16_t*>(GPtr(va)); }

// BOOL PathRemoveFileSpecW(WCHAR* path): cuts the last component and the
// backslash before it, in place, keeping a root ("C:\", "\"); a path with
// no backslash keeps only a drive ("C:") or becomes empty. TRUE if the
// path changed.
HLE("shlwapi.dll", PathRemoveFileSpecW, 1) {
  uint32_t p = Arg(c, 0);
  if (!p) return 0;
  std::u16string s = GWStr(p);
  size_t keep;
  size_t slash = s.rfind(u'\\');
  if (slash == std::u16string::npos)
    keep = s.size() >= 2 && s[1] == u':' ? 2 : 0;
  else if (slash == 0 || (slash == 2 && s[1] == u':'))
    keep = slash + 1;  // the root's backslash stays
  else
    keep = slash;
  if (keep >= s.size()) return 0;
  WPtr(p)[keep] = 0;
  return 1;
}

char16_t FoldW(char16_t ch) {
  if (ch >= u'A' && ch <= u'Z') return char16_t(ch + 32);
  if (ch >= 0xC0 && ch <= 0xDE && ch != 0xD7) return char16_t(ch + 32);  // Latin-1
  return ch;
}

// WCHAR* StrStrIW(const WCHAR* hay, const WCHAR* needle): the first match
// ignoring case, or NULL; an empty needle matches nothing.
HLE("shlwapi.dll", StrStrIW, 2) {
  uint32_t h = Arg(c, 0), n = Arg(c, 1);
  if (!h || !n) return 0;
  std::u16string hs = GWStr(h), ns = GWStr(n);
  if (ns.empty() || ns.size() > hs.size()) return 0;
  for (size_t i = 0; i + ns.size() <= hs.size(); i++) {
    size_t k = 0;
    while (k < ns.size() && FoldW(hs[i + k]) == FoldW(ns[k])) k++;
    if (k == ns.size()) return h + uint32_t(2 * i);
  }
  return 0;
}

}  // namespace
}  // namespace recomp::hle
