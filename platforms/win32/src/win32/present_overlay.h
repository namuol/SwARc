#pragma once
// Things other HLE modules draw over a window's presented frame: what on
// Windows is a separate child window over the game's (DirectShow's video
// window). The graphics API's Present calls DrawPresentOverlays with the
// back buffer bound as the target, before the frame goes out.

#include <cstdint>

namespace recomp::gfx {
class Device;
}

namespace recomp {

struct OverlayTarget {
  gfx::Device* dev;  // the back buffer is its render target
  uint32_t hwnd;     // the window being presented
  int bb_w, bb_h;    // the back buffer's size in pixels
};

using PresentOverlay = void (*)(const OverlayTarget& t);

// Registers an overlay (once, at load time or first use).
void AddPresentOverlay(PresentOverlay fn);
// Draws every registered overlay onto t's back buffer.
void DrawPresentOverlays(const OverlayTarget& t);

}  // namespace recomp
