// runtime/gfx/present.h.

#include "runtime/gfx/present.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "runtime/env.h"
#include "runtime/log.h"

namespace recomp::gfx {

// Dev aid: <ID>_SHOT_EVERY=N saves every Nth presented frame (from frame
// <ID>_SHOT_FROM on) as $<ID>_SHOT_DIR/frame_NNNNN.bmp (default dir: current
// directory).
namespace {

int frame = 0;  // frames presented (back or front buffer)

std::mutex g_shot_mu;
std::string g_shot_name;  // RequestShot: saved with the next frame
// RequestProbe: points to read from the next frame, and what was read.
std::vector<std::pair<int, int>> g_probe_points;
bool g_probe_due = false, g_probe_ready = false;
std::vector<uint32_t> g_probe_pixels;

using ReadFn = std::function<void(const Rect&, uint8_t*, uint32_t)>;

void SaveBmp(int w, int h, const std::vector<uint8_t>& px, const char* name) {
  SDL_Surface* surf =
      SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_XRGB8888, const_cast<uint8_t*>(px.data()), w * 4);
  const char* dir = Env("SHOT_DIR");
  std::string path = std::string(dir ? dir : ".") + "/" + name + ".bmp";
  if (surf) {
    SDL_SaveBMP(surf, path.c_str());
    SDL_DestroySurface(surf);
  }
}

// <ID>_SHOT_EVERY / _SHOT_FROM / _SHOT_DIR: counts presented frames (back
// or front buffer) and saves the due ones, read by `read`.
// The test aids that need a frame's pixels (TEST_INPUT's named shots and
// probes), counting the frame; `read_rect` is called only for those due.
void ShotsAndProbes(int w, int h, const ReadFn& read_rect) {
  frame++;
  std::string named;
  std::vector<std::pair<int, int>> probe;
  {
    std::lock_guard<std::mutex> l(g_shot_mu);
    named.swap(g_shot_name);
    if (g_probe_due) probe = g_probe_points;
  }
  if (!probe.empty()) {
    std::vector<uint32_t> got;
    for (auto [x, y] : probe) {
      uint32_t v = 0xFF000000u;
      if (x >= 0 && y >= 0 && x < w && y < h) {
        uint8_t px[4] = {};
        read_rect({x, y, x + 1, y + 1}, px, 4);
        v = uint32_t(px[2]) << 16 | uint32_t(px[1]) << 8 | px[0];
      }
      got.push_back(v);
    }
    std::lock_guard<std::mutex> l(g_shot_mu);
    g_probe_pixels = std::move(got);
    g_probe_due = false;
    g_probe_ready = true;
  }
  if (!named.empty()) {
    std::vector<uint8_t> px(size_t(w) * h * 4);
    read_rect({0, 0, w, h}, px.data(), uint32_t(w * 4));
    SaveBmp(w, h, px, named.c_str());
  }
}

void MaybeShoot(int w, int h, const ReadFn& read_rect) {
  auto read = [&](uint8_t* px, uint32_t pitch) { read_rect({0, 0, w, h}, px, pitch); };
  static const int every = [] {
    const char* e = Env("SHOT_EVERY");
    return e ? std::atoi(e) : 0;
  }();
  static const int from = [] {
    const char* e = Env("SHOT_FROM");
    return e ? std::atoi(e) : 0;
  }();
  ShotsAndProbes(w, h, read_rect);
  if (every > 0 && frame >= from && frame % every == 0) {
    std::vector<uint8_t> px(size_t(w) * h * 4);
    read(px.data(), uint32_t(w * 4));
    SDL_Surface* surf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_XRGB8888,
                                              px.data(), w * 4);
    const char* dir = Env("SHOT_DIR");
    char path[512];
    std::snprintf(path, sizeof path, "%s/frame_%05d.bmp", dir ? dir : ".", frame);
    if (surf) {
      SDL_SaveBMP(surf, path);
      SDL_DestroySurface(surf);
    }
  }
}

}  // namespace

int FramesPresented() { return frame; }

void TestAidsForFrame(int w, int h, const std::function<void(const Rect&, uint8_t*, uint32_t)>& read) {
  ShotsAndProbes(w, h, read);
}

void RequestShot(const char* name) {
  std::lock_guard<std::mutex> l(g_shot_mu);
  g_shot_name = name;
}

bool ShotPending() {
  std::lock_guard<std::mutex> l(g_shot_mu);
  return !g_shot_name.empty() || g_probe_due;
}

void RequestProbe(const std::vector<std::pair<int, int>>& points) {
  std::lock_guard<std::mutex> l(g_shot_mu);
  g_probe_points = points;
  g_probe_due = !points.empty();
  g_probe_ready = false;
}

bool TakeProbe(std::vector<uint32_t>* pixels) {
  std::lock_guard<std::mutex> l(g_shot_mu);
  if (!g_probe_ready) return false;
  g_probe_ready = false;
  *pixels = g_probe_pixels;
  return true;
}

// Games that draw on the front buffer (DirectDraw without a back buffer):
// the same screenshots, of the front buffer.
void PresentFrontWithDevAids(Device& dev, int w, int h) {
  MaybeShoot(w, h, [&](const Rect& r, uint8_t* px, uint32_t pitch) {
    dev.ReadBackFront(r, px, pitch, Format::X8R8G8B8);
  });
  dev.PresentFront();
}

void PresentWithDevAids(Device& dev, int w, int h) {
  MaybeShoot(w, h, [&](const Rect& r, uint8_t* px, uint32_t pitch) {
    dev.ReadBack(r, px, pitch, Format::X8R8G8B8);
  });
  dev.Present();
  // <ID>_HITCH=ms: report frames that took longer than that.
  static const double hitch_ms = [] {
    const char* e = Env("HITCH");
    return e ? std::atof(e) : 0.0;
  }();
  if (hitch_ms > 0) {
    static Uint64 last = 0, start = 0;
    Uint64 now = SDL_GetTicksNS();
    if (!start) start = now;
    double dt = last ? (now - last) / 1e6 : 0;
    if (dt > hitch_ms)
      WarnF("hitch: frame %d took %.1f ms (t=%.2f s)\n", frame, dt,
            (now - start) / 1e9);
    last = now;
  }
  // <ID>_FPS_REPORT=s: every s seconds, the frame rate and the spread of
  // frame intervals.
  static const double report_s = [] {
    const char* e = Env("FPS_REPORT");
    return e ? std::atof(e) : 0.0;
  }();
  if (report_s > 0) {
    static Uint64 last = 0, first = 0, t0 = 0;
    static std::vector<double> dts;
    Uint64 now = SDL_GetTicksNS();
    if (!t0) t0 = now;
    if (last) dts.push_back((now - last) / 1e6);
    if (!first) first = now;
    last = now;
    if ((now - first) / 1e9 >= report_s && !dts.empty()) {
      std::sort(dts.begin(), dts.end());
      auto q = [&](double p) { return dts[size_t(p * double(dts.size() - 1) + 0.5)]; };
      WarnF("fps: %.2f at t=%.1f s (interval ms: min %.3f p1 %.3f median %.3f p99 %.3f max %.3f)\n",
            dts.size() / ((now - first) / 1e9), (now - t0) / 1e9, dts.front(), q(0.01),
            q(0.5), q(0.99), dts.back());
      dts.clear();
      first = now;
    }
  }
}

Backend ChosenBackend() {
  static const Backend b = [] {
    const char* e = Env("GFX");
    if (e && !std::strcmp(e, "gpu")) return Backend::Gpu;
    if (e && !std::strcmp(e, "d3d9")) {
#ifdef RECOMP_HAVE_D3D9
      return Backend::D3D9;
#else
      FatalF("graphics backend d3d9 is not in this build (no dxvk-native)");
#endif
    }
    if (e && *e) FatalF("unknown graphics backend '%s' (d3d9 or gpu)", e);
    return Backend::Gpu;
  }();
  return b;
}

uint64_t ChosenWindowFlags() {
  switch (ChosenBackend()) {
    case Backend::D3D9:
      return SDL_WINDOW_VULKAN;  // dxvk's SDL3 WSI makes a Vulkan surface
    case Backend::Gpu:
#if defined(__APPLE__)
      return SDL_WINDOW_METAL;
#else
      return SDL_WINDOW_VULKAN;
#endif
  }
  return 0;
}

std::unique_ptr<Device> CreateChosenDevice(SDL_Window* w) {
  std::unique_ptr<Device> d = CreateDevice(ChosenBackend(), w);
  if (!d) FatalF("cannot create the graphics device");
  return d;
}

}  // namespace recomp::gfx
