// gdi32 drawing: DIB sections, memory DCs, palettes and solid brushes, and
// the blits between them and onto windows, in software.
//
// For games that render into a DIB section with the CPU and put it on screen
// with BitBlt/StretchBlt/SetDIBitsToDevice (the 2D games of the GDI era).
//
// - Bitmaps: DIB sections (CreateDIBSection) and compatible bitmaps
//   (CreateCompatibleBitmap, 32 bpp) keep their pixels in guest memory, so
//   the game draws into them directly. 1, 4, 8 bpp read through their colour
//   table; 16 bpp is 5-5-5 or BI_BITFIELDS; 24 and 32 bpp are BGR(X).
// - Memory DCs (CreateCompatibleDC) select a bitmap, a palette, a brush.
// - Window DCs (user32 GetDC: DcForWindow) draw into a render target of the
//   window's client size on its gfx device: a blit onto a window uploads the
//   source rectangle at its own size and the GPU scales it (linear
//   filtering, as HALFTONE stretching looks), fills are GPU fills. A blit
//   onto a window presents the target (present.h: screenshots and frame
//   reports apply); fills alone don't present, they only change what the
//   next present shows. Windows are not read back: a window as a blit
//   source, or ScrollDC on one, is not supported (a warning).
// - Raster operations: SRCCOPY and the constant ones (BLACKNESS, WHITENESS,
//   PATCOPY with a solid brush); others are drawn as SRCCOPY, with a warning.
// - Stretching between bitmaps is nearest-neighbour whatever
//   SetStretchBltMode asked for.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "runtime/gfx/present.h"
#include "runtime/log.h"
#include "win32/font_raster.h"
#include "win32/gdi_state.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "win32/user32_window.h"
#include "x86/present.h"

namespace recomp::hle {
namespace {

// ---- objects ----------------------------------------------------------------------

struct Bitmap {
  int w = 1, h = 1;          // h > 0
  bool bottom_up = false;
  int bpp = 32;
  uint32_t pitch = 4;        // bytes per row
  gaddr bits = 0;            // guest memory, pitch * h bytes
  bool owns_bits = true;
  uint32_t masks[3] = {0, 0, 0};  // 16/32 bpp BI_BITFIELDS (0: BI_RGB)
  uint32_t pal[256] = {};    // colour table as XRGB
  int pal_n = 0;
  uint32_t compression = 0;
};

struct Palette {
  std::vector<uint32_t> xrgb;
};

struct Brush {
  uint32_t xrgb = 0;
};

struct Dc {
  uint32_t bitmap = 0, palette = 0, brush = 0;
  int stretch_mode = 1;  // BLACKONWHITE
};

std::recursive_mutex g_mu;
uint32_t g_next = 0x01000000;
std::map<uint32_t, Bitmap> g_bitmaps;
std::map<uint32_t, Palette> g_palettes;
std::map<uint32_t, Brush> g_brushes;
std::map<uint32_t, Dc> g_dcs;

// Fonts (CreateFont*) and each DC's text state (any DC, window DCs too).
std::map<uint32_t, FontDesc> g_fonts;
struct TextState {
  uint32_t font = 0;  // 0: the stock SYSTEM_FONT
  uint32_t color = 0, bk_color = 0xFFFFFF;
  int bk_mode = 2;  // OPAQUE
  uint32_t align = 0;
};
std::map<uint32_t, TextState> g_text;

// A memory DC's initial bitmap: the stock 1x1 monochrome bitmap
// (GetStockObject has no index for it; this is its handle here).
constexpr uint32_t kStockBitmap = 0x00E000FF;
constexpr uint32_t kStockPalette = 0x00E0000F;  // DEFAULT_PALETTE
constexpr uint32_t kStockBlackBrush = 0x00E00004;

uint32_t NewHandle() { return g_next += 4; }

uint32_t ColorRefToXrgb(uint32_t cr) {
  return (cr & 0xFF) << 16 | (cr & 0xFF00) | (cr >> 16 & 0xFF);
}

// Brushes: ours, the stock ones (GetStockObject: 0x00E000nn), and
// system-colour brushes ((HBRUSH)(COLOR_x + 1)).
uint32_t BrushColor(uint32_t h) {
  auto it = g_brushes.find(h);
  if (it != g_brushes.end()) return it->second.xrgb;
  if ((h & 0xFFFFFF00) == 0x00E00000) {
    switch (h & 0xFF) {
      case 0: return 0xFFFFFF;
      case 1: return 0xC0C0C0;
      case 2: return 0x808080;
      case 3: return 0x404040;
      default: return 0;
    }
  }
  if (h && h <= 31) {  // system colours: the classic Windows scheme
    static const uint32_t kSys[31] = {
        0xC0C0C0, 0x008080, 0x000080, 0x808080, 0xC0C0C0, 0xFFFFFF, 0x000000, 0x000000,
        0x000000, 0xFFFFFF, 0xC0C0C0, 0xC0C0C0, 0x808080, 0x000080, 0xFFFFFF, 0xC0C0C0,
        0x808080, 0x808080, 0x000000, 0xC0C0C0, 0xFFFFFF, 0x000000, 0xDFDFDF, 0x000000,
        0xFFFFE1, 0xB5B5B5, 0x0000FF, 0x1084D0, 0xB5B5B5, 0x000080, 0xC0C0C0};
    return kSys[h - 1];
  }
  return 0;
}

// Stock fonts (GetStockObject OEM_FIXED_FONT 10 .. DEFAULT_GUI_FONT 17).
constexpr uint32_t kStockSystemFont = 0x00E0000D;
bool IsFont(uint32_t h) {
  if (g_fonts.count(h)) return true;
  if ((h & 0xFFFFFF00) != 0x00E00000) return false;
  uint32_t i = h & 0xFF;
  return (i >= 10 && i <= 14) || i == 16 || i == 17;
}
FontDesc StockFont(uint32_t i) {
  switch (i) {
    case 10: case 11: case 16: return {"Courier New", 16, 400, false};  // fixed
    case 12: case 17: return {"MS Sans Serif", -11, 400, false};       // ANSI_VAR, GUI
    default: return {"System", 16, 700, false};                       // SYSTEM_FONT
  }
}
FontDesc FontOfDc(uint32_t hdc) {
  auto t = g_text.find(hdc);
  uint32_t h = t == g_text.end() ? 0 : t->second.font;
  auto f = g_fonts.find(h);
  if (f != g_fonts.end()) return f->second;
  return StockFont(h ? h & 0xFF : 13);
}

// ---- windows: a render target each ------------------------------------------------

struct WinSurface {
  std::unique_ptr<gfx::Device> dev;
  gfx::TextureId target = 0;
  int w = 0, h = 0;
};
std::map<uint32_t, WinSurface> g_windows;

// The window's target at its current client size; null off the main
// thread (SDL video belongs to it) or before the window has a host window.
// A resize keeps what fits of the old contents.
WinSurface* WinFor(uint32_t hwnd) {
  if (!OnMainThread()) return nullptr;
  WinSurface& s = g_windows[hwnd];
  if (!s.dev) {
    SDL_Window* sdl = SdlWindowFor(hwnd);
    if (!sdl) return nullptr;
    s.dev = gfx::CreateChosenDevice(sdl);
  }
  int w = 0, h = 0;
  ClientAreaSize(hwnd, &w, &h);
  w = std::max(w, 1);
  h = std::max(h, 1);
  if (w != s.w || h != s.h) {
    s.dev->ResetSwapChain(w, h, false);
    gfx::TextureId t = s.dev->CreateTexture(w, h, 1, gfx::Format::X8R8G8B8, true);
    s.dev->FillTexture(t, {0, 0, w, h}, 0xFF000000u);
    if (s.target) {
      gfx::Rect keep{0, 0, std::min(w, s.w), std::min(h, s.h)};
      s.dev->StretchRect({s.target, 0}, keep, {t, 0}, keep, false);
      s.dev->DestroyTexture(s.target);
    }
    s.target = t;
    s.w = w;
    s.h = h;
  }
  return &s;
}

void PresentWindow(uint32_t hwnd) {
  WinSurface* s = WinFor(hwnd);
  if (!s) return;
  gfx::Rect r{0, 0, s->w, s->h};
  s->dev->SetRenderTarget(0, 0);
  s->dev->StretchRect({s->target, 0}, r, {0, 0}, r, false);
  PresentFrame(*s->dev, s->w, s->h);
}

void FillWindow(uint32_t hwnd, int l, int t, int r, int b, uint32_t xrgb) {
  WinSurface* s = WinFor(hwnd);
  if (!s) return;
  gfx::Rect c{std::max(l, 0), std::max(t, 0), std::min(r, s->w), std::min(b, s->h)};
  if (c.right <= c.left || c.bottom <= c.top) return;
  s->dev->FillTexture(s->target, c, 0xFF000000u | xrgb);
}

void WarnWindowRead(const char* what) {
  static bool warned = false;
  if (!warned) {
    warned = true;
    WarnF("gdi: %s reads a window's pixels: not supported, skipped\n", what);
  }
}

// ---- surfaces: what a DC draws on -------------------------------------------------------

struct Surf {
  uint8_t* row0 = nullptr;  // top row
  ptrdiff_t stride = 0;     // bytes from one row to the one below
  int w = 0, h = 0, bpp = 32;
  const uint32_t* pal = nullptr;
  int pal_n = 0;
  uint32_t masks[3] = {0, 0, 0};
  uint32_t window = 0;      // a window (drawn on the GPU, row0 null)
  explicit operator bool() const { return row0 != nullptr; }
};

Surf SurfOfBitmap(const Bitmap& b) {
  Surf s;
  if (!b.bits) return s;
  uint8_t* base = GPtr(b.bits);
  s.w = b.w;
  s.h = b.h;
  s.bpp = b.bpp;
  if (b.bottom_up) {
    s.row0 = base + size_t(b.h - 1) * b.pitch;
    s.stride = -ptrdiff_t(b.pitch);
  } else {
    s.row0 = base;
    s.stride = ptrdiff_t(b.pitch);
  }
  s.pal = b.pal;
  s.pal_n = b.pal_n;
  std::memcpy(s.masks, b.masks, sizeof s.masks);
  return s;
}

Surf SurfOfDc(uint32_t hdc) {
  if (uint32_t hwnd = WindowForDc(hdc)) {
    Surf s;  // no pixels on the CPU (see the top of the file)
    s.window = hwnd;
    return s;
  }
  auto it = g_dcs.find(hdc);
  if (it == g_dcs.end()) return {};
  auto b = g_bitmaps.find(it->second.bitmap);
  if (b == g_bitmaps.end()) return {};
  return SurfOfBitmap(b->second);
}

inline int MaskShift(uint32_t m) { return m ? __builtin_ctz(m) : 0; }
inline uint32_t Expand(uint32_t v, uint32_t mask) {
  if (!mask) return 0;
  int bits = __builtin_popcount(mask);
  v = (v & mask) >> MaskShift(mask);
  if (bits >= 8) return v >> (bits - 8);
  uint32_t r = v << (8 - bits);
  return r | (r >> bits);
}

inline uint32_t ReadPx(const Surf& s, int x, int y) {
  const uint8_t* row = s.row0 + y * s.stride;
  switch (s.bpp) {
    case 32:
      if (s.masks[0])
        return Expand(reinterpret_cast<const uint32_t*>(row)[x], s.masks[0]) << 16 |
               Expand(reinterpret_cast<const uint32_t*>(row)[x], s.masks[1]) << 8 |
               Expand(reinterpret_cast<const uint32_t*>(row)[x], s.masks[2]);
      return reinterpret_cast<const uint32_t*>(row)[x] & 0xFFFFFF;
    case 24: {
      const uint8_t* p = row + 3 * x;
      return uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
    }
    case 16: {
      uint32_t v = reinterpret_cast<const uint16_t*>(row)[x];
      uint32_t rm = s.masks[0] ? s.masks[0] : 0x7C00, gm = s.masks[0] ? s.masks[1] : 0x03E0,
               bm = s.masks[0] ? s.masks[2] : 0x001F;
      return Expand(v, rm) << 16 | Expand(v, gm) << 8 | Expand(v, bm);
    }
    case 8: return s.pal ? s.pal[row[x]] : 0;
    case 4: {
      uint8_t v = row[x >> 1];
      return s.pal ? s.pal[(x & 1) ? v & 15 : v >> 4] : 0;
    }
    case 1: return s.pal ? s.pal[(row[x >> 3] >> (7 - (x & 7))) & 1] : 0;
  }
  return 0;
}

inline uint32_t Pack(uint32_t xrgb, uint32_t mask) {
  if (!mask) return 0;
  int bits = __builtin_popcount(mask);
  return ((xrgb >> (8 - std::min(bits, 8))) << MaskShift(mask)) & mask;
}

int NearestIndex(const Surf& s, uint32_t xrgb) {
  int best = 0;
  int bd = 1 << 30;
  for (int i = 0; i < s.pal_n; i++) {
    int dr = int(s.pal[i] >> 16 & 255) - int(xrgb >> 16 & 255);
    int dg = int(s.pal[i] >> 8 & 255) - int(xrgb >> 8 & 255);
    int db = int(s.pal[i] & 255) - int(xrgb & 255);
    int d = dr * dr + dg * dg + db * db;
    if (d < bd) {
      bd = d;
      best = i;
    }
  }
  return best;
}

inline void WritePx(const Surf& s, int x, int y, uint32_t xrgb) {
  uint8_t* row = s.row0 + y * s.stride;
  switch (s.bpp) {
    case 32:
      if (s.masks[0])
        reinterpret_cast<uint32_t*>(row)[x] = Pack(xrgb >> 16 & 255, s.masks[0]) |
                                              Pack(xrgb >> 8 & 255, s.masks[1]) |
                                              Pack(xrgb & 255, s.masks[2]);
      else
        reinterpret_cast<uint32_t*>(row)[x] = xrgb & 0xFFFFFF;
      return;
    case 24: {
      uint8_t* p = row + 3 * x;
      p[0] = uint8_t(xrgb);
      p[1] = uint8_t(xrgb >> 8);
      p[2] = uint8_t(xrgb >> 16);
      return;
    }
    case 16: {
      uint32_t rm = s.masks[0] ? s.masks[0] : 0x7C00, gm = s.masks[0] ? s.masks[1] : 0x03E0,
               bm = s.masks[0] ? s.masks[2] : 0x001F;
      reinterpret_cast<uint16_t*>(row)[x] =
          uint16_t(Pack(xrgb >> 16 & 255, rm) | Pack(xrgb >> 8 & 255, gm) | Pack(xrgb & 255, bm));
      return;
    }
    case 8: row[x] = uint8_t(NearestIndex(s, xrgb)); return;
    case 4: {
      uint8_t i = uint8_t(NearestIndex(s, xrgb));
      uint8_t& v = row[x >> 1];
      v = (x & 1) ? (v & 0xF0) | i : (v & 0x0F) | i << 4;
      return;
    }
    case 1: {
      uint8_t bit = uint8_t(0x80 >> (x & 7));
      if (NearestIndex(s, xrgb)) row[x >> 3] |= bit; else row[x >> 3] &= ~bit;
      return;
    }
  }
}

bool SameFormat(const Surf& a, const Surf& b) {
  return a.bpp == b.bpp && a.bpp >= 8 && std::memcmp(a.masks, b.masks, sizeof a.masks) == 0;
}

// Copies sr (x, y, w, h) of src onto dr of dst, scaling nearest-neighbour,
// clipped to both. Overlapping copies within one surface work for
// unscaled same-format blits (memmove), which is what ScrollDC-style
// self-blits are.
void BlitSurf(const Surf& dst, int dx, int dy, int dw, int dh, const Surf& src, int sx, int sy,
              int sw, int sh) {
  if (dw <= 0 || dh <= 0 || sw == 0 || sh == 0) return;
  bool mirror_x = sw < 0, mirror_y = sh < 0;
  sw = std::abs(sw);
  sh = std::abs(sh);
  if (dw == sw && dh == sh && !mirror_x && !mirror_y) {
    // Clip the source and destination together.
    int x0 = std::max({0, -dx, -sx}), y0 = std::max({0, -dy, -sy});
    int x1 = std::min({dw, dst.w - dx, src.w - sx}), y1 = std::min({dh, dst.h - dy, src.h - sy});
    if (x1 <= x0 || y1 <= y0) return;
    if (SameFormat(dst, src)) {
      size_t bytes = size_t(x1 - x0) * dst.bpp / 8;
      bool up = src.row0 == dst.row0 && dy > sy;  // overlapping, moving down: bottom first
      for (int i = 0; i < y1 - y0; i++) {
        int yy = up ? y1 - 1 - i : y0 + i;
        std::memmove(dst.row0 + (dy + yy) * dst.stride + size_t(dx + x0) * dst.bpp / 8,
                     src.row0 + (sy + yy) * src.stride + size_t(sx + x0) * src.bpp / 8, bytes);
      }
      return;
    }
    for (int y = y0; y < y1; y++)
      for (int x = x0; x < x1; x++) WritePx(dst, dx + x, dy + y, ReadPx(src, sx + x, sy + y));
    return;
  }
  bool same = SameFormat(dst, src) && dst.bpp % 8 == 0;
  int bypp = dst.bpp / 8;
  for (int y = std::max(0, -dy); y < dh && dy + y < dst.h; y++) {
    int syy = int((int64_t(y) * sh) / dh);
    if (mirror_y) syy = sh - 1 - syy;
    syy += sy;
    if (syy < 0 || syy >= src.h) continue;
    for (int x = std::max(0, -dx); x < dw && dx + x < dst.w; x++) {
      int sxx = int((int64_t(x) * sw) / dw);
      if (mirror_x) sxx = sw - 1 - sxx;
      sxx += sx;
      if (sxx < 0 || sxx >= src.w) continue;
      if (same)
        std::memcpy(dst.row0 + (dy + y) * dst.stride + size_t(dx + x) * bypp,
                    src.row0 + syy * src.stride + size_t(sxx) * bypp, bypp);
      else
        WritePx(dst, dx + x, dy + y, ReadPx(src, sxx, syy));
    }
  }
}

void FillSurf(const Surf& s, int l, int t, int r, int b, uint32_t xrgb) {
  l = std::max(l, 0);
  t = std::max(t, 0);
  r = std::min(r, s.w);
  b = std::min(b, s.h);
  for (int y = t; y < b; y++)
    for (int x = l; x < r; x++) WritePx(s, x, y, xrgb);
}

// Reads a BITMAPINFO at `bmi` (BITMAPINFOHEADER or later; BITMAPCOREHEADER
// is not expected) into a Bitmap description. `usage` DIB_PAL_COLORS (1)
// takes the colour table as indices into `pal` (or nothing).
bool ParseBitmapInfo(gaddr bmi, uint32_t usage, const Palette* pal, Bitmap* b) {
  uint32_t size = Rd32(bmi);
  if (size < 40) {
    WarnF("gdi: BITMAPINFO header size %u not supported\n", size);
    return false;
  }
  int32_t w = int32_t(Rd32(bmi + 4)), h = int32_t(Rd32(bmi + 8));
  b->bpp = Rd16(bmi + 14);
  b->compression = Rd32(bmi + 16);
  if (w <= 0 || h == 0 || !(b->bpp == 1 || b->bpp == 4 || b->bpp == 8 || b->bpp == 16 ||
                             b->bpp == 24 || b->bpp == 32))
    return false;
  if (b->compression != 0 && b->compression != 3) {
    WarnF("gdi: DIB compression %u not supported\n", b->compression);
    return false;
  }
  b->w = w;
  b->h = h < 0 ? -h : h;
  b->bottom_up = h > 0;
  b->pitch = ((uint32_t(w) * b->bpp + 31) / 32) * 4;
  gaddr table = bmi + size;
  if (b->compression == 3) {
    for (int i = 0; i < 3; i++) b->masks[i] = Rd32(size == 40 ? table + 4 * i : bmi + 40 + 4 * i);
    if (size == 40) table += 12;
  } else if (b->bpp == 32 || b->bpp == 16) {
    std::memset(b->masks, 0, sizeof b->masks);
  }
  if (b->bpp <= 8) {
    uint32_t used = Rd32(bmi + 32);
    int n = used ? int(std::min<uint32_t>(used, 1u << b->bpp)) : 1 << b->bpp;
    b->pal_n = 1 << b->bpp;
    for (int i = 0; i < n; i++) {
      if (usage == 1) {
        uint16_t idx = Rd16(table + 2 * i);
        b->pal[i] = pal && idx < pal->xrgb.size() ? pal->xrgb[idx] : 0;
      } else {
        b->pal[i] = Rd32(table + 4 * i) & 0xFFFFFF;
      }
    }
  }
  return true;
}

Palette* DcPalette(uint32_t hdc) {
  auto d = g_dcs.find(hdc);
  if (d == g_dcs.end()) return nullptr;
  auto p = g_palettes.find(d->second.palette);
  return p == g_palettes.end() ? nullptr : &p->second;
}

// ---- object creation and selection ---------------------------------------------------

// (hdc, const BITMAPINFO*, iUsage, void** ppvBits, hSection, offset)
HLE("gdi32.dll", CreateDIBSection, 6) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Bitmap b;
  if (!ParseBitmapInfo(Arg(c, 1), Arg(c, 2), DcPalette(Arg(c, 0)), &b)) {
    if (Arg(c, 3)) Wr32(Arg(c, 3), 0);
    return 0;
  }
  if (Arg(c, 4)) WarnF("CreateDIBSection: file mapping sections not supported; using memory\n");
  b.bits = HeapAllocProcess(uint64_t(b.pitch) * b.h, true);
  if (!b.bits) return 0;
  uint32_t h = NewHandle();
  g_bitmaps[h] = b;
  if (Arg(c, 3)) Wr32(Arg(c, 3), b.bits);
  RECOMP_TRACE("  gdi: DIB section %08X %dx%d %d bpp%s at %08X\n", h, b.w, b.h, b.bpp,
               b.bottom_up ? " bottom-up" : "", uint32_t(b.bits));
  return h;
}

// (hdc, w, h): the screen's format, 32 bpp.
HLE("gdi32.dll", CreateCompatibleBitmap, 3) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  int w = int32_t(Arg(c, 1)), h = int32_t(Arg(c, 2));
  if (w <= 0 || h <= 0) return 0;
  Bitmap b;
  b.w = w;
  b.h = h;
  b.bpp = 32;
  b.pitch = uint32_t(w) * 4;
  b.bits = HeapAllocProcess(uint64_t(b.pitch) * h, true);
  if (!b.bits) return 0;
  uint32_t hb = NewHandle();
  g_bitmaps[hb] = b;
  return hb;
}

HLE("gdi32.dll", CreateCompatibleDC, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t h = NewHandle();
  Dc d;
  d.bitmap = kStockBitmap;
  d.palette = kStockPalette;
  d.brush = 0x00E00000;  // WHITE_BRUSH
  g_dcs[h] = d;
  return h;
}

HLE("gdi32.dll", DeleteDC, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  g_text.erase(Arg(c, 0));
  return g_dcs.erase(Arg(c, 0)) ? 1 : (WindowForDc(Arg(c, 0)) ? 1 : 0);
}

HLE("gdi32.dll", CreateSolidBrush, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t h = NewHandle();
  g_brushes[h].xrgb = ColorRefToXrgb(Arg(c, 0));
  return h;
}

// (const LOGPALETTE*): palVersion, palNumEntries, PALETTEENTRY {r, g, b, flags}.
HLE("gdi32.dll", CreatePalette, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  gaddr lp = Arg(c, 0);
  uint32_t n = Rd16(lp + 2);
  Palette p;
  p.xrgb.resize(n);
  for (uint32_t i = 0; i < n; i++) {
    gaddr e = lp + 4 + 4 * i;
    p.xrgb[i] = uint32_t(Rd8(e)) << 16 | uint32_t(Rd8(e + 1)) << 8 | Rd8(e + 2);
  }
  uint32_t h = NewHandle();
  g_palettes[h] = std::move(p);
  return h;
}

HLE("gdi32.dll", DeleteObject, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t h = Arg(c, 0);
  auto b = g_bitmaps.find(h);
  if (b != g_bitmaps.end()) {
    for (auto& [_, d] : g_dcs)
      if (d.bitmap == h) return 0;  // still selected: Windows refuses too
    if (b->second.owns_bits && b->second.bits) HeapFreeProcess(b->second.bits);
    g_bitmaps.erase(b);
    return 1;
  }
  g_palettes.erase(h);
  g_brushes.erase(h);
  g_fonts.erase(h);
  return 1;
}

// SelectObject(hdc, h): bitmaps and brushes on memory DCs; the previous
// one is returned. Anything else (fonts, pens, window DCs) is accepted
// without effect and a stock handle returned.
HLE("gdi32.dll", SelectObject, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t hdc = Arg(c, 0), h = Arg(c, 1);
  if (IsFont(h)) {
    TextState& t = g_text[hdc];
    uint32_t prev = t.font ? t.font : kStockSystemFont;
    t.font = h == kStockSystemFont ? 0 : h;
    return prev;
  }
  auto d = g_dcs.find(hdc);
  if (d != g_dcs.end()) {
    if (g_bitmaps.count(h) || h == kStockBitmap) {
      uint32_t prev = d->second.bitmap;
      d->second.bitmap = h;
      return prev;
    }
    if (g_brushes.count(h) || (h & 0xFFFFFF00) == 0x00E00000) {
      if (!g_brushes.count(h) && (h & 0xFF) > 5) return 0x00E00000u;  // a stock non-brush
      uint32_t prev = d->second.brush;
      d->second.brush = h;
      return prev;
    }
  }
  return 0x00E00000u;
}

// (hdc, hpal, bForceBackground): the previous palette.
HLE("gdi32.dll", SelectPalette, 3) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  auto d = g_dcs.find(Arg(c, 0));
  if (d == g_dcs.end()) {
    // Window DCs: remember per DC handle in the same table.
    Dc& w = g_dcs[Arg(c, 0)];
    uint32_t prev = w.palette ? w.palette : kStockPalette;
    w.palette = Arg(c, 1);
    return prev;
  }
  uint32_t prev = d->second.palette;
  d->second.palette = Arg(c, 1);
  return prev;
}

HLE("gdi32.dll", RealizePalette, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Palette* p = DcPalette(Arg(c, 0));
  return p ? uint32_t(p->xrgb.size()) : 0;
}

// (hdc, iStart, cEntries, const RGBQUAD*): the colour table of the DIB
// section selected into hdc.
HLE("gdi32.dll", SetDIBColorTable, 4) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  auto d = g_dcs.find(Arg(c, 0));
  if (d == g_dcs.end()) return 0;
  auto b = g_bitmaps.find(d->second.bitmap);
  if (b == g_bitmaps.end() || b->second.bpp > 8) return 0;
  uint32_t start = Arg(c, 1), n = Arg(c, 2);
  uint32_t max = 1u << b->second.bpp;
  if (start >= max) return 0;
  n = std::min(n, max - start);
  for (uint32_t i = 0; i < n; i++) b->second.pal[start + i] = Rd32(Arg(c, 3) + 4 * i) & 0xFFFFFF;
  return n;
}

HLE("gdi32.dll", GdiFlush, 0) { return 1; }

HLE("gdi32.dll", SetStretchBltMode, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  auto d = g_dcs.find(Arg(c, 0));
  if (d == g_dcs.end()) return 1;
  int prev = d->second.stretch_mode;
  d->second.stretch_mode = int(Arg(c, 1));
  return uint32_t(prev);
}

// (hdc, x, y, POINT* old)
HLE("gdi32.dll", SetBrushOrgEx, 4) {
  if (Arg(c, 3)) {
    Wr32(Arg(c, 3), 0);
    Wr32(Arg(c, 3) + 4, 0);
  }
  return 1;
}

// GetCurrentObject(hdc, type): OBJ_BITMAP (7), OBJ_PAL (5), OBJ_BRUSH (2).
HLE("gdi32.dll", GetCurrentObject, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  auto d = g_dcs.find(Arg(c, 0));
  switch (Arg(c, 1)) {
    case 7: return d != g_dcs.end() ? d->second.bitmap : kStockBitmap;
    case 5: return d != g_dcs.end() ? d->second.palette : kStockPalette;
    case 2: return d != g_dcs.end() ? d->second.brush : 0x00E00000u;
    default: return 0x00E00000u;
  }
}

// GetObjectA(h, size, out): our bitmaps as BITMAP (24 bytes) or, for 84
// bytes or more, DIBSECTION; bitmaps loaded from files as BITMAP.
HLE("gdi32.dll", GetObjectA, 3) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t h = Arg(c, 0), size = Arg(c, 1), out = Arg(c, 2);
  auto it = g_bitmaps.find(h);
  if (it != g_bitmaps.end()) {
    const Bitmap& b = it->second;
    uint32_t n = size >= 84 ? 84 : 24;
    if (!out) return 24;
    if (size < 24) return 0;
    std::memset(GPtr(out), 0, n);
    Wr32(out + 4, uint32_t(b.w));
    Wr32(out + 8, uint32_t(b.h));
    Wr32(out + 12, b.pitch);
    Wr16(out + 16, 1);
    Wr16(out + 18, uint16_t(b.bpp));
    Wr32(out + 20, b.bits);
    if (n == 84) {
      gaddr ih = out + 24;
      Wr32(ih, 40);
      Wr32(ih + 4, uint32_t(b.w));
      Wr32(ih + 8, uint32_t(b.bottom_up ? b.h : -b.h));
      Wr16(ih + 12, 1);
      Wr16(ih + 14, uint16_t(b.bpp));
      Wr32(ih + 16, b.compression);
      Wr32(ih + 20, b.pitch * uint32_t(b.h));
      for (int i = 0; i < 3; i++) Wr32(out + 64 + 4 * i, b.masks[i]);
    }
    return n;
  }
  BitmapDesc d;
  if (!FindBitmap(h, &d)) {
    WarnF("GetObjectA(%08X): not a bitmap\n", h);
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

// ---- drawing --------------------------------------------------------------------------

void WarnRop(uint32_t rop) {
  static std::map<uint32_t, bool> seen;
  if (!seen[rop]) {
    seen[rop] = true;
    WarnF("gdi: raster operation %08X drawn as SRCCOPY\n", rop);
  }
}

// The constant raster operations: their colour, or false for ones that
// read the source.
bool ConstantRop(uint32_t rop, uint32_t hdc, uint32_t* xrgb) {
  switch (rop) {
    case 0x00000042: *xrgb = 0; return true;            // BLACKNESS
    case 0x00FF0062: *xrgb = 0xFFFFFF; return true;     // WHITENESS
    case 0x00F00021: {                                   // PATCOPY
      auto d = g_dcs.find(hdc);
      *xrgb = BrushColor(d != g_dcs.end() ? d->second.brush : 0x00E00000);
      return true;
    }
  }
  return false;
}

// Draws (sx, sy, sw, sh) of `src` onto (dx, dy, dw, dh) of a window's
// target, scaled on the GPU. Negative sizes mirror (on the CPU, rare).
void DrawToWindow(uint32_t hwnd, const Surf& src, int sx, int sy, int sw, int sh, int dx, int dy,
                  int dw, int dh) {
  if (!src) {
    if (src.window) WarnWindowRead("a blit from a window");
    return;
  }
  if (!dw || !dh || !sw || !sh) return;
  WinSurface* win = WinFor(hwnd);
  if (!win) return;
  bool mx = (sw < 0) != (dw < 0), my = (sh < 0) != (dh < 0);
  if (dw < 0) { dx += dw; dw = -dw; }
  if (dh < 0) { dy += dh; dh = -dh; }
  if (sw < 0) { sx += sw; sw = -sw; }
  if (sh < 0) { sy += sh; sh = -sh; }
  // The part of the source inside the surface, and where it lands.
  int x0 = std::max(sx, 0), y0 = std::max(sy, 0);
  int x1 = std::min(sx + sw, src.w), y1 = std::min(sy + sh, src.h);
  if (x1 <= x0 || y1 <= y0) return;
  auto map = [](int v, int s0, int s, int d0, int d) { return d0 + int(int64_t(v - s0) * d / s); };
  int lx = mx ? sx + sw - x1 : x0, rx = mx ? sx + sw - x0 : x1;
  int ty = my ? sy + sh - y1 : y0, by = my ? sy + sh - y0 : y1;
  gfx::Rect dr{map(lx, sx, sw, dx, dw), map(ty, sy, sh, dy, dh), map(rx, sx, sw, dx, dw),
               map(by, sy, sh, dy, dh)};
  int w = x1 - x0, h = y1 - y0;
  gfx::Format fmt = gfx::Format::Unknown;
  if (src.bpp == 32 && !src.masks[0]) fmt = gfx::Format::X8R8G8B8;
  else if (src.bpp == 24) fmt = gfx::Format::R8G8B8;
  else if (src.bpp == 16 && (!src.masks[0] || src.masks[0] == 0x7C00)) fmt = gfx::Format::X1R5G5B5;
  else if (src.bpp == 16 && src.masks[0] == 0xF800 && src.masks[1] == 0x07E0) fmt = gfx::Format::R5G6B5;
  win->dev->SetRenderTarget(win->target, 0);
  if (fmt != gfx::Format::Unknown && !mx && !my && src.stride > 0) {
    // Straight from the DIB's memory.
    win->dev->Blit(src.row0, uint32_t(src.stride), fmt, {x0, y0, x1, y1}, dr, false, 0);
    return;
  }
  static std::vector<uint8_t> tmp;
  if (fmt != gfx::Format::Unknown && !mx) {
    // Bottom-up rows (or mirrored vertically): copy them top-down.
    size_t row = size_t(w) * (src.bpp / 8);
    tmp.resize(row * h);
    for (int y = 0; y < h; y++)
      std::memcpy(&tmp[row * y], src.row0 + (my ? y1 - 1 - y : y0 + y) * src.stride + size_t(x0) * (src.bpp / 8),
                  row);
    win->dev->Blit(tmp.data(), uint32_t(row), fmt, {0, 0, w, h}, dr, false, 0);
    return;
  }
  // Palettes, other masks, mirroring: convert.
  tmp.resize(size_t(w) * h * 4);
  uint32_t* px = reinterpret_cast<uint32_t*>(tmp.data());
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      px[size_t(y) * w + x] = ReadPx(src, mx ? x1 - 1 - x : x0 + x, my ? y1 - 1 - y : y0 + y);
  win->dev->Blit(px, uint32_t(w) * 4, gfx::Format::X8R8G8B8, {0, 0, w, h}, dr, false, 0);
}

uint32_t StretchImpl(uint32_t hdst, int dx, int dy, int dw, int dh, uint32_t hsrc, int sx, int sy,
                     int sw, int sh, uint32_t rop) {
  uint32_t present = 0;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Surf dst = SurfOfDc(hdst);
    if (!dst && !dst.window) return 0;
    uint32_t color;
    if (ConstantRop(rop, hdst, &color)) {
      if (dst.window)
        FillWindow(dst.window, std::min(dx, dx + dw), std::min(dy, dy + dh), std::max(dx, dx + dw),
                   std::max(dy, dy + dh), color);
      else
        FillSurf(dst, dx, dy, dx + dw, dy + dh, color);
    } else if (dst.window) {
      if (rop != 0x00CC0020) WarnRop(rop);
      DrawToWindow(dst.window, SurfOfDc(hsrc), sx, sy, sw, sh, dx, dy, dw, dh);
    } else {
      if (rop != 0x00CC0020) WarnRop(rop);
      Surf src = SurfOfDc(hsrc);
      if (!src) {
        if (src.window) WarnWindowRead("a blit from a window");
        return 0;
      }
      // A negative destination size mirrors, as the source's does.
      if (dw < 0) { dx += dw; dw = -dw; sw = -sw; }
      if (dh < 0) { dy += dh; dh = -dh; sh = -sh; }
      BlitSurf(dst, dx, dy, dw, dh, src, sx, sy, sw, sh);
    }
    present = dst.window;
  }
  if (present) PresentWindow(present);
  return 1;
}

// (hdcDest, x, y, w, h, hdcSrc, x1, y1, rop)
HLE("gdi32.dll", BitBlt, 9) {
  int w = int32_t(Arg(c, 3)), h = int32_t(Arg(c, 4));
  return StretchImpl(Arg(c, 0), int32_t(Arg(c, 1)), int32_t(Arg(c, 2)), w, h, Arg(c, 5),
                     int32_t(Arg(c, 6)), int32_t(Arg(c, 7)), w, h, Arg(c, 8));
}

// (hdcDest, xD, yD, wD, hD, hdcSrc, xS, yS, wS, hS, rop)
HLE("gdi32.dll", StretchBlt, 11) {
  return StretchImpl(Arg(c, 0), int32_t(Arg(c, 1)), int32_t(Arg(c, 2)), int32_t(Arg(c, 3)),
                     int32_t(Arg(c, 4)), Arg(c, 5), int32_t(Arg(c, 6)), int32_t(Arg(c, 7)),
                     int32_t(Arg(c, 8)), int32_t(Arg(c, 9)), Arg(c, 10));
}

// (hdc, xDest, yDest, w, h, xSrc, ySrc, uStartScan, cScanLines, lpvBits,
// lpbmi, ColorUse): DIB coordinates are bottom-up for a bottom-up DIB.
HLE("gdi32.dll", SetDIBitsToDevice, 12) {
  uint32_t present = 0, lines = 0;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Surf dst = SurfOfDc(Arg(c, 0));
    if (!dst && !dst.window) return 0;
    Bitmap b;
    if (!ParseBitmapInfo(Arg(c, 10), Arg(c, 11), DcPalette(Arg(c, 0)), &b)) return 0;
    uint32_t start = Arg(c, 7), count = Arg(c, 8);
    if (start != 0 || count < uint32_t(b.h)) {
      // A band of scan lines: describe just those rows.
      if (count == 0) return 0;
      if (b.bottom_up) {
        b.h = int(std::min<uint32_t>(count, uint32_t(b.h) - std::min<uint32_t>(start, b.h)));
      } else {
        b.h = int(std::min<uint32_t>(count, uint32_t(b.h)));
      }
    }
    b.bits = Arg(c, 9);
    b.owns_bits = false;
    Surf src = SurfOfBitmap(b);
    int w = int32_t(Arg(c, 3)), h = int32_t(Arg(c, 4));
    int sx = int32_t(Arg(c, 5)), sy = int32_t(Arg(c, 6));
    // Bottom-up: ySrc counts from the bottom row, offset by the band start.
    int top = b.bottom_up ? b.h - (sy - int(start)) - h : sy;
    if (dst.window)
      DrawToWindow(dst.window, src, sx, top, w, h, int32_t(Arg(c, 1)), int32_t(Arg(c, 2)), w, h);
    else
      BlitSurf(dst, int32_t(Arg(c, 1)), int32_t(Arg(c, 2)), w, h, src, sx, top, w, h);
    lines = uint32_t(b.h);
    present = dst.window;
  }
  if (present) PresentWindow(present);
  return lines;
}

// FillRect(hdc, const RECT*, hbr)
HLE("user32.dll", FillRect, 3) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Surf s = SurfOfDc(Arg(c, 0));
  gaddr r = Arg(c, 1);
  if (s.window) {
    FillWindow(s.window, int32_t(Rd32(r)), int32_t(Rd32(r + 4)), int32_t(Rd32(r + 8)),
               int32_t(Rd32(r + 12)), BrushColor(Arg(c, 2)));
    return 1;
  }
  if (!s) return 1;
  FillSurf(s, int32_t(Rd32(r)), int32_t(Rd32(r + 4)), int32_t(Rd32(r + 8)), int32_t(Rd32(r + 12)),
           BrushColor(Arg(c, 2)));
  return 1;
}

// ScrollDC(hdc, dx, dy, lprcScroll, lprcClip, hrgnUpdate, lprcUpdate):
// moves the pixels of lprcScroll (or all of the DC) by (dx, dy), within
// lprcClip; the uncovered part keeps its pixels. lprcUpdate gets the
// uncovered strip's bounds.
HLE("user32.dll", ScrollDC, 7) {
  uint32_t present = 0;
  {
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    Surf s = SurfOfDc(Arg(c, 0));
    if (s.window) {
      WarnWindowRead("ScrollDC on a window");
      return 0;
    }
    if (!s) return 0;
    int dx = int32_t(Arg(c, 1)), dy = int32_t(Arg(c, 2));
    auto rect = [](gaddr p, int* l, int* t, int* r, int* b) {
      *l = int32_t(Rd32(p));
      *t = int32_t(Rd32(p + 4));
      *r = int32_t(Rd32(p + 8));
      *b = int32_t(Rd32(p + 12));
    };
    int l = 0, t = 0, r = s.w, b = s.h;
    if (Arg(c, 3)) rect(Arg(c, 3), &l, &t, &r, &b);
    int cl = 0, ct = 0, cr = s.w, cb = s.h;
    if (Arg(c, 4)) rect(Arg(c, 4), &cl, &ct, &cr, &cb);
    // Destination = source moved, clipped to the clip rectangle.
    int dl = std::max(l + dx, cl), dt = std::max(t + dy, ct);
    int dr = std::min(r + dx, cr), db = std::min(b + dy, cb);
    if (dr > dl && db > dt)
      BlitSurf(s, dl, dt, dr - dl, db - dt, s, dl - dx, dt - dy, dr - dl, db - dt);
    if (gaddr up = Arg(c, 6)) {
      // The uncovered strip (one axis, the usual case).
      int ul = l, ut = t, ur = r, ub = b;
      if (dx > 0) ur = std::min(r, l + dx);
      else if (dx < 0) ul = std::max(l, r + dx);
      else if (dy > 0) ub = std::min(b, t + dy);
      else if (dy < 0) ut = std::max(t, b + dy);
      else ur = ul;
      Wr32(up, uint32_t(ul));
      Wr32(up + 4, uint32_t(ut));
      Wr32(up + 8, uint32_t(ur));
      Wr32(up + 12, uint32_t(ub));
    }
    present = s.window;
  }
  if (present) PresentWindow(present);
  return 1;
}


// ---- text ---------------------------------------------------------------------------
// Fonts are the host's (font_raster.h); text is drawn into memory DCs'
// pixels (DIB sections, DirectDraw surfaces' GetDC). Text on a window DC
// is measured but not drawn.

FontDesc ReadLogFont(gaddr lf) {
  FontDesc f;
  f.height = int32_t(Rd32(lf));
  f.weight = int32_t(Rd32(lf + 16));
  f.italic = Rd8(lf + 20) != 0;
  char face[33] = {};
  for (int i = 0; i < 32; i++) face[i] = char(Rd8(lf + 28 + i));
  f.face = face;
  return f;
}

HLE("gdi32.dll", CreateFontIndirectA, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t h = NewHandle();
  g_fonts[h] = ReadLogFont(Arg(c, 0));
  return h;
}

// (height, width, escapement, orientation, weight, italic, underline,
// strikeout, charset, out precision, clip precision, quality, pitch and
// family, face)
HLE("gdi32.dll", CreateFontA, 14) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  FontDesc f;
  f.height = int32_t(Arg(c, 0));
  f.weight = int32_t(Arg(c, 4));
  f.italic = Arg(c, 5) != 0;
  if (gaddr face = Arg(c, 13))
    for (int i = 0; i < 32 && Rd8(face + i); i++) f.face += char(Rd8(face + i));
  uint32_t h = NewHandle();
  g_fonts[h] = f;
  return h;
}

HLE("gdi32.dll", SetTextColor, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t& v = g_text[Arg(c, 0)].color;
  uint32_t prev = v;
  v = Arg(c, 1);
  return prev;
}
HLE("gdi32.dll", GetTextColor, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return g_text[Arg(c, 0)].color;
}
HLE("gdi32.dll", SetBkColor, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t& v = g_text[Arg(c, 0)].bk_color;
  uint32_t prev = v;
  v = Arg(c, 1);
  return prev;
}
HLE("gdi32.dll", GetBkColor, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return g_text[Arg(c, 0)].bk_color;
}
HLE("gdi32.dll", SetBkMode, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  int& v = g_text[Arg(c, 0)].bk_mode;
  int prev = v;
  v = int(Arg(c, 1));
  return uint32_t(prev);
}
HLE("gdi32.dll", GetBkMode, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return uint32_t(g_text[Arg(c, 0)].bk_mode);
}
HLE("gdi32.dll", SetTextAlign, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t& v = g_text[Arg(c, 0)].align;
  uint32_t prev = v;
  v = Arg(c, 1);
  return prev;
}
HLE("gdi32.dll", GetTextAlign, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return g_text[Arg(c, 0)].align;
}

std::string GuestText(gaddr s, int n) {
  std::string t;
  if (!s || n <= 0) return t;
  t.resize(size_t(n));
  for (int i = 0; i < n; i++) t[size_t(i)] = char(Rd8(s + uint32_t(i)));
  return t;
}

HLE("gdi32.dll", GetTextMetricsA, 2) {
  FontDesc f;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    f = FontOfDc(Arg(c, 0));
  }
  const FontMetrics m = MeasureFont(f);
  uint32_t tm = Arg(c, 1);
  std::memset(GPtr(tm), 0, 56);
  Wr32(tm + 0, uint32_t(m.ascent + m.descent));  // tmHeight
  Wr32(tm + 4, uint32_t(m.ascent));
  Wr32(tm + 8, uint32_t(m.descent));
  Wr32(tm + 20, uint32_t(m.ave_width));
  Wr32(tm + 24, uint32_t(m.max_width));
  Wr32(tm + 28, uint32_t(f.weight ? f.weight : 400));
  Wr32(tm + 36, 96);   // tmDigitizedAspectX
  Wr32(tm + 40, 96);   // tmDigitizedAspectY
  Wr8(tm + 44, 0x20);  // tmFirstChar
  Wr8(tm + 45, 0xFF);  // tmLastChar
  Wr8(tm + 46, '?');   // tmDefaultChar
  Wr8(tm + 47, ' ');   // tmBreakChar
  Wr8(tm + 48, f.italic ? 1 : 0);
  Wr8(tm + 51, 0x26);  // tmPitchAndFamily: variable pitch, TrueType, swiss
  return 1;
}

HLE("gdi32.dll", GetCharWidthA, 4) {
  FontDesc f;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    f = FontOfDc(Arg(c, 0));
  }
  uint32_t first = Arg(c, 1), last = Arg(c, 2), buf = Arg(c, 3);
  for (uint32_t ch = first; ch <= last && ch <= 0xFF; ch++) {
    char b = char(ch);
    Wr32(buf + 4 * (ch - first), uint32_t(TextWidth(f, &b, 1)));
  }
  return 1;
}

uint32_t TextExtent(Ctx& c) {
  FontDesc f;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    f = FontOfDc(Arg(c, 0));
  }
  const std::string t = GuestText(Arg(c, 1), int(Arg(c, 2)));
  const FontMetrics m = MeasureFont(f);
  Wr32(Arg(c, 3), uint32_t(TextWidth(f, t.data(), int(t.size()))));
  Wr32(Arg(c, 3) + 4, uint32_t(m.ascent + m.descent));
  return 1;
}
HLE("gdi32.dll", GetTextExtentPoint32A, 4) { return TextExtent(c); }
HLE("gdi32.dll", GetTextExtentPointA, 4) { return TextExtent(c); }

struct IRect {
  int l, t, r, b;
};

// Draws `t` at (x, y) per the DC's alignment, colours and background mode;
// `opaque` (ETO_OPAQUE) is filled with the background colour first, `clip`
// (ETO_CLIPPED) limits the text.
void DrawTextAt(uint32_t hdc, int x, int y, const std::string& t, const IRect* clip,
                const IRect* opaque) {
  FontDesc f;
  TextState st;
  Surf s;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    f = FontOfDc(hdc);
    auto it = g_text.find(hdc);
    if (it != g_text.end()) st = it->second;
    s = SurfOfDc(hdc);
  }
  if (!s) {
    static bool warned = false;
    if (!warned) WarnF("gdi32: text on a window DC is not drawn\n");
    warned = true;
    return;
  }
  const uint32_t fg = ColorRefToXrgb(st.color & 0xFFFFFF), bg = ColorRefToXrgb(st.bk_color & 0xFFFFFF);
  if (opaque) FillSurf(s, opaque->l, opaque->t, opaque->r, opaque->b, bg);
  if (t.empty()) return;
  std::vector<uint8_t> mask;
  int w = 0, h = 0;
  RasterizeText(f, t.data(), int(t.size()), &mask, &w, &h);
  const FontMetrics m = MeasureFont(f);
  switch (st.align & 6) {  // TA_CENTER 6, TA_RIGHT 2
    case 6: x -= w / 2; break;
    case 2: x -= w; break;
  }
  if ((st.align & 24) == 24) y -= m.ascent;  // TA_BASELINE
  else if (st.align & 8) y -= h;             // TA_BOTTOM
  IRect lim{0, 0, s.w, s.h};
  if (clip) lim = {std::max(lim.l, clip->l), std::max(lim.t, clip->t), std::min(lim.r, clip->r),
                   std::min(lim.b, clip->b)};
  if (st.bk_mode == 2)
    FillSurf(s, std::max(x, lim.l), std::max(y, lim.t), std::min(x + w, lim.r),
             std::min(y + h, lim.b), bg);
  for (int j = 0; j < h; j++) {
    const int py = y + j;
    if (py < lim.t || py >= lim.b) continue;
    for (int i = 0; i < w; i++) {
      const int px = x + i;
      if (px < lim.l || px >= lim.r || !mask[size_t(j) * w + i]) continue;
      WritePx(s, px, py, fg);
    }
  }
}

// (hdc, x, y, string, count)
HLE("gdi32.dll", TextOutA, 5) {
  DrawTextAt(Arg(c, 0), int32_t(Arg(c, 1)), int32_t(Arg(c, 2)),
             GuestText(Arg(c, 3), int32_t(Arg(c, 4))), nullptr, nullptr);
  return 1;
}

// (hdc, x, y, options, rect, string, count, dx): ETO_OPAQUE (2) and
// ETO_CLIPPED (4) with the rectangle; per-character spacing is ignored.
HLE("gdi32.dll", ExtTextOutA, 8) {
  uint32_t opts = Arg(c, 3);
  IRect r{};
  bool have = Arg(c, 4) != 0;
  if (have)
    r = {int32_t(Rd32(Arg(c, 4))), int32_t(Rd32(Arg(c, 4) + 4)), int32_t(Rd32(Arg(c, 4) + 8)),
         int32_t(Rd32(Arg(c, 4) + 12))};
  DrawTextAt(Arg(c, 0), int32_t(Arg(c, 1)), int32_t(Arg(c, 2)),
             GuestText(Arg(c, 5), int32_t(Arg(c, 6))), have && (opts & 4) ? &r : nullptr,
             have && (opts & 2) ? &r : nullptr);
  return 1;
}

}  // namespace

uint32_t GdiDcForMemory(uint32_t bits, int w, int h, int bpp, uint32_t pitch, uint32_t rmask,
                        uint32_t gmask, uint32_t bmask) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Bitmap b;
  b.w = w;
  b.h = h;
  b.bpp = bpp;
  b.pitch = pitch;
  b.bits = bits;
  b.owns_bits = false;
  if (bpp == 16) {
    b.masks[0] = rmask;
    b.masks[1] = gmask;
    b.masks[2] = bmask;
    b.compression = 3;  // BI_BITFIELDS
  }
  uint32_t hb = NewHandle();
  g_bitmaps[hb] = b;
  uint32_t hdc = NewHandle();
  Dc d;
  d.bitmap = hb;
  d.palette = kStockPalette;
  d.brush = 0x00E00000;
  g_dcs[hdc] = d;
  return hdc;
}

void GdiReleaseMemoryDc(uint32_t hdc) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  auto d = g_dcs.find(hdc);
  if (d == g_dcs.end()) return;
  g_bitmaps.erase(d->second.bitmap);
  g_dcs.erase(d);
  g_text.erase(hdc);
}

}  // namespace recomp::hle
