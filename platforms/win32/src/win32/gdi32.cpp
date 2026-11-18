// gdi32: device contexts, fonts and metrics. The game draws through
// DirectDraw; GDI is only queried (metrics, device caps). Drawing calls
// succeed without output. If the game turns out to render glyphs through
// GDI (TextOutW/GetDIBits suggest IME text), real rasterisation goes here.

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

#include "runtime/log.h"
#include "win32/gdi_state.h"
#include "win32/hle.h"
#include "win32/input_state.h"

namespace recomp::hle {

// ---- bitmaps loaded from files (gdi_state.h) ----------------------------------

namespace {
std::mutex g_bitmaps_mu;
std::map<uint32_t, BitmapDesc> g_bitmaps;
}  // namespace

bool ReadBmpHeader(const std::string& host_path, BitmapDesc* out) {
  std::FILE* f = std::fopen(host_path.c_str(), "rb");
  if (!f) return false;
  uint8_t h[14 + 16];
  size_t n = std::fread(h, 1, sizeof h, f);
  std::fclose(f);
  if (n < 14 + 12 || h[0] != 'B' || h[1] != 'M') return false;
  auto u16 = [&](int o) { return uint16_t(h[o] | h[o + 1] << 8); };
  auto u32 = [&](int o) { return uint32_t(u16(o) | uint32_t(u16(o + 2)) << 16); };
  uint32_t size = u32(14);
  if (size == 12) {  // BITMAPCOREHEADER: 16-bit width and height
    out->width = u16(18);
    out->height = u16(20);
    out->bits_per_pixel = u16(24);
  } else if (size >= 40 && n >= 14 + 16) {  // BITMAPINFOHEADER and later
    out->width = int32_t(u32(18));
    int32_t height = int32_t(u32(22));
    out->height = height < 0 ? -height : height;  // top-down
    out->bits_per_pixel = u16(28);
  } else {
    return false;
  }
  return out->width > 0 && out->height > 0;
}

void RememberBitmap(uint32_t handle, const BitmapDesc& d) {
  std::lock_guard<std::mutex> l(g_bitmaps_mu);
  g_bitmaps[handle] = d;
}

bool FindBitmap(uint32_t handle, BitmapDesc* out) {
  std::lock_guard<std::mutex> l(g_bitmaps_mu);
  auto it = g_bitmaps.find(handle);
  if (it == g_bitmaps.end()) return false;
  *out = it->second;
  return true;
}

namespace {

std::atomic<uint32_t> g_next_gdi{0x00E00010};
uint32_t NewGdi() { return g_next_gdi += 0x10; }

// Stock objects get fixed handles so repeated GetStockObject calls agree.
HLE("gdi32.dll", GetStockObject, 1) { return 0x00E00000u | (Arg(c, 0) & 0xFF); }
HLE("gdi32.dll", CreateFontA, 14) { return NewGdi(); }
HLE("gdi32.dll", CreateFontIndirectA, 1) { return NewGdi(); }
HLE("gdi32.dll", CreateHatchBrush, 2) { return NewGdi(); }
HLE("gdi32.dll", CreateSolidBrush, 1) { return NewGdi(); }
HLE("gdi32.dll", CreateCompatibleDC, 1) { return NewGdi(); }
HLE("gdi32.dll", CreateCompatibleBitmap, 3) { return NewGdi(); }
HLE("gdi32.dll", DeleteObject, 1) { return 1; }
HLE("gdi32.dll", DeleteDC, 1) { return 1; }
HLE("gdi32.dll", SelectObject, 2) { return 0x00E00000u; }

HLE("gdi32.dll", SetBkMode, 2) { return 2; }      // previous: OPAQUE
HLE("gdi32.dll", SetTextColor, 2) { return 0; }
HLE("gdi32.dll", SetBkColor, 2) { return 0xFFFFFF; }

HLE("gdi32.dll", GetDeviceCaps, 2) {
  switch (Arg(c, 1)) {
    case 8: return uint32_t(g_display_w);   // HORZRES
    case 10: return uint32_t(g_display_h);  // VERTRES
    case 12: return 32;                     // BITSPIXEL
    case 14: return 1;                      // PLANES
    case 24: return 0xFFFFFFFF;             // NUMCOLORS: > 8 bpp
    case 88: case 90: return 96;            // LOGPIXELSX/Y
    case 116: return 60;                    // VREFRESH
    case 4: return 320;                     // HORZSIZE (mm)
    case 6: return 240;                     // VERTSIZE
    case 38: return 0x7E99;                 // RASTERCAPS
    default:
      WarnF("GetDeviceCaps(%u): returning 0\n", uint32_t(Arg(c, 1)));
      return 0;
  }
}

// Metrics of a generic 16 px proportional font.
HLE("gdi32.dll", GetTextMetricsA, 2) {
  uint32_t tm = Arg(c, 1);
  std::memset(GPtr(tm), 0, 56);
  Wr32(tm + 0, 16);   // tmHeight
  Wr32(tm + 4, 13);   // tmAscent
  Wr32(tm + 8, 3);    // tmDescent
  Wr32(tm + 20, 7);   // tmAveCharWidth
  Wr32(tm + 24, 14);  // tmMaxCharWidth
  Wr32(tm + 28, 400); // tmWeight
  Wr32(tm + 36, 96);  // tmDigitizedAspectX
  Wr32(tm + 40, 96);  // tmDigitizedAspectY
  Wr8(tm + 44, 0x20); // tmFirstChar
  Wr8(tm + 45, 0xFF); // tmLastChar
  Wr8(tm + 46, '?');  // tmDefaultChar
  Wr8(tm + 47, ' ');  // tmBreakChar
  Wr8(tm + 51, 0x26); // tmPitchAndFamily: variable pitch, TrueType, swiss
  return 1;
}

HLE("gdi32.dll", GetCharWidthA, 4) {
  uint32_t first = Arg(c, 1), last = Arg(c, 2), buf = Arg(c, 3);
  for (uint32_t ch = first; ch <= last; ch++) Wr32(buf + 4 * (ch - first), 7);
  return 1;
}

HLE("gdi32.dll", TextOutW, 5) { return 1; }
HLE("gdi32.dll", ExtTextOutA, 8) { return 1; }
HLE("gdi32.dll", GetPixel, 3) { return 0; }
HLE("gdi32.dll", DPtoLP, 3) { return 1; }

// GetObjectA(h, size, out): a file bitmap as a BITMAP (24 bytes: bmType 0,
// width, height, bytes per row rounded to 2 bytes as GDI does, one plane,
// bits per pixel, no bits pointer for a device-dependent bitmap). Returns
// the bytes written, or the size needed when out is NULL. Other objects
// have nothing to describe here: 0.
HLE("gdi32.dll", GetObjectA, 3) {
  uint32_t h = Arg(c, 0), size = Arg(c, 1), out = Arg(c, 2);
  BitmapDesc d;
  if (!FindBitmap(h, &d)) {
    WarnF("GetObjectA(%08X): not a bitmap loaded from a file\n", h);
    return 0;
  }
  if (!out) return 24;
  if (size < 24) return 0;
  std::memset(GPtr(out), 0, 24);
  Wr32(out + 4, uint32_t(d.width));
  Wr32(out + 8, uint32_t(d.height));
  Wr32(out + 12, ((uint32_t(d.width) * d.bits_per_pixel + 15) / 16) * 2);
  Wr16(out + 16, 1);
  Wr16(out + 18, d.bits_per_pixel);
  return 24;
}

// GetCurrentObject(hdc, type): the stock object of that kind; DCs here
// hold no selection (SelectObject returns a stock handle too).
HLE("gdi32.dll", GetCurrentObject, 2) { return 0x00E00000u; }

// Drawing to a DC has no output (see the top of this file).
HLE("gdi32.dll", TextOutA, 5) { return 1; }
HLE("gdi32.dll", BitBlt, 9) { return 1; }

HLE("gdi32.dll", GetDIBits, 7) {
  WarnF("GetDIBits: no GDI surfaces; returning 0 lines\n");
  return 0;
}

}  // namespace
}  // namespace recomp::hle
