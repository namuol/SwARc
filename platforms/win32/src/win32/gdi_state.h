// What GDI knows about the objects user32 and gdi32 hand out. Handles are
// opaque and nothing is drawn through GDI (see gdi32.cpp), but a bitmap
// loaded from a file keeps its dimensions, so GetObject can describe it.
#pragma once

#include <cstdint>
#include <string>

struct SDL_Window;

namespace recomp::hle {

struct BitmapDesc {
  int32_t width = 0, height = 0;  // height > 0 (GetObject reports it so)
  uint16_t bits_per_pixel = 0;
};

// Reads the BITMAPINFOHEADER (or OS/2 BITMAPCOREHEADER) of a .bmp file;
// false if it is not one.
bool ReadBmpHeader(const std::string& host_path, BitmapDesc* out);

void RememberBitmap(uint32_t handle, const BitmapDesc& d);

// A memory DC drawing into existing guest pixels (a DirectDraw surface's
// GetDC): top-down rows `pitch` bytes apart, 16 bpp with the given masks
// or 32 bpp XRGB. GdiReleaseMemoryDc deletes it (the pixels stay).
uint32_t GdiDcForMemory(uint32_t bits, int w, int h, int bpp, uint32_t pitch, uint32_t rmask,
                        uint32_t gmask, uint32_t bmask);
void GdiReleaseMemoryDc(uint32_t hdc);

// GetSysColor's colours (COLORREF), by COLOR_* index; 0 past the table.
uint32_t SysColor(uint32_t index);

// A graphics API (Direct3D, DirectDraw, OpenGL) is about to create its
// device for this host window: GDI drawing on the window (which keeps a
// GPU device of its own once the game draws on a window DC) lets go of it
// and stops drawing there, as only one device can present to a window.
void GdiYieldWindow(SDL_Window* w);
bool FindBitmap(uint32_t handle, BitmapDesc* out);

}  // namespace recomp::hle
