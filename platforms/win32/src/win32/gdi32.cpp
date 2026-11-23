// gdi32: fonts, text and metrics. Bitmaps, DCs and blits are in
// gdi_draw.cpp. Text drawing succeeds without output; games that draw
// glyphs through GDI need real rasterisation here.

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
HLE("gdi32.dll", CreateHatchBrush, 2) { return NewGdi(); }


// SetMapMode(hdc, mode): only MM_TEXT (1), one logical unit per pixel,
// which every DC starts in; the previous mode is always MM_TEXT.
HLE("gdi32.dll", SetMapMode, 2) {
  if (Arg(c, 1) != 1) WarnF("SetMapMode(%u): only MM_TEXT is supported\n", Arg(c, 1));
  return 1;
}

// GetSystemPaletteEntries(hdc, start, count, entries): the display is not
// palette-based (no RC_PALETTE in RASTERCAPS), and for such a device
// Windows returns 0 and fills nothing.
HLE("gdi32.dll", GetSystemPaletteEntries, 4) { return 0; }

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

HLE("gdi32.dll", TextOutW, 5) { return 1; }
HLE("gdi32.dll", GetPixel, 3) { return 0; }
HLE("gdi32.dll", DPtoLP, 3) { return 1; }


HLE("gdi32.dll", GetDIBits, 7) {
  WarnF("GetDIBits: no GDI surfaces; returning 0 lines\n");
  return 0;
}

// ---- gamma ------------------------------------------------------------------
// No hardware gamma ramp: games fall back to their own brightness
// handling, as on drivers without one.
HLE("gdi32.dll", GetDeviceGammaRamp, 2) { return 0; }
HLE("gdi32.dll", SetDeviceGammaRamp, 2) { return 0; }

}  // namespace
}  // namespace recomp::hle
