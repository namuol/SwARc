// Host fonts for GDI text (gdi_draw.cpp): measuring and rasterising a
// LOGFONT's text with the host's font engine (Core Text on macOS).
//
// Text is rasterised without antialiasing, as Windows 9x drew it (no
// ClearType): games draw text onto 8 and 16 bpp surfaces and colour-keyed
// sprites, where blended edges would show as fringes. Strings are
// Windows-1252.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace recomp::hle {

// What a LOGFONT asks for. `height` is lfHeight: < 0 the character (em)
// height in pixels, > 0 the cell height, 0 a default size.
struct FontDesc {
  std::string face;
  int height = 0;
  int weight = 400;
  bool italic = false;
};

struct FontMetrics {
  int ascent = 13, descent = 3;  // tmAscent, tmDescent (tmHeight = their sum)
  int ave_width = 7, max_width = 14;
};

FontMetrics MeasureFont(const FontDesc& f);

// Makes the font file at host path `path` usable by face name for the
// process (AddFontResource); false if it is not a font.
bool AddPrivateFont(const std::string& path);
bool RemovePrivateFont(const std::string& path);

// Advance width of `n` bytes of text, in pixels.
int TextWidth(const FontDesc& f, const char* s, int n);

// Rasterises `n` bytes of text into `mask` (row-major, top row first,
// *w by *h = ascent + descent; 0 or 255 per pixel) with the cell's top-left
// at (0, 0).
void RasterizeText(const FontDesc& f, const char* s, int n, std::vector<uint8_t>* mask, int* w,
                   int* h);

}  // namespace recomp::hle
