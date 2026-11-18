#include "win32/present_overlay.h"

#include <mutex>
#include <vector>

namespace recomp {
namespace {
std::mutex g_mu;
std::vector<PresentOverlay> g_overlays;
}  // namespace

void AddPresentOverlay(PresentOverlay fn) {
  std::lock_guard<std::mutex> l(g_mu);
  for (PresentOverlay f : g_overlays)
    if (f == fn) return;
  g_overlays.push_back(fn);
}

void DrawPresentOverlays(const OverlayTarget& t) {
  std::vector<PresentOverlay> fns;
  {
    std::lock_guard<std::mutex> l(g_mu);
    fns = g_overlays;
  }
  for (PresentOverlay f : fns) f(t);
}

}  // namespace recomp
