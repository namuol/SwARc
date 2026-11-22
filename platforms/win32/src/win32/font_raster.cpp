// font_raster.h on Core Text (macOS); elsewhere a fixed 8x16 cell with no
// glyphs drawn.

#include "win32/font_raster.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <mutex>
#include <tuple>

#ifdef __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#endif

namespace recomp::hle {

#ifdef __APPLE__
namespace {

// Windows face names to fonts every Mac has. Unknown faces are tried by
// name; Core Text falls back on its own when there is no such font.
std::string HostFace(std::string face) {
  std::string l = face;
  for (char& ch : l) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  if (l.empty() || l == "system" || l == "ms sans serif" || l == "ms shell dlg" ||
      l == "ms shell dlg 2" || l == "microsoft sans serif" || l == "tahoma" ||
      l == "small fonts" || l == "verdana")
    return l == "verdana" ? "Verdana" : (l == "tahoma" ? "Tahoma" : "Arial");
  if (l == "fixedsys" || l == "terminal" || l == "courier" || l == "lucida console")
    return "Courier New";
  if (l == "ms serif" || l == "times") return "Times New Roman";
  return face;
}

std::mutex g_mu;
std::map<std::tuple<std::string, int, int, bool>, CTFontRef> g_fonts;

CTFontRef FontFor(const FontDesc& d) {
  std::lock_guard<std::mutex> l(g_mu);
  auto key = std::make_tuple(d.face, d.height, d.weight, d.italic);
  auto it = g_fonts.find(key);
  if (it != g_fonts.end()) return it->second;
  CFStringRef name = CFStringCreateWithCString(nullptr, HostFace(d.face).c_str(),
                                               kCFStringEncodingUTF8);
  CTFontRef base = CTFontCreateWithName(name, 1.0, nullptr);
  CFRelease(name);
  const double cell1 = CTFontGetAscent(base) + CTFontGetDescent(base);
  double em;
  if (d.height < 0)
    em = -d.height;  // character height: the em
  else
    em = (d.height ? d.height : 16) / (cell1 > 0 ? cell1 : 1.0);  // cell height
  CTFontRef f = CTFontCreateCopyWithAttributes(base, em, nullptr, nullptr);
  CFRelease(base);
  CTFontSymbolicTraits traits = 0;
  if (d.weight >= 600) traits |= kCTFontBoldTrait;
  if (d.italic) traits |= kCTFontItalicTrait;
  if (traits) {
    if (CTFontRef styled = CTFontCreateCopyWithSymbolicTraits(f, em, nullptr, traits, traits)) {
      CFRelease(f);
      f = styled;
    }
  }
  g_fonts[key] = f;
  return f;
}

CTLineRef LineFor(CTFontRef f, const char* s, int n) {
  CFStringRef str = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(s), n,
                                            kCFStringEncodingWindowsLatin1, false);
  if (!str) str = CFStringCreateWithCString(nullptr, "", kCFStringEncodingUTF8);
  const void* keys[] = {kCTFontAttributeName, kCTForegroundColorFromContextAttributeName};
  const void* vals[] = {f, kCFBooleanTrue};
  CFDictionaryRef attrs = CFDictionaryCreate(nullptr, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks,
                                             &kCFTypeDictionaryValueCallBacks);
  CFAttributedStringRef as = CFAttributedStringCreate(nullptr, str, attrs);
  CTLineRef line = CTLineCreateWithAttributedString(as);
  CFRelease(as);
  CFRelease(attrs);
  CFRelease(str);
  return line;
}

int Ceil(double v) { return int(std::ceil(v - 1e-6)); }

}  // namespace

FontMetrics MeasureFont(const FontDesc& d) {
  CTFontRef f = FontFor(d);
  FontMetrics m;
  m.ascent = Ceil(CTFontGetAscent(f));
  m.descent = Ceil(CTFontGetDescent(f));
  m.ave_width = std::max(1, TextWidth(d, "x", 1));
  m.max_width = std::max(m.ave_width, Ceil(CTFontGetBoundingBox(f).size.width));
  return m;
}

int TextWidth(const FontDesc& d, const char* s, int n) {
  if (n <= 0) return 0;
  CTLineRef line = LineFor(FontFor(d), s, n);
  int w = Ceil(CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr));
  CFRelease(line);
  return w;
}

void RasterizeText(const FontDesc& d, const char* s, int n, std::vector<uint8_t>* mask, int* w,
                   int* h) {
  const FontMetrics m = MeasureFont(d);
  *h = m.ascent + m.descent;
  *w = TextWidth(d, s, n);
  mask->assign(size_t(std::max(*w, 0)) * std::max(*h, 0), 0);
  if (*w <= 0 || *h <= 0) return;
  CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
  CGContextRef ctx = CGBitmapContextCreate(mask->data(), size_t(*w), size_t(*h), 8, size_t(*w),
                                           gray, kCGImageAlphaNone);
  CGColorSpaceRelease(gray);
  if (!ctx) return;
  CGContextSetAllowsAntialiasing(ctx, false);
  CGContextSetShouldAntialias(ctx, false);
  CGContextSetAllowsFontSmoothing(ctx, false);
  CGContextSetShouldSmoothFonts(ctx, false);
  CGContextSetGrayFillColor(ctx, 1, 1);
  CGContextSetTextPosition(ctx, 0, m.descent);  // Core Graphics: y up, baseline
  CTLineRef line = LineFor(FontFor(d), s, n);
  CTLineDraw(line, ctx);
  CFRelease(line);
  CGContextRelease(ctx);
  for (uint8_t& v : *mask) v = v >= 128 ? 255 : 0;
}

bool PrivateFont(const std::string& path, bool add) {
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(
      nullptr, reinterpret_cast<const UInt8*>(path.c_str()), CFIndex(path.size()), false);
  if (!url) return false;
  bool ok = add ? CTFontManagerRegisterFontsForURL(url, kCTFontManagerScopeProcess, nullptr)
                : CTFontManagerUnregisterFontsForURL(url, kCTFontManagerScopeProcess, nullptr);
  CFRelease(url);
  if (ok) {
    // Faces looked up before may now resolve differently.
    std::lock_guard<std::mutex> l(g_mu);
    for (auto& [k, f] : g_fonts) CFRelease(f);
    g_fonts.clear();
  }
  return ok;
}
bool AddPrivateFont(const std::string& path) { return PrivateFont(path, true); }
bool RemovePrivateFont(const std::string& path) { return PrivateFont(path, false); }

#else  // no host font engine: 8x16 cells, nothing drawn

bool AddPrivateFont(const std::string&) { return false; }
bool RemovePrivateFont(const std::string&) { return false; }

FontMetrics MeasureFont(const FontDesc&) { return {}; }
int TextWidth(const FontDesc&, const char*, int n) { return 8 * std::max(n, 0); }
void RasterizeText(const FontDesc&, const char*, int n, std::vector<uint8_t>* mask, int* w,
                   int* h) {
  *w = 8 * std::max(n, 0);
  *h = 16;
  mask->assign(size_t(*w) * *h, 0);
}

#endif

}  // namespace recomp::hle
