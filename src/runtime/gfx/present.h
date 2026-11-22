#pragma once
// What every graphics front end does with a finished frame and with its
// device: presents a w x h back buffer on `dev` with the dev aids they
// all share (<ID>_SHOT_EVERY / _SHOT_FROM / _SHOT_DIR screenshots,
// <ID>_HITCH long-frame and <ID>_FPS_REPORT frame-rate reports), and
// picks the backend (<ID>_GFX). The caller then services the window
// system and its core's watchdog (x86/present.h).

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "runtime/gfx/device.h"

namespace recomp::gfx {

void PresentWithDevAids(Device& dev, int w, int h);
// The same for a frame drawn on the front buffer (Device::PresentFront).
void PresentFrontWithDevAids(Device& dev, int w, int h);

// Test aid (<ID>_TEST_INPUT "shot NAME"): the next presented frame is saved
// as $<ID>_SHOT_DIR/NAME.bmp. Front ends that present only when something
// changes (a still menu on the front buffer) present when ShotPending().
void RequestShot(const char* name);
bool ShotPending();

// Test aid (<ID>_TEST_INPUT "wait"): reads the pixels at `points` (back
// buffer pixels, as in the shots) of the next presented frame. TakeProbe
// gives them (0xRRGGBB; out of the frame: 0xFF000000) once read, true
// then and only once per request. A probe due counts as ShotPending, so
// front ends that present only on change (a still DirectDraw menu)
// present for it.
// Frames presented so far (back or front buffer).
int FramesPresented();

// The gamma ramp a device applies to what it shows (Device::SetGammaRamp):
// the test aids' shots and probes apply it too, so they hold what is shown.
// 256 red, green and blue 16-bit entries; null: none.
void NoteShownGamma(const uint16_t* ramp);

// For front ends that present outside gfx::Device (a game's own Vulkan
// swapchain): call once per presented w x h frame. Services TEST_INPUT's
// named shots and probes and counts the frame; `read` (a rect of the
// frame into dst, BGRX, pitch bytes per row) is called only when one of
// them is due, so it may be expensive. <ID>_SHOT_EVERY is the front end's.
void TestAidsForFrame(int w, int h,
                      const std::function<void(const Rect&, uint8_t*, uint32_t)>& read);

void RequestProbe(const std::vector<std::pair<int, int>>& points);
bool TakeProbe(std::vector<uint32_t>* pixels);

// The backend every graphics front end uses: <ID>_GFX=d3d9 or gpu; by
// default SDL_GPU.
Backend ChosenBackend();
// SDL_CreateWindow flags the backend needs on top-level windows.
uint64_t ChosenWindowFlags();
// Creates the device for `w` on ChosenBackend(); fatal if that fails.
std::unique_ptr<Device> CreateChosenDevice(SDL_Window* w);

}  // namespace recomp::gfx
