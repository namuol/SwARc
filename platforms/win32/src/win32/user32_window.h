#pragma once
// Window state shared between user32_window.cpp and other HLE modules
// (ddraw needs the SDL window behind an HWND).

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <vector>

namespace recomp {

// The SDL window behind a top-level HWND, or nullptr.
SDL_Window* SdlWindowFor(uint32_t hwnd);

// HDCs: GetDC(hwnd) returns DcForWindow(hwnd); WindowForDc maps it back
// (0 for the screen DC or anything else).
uint32_t DcForWindow(uint32_t hwnd);
uint32_t WindowForDc(uint32_t hdc);

// Whether the caller is the guest's main thread (the one SDL's video and
// events belong to).
bool OnMainThread();

// Ensures SDL video is initialised (main thread only).
void EnsureSdlVideo();

// Pumps SDL events into the guest message queues. Main thread only;
// called from PeekMessage/GetMessage and long waits.
void PumpHostEvents();
// Called on every message pump (main thread): a graphics front end with a
// present it deferred (DirectDraw's throttled front-buffer presents) does
// it here once it is due.
extern void (*g_pump_present)();

// Games that link SDL2 themselves (recomp_sdl2): an SDL2 window is an
// HWND too, so Win32 and DXGI calls on the HWND the game gets from
// SDL_GetWindowWMInfo reach its SDL window. size is the client area in
// pixels; flags are SDL3 window flags added to the runtime's own.
uint32_t CreateSdl2Window(const std::string& title_utf8, int w, int h, SDL_WindowFlags flags,
                          SDL_Window** sdl);
void DestroySdl2Window(uint32_t hwnd);
// An SDL2 game owns the host event stream: once a sink is set,
// PumpHostEvents hands it every event instead of turning them into
// window messages (input filtering for background runs still applies).
void SetHostEventSink(void (*sink)(const SDL_Event&));

// DirectDraw exclusive mode on hwnd: the SDL window's client area is the
// display mode (w x h) and the game's own window placement (MoveWindow,
// minimize/restore on deactivation) is bookkeeping only, as it would be
// behind a fullscreen exclusive surface on Windows. w = 0 ends it.
void SetExclusiveMode(uint32_t hwnd, int w, int h);

// Shows hwnd without activating it, as SetWindowPos with SWP_SHOWWINDOW |
// SWP_NOACTIVATE does. Direct3D 9 does this to the device window when it
// goes fullscreen; games may leave that window hidden until then.
void ShowWindowNoActivate(uint32_t hwnd);

// The size of hwnd's client area as the game sees it (in exclusive mode,
// the display mode): what its frames are drawn at. The host window may
// differ (fullscreen, or resized by the window manager); present fits the
// frame into it (gfx::FitRect) and the pointer is mapped back.
void ClientAreaSize(uint32_t hwnd, int* w, int* h);

// The cursor as GetCursorPos reports it, in hwnd's client coordinates.
void CursorInClient(uint32_t hwnd, int* x, int* y);
// PostMessage from host code (any thread): queues msg for hwnd's thread.
// False if hwnd is not a window.
bool PostWindowMessage(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam);
// Screen coordinates to hwnd's client coordinates (ScreenToClient).
void ScreenPointToClient(uint32_t hwnd, int* x, int* y);

// The display mode sizes games can switch to, smallest first: the classic
// 4:3 and 5:4 ones, common wide ones and the desktop's own size, up to the
// desktop's size (EnumDisplaySettings, ChangeDisplaySettings, DirectDraw's
// EnumDisplayModes).
struct ModeSize {
  int w, h;
};
std::vector<ModeSize> DisplayModeSizes();

// The refresh rates every mode size is offered at, lowest first: 60 Hz,
// and the desktop's own rate when it differs (a 120 Hz display offers 60
// and 120). Presentation follows the host display whatever the game picks;
// the list lets games that cap or pace by the mode's rate run at the
// host's.
std::vector<int> DisplayModeRates();

}  // namespace recomp
