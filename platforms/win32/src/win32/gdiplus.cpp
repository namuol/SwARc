// gdiplus: the flat API's bitmaps, for games that load images with GDI+
// and read their pixels (LockBits), and save screenshots with it.
//
// Images decode with SDL (PNG and BMP; other formats fail with
// UnknownImageFormat) and are kept on the host as 32-bit ARGB, straight
// alpha, the GDI+ memory layout (B, G, R, A bytes). LockBits hands out a
// guest copy in the requested format (32 bpp ARGB/PARGB/RGB, 24 bpp RGB);
// UnlockBits writes a write-mode lock back. A bitmap made from caller
// memory (GdipCreateBitmapFromScan0 with Scan0) reads that memory each
// time it is used, as GDI+ keeps using it. Saving writes PNG for the PNG
// encoder and BMP otherwise.
//
// GpImage* values are small guest allocations, so they are real pointers
// should code compare or test them.

#include <SDL3/SDL.h>

#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "runtime/log.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"

namespace recomp::hle {
namespace {

constexpr const char* kDll = "gdiplus.dll";

enum Status : uint32_t {
  Ok = 0,
  GenericError = 1,
  InvalidParameter = 2,
  OutOfMemory = 3,
  NotImplemented = 6,
  Win32Error = 7,
  FileNotFound = 10,
  UnknownImageFormat = 13,
};

constexpr uint32_t kFmt32bppRGB = 0x00022009;
constexpr uint32_t kFmt32bppARGB = 0x0026200A;
constexpr uint32_t kFmt32bppPARGB = 0x000E200B;
constexpr uint32_t kFmt24bppRGB = 0x00021808;

struct Image {
  int w = 0, h = 0;
  std::vector<uint32_t> argb;  // straight alpha
  // Caller memory (FromScan0 with Scan0): the pixels live there.
  gaddr scan0 = 0;
  int32_t stride = 0;
  uint32_t format = kFmt32bppARGB;
};

struct Lock {
  gaddr buf = 0;
  bool ours = false;
  int x = 0, y = 0, w = 0, h = 0;
  uint32_t format = 0, flags = 0;
  int32_t stride = 0;
};

std::recursive_mutex g_mu;
std::map<uint32_t, Image> g_images;
std::map<uint32_t, Lock> g_locks;  // by image

int BytesPerPixel(uint32_t f) { return f == kFmt24bppRGB ? 3 : 4; }
bool Supported(uint32_t f) {
  return f == kFmt32bppRGB || f == kFmt32bppARGB || f == kFmt32bppPARGB || f == kFmt24bppRGB;
}

// Pixel `p` in format f (guest bytes at q) to straight ARGB and back.
uint32_t Load(const uint8_t* q, uint32_t f) {
  switch (f) {
    case kFmt24bppRGB: return 0xFF000000u | uint32_t(q[2]) << 16 | uint32_t(q[1]) << 8 | q[0];
    case kFmt32bppRGB: return 0xFF000000u | (uint32_t(q[2]) << 16 | uint32_t(q[1]) << 8 | q[0]);
    case kFmt32bppPARGB: {
      uint32_t a = q[3];
      if (!a) return 0;
      auto un = [&](uint32_t v) { return std::min<uint32_t>(255, (v * 255 + a / 2) / a); };
      return a << 24 | un(q[2]) << 16 | un(q[1]) << 8 | un(q[0]);
    }
    default: return uint32_t(q[3]) << 24 | uint32_t(q[2]) << 16 | uint32_t(q[1]) << 8 | q[0];
  }
}
void Store(uint8_t* q, uint32_t f, uint32_t argb) {
  uint32_t a = argb >> 24, r = argb >> 16 & 255, g = argb >> 8 & 255, b = argb & 255;
  switch (f) {
    case kFmt24bppRGB: q[0] = uint8_t(b); q[1] = uint8_t(g); q[2] = uint8_t(r); return;
    case kFmt32bppRGB: q[0] = uint8_t(b); q[1] = uint8_t(g); q[2] = uint8_t(r); q[3] = 0xFF; return;
    case kFmt32bppPARGB:
      q[0] = uint8_t((b * a + 127) / 255);
      q[1] = uint8_t((g * a + 127) / 255);
      q[2] = uint8_t((r * a + 127) / 255);
      q[3] = uint8_t(a);
      return;
    default: q[0] = uint8_t(b); q[1] = uint8_t(g); q[2] = uint8_t(r); q[3] = uint8_t(a); return;
  }
}

// The pixels as they are now (caller memory re-read).
std::vector<uint32_t>& Pixels(Image& im) {
  if (im.scan0) {
    im.argb.resize(size_t(im.w) * im.h);
    int bpp = BytesPerPixel(im.format);
    for (int y = 0; y < im.h; y++) {
      const uint8_t* row = GPtr(gaddr(int64_t(im.scan0) + int64_t(y) * im.stride));
      for (int x = 0; x < im.w; x++) im.argb[size_t(y) * im.w + x] = Load(row + x * bpp, im.format);
    }
  }
  return im.argb;
}

uint32_t NewImage(Image im) {
  gaddr h = HeapAllocProcess(16, true);
  if (!h) return 0;
  g_images[uint32_t(h)] = std::move(im);
  return uint32_t(h);
}

Image* Find(uint32_t h) {
  auto it = g_images.find(h);
  return it == g_images.end() ? nullptr : &it->second;
}

std::string GuestWidePath(gaddr p) { return NarrowPath(GWStr(p)); }

HLE(kDll, GdiplusStartup, 3) {
  if (Arg(c, 0)) Wr32(Arg(c, 0), 1);
  if (Arg(c, 2)) {
    // GdiplusStartupOutput: no background thread hooks (NotificationHook,
    // NotificationUnhook), as when the input asked for none.
    Wr32(Arg(c, 2), 0);
    Wr32(Arg(c, 2) + 4, 0);
  }
  return Ok;
}
HLE(kDll, GdiplusShutdown, 1) { return 0; }

HLE(kDll, GdipAlloc, 1) { return HeapAllocProcess(Arg(c, 0), false); }
HLE(kDll, GdipFree, 1) {
  if (Arg(c, 0)) HeapFreeProcess(Arg(c, 0));
  return 0;
}

uint32_t FromFile(Ctx& c) {
  gaddr out = Arg(c, 1);
  if (!Arg(c, 0) || !out) return InvalidParameter;
  Wr32(out, 0);
  std::string guest = GuestWidePath(Arg(c, 0));
  std::string host = HostPath(guest);
  SDL_Surface* s = nullptr;
  std::FILE* f = std::fopen(host.c_str(), "rb");
  if (!f) return Win32Error;  // GDI+ reports a missing file as Win32Error
  unsigned char magic[8] = {};
  size_t n = std::fread(magic, 1, sizeof magic, f);
  std::fclose(f);
  if (n >= 8 && !std::memcmp(magic, "\x89PNG\r\n\x1a\n", 8))
    s = SDL_LoadPNG(host.c_str());
  else if (n >= 2 && magic[0] == 'B' && magic[1] == 'M')
    s = SDL_LoadBMP(host.c_str());
  else {
    WarnF("GdipCreateBitmapFromFile(%s): format not supported\n", guest.c_str());
    return UnknownImageFormat;
  }
  if (!s) {
    WarnF("GdipCreateBitmapFromFile(%s): %s\n", guest.c_str(), SDL_GetError());
    return UnknownImageFormat;
  }
  SDL_Surface* a = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ARGB8888);
  SDL_DestroySurface(s);
  if (!a) return OutOfMemory;
  Image im;
  im.w = a->w;
  im.h = a->h;
  im.argb.resize(size_t(im.w) * im.h);
  for (int y = 0; y < im.h; y++)
    std::memcpy(&im.argb[size_t(y) * im.w], static_cast<uint8_t*>(a->pixels) + size_t(y) * a->pitch,
                size_t(im.w) * 4);
  SDL_DestroySurface(a);
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t h = NewImage(std::move(im));
  if (!h) return OutOfMemory;
  Wr32(out, h);
  RECOMP_TRACE("  gdiplus: %s -> %08X\n", guest.c_str(), h);
  return Ok;
}

// (const WCHAR* filename, GpBitmap** bitmap)
HLE(kDll, GdipCreateBitmapFromFile, 2) { return FromFile(c); }
HLE(kDll, GdipCreateBitmapFromFileICM, 2) { return FromFile(c); }

// (INT width, INT height, INT stride, PixelFormat, BYTE* scan0, GpBitmap**)
HLE(kDll, GdipCreateBitmapFromScan0, 6) {
  int w = int32_t(Arg(c, 0)), h = int32_t(Arg(c, 1));
  uint32_t fmt = Arg(c, 3);
  gaddr out = Arg(c, 5);
  if (!out || w <= 0 || h <= 0) return InvalidParameter;
  Wr32(out, 0);
  if (!Supported(fmt)) {
    WarnF("GdipCreateBitmapFromScan0: pixel format %08X not supported\n", fmt);
    return NotImplemented;
  }
  Image im;
  im.w = w;
  im.h = h;
  im.format = fmt;
  if (Arg(c, 4)) {
    im.scan0 = Arg(c, 4);
    im.stride = int32_t(Arg(c, 2));
  } else {
    im.argb.assign(size_t(w) * h, fmt == kFmt32bppARGB || fmt == kFmt32bppPARGB ? 0 : 0xFF000000u);
  }
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t hi = NewImage(std::move(im));
  if (!hi) return OutOfMemory;
  Wr32(out, hi);
  return Ok;
}

// (GpImage*, GpImage** clone): an independent copy with its own pixels.
HLE(kDll, GdipCloneImage, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Image* im = Find(Arg(c, 0));
  if (!im || !Arg(c, 1)) return InvalidParameter;
  Image copy;
  copy.w = im->w;
  copy.h = im->h;
  copy.argb = Pixels(*im);
  uint32_t h = NewImage(std::move(copy));
  if (!h) return OutOfMemory;
  Wr32(Arg(c, 1), h);
  return Ok;
}

HLE(kDll, GdipDisposeImage, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t h = Arg(c, 0);
  if (!g_images.erase(h)) return InvalidParameter;
  auto lk = g_locks.find(h);
  if (lk != g_locks.end()) {
    if (lk->second.ours) HeapFreeProcess(lk->second.buf);
    g_locks.erase(lk);
  }
  HeapFreeProcess(h);
  return Ok;
}

HLE(kDll, GdipGetImageWidth, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Image* im = Find(Arg(c, 0));
  if (!im || !Arg(c, 1)) return InvalidParameter;
  Wr32(Arg(c, 1), uint32_t(im->w));
  return Ok;
}
HLE(kDll, GdipGetImageHeight, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Image* im = Find(Arg(c, 0));
  if (!im || !Arg(c, 1)) return InvalidParameter;
  Wr32(Arg(c, 1), uint32_t(im->h));
  return Ok;
}

// (GpBitmap*, const GpRect* (x, y, w, h; NULL = all), UINT flags
// (ImageLockModeRead 1, Write 2, UserInputBuf 4), PixelFormat,
// BitmapData* {Width, Height, Stride, PixelFormat, Scan0, Reserved}).
HLE(kDll, GdipBitmapLockBits, 5) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t h = Arg(c, 0), flags = Arg(c, 2), fmt = Arg(c, 3);
  gaddr rect = Arg(c, 1), bd = Arg(c, 4);
  Image* im = Find(h);
  if (!im || !bd) return InvalidParameter;
  if (g_locks.count(h)) return 4;  // ObjectBusy
  if (!Supported(fmt)) {
    WarnF("GdipBitmapLockBits: pixel format %08X not supported\n", fmt);
    return NotImplemented;
  }
  Lock lk;
  lk.flags = flags;
  lk.format = fmt;
  lk.w = im->w;
  lk.h = im->h;
  if (rect) {
    lk.x = int32_t(Rd32(rect));
    lk.y = int32_t(Rd32(rect + 4));
    lk.w = int32_t(Rd32(rect + 8));
    lk.h = int32_t(Rd32(rect + 12));
  }
  if (lk.x < 0 || lk.y < 0 || lk.w <= 0 || lk.h <= 0 || lk.x + lk.w > im->w || lk.y + lk.h > im->h)
    return InvalidParameter;
  int bpp = BytesPerPixel(fmt);
  if (flags & 4) {
    lk.buf = Rd32(bd + 16);
    lk.stride = int32_t(Rd32(bd + 8));
  } else {
    lk.stride = int32_t((uint32_t(lk.w) * bpp + 3) & ~3u);
    lk.buf = HeapAllocProcess(uint64_t(lk.stride) * lk.h, false);
    if (!lk.buf) return OutOfMemory;
    lk.ours = true;
  }
  const std::vector<uint32_t>& px = Pixels(*im);
  if (flags & 1) {
    for (int y = 0; y < lk.h; y++) {
      uint8_t* row = GPtr(gaddr(int64_t(lk.buf) + int64_t(y) * lk.stride));
      for (int x = 0; x < lk.w; x++)
        Store(row + x * bpp, fmt, px[size_t(lk.y + y) * im->w + lk.x + x]);
    }
  }
  Wr32(bd, uint32_t(lk.w));
  Wr32(bd + 4, uint32_t(lk.h));
  Wr32(bd + 8, uint32_t(lk.stride));
  Wr32(bd + 12, fmt);
  Wr32(bd + 16, lk.buf);
  Wr32(bd + 20, 0);
  g_locks[h] = lk;
  return Ok;
}

HLE(kDll, GdipBitmapUnlockBits, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t h = Arg(c, 0);
  Image* im = Find(h);
  auto it = g_locks.find(h);
  if (!im || it == g_locks.end()) return 8;  // WrongState
  Lock lk = it->second;
  g_locks.erase(it);
  if (lk.flags & 2) {
    std::vector<uint32_t>& px = Pixels(*im);
    int bpp = BytesPerPixel(lk.format);
    for (int y = 0; y < lk.h; y++) {
      const uint8_t* row = GPtr(gaddr(int64_t(lk.buf) + int64_t(y) * lk.stride));
      for (int x = 0; x < lk.w; x++)
        px[size_t(lk.y + y) * im->w + lk.x + x] = Load(row + x * bpp, lk.format);
    }
    if (im->scan0) {  // write through to the caller's memory
      int ib = BytesPerPixel(im->format);
      for (int y = 0; y < im->h; y++) {
        uint8_t* row = GPtr(gaddr(int64_t(im->scan0) + int64_t(y) * im->stride));
        for (int x = 0; x < im->w; x++) Store(row + x * ib, im->format, px[size_t(y) * im->w + x]);
      }
    }
  }
  if (lk.ours) HeapFreeProcess(lk.buf);
  return Ok;
}

// (GpImage*, const WCHAR* filename, const CLSID* encoder, params): the PNG
// encoder ({557CF406-1A04-11D3-...}) writes PNG, the others BMP.
HLE(kDll, GdipSaveImageToFile, 4) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Image* im = Find(Arg(c, 0));
  if (!im || !Arg(c, 1)) return InvalidParameter;
  std::string guest = GuestWidePath(Arg(c, 1));
  std::string host = HostPath(guest, PathAccess::kWrite);
  std::vector<uint32_t>& px = Pixels(*im);
  SDL_Surface* s = SDL_CreateSurfaceFrom(im->w, im->h, SDL_PIXELFORMAT_ARGB8888, px.data(), im->w * 4);
  if (!s) return OutOfMemory;
  bool png = Arg(c, 2) && Rd32(Arg(c, 2)) == 0x557CF406;
  bool ok = png ? SDL_SavePNG(s, host.c_str()) : SDL_SaveBMP(s, host.c_str());
  SDL_DestroySurface(s);
  if (!ok) {
    WarnF("GdipSaveImageToFile(%s): %s\n", guest.c_str(), SDL_GetError());
    return Win32Error;
  }
  return Ok;
}

}  // namespace
}  // namespace recomp::hle
