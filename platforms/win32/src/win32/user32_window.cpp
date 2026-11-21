// user32: window classes, windows, message queues and the SDL3 event pump.
//
// Every top-level window gets an SDL window sized to its client area; child
// windows are bookkeeping only. SDL events are translated into WM_* messages
// on the queue of the thread that owns the window, and DispatchMessage calls
// the guest WndProc. Messages Windows sends synchronously (creation,
// activation, size) are delivered by calling the WndProc directly.

#include <SDL3/SDL.h>

#include <atomic>
#include <bit>
#include <cmath>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <string>
#include <vector>

#include "runtime/display.h"
#include "runtime/log.h"
#include "runtime/platform.h"
#include "win32/hle.h"
#include "win32/input_state.h"
#include "win32/kernel32_state.h"
#include "win32/user32_window.h"
#include "win32/win32.h"
#include "runtime/gfx/present.h"
#include "x86/watchdog.h"

namespace recomp {

// SDL_MouseID of <ID>_TEST_INPUT "pointer" events: they stand for the host
// pointer, which automated (background) runs otherwise ignore.
constexpr SDL_MouseID kTestPointer = 0xD51D51;
// TEST_INPUT "warp": resolved to a relative move when injected.
constexpr SDL_MouseID kWarp = 0xD51D52;
// TEST_INPUT relative moves ("move", "goto", a resolved "warp"): they carry
// no host position, so they always move the cursor by their motion.
constexpr SDL_MouseID kTestRelative = 0xD51D53;
// TEST_INPUT "shot NAME": an SDL user event whose code indexes g_shot_names.
constexpr Uint32 kShotEvent = SDL_EVENT_USER + 0x51;
std::vector<std::string> g_shot_names;
// TEST_INPUT "wait" (code indexes g_waits) and "exit".
constexpr Uint32 kWaitEvent = SDL_EVENT_USER + 0x52;
constexpr Uint32 kExitEvent = SDL_EVENT_USER + 0x53;
struct TestWait {
  int line = 0;
  double timeout = 0;
  std::vector<std::pair<int, int>> points;
  std::vector<uint32_t> colours;  // 0xRRGGBB, per point
  std::vector<bool> unlike;       // per point: "!=": must not be that colour
  int tolerance = 24;             // per channel
  SDL_Scancode press = SDL_SCANCODE_UNKNOWN;
  double every = 0;               // seconds between presses
};
std::vector<TestWait> g_waits;

namespace {


// ---- messages and styles -----------------------------------------------------
enum : uint32_t {
  WM_CREATE = 0x0001, WM_DESTROY = 0x0002, WM_MOVE = 0x0003,
  WM_SIZE = 0x0005, WM_ACTIVATE = 0x0006, WM_SETFOCUS = 0x0007,
  WM_KILLFOCUS = 0x0008, WM_SETTEXT = 0x000C, WM_GETTEXT = 0x000D,
  WM_GETTEXTLENGTH = 0x000E, WM_CLOSE = 0x0010,
  WM_QUIT = 0x0012, WM_ERASEBKGND = 0x0014, WM_SHOWWINDOW = 0x0018,
  WM_SETFONT = 0x0030, WM_GETFONT = 0x0031, WM_TIMER = 0x0113,
  WM_ACTIVATEAPP = 0x001C, WM_NCCREATE = 0x0081, WM_NCDESTROY = 0x0082,
  WM_NCHITTEST = 0x0084, WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101, WM_CHAR = 0x0102,
  WM_SYSKEYDOWN = 0x0104, WM_SYSKEYUP = 0x0105, WM_SYSCHAR = 0x0106,
  WM_SYSCOMMAND = 0x0112, WM_MOUSEMOVE = 0x0200, WM_LBUTTONDOWN = 0x0201,
  WM_LBUTTONUP = 0x0202, WM_LBUTTONDBLCLK = 0x0203, WM_RBUTTONDOWN = 0x0204,
  WM_RBUTTONUP = 0x0205, WM_RBUTTONDBLCLK = 0x0206, WM_MBUTTONDOWN = 0x0207,
  WM_MBUTTONUP = 0x0208, WM_MBUTTONDBLCLK = 0x0209, WM_MOUSEWHEEL = 0x020A,
};
constexpr uint32_t WS_CHILD = 0x40000000, WS_VISIBLE = 0x10000000,
                   WS_CAPTION = 0x00C00000,
                   WS_THICKFRAME = 0x00040000;
constexpr uint32_t CS_DBLCLKS = 0x0008;
constexpr int32_t GWL_WNDPROC = -4, GWL_HINSTANCE = -6, GWL_HWNDPARENT = -8,
                  GWL_STYLE = -16, GWL_EXSTYLE = -20, GWL_USERDATA = -21,
                  GWL_ID = -12;

struct WndClass {
  std::string name;  // lower case
  uint32_t style = 0, wndproc = 0, wnd_extra = 0;
  uint16_t atom = 0;
};

struct Window {
  uint32_t hwnd = 0;
  std::string cls;
  uint32_t wndproc = 0, style = 0, exstyle = 0, parent = 0, id = 0;
  uint32_t hinst = 0, userdata = 0, class_style = 0;
  int x = 0, y = 0, w = 0, h = 0;  // window rect (screen)
  std::string title;
  std::vector<uint8_t> extra;      // cbWndExtra bytes (GetWindowLong >= 0)
  uint32_t tid = 0;
  bool visible = false, enabled = true, destroyed = false;
  bool minimized = false;      // as the game asked (IsIconic)
  // Maximized (ShowWindow SW_SHOWMAXIMIZED): the window rect is the
  // screen's (its frame outside it), and the rect to restore is kept.
  bool maximized = false;
  int nx = 0, ny = 0, nw = 0, nh = 0;
  int mode_w = 0, mode_h = 0;  // DirectDraw exclusive mode, see header
  // Host fullscreen (UpdateFullscreen), entered for a screen of fs_w x fs_h.
  bool fullscreen = false;
  int fs_w = 0, fs_h = 0;
  SDL_Window* sdl = nullptr;
  // Built-in controls (see "built-in control classes"): the text is
  // `title`; selection, font, image and check state are kept here.
  uint32_t sel_lo = 0, sel_hi = 0, font = 0, image = 0, check = 0;
  uint32_t limit = 30000;  // edit text limit (EM_SETLIMITTEXT)
  // A dialog (user32_window.cpp, "dialog boxes"): no host window of its
  // own; it is drawn by the game into its window, and the mouse reaches it
  // through RouteMouse. A dialog's or a child's rect is relative to its
  // owner's or parent's client area (x, y), as the game created it.
  bool is_dialog = false;
  // Needs painting (InvalidateRect, shown): WM_PAINT is synthesised for
  // it when its thread's queue has nothing else, until it is validated
  // (BeginPaint, ValidateRect, DefWindowProc's WM_PAINT).
  bool dirty = false;
  bool dialog_ended = false;
  uint32_t dialog_result = 0;
  // List box / combo box items and selection; trackbar / scroll bar range
  // and position.
  std::vector<std::pair<std::string, uint32_t>> items;
  int32_t cur_sel = -1, top_index = 0;
  int32_t range_lo = 0, range_hi = 100, pos = 0;
  bool dropped = false;
};

struct Msg {
  uint32_t hwnd, msg, wparam, lparam, time;
  int32_t ptx, pty;
};

struct Queue {
  std::deque<Msg> msgs;
  bool quit = false;
  uint32_t quit_code = 0;
};

// SetTimer: WM_TIMER is not queued but synthesised when the owning
// thread's queue has nothing else for the caller, as on Windows.
struct Timer {
  uint32_t hwnd, id, elapse, proc, tid;
  int64_t due;  // TickCount() of the next WM_TIMER
};

std::recursive_mutex g_mu;
std::map<std::string, WndClass> g_classes;
std::map<uint32_t, Window> g_windows;
std::map<uint32_t, Queue> g_queues;  // by tid
std::vector<Timer> g_timers;

// ClipCursor: while the game confines the cursor to one of our windows
// (mouse look), the cursor is virtual, as in exclusive mode.
struct Clip {
  bool on = false;
  int32_t l = 0, t = 0, r = 0, b = 0;  // screen coordinates
  uint32_t hwnd = 0;
};
Clip g_clip;
uint32_t g_capture = 0;  // SetCapture

// RegisterHotKey: a matching key press becomes WM_HOTKEY instead of a key
// message, as on Windows (here only while one of our windows has focus).
struct HotKey {
  uint32_t hwnd, id, mods, vk, tid;
};
std::vector<HotKey> g_hotkeys;
std::condition_variable_any g_queue_cv;
uint32_t g_next_hwnd = 0x00010010;
uint16_t g_next_atom = 0xC100;
uint32_t g_focus = 0, g_active = 0;

// Hooks run by this module (WH_KEYBOARD, WH_CBT), the last installed
// first (see "hooks").
constexpr int32_t kWhKeyboard = 2, kWhCbt = 5;
struct Hook {
  int32_t type;
  uint32_t handle, proc, tid;  // tid 0: every thread
};
std::vector<Hook> g_hooks;

// The procedure of the calling thread's first `type` hook after the hook
// `after` (0: the first one); 0 if there is none.
uint32_t NextHook(int32_t type, uint32_t after) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t tid = CurrentThread()->tid;
  bool past = after == 0;
  for (const Hook& k : g_hooks) {
    if (!past) {
      past = k.handle == after;
      continue;
    }
    if (k.type == type && (!k.tid || k.tid == tid)) return k.proc;
  }
  return 0;
}
uint32_t NextCbtHook(uint32_t after) { return NextHook(kWhCbt, after); }
int g_cursor_count = 0;  // ShowCursor display counter
int32_t g_mouse_x = 0, g_mouse_y = 0;  // screen coordinates
// The host pointer over the game window, in client (mode) coordinates;
// valid while g_pointer_inside. Written on the main thread.
std::atomic<bool> g_pointer_inside{false};
std::atomic<int32_t> g_real_x{0}, g_real_y{0};
// When the game last re-centred the cursor (it does so every frame while
// it steers its own pointer by relative motion).
std::atomic<int64_t> g_last_recentre_ms{-100000};
std::atomic<int32_t> g_recentre_x{0}, g_recentre_y{0};  // screen coordinates
bool g_sdl_up = false;
std::thread::id g_main_thread_id = std::this_thread::get_id();

Window* Win(uint32_t hwnd) {
  auto it = g_windows.find(hwnd);
  return it == g_windows.end() || it->second.destroyed ? nullptr : &it->second;
}

// Frame sizes for a top-level style: border on each side, caption on top.
void FrameSize(uint32_t style, int* border, int* caption) {
  bool framed = style & (WS_CAPTION | WS_THICKFRAME);
  *border = !framed ? 0 : (style & WS_THICKFRAME) ? 4 : 3;
  *caption = framed && (style & WS_CAPTION) == WS_CAPTION ? 19 : 0;
}

// Client-area size for a window rect, mirroring what AdjustWindowRect added.
void ClientSize(const Window& w, int* cw, int* ch) {
  int border = 0, caption = 0;
  if (!(w.style & WS_CHILD)) FrameSize(w.style, &border, &caption);
  *cw = std::max(0, w.w - 2 * border);
  *ch = std::max(0, w.h - 2 * border - caption);
}

void PostTo(uint32_t tid, const Msg& m) {
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    g_queues[tid].msgs.push_back(m);
  }
  g_queue_cv.notify_all();
}

void Post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
  uint32_t tid;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    tid = w ? w->tid : CurrentThread()->tid;
  }
  PostTo(tid, Msg{hwnd, msg, wp, lp, TickCount(), g_mouse_x, g_mouse_y});
}

// Synchronous delivery to the guest WndProc (SendMessage semantics).
uint32_t Send(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
  uint32_t proc;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return 0;
    if (w->tid != CurrentThread()->tid)
      WarnF("SendMessage %X to hwnd %X owned by thread %X from thread %X: "
            "delivered on the calling thread\n",
            msg, hwnd, w->tid, CurrentThread()->tid);
    proc = w->wndproc;
  }
  Ctx& c = CurrentThread()->ctx;
  return CallGuest(c, proc, {hwnd, msg, wp, lp});
}

uint32_t MkLparam(int lo, int hi) {
  return (uint32_t(uint16_t(lo))) | (uint32_t(uint16_t(hi)) << 16);
}

// Top-level window under SDL window id.
uint32_t HwndForSdl(SDL_WindowID id) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  for (auto& [h, w] : g_windows)
    if (w.sdl && SDL_GetWindowID(w.sdl) == id && !w.destroyed) return h;
  return 0;
}

// ---- SDL key -> Windows VK ----------------------------------------------------

uint8_t VkFromScancode(SDL_Scancode sc) {
  if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return uint8_t('A' + (sc - SDL_SCANCODE_A));
  if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return uint8_t('1' + (sc - SDL_SCANCODE_1));
  if (sc == SDL_SCANCODE_0) return '0';
  if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return uint8_t(0x70 + (sc - SDL_SCANCODE_F1));
  switch (sc) {
    case SDL_SCANCODE_RETURN: return 0x0D;
    case SDL_SCANCODE_ESCAPE: return 0x1B;
    case SDL_SCANCODE_BACKSPACE: return 0x08;
    case SDL_SCANCODE_TAB: return 0x09;
    case SDL_SCANCODE_SPACE: return 0x20;
    case SDL_SCANCODE_MINUS: return 0xBD;
    case SDL_SCANCODE_EQUALS: return 0xBB;
    case SDL_SCANCODE_LEFTBRACKET: return 0xDB;
    case SDL_SCANCODE_RIGHTBRACKET: return 0xDD;
    case SDL_SCANCODE_BACKSLASH: return 0xDC;
    case SDL_SCANCODE_SEMICOLON: return 0xBA;
    case SDL_SCANCODE_APOSTROPHE: return 0xDE;
    case SDL_SCANCODE_GRAVE: return 0xC0;
    case SDL_SCANCODE_COMMA: return 0xBC;
    case SDL_SCANCODE_PERIOD: return 0xBE;
    case SDL_SCANCODE_SLASH: return 0xBF;
    case SDL_SCANCODE_CAPSLOCK: return 0x14;
    case SDL_SCANCODE_PRINTSCREEN: return 0x2C;
    case SDL_SCANCODE_SCROLLLOCK: return 0x91;
    case SDL_SCANCODE_PAUSE: return 0x13;
    case SDL_SCANCODE_INSERT: return 0x2D;
    case SDL_SCANCODE_HOME: return 0x24;
    case SDL_SCANCODE_PAGEUP: return 0x21;
    case SDL_SCANCODE_DELETE: return 0x2E;
    case SDL_SCANCODE_END: return 0x23;
    case SDL_SCANCODE_PAGEDOWN: return 0x22;
    case SDL_SCANCODE_RIGHT: return 0x27;
    case SDL_SCANCODE_LEFT: return 0x25;
    case SDL_SCANCODE_DOWN: return 0x28;
    case SDL_SCANCODE_UP: return 0x26;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 0x90;
    case SDL_SCANCODE_KP_DIVIDE: return 0x6F;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x6A;
    case SDL_SCANCODE_KP_MINUS: return 0x6D;
    case SDL_SCANCODE_KP_PLUS: return 0x6B;
    case SDL_SCANCODE_KP_ENTER: return 0x0D;
    case SDL_SCANCODE_KP_1: return 0x61;
    case SDL_SCANCODE_KP_2: return 0x62;
    case SDL_SCANCODE_KP_3: return 0x63;
    case SDL_SCANCODE_KP_4: return 0x64;
    case SDL_SCANCODE_KP_5: return 0x65;
    case SDL_SCANCODE_KP_6: return 0x66;
    case SDL_SCANCODE_KP_7: return 0x67;
    case SDL_SCANCODE_KP_8: return 0x68;
    case SDL_SCANCODE_KP_9: return 0x69;
    case SDL_SCANCODE_KP_0: return 0x60;
    case SDL_SCANCODE_KP_PERIOD: return 0x6E;
    case SDL_SCANCODE_LCTRL: return 0xA2;
    case SDL_SCANCODE_RCTRL: return 0xA3;
    case SDL_SCANCODE_LSHIFT: return 0xA0;
    case SDL_SCANCODE_RSHIFT: return 0xA1;
    case SDL_SCANCODE_LALT: return 0xA4;
    case SDL_SCANCODE_RALT: return 0xA5;
    case SDL_SCANCODE_LGUI: return 0x5B;
    case SDL_SCANCODE_RGUI: return 0x5C;
    case SDL_SCANCODE_APPLICATION: return 0x5D;
    default: return 0;
  }
}

// Messages use the generic VK for modifiers; key state tracks both.
uint8_t GenericVk(uint8_t vk) {
  switch (vk) {
    case 0xA0: case 0xA1: return 0x10;
    case 0xA2: case 0xA3: return 0x11;
    case 0xA4: case 0xA5: return 0x12;
    default: return vk;
  }
}

uint32_t MouseKeys() {
  uint32_t mk = 0;
  if (g_key_down[0x01]) mk |= 0x01;  // MK_LBUTTON
  if (g_key_down[0x02]) mk |= 0x02;  // MK_RBUTTON
  if (g_key_down[0x10]) mk |= 0x04;  // MK_SHIFT
  if (g_key_down[0x11]) mk |= 0x08;  // MK_CONTROL
  if (g_key_down[0x04]) mk |= 0x10;  // MK_MBUTTON
  return mk;
}

void SetKey(uint8_t vk, bool down) {
  if (down && !g_key_down[vk]) {
    g_key_pressed_since[vk] = 1;
    g_key_toggled[vk] = !g_key_toggled[vk];
  }
  g_key_down[vk] = down;
}

// The screen position of a window's rect: a child's or a dialog's x, y are
// relative to its parent's (owner's) client area.
void WindowScreenPos(const Window& w, int* x, int* y);

void WindowOrigin(uint32_t hwnd, int* x, int* y) {
  *x = *y = 0;
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (Window* w = Win(hwnd)) {
    int cw, ch, sx, sy;
    ClientSize(*w, &cw, &ch);
    WindowScreenPos(*w, &sx, &sy);
    *x = sx + (w->w - cw) / 2;
    *y = sy + (w->h - ch) - (w->w - cw) / 2;
  }
}

void WindowScreenPos(const Window& w, int* x, int* y) {
  *x = w.x;
  *y = w.y;
  if ((w.style & WS_CHILD) || w.is_dialog) {
    int px = 0, py = 0;
    if (w.parent) WindowOrigin(w.parent, &px, &py);
    *x += px;
    *y += py;
  }
}

bool ContainsScreen(const Window& w, int sx, int sy) {
  int x, y;
  WindowScreenPos(w, &x, &y);
  return sx >= x && sy >= y && sx < x + w.w && sy < y + w.h;
}

// Whether hwnd is `top` or one of its dialogs (owner chain).
bool OwnedBy(uint32_t hwnd, uint32_t top) {
  for (int i = 0; hwnd && i < 16; i++) {
    if (hwnd == top) return true;
    Window* w = Win(hwnd);
    if (!w) return false;
    hwnd = w->parent;
  }
  return false;
}

// The window the mouse at screen sx, sy over top-level `top` talks to: the
// newest visible dialog of `top` under it, then the deepest visible,
// enabled child there (a static without SS_NOTIFY is transparent, as its
// WM_NCHITTEST answers HTTRANSPARENT); `top` when no dialog is open.
// *cx, *cy: the point in that window's client area.
uint32_t RouteMouse(uint32_t top, int sx, int sy, int* cx, int* cy) {
  uint32_t node = top;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    uint32_t dlg = 0;
    bool any = false;
    for (auto& [hw, w] : g_windows)
      if (!w.destroyed && w.is_dialog && w.visible && OwnedBy(hw, top)) {
        any = true;
        if (ContainsScreen(w, sx, sy)) dlg = hw;
      }
    if (any) {
      if (dlg) node = dlg;
      for (int depth = 0; depth < 8; depth++) {
        uint32_t best = 0;
        for (auto& [hw, w] : g_windows) {
          if (w.destroyed || w.parent != node || !(w.style & WS_CHILD) || !w.visible ||
              !w.enabled || !ContainsScreen(w, sx, sy))
            continue;
          if (w.cls == "static" && !(w.style & 0x100 /* SS_NOTIFY */)) continue;
          best = hw;
        }
        if (!best) break;
        node = best;
      }
    }
  }
  int ox, oy;
  WindowOrigin(node, &ox, &oy);
  *cx = sx - ox;
  *cy = sy - oy;
  return node;
}

// True (with the mode size) while hwnd is in DirectDraw exclusive mode.
bool ExclusiveSize(uint32_t hwnd, int* w, int* h) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* win = Win(hwnd);
  if (!win || !win->mode_w) return false;
  *w = win->mode_w;
  *h = win->mode_h;
  return true;
}

// The client area as the game sees it: the display mode in exclusive mode.
void GameClientSize(const Window& w, int* cw, int* ch) {
  if (w.mode_w) {
    *cw = w.mode_w;
    *ch = w.mode_h;
  } else {
    ClientSize(w, cw, ch);
  }
}

// Host window coordinates -> client coordinates: client = (host - x0) * k.
// The frame (the client area) is shown fitted into the host window with
// its aspect kept (gfx::FitRect, as present does), so when the host window
// has another size (fullscreen, or resized by the window manager or by
// desktop scaling) points map through that rectangle. `fitted` is false
// when the two have the same size (k = 1, no offset).
struct ClientMap {
  float kx = 1, ky = 1, x0 = 0, y0 = 0;
  int cw = 0, ch = 0;
  bool fitted = false;
};
// SetPointerRemap: by hwnd, {shown l, t, w, h, asked l, t, w, h}.
std::map<uint32_t, std::array<int, 8>> g_pointer_remap;

// A client point through hwnd's pointer remap, if it has one.
void RemapPointer(uint32_t hwnd, int* x, int* y) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  auto it = g_pointer_remap.find(hwnd);
  if (it == g_pointer_remap.end()) return;
  const auto& r = it->second;
  *x = r[4] + int(std::floor(double(*x - r[0]) * r[6] / r[2]));
  *y = r[5] + int(std::floor(double(*y - r[1]) * r[7] / r[3]));
}

ClientMap MapToClient(uint32_t hwnd) {
  ClientMap m;
  SDL_Window* sdl;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w || !w->sdl) return m;
    sdl = w->sdl;
    GameClientSize(*w, &m.cw, &m.ch);
  }
  int ww = 0, wh = 0;
  if (!SDL_GetWindowSize(sdl, &ww, &wh) || ww <= 0 || wh <= 0 || m.cw <= 0 || m.ch <= 0 ||
      (ww == m.cw && wh == m.ch))
    return m;
  const gfx::Rect r = gfx::FitRect(m.cw, m.ch, ww, wh);
  m.kx = float(m.cw) / float(r.right - r.left);
  m.ky = float(m.ch) / float(r.bottom - r.top);
  m.x0 = float(r.left);
  m.y0 = float(r.top);
  m.fitted = true;
  return m;
}

// The window whose client area holds the clip rectangle, or 0.
uint32_t ClipWindow() {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  for (auto& [h, w] : g_windows) {
    if (w.destroyed || !w.sdl) continue;
    int ox, oy, cw, ch;
    WindowOrigin(h, &ox, &oy);
    ClientSize(w, &cw, &ch);
    if (g_clip.l >= ox && g_clip.t >= oy && g_clip.r <= ox + cw + 1 &&
        g_clip.b <= oy + ch + 1)
      return h;
  }
  return 0;
}

// A clip is a screen rectangle and outlives the window it was set for, as
// on Windows: a game that recreates its window (e.g. to restart its
// renderer) without clipping again stays confined. Once a window covers
// the rectangle again, its cursor is virtual and the host pointer grabbed.
void AttachClip() {
  SDL_Window* grab = nullptr;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (!g_clip.on || g_clip.hwnd) return;
    g_clip.hwnd = ClipWindow();
    if (Window* w = Win(g_clip.hwnd)) grab = w->sdl;
  }
  if (grab && !Background()) SDL_SetWindowRelativeMouseMode(grab, true);
}

// Where a key typed into top-level `h` goes: the focus window when it is
// `h` or lies within it (a control, or a dialog drawn into it and its
// controls, through parents and owners), or within a dialog that has no
// owner (it has no host window of its own to type into); else `h`.
uint32_t KeyTarget(uint32_t h) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  for (uint32_t w = g_focus; w; ) {
    if (w == h) return g_focus;
    Window* win = Win(w);
    if (!win || win->destroyed) break;
    if (win->is_dialog && !win->parent && !win->sdl) return g_focus;
    w = win->parent;
  }
  return h;
}

void ActivateApp(uint32_t hwnd, bool active) {
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (active) {
      if (g_active == hwnd) return;  // already active: nothing to report
      g_active = g_focus = hwnd;
    } else {
      if (g_active != hwnd) return;
      g_active = g_focus = 0;
    }
  }
  WatchdogAppActive(active);
  Post(hwnd, WM_ACTIVATEAPP, active ? 1 : 0, 0);
  Post(hwnd, WM_ACTIVATE, active ? 1 : 0, 0);
  Post(hwnd, active ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0);
}

void HandleSdlEvent(const SDL_Event& e) {
  switch (e.type) {
    case SDL_EVENT_QUIT: {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      for (auto& [h, w] : g_windows)
        if (w.sdl && !w.destroyed) Post(h, WM_CLOSE, 0, 0);
      break;
    }
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      if (uint32_t h = HwndForSdl(e.window.windowID)) Post(h, WM_CLOSE, 0, 0);
      break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      AttachClip();
      if (uint32_t h = HwndForSdl(e.window.windowID)) ActivateApp(h, true);
      break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      if (uint32_t h = HwndForSdl(e.window.windowID)) {
        // Release keys so nothing stays stuck while we're away.
        for (int vk = 0; vk < 256; vk++) g_key_down[vk] = 0;
        ActivateApp(h, false);
      }
      break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
      uint32_t h = e.key.windowID ? HwndForSdl(e.key.windowID) : 0;
      if (!e.key.windowID) {
        std::lock_guard<std::recursive_mutex> l(g_mu);
        Window* f = Win(g_focus);
        if (f && !f->destroyed) h = g_focus;
      }
      uint8_t vk = VkFromScancode(e.key.scancode);
      if (!h || !vk) break;
      bool down = e.type == SDL_EVENT_KEY_DOWN;
      bool was_down = g_key_down[GenericVk(vk)];
      SetKey(vk, down);
      SetKey(GenericVk(vk), down);
      uint8_t gvk = GenericVk(vk);
      bool ext;
      uint8_t sc = ScanCodeForVk(vk, &ext);
      if (e.key.scancode == SDL_SCANCODE_KP_ENTER) ext = true;
      if (!down || !was_down)  // DirectInput reports changes, not repeats
        EmitRawInput(RawInput::Key, uint8_t(sc | (ext ? 0x80 : 0)), down ? 1 : 0);
      uint32_t lp = 1u | (uint32_t(sc) << 16) | (ext ? 1u << 24 : 0);
      if (!down) lp |= 0xC0000000u;
      else if (was_down) lp |= 0x40000000u;
      bool alt = g_key_down[0x12];
      bool sys = alt || gvk == 0x79 /* F10 */ || gvk == 0x12;
      if (alt) lp |= 1u << 29;
      if (down) {
        uint32_t mods = (alt ? 1u : 0) | (g_key_down[0x11] ? 2u : 0) |
                        (g_key_down[0x10] ? 4u : 0);  // MOD_ALT, CONTROL, SHIFT
        const HotKey* hk = nullptr;
        {
          std::lock_guard<std::recursive_mutex> l(g_mu);
          for (const HotKey& k : g_hotkeys)
            if (k.vk == gvk && (k.mods & 7) == mods) hk = &k;
          if (hk) {
            HotKey k = *hk;
            Msg m{k.hwnd, 0x0312 /* WM_HOTKEY */, k.id, mods | gvk << 16, TickCount(),
                  g_mouse_x, g_mouse_y};
            PostTo(k.tid, m);
            break;
          }
        }
      }
      uint32_t msg = down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN)
                          : (sys ? WM_SYSKEYUP : WM_KEYUP);
      Post(KeyTarget(h), msg, gvk, lp);
      break;
    }
    case SDL_EVENT_MOUSE_MOTION: {
      uint32_t h = HwndForSdl(e.motion.windowID);
      if (!h) break;
      int ox, oy;
      WindowOrigin(h, &ox, &oy);
      const ClientMap cm = MapToClient(h);
      int mw, mh;
      if (e.motion.which == kTestPointer) {
        // Already client pixels, whatever size the host window has.
        g_real_x = int(e.motion.x);
        g_real_y = int(e.motion.y);
        g_pointer_inside = true;
      } else if (!Background()) {
        g_real_x = int((e.motion.x - cm.x0) * cm.kx);
        g_real_y = int((e.motion.y - cm.y0) * cm.ky);
        g_pointer_inside = true;
      }
      {
        static float rx = 0, ry = 0;  // sub-mickey remainder
        rx += e.motion.xrel;
        ry += e.motion.yrel;
        int dx = int(rx), dy = int(ry);
        rx -= dx;
        ry -= dy;
        if (dx) EmitRawInput(RawInput::MouseX, 0, dx);
        if (dy) EmitRawInput(RawInput::MouseY, 0, dy);
      }
      AttachClip();
      bool clipped = g_clip.on && g_clip.hwnd == h;
      // In exclusive mode the cursor is virtual only while the game
      // re-centres it (mouse look). A game that just reads GetCursorPos,
      // as RTS menus and maps do, gets the host pointer's position mapped
      // onto its screen, so its own cursor stays under the real one.
      bool relative = e.motion.which == kTestRelative ||
                      int64_t(TickCount()) - g_last_recentre_ms <= 250;
      if (clipped || (relative && ExclusiveSize(h, &mw, &mh))) {
        // Virtual cursor: the game recentres it with SetCursorPos every
        // frame and steers its own pointer by the difference, as on a
        // fullscreen Windows desktop. Real motion moves the virtual
        // position; the host pointer is never warped (while clipped it
        // is grabbed in SDL's relative mode instead). Motion is not
        // scaled by how the frame is shown: one unit of host motion is
        // one pixel of the game's screen, as a mickey is on Windows, so
        // mouse look turns the same windowed, fullscreen and in any mode.
        static float fx = 0, fy = 0;  // sub-pixel remainder
        fx += e.motion.xrel;
        fy += e.motion.yrel;
        int dx = int(fx), dy = int(fy);
        fx -= dx;
        fy -= dy;
        if (clipped) {
          g_mouse_x = std::clamp(g_mouse_x + dx, g_clip.l, std::max(g_clip.l, g_clip.r - 1));
          g_mouse_y = std::clamp(g_mouse_y + dy, g_clip.t, std::max(g_clip.t, g_clip.b - 1));
        } else {
          g_mouse_x = std::clamp(g_mouse_x + dx, ox, ox + mw - 1);
          g_mouse_y = std::clamp(g_mouse_y + dy, oy, oy + mh - 1);
        }
      } else {
        int x = int((e.motion.x - cm.x0) * cm.kx), y = int((e.motion.y - cm.y0) * cm.ky);
        if (e.motion.which == kTestPointer) {  // already client pixels
          x = int(e.motion.x);
          y = int(e.motion.y);
        }
        RemapPointer(h, &x, &y);
        if (cm.fitted || x < 0 || y < 0 || x >= cm.cw || y >= cm.ch) {
          // Over the black bars: the pointer is at the screen's edge.
          x = std::clamp(x, 0, std::max(cm.cw - 1, 0));
          y = std::clamp(y, 0, std::max(cm.ch - 1, 0));
        }
        g_mouse_x = ox + x;
        g_mouse_y = oy + y;
      }
      {
        int cx, cy;
        uint32_t t = RouteMouse(h, g_mouse_x, g_mouse_y, &cx, &cy);
        Post(t, WM_MOUSEMOVE, MouseKeys(), MkLparam(cx, cy));
      }
      break;
    }
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
      g_pointer_inside = false;
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      uint32_t h = HwndForSdl(e.button.windowID);
      if (!h) break;
      bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
      int ox, oy;
      WindowOrigin(h, &ox, &oy);
      int x = g_mouse_x - ox, y = g_mouse_y - oy;  // where motion left it
      if (e.button.button >= SDL_BUTTON_LEFT && e.button.button <= SDL_BUTTON_X2) {
        // SDL: left, middle, right, X1, X2; DirectInput: left, right, middle.
        static const uint8_t kDiButton[] = {0, 2, 1, 3, 4};
        EmitRawInput(RawInput::MouseButton, kDiButton[e.button.button - 1], down ? 1 : 0);
      }
      uint32_t base;
      uint8_t vk;
      switch (e.button.button) {
        case SDL_BUTTON_LEFT: base = WM_LBUTTONDOWN; vk = 0x01; break;
        case SDL_BUTTON_RIGHT: base = WM_RBUTTONDOWN; vk = 0x02; break;
        case SDL_BUTTON_MIDDLE: base = WM_MBUTTONDOWN; vk = 0x04; break;
        default: return;
      }
      SetKey(vk, down);
      bool dbl = false;
      {
        std::lock_guard<std::recursive_mutex> l(g_mu);
        Window* w = Win(h);
        dbl = down && w && (w->class_style & CS_DBLCLKS) && e.button.clicks == 2;
      }
      uint32_t msg = !down ? base + 1 : dbl ? base + 2 : base;
      int cx, cy;
      uint32_t t = RouteMouse(h, g_mouse_x, g_mouse_y, &cx, &cy);
      if (t == h)
        Post(h, msg, MouseKeys(), MkLparam(x, y));
      else
        Post(t, msg, MouseKeys(), MkLparam(cx, cy));
      break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
      uint32_t h = HwndForSdl(e.wheel.windowID);
      if (!h) break;
      int delta = int(e.wheel.y * 120.0f);
      if (!delta) break;
      EmitRawInput(RawInput::Wheel, 0, delta);
      Post(h, WM_MOUSEWHEEL, MouseKeys() | (uint32_t(uint16_t(int16_t(delta))) << 16),
           MkLparam(g_mouse_x, g_mouse_y));
      break;
    }
    default:
      break;
  }
}

// Writes a guest MSG.
void PutMsg(uint32_t va, const Msg& m) {
  Wr32(va + 0, m.hwnd);
  Wr32(va + 4, m.msg);
  Wr32(va + 8, m.wparam);
  Wr32(va + 12, m.lparam);
  Wr32(va + 16, m.time);
  Wr32(va + 20, uint32_t(m.ptx));
  Wr32(va + 24, uint32_t(m.pty));
}

bool Matches(const Msg& m, uint32_t hwnd, uint32_t lo, uint32_t hi) {
  if (hwnd && m.hwnd != hwnd) return false;
  if (lo == 0 && hi == 0) return true;
  return m.msg >= lo && m.msg <= hi;
}

// Takes (or peeks) the first matching message for the current thread.
bool TakeMessage(uint32_t hwnd, uint32_t lo, uint32_t hi, bool remove, Msg* out) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Queue& q = g_queues[CurrentThread()->tid];
  for (auto it = q.msgs.begin(); it != q.msgs.end(); ++it)
    if (Matches(*it, hwnd, lo, hi)) {
      *out = *it;
      if (remove) q.msgs.erase(it);
      return true;
    }
  if (q.quit) {
    *out = Msg{0, WM_QUIT, q.quit_code, 0, TickCount(), g_mouse_x, g_mouse_y};
    if (remove) q.quit = false;
    return true;
  }
  const uint32_t now = TickCount();
  for (Timer& t : g_timers) {
    Msg m{t.hwnd, WM_TIMER, t.id, t.proc, now, g_mouse_x, g_mouse_y};
    if (t.tid != CurrentThread()->tid || int64_t(now) < t.due || !Matches(m, hwnd, lo, hi))
      continue;
    *out = m;
    if (remove) t.due = int64_t(now) + t.elapse;
    return true;
  }
  // WM_PAINT for the first dirty visible window of this thread (parents
  // before their children: they were created first).
  constexpr uint32_t kWmPaint = 0x000F;
  if (!lo && !hi ? true : (kWmPaint >= lo && kWmPaint <= hi)) {
    for (auto& [h, w] : g_windows) {
      if (w.destroyed || !w.dirty || !w.visible || w.tid != CurrentThread()->tid) continue;
      if (hwnd && h != hwnd) continue;
      *out = Msg{h, kWmPaint, 0, 0, now, g_mouse_x, g_mouse_y};
      return true;
    }
  }
  return false;
}

// WH_KEYBOARD: GetMessage and PeekMessage show each key message they
// return (WM_KEYDOWN, WM_KEYUP, WM_SYSKEYDOWN, WM_SYSKEYUP) to the
// thread's keyboard hooks first: KeyboardProc(HC_ACTION (0), or
// HC_NOREMOVE (3) when the message stays queued, vk, keystroke lParam).
// A nonzero return discards a removed message: true when it was.
bool KeyboardHookDiscards(Ctx& c, const Msg& m, bool removed) {
  if (m.msg != WM_KEYDOWN && m.msg != WM_KEYUP && m.msg != WM_SYSKEYDOWN &&
      m.msg != WM_SYSKEYUP)
    return false;
  uint32_t proc = NextHook(kWhKeyboard, 0);
  if (!proc) return false;
  uint32_t r = CallGuest(c, proc, {removed ? 0u : 3u, m.wparam, m.lparam});
  return removed && r != 0;
}

// Takes the first matching message, as TakeMessage, after the keyboard
// hooks have seen it (and not discarded it).
bool TakeHookedMessage(Ctx& c, uint32_t hwnd, uint32_t lo, uint32_t hi, bool remove,
                       Msg* out) {
  for (;;) {
    if (!TakeMessage(hwnd, lo, hi, remove, out)) return false;
    if (!KeyboardHookDiscards(c, *out, remove)) return true;
  }
}

// Marks hwnd (and with `children`, its descendants) as needing paint.
void Invalidate(uint32_t hwnd, bool children) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (!hwnd) {
    for (auto& [h, w] : g_windows) w.dirty = true;
    return;
  }
  if (Window* w = Win(hwnd)) w->dirty = true;
  if (!children) return;
  for (auto& [h, w] : g_windows)
    if (w.parent == hwnd && (w.style & WS_CHILD)) Invalidate(h, true);
}

// A child window or dialog moved or resized: the part of its parent it
// uncovered needs painting (the parent is invalidated whole), and so does
// the window itself.
void ExposeParent(uint32_t hwnd) {
  uint32_t parent = 0;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w || !w->visible || !((w->style & WS_CHILD) || w->is_dialog)) return;
    parent = w->parent;
  }
  if (parent) Invalidate(parent, false);
  Invalidate(hwnd, false);
}

void Validate(uint32_t hwnd) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (Window* w = Win(hwnd)) w->dirty = false;
}


win32::GameConfig::Fullscreen FullscreenPolicy() {
  using F = win32::GameConfig::Fullscreen;
  static const F f = [] {
    const char* e = Env("FULLSCREEN");
    if (!e || !*e) return Background() ? F::Window : Game().fullscreen;
    if (!std::strcmp(e, "window")) return F::Window;
    if (!std::strcmp(e, "desktop")) return F::Desktop;
    if (!std::strcmp(e, "exclusive")) return F::Exclusive;
    FatalF("FULLSCREEN=%s: expected window, desktop or exclusive", e);
  }();
  return f;
}

// Game sizes (screens, modes, client areas) are pixels; SDL sizes host
// windows in points, which differ on a scaled desktop. Windows are
// created with high pixel density, so a client of w x h pixels is a host
// window of w x h pixels at w / d x h / d points.
int ToPoints(SDL_Window* sdl, int px) {
  float d = sdl ? SDL_GetWindowPixelDensity(sdl) : g_desktop_density;
  if (!(d > 0)) d = 1;
  return std::max(int(std::lround(float(px) / d)), 1);
}

// The screen a window would cover: the display mode in exclusive mode,
// else the screen the game sees.
void ScreenFor(const Window& w, int* sw, int* sh) {
  *sw = w.mode_w ? w.mode_w : g_display_w;
  *sh = w.mode_w ? w.mode_h : g_display_h;
}

// Whether a visible top-level window covers the whole screen the game
// sees: DirectDraw exclusive mode, or a client area spanning the screen
// (a fullscreen window after a display mode change, or a borderless one at
// the desktop's size). Caller holds g_mu.
bool CoversScreen(const Window& w) {
  if (w.destroyed || !w.sdl || !w.visible) return false;
  if (w.mode_w) return true;
  int ox, oy, cw, ch;
  WindowOrigin(w.hwnd, &ox, &oy);
  ClientSize(w, &cw, &ch);
  return ox <= 0 && oy <= 0 && ox + cw >= g_display_w && oy + ch >= g_display_h;
}

// Host fullscreen follows CoversScreen (GameConfig::fullscreen). Called
// after anything that changes a window's placement, visibility or mode,
// or the screen size. Leaving it gives the host window the client size
// again.
void UpdateFullscreen(uint32_t hwnd) {
  using F = win32::GameConfig::Fullscreen;
  SDL_Window* sdl;
  bool want;
  int sw, sh, cw, ch;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w || !w->sdl) return;
    want = FullscreenPolicy() != F::Window && CoversScreen(*w);
    ScreenFor(*w, &sw, &sh);
    if (want == w->fullscreen && (!want || (w->fs_w == sw && w->fs_h == sh))) return;
    w->fullscreen = want;
    w->fs_w = sw;
    w->fs_h = sh;
    sdl = w->sdl;
    GameClientSize(*w, &cw, &ch);
  }
  if (want) {
    // Exclusive: the smallest host mode that holds the game's screen at
    // the display's own aspect (a 4:3 mode on a 16:9 display asks for
    // 16:9 at the same height), so the monitor or compositor never
    // stretches it and present adds the bars. None found, or desktop
    // policy: the desktop's mode.
    // Sizes compared in pixels (a mode's w x h times its pixel density).
    SDL_DisplayMode m;
    bool exclusive = false;
    const SDL_DisplayID display = SDL_GetDisplayForWindow(sdl);
    const SDL_DisplayMode* d = SDL_GetDesktopDisplayMode(display);
    if (FullscreenPolicy() == F::Exclusive && d && d->w > 0 && d->h > 0) {
      int tw = sw, th = sh;
      if (int64_t(sw) * d->h < int64_t(sh) * d->w)
        tw = int((int64_t(sh) * d->w + d->h - 1) / d->h);
      else
        th = int((int64_t(sw) * d->h + d->w - 1) / d->w);
      int n = 0;
      SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(display, &n);
      int64_t best = 0;
      for (int i = 0; i < n; i++) {
        const float k = modes[i]->pixel_density > 0 ? modes[i]->pixel_density : 1.0f;
        const int64_t pw = std::lround(float(modes[i]->w) * k);
        const int64_t ph = std::lround(float(modes[i]->h) * k);
        if (pw < tw || ph < th || (exclusive && pw * ph >= best)) continue;
        m = *modes[i];
        best = pw * ph;
        exclusive = true;
      }
      SDL_free(modes);
    }
    SDL_SetWindowFullscreenMode(sdl, exclusive ? &m : nullptr);
    SDL_SetWindowFullscreen(sdl, true);
    if (exclusive)
      std::fprintf(stderr, "user32: window %X fullscreen for a %dx%d screen, host mode %dx%d\n",
                   hwnd, sw, sh, m.w, m.h);
    else
      std::fprintf(stderr, "user32: window %X fullscreen for a %dx%d screen\n", hwnd, sw, sh);
  } else {
    SDL_SetWindowFullscreen(sdl, false);
    SDL_SetWindowSize(sdl, ToPoints(sdl, cw), ToPoints(sdl, ch));
  }
}

void UpdateFullscreenAll() {
  std::vector<uint32_t> hwnds;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    for (auto& [h, w] : g_windows)
      if (w.sdl && !w.destroyed) hwnds.push_back(h);
  }
  for (uint32_t h : hwnds) UpdateFullscreen(h);
}

}  // namespace

bool OnMainThread() { return std::this_thread::get_id() == g_main_thread_id; }

void EnsureSdlVideo() {
  if (g_sdl_up) return;
  if (!OnMainThread()) FatalF("SDL video requested off the main thread");
  if (Background()) {
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    // On macOS SDL_Init otherwise brings the app to the foreground: the
    // focus would leave whatever the user is doing.
    SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_RAISED, "0");
  }
  // Leave SIGINT/SIGTERM alone: SDL would turn them into quit events the
  // game may ignore, making the process hard to stop.
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  if (!SDL_Init(SDL_INIT_VIDEO)) FatalF("SDL_Init: %s", SDL_GetError());
  g_sdl_up = true;
  platform::AddPlayerMenu();  // an installed app's Check for Updates, Uninstall...
  if (const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay())) {
    // Games see pixels: a scaled desktop (Wayland, macOS) reports points.
    const float d = m->pixel_density > 0 ? m->pixel_density : 1.0f;
    g_desktop_density = d;
    g_display_w = g_desktop_w = int(std::lround(float(m->w) * d));
    // Less the strip a fullscreen window does not get on a display with a
    // camera housing (a notched Mac's): the menu bar's height, where the
    // usable bounds start (one point more than the safe area's top inset:
    // 33 against 32 points on a 16-inch MacBook Pro, whose fullscreen
    // windows are 1084 points tall of 1117). A game that fills the desktop
    // then gets a window, and a swap chain, of the size its fullscreen
    // surface has; otherwise every present is suboptimal, and a game that
    // rebuilds its swap chain on that rebuilds it every frame.
    int inset = 0;
    SDL_Rect full{}, usable{};
    const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    if (platform::PrimaryDisplayTopInset() > 0 && SDL_GetDisplayBounds(primary, &full) &&
        SDL_GetDisplayUsableBounds(primary, &usable) && usable.y > full.y)
      inset = int(std::lround(float(usable.y - full.y) * d));
    g_display_h = g_desktop_h = int(std::lround(float(m->h) * d)) - inset;
    std::fprintf(stderr, "user32: desktop %dx%d pixels (display %dx%d points at %gx, top inset %d)\n",
                 g_desktop_w, g_desktop_h, m->w, m->h, double(d), inset);
    if (m->refresh_rate > 0) g_desktop_hz = int(std::lround(m->refresh_rate));
  }
  // Test hook: <ID>_DESKTOP=WxH (pixels) is the desktop the game sees, so
  // automated runs of games that size their window to the desktop stay
  // in a window smaller than the host's.
  if (const char* e = Env("DESKTOP"); e && *e) {
    int w, h;
    if (std::sscanf(e, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0)
      FatalF("DESKTOP=%s: expected WxH", e);
    g_display_w = g_desktop_w = w;
    g_display_h = g_desktop_h = h;
  }
}

void DesktopSize(int* w, int* h) {
  if (OnMainThread()) EnsureSdlVideo();
  *w = g_desktop_w;
  *h = g_desktop_h;
}

int DesktopRefreshRate() {
  if (OnMainThread()) EnsureSdlVideo();
  return g_desktop_hz;
}

namespace {

// Test hook: <ID>_TEST_FOCUS="lost,gained" (seconds since the first pump)
// injects window focus loss and regain, to exercise the game's
// deactivation path without touching the desktop.
void InjectTestFocus() {
  static int stage = -1;
  static double lost = 0, gained = 0;
  static Uint64 t0 = 0;
  if (stage == -1) {
    stage = 2;
    const char* env = Env("TEST_FOCUS");
    if (env && std::sscanf(env, "%lf,%lf", &lost, &gained) == 2) stage = 0;
    t0 = SDL_GetTicks();
  }
  if (stage >= 2) return;
  double t = (SDL_GetTicks() - t0) / 1000.0;
  if (t < (stage == 0 ? lost : gained)) return;
  uint32_t hwnd = 0;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    for (auto& [h, w] : g_windows)
      if (w.sdl && !w.destroyed) hwnd = h;
  }
  if (!hwnd) return;
  WarnF("TEST_FOCUS: focus %s at %.1f s\n", stage == 0 ? "lost" : "gained", t);
  if (stage == 0) {
    for (int vk = 0; vk < 256; vk++) g_key_down[vk] = 0;
    ActivateApp(hwnd, false);
  } else {
    ActivateApp(hwnd, true);
  }
  stage++;
}

// Test hook: <ID>_TEST_INPUT=<file> replays scripted input, one command
// per line, times in seconds since the first event pump:
//   <t> goto <x> <y> [step]   move the game's cursor to client (x, y): it
//                         is driven by relative motion, so this homes it to
//                         the top-left corner first, over a few frames, then
//                         moves in steps of at most `step` px (default 200;
//                         small ones for games that accelerate the cursor)
//   <t> move <dx> <dy>    relative motion
//   <t> pointer <x> <y>   the host pointer is at client pixel (x, y)
//                         (the game's cursor should follow it exactly),
//                         independent of how the frame is shown
//   <t> click [left|right] [seconds]   held for that long (default 0.08)
//   <t> wheel <notches>   turn the wheel (positive: away from the user);
//                         a fraction is a fine-grained (trackpad) step
//   <t> key <SDL scancode name> [seconds]   e.g. Escape, Return, Space, A;
//                         held for that long (default 0.08)
//   <t> warp <x> <y>      to client (x, y) in one relative move
//   <t> shot <name>       save the next frame as $<ID>_SHOT_DIR/<name>.bmp
//                         (tools/smoke.py compares it with a reference)
//   <t> wait <timeout> <x>,<y>=<rrggbb>... [tol <n>] [press <key> <every>]
//                         stop the script's clock until every listed pixel
//                         of the presented frame (back buffer pixels, as in
//                         the shots) is that colour, within n per channel
//                         (default 24), and every <x>,<y>!=<rrggbb> one is
//                         not (a dialog's text absent, say); meanwhile press
//                         <key> every <every> seconds (to skip videos, say;
//                         frames within 0.3 s of a press don't count, so a
//                         press that opened something is seen). Later times count
//                         on from <t> once it matches. After <timeout>
//                         seconds (times <ID>_TEST_WAIT_SCALE) it gives up,
//                         warns "timed out" (a smoke failure) and goes on.
//   <t> exit              end the process (status 0) once any shot is
//                         saved: the run need not wait out its time
// '#' starts a comment. Each match and timeout is logged with the
// script's line number and how long it waited, to tune scripts by.
struct TestEvent {
  double t;
  SDL_Event e;
};

void InjectTestInput() {
  static std::vector<TestEvent> q;
  static size_t next = 0;
  static Uint64 t0 = 0;
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    t0 = SDL_GetTicks();
    const char* path = Env("TEST_INPUT");
    if (!path) return;
    FILE* f = std::fopen(path, "r");
    if (!f) {
      WarnF("TEST_INPUT: cannot open %s\n", path);
      return;
    }
    char line[512];
    int lineno = 0;
    // The latest release (click's button up, key up) of the lines so far:
    // a wait starts no earlier, or it would hold back the release of the
    // click before it (games act on the release).
    double last_release = 0;
    auto motion = [](double t, float dx, float dy) {
      SDL_Event e{};
      e.type = SDL_EVENT_MOUSE_MOTION;
      e.motion.which = kTestRelative;
      e.motion.xrel = dx;
      e.motion.yrel = dy;
      return TestEvent{t, e};
    };
    while (std::fgets(line, sizeof line, f)) {
      lineno++;
      if (char* h = std::strchr(line, '#')) *h = 0;
      double t;
      char cmd[32] = {}, a[64] = {}, b[64] = {};
      int n = std::sscanf(line, "%lf %31s %63s %63s", &t, cmd, a, b);
      if (n < 2) continue;
      std::string c = cmd;
      if (c == "goto" && n >= 4) {
        // Home: two big moves (the virtual cursor clamps at the edge and
        // the game re-centres it every frame), then steps of <= 200 px
        // (or the given step, for games that accelerate large motion),
        // 0.1 s apart (proportionally less for smaller steps).
        char st[32] = {};
        std::sscanf(line, "%*f %*s %*s %*s %31s", st);
        float step = *st ? std::max(1.0f, float(std::atof(st))) : 200.0f;
        q.push_back(motion(t, -4000, -4000));
        q.push_back(motion(t + 0.1, -4000, -4000));
        q.push_back(motion(t + 0.2, -4000, -4000));
        float x = std::atof(a), y = std::atof(b);
        double tt = t + 0.3, gap = std::max(0.03, 0.1 * step / 200.0);
        while (x > 0 || y > 0) {
          float sx = std::min(x, step), sy = std::min(y, step);
          if (*st) {  // a given step: both axes in proportion (a straight path)
            float k = std::min(1.0f, step / std::max(x, y));
            sx = x - x * k < 0.5f ? x : x * k;
            sy = y - y * k < 0.5f ? y : y * k;
          }
          q.push_back(motion(tt, sx, sy));
          x -= sx;
          y -= sy;
          tt += gap;
        }
      } else if (c == "pointer" && n == 4) {
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.which = kTestPointer;
        e.motion.x = std::atof(a);
        e.motion.y = std::atof(b);
        q.push_back({t, e});
      } else if (c == "move" && n == 4) {
        q.push_back(motion(t, std::atof(a), std::atof(b)));
      } else if (c == "warp" && n == 4) {
        // To a client point in one relative move computed when it is
        // injected (no homing: a cursor at the screen edge scrolls many
        // games' maps). Marked by `which` = kWarp, x/y = the target.
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.which = kWarp;
        e.motion.x = std::atof(a);
        e.motion.y = std::atof(b);
        q.push_back({t, e});
      } else if (c == "shot" && n >= 3) {
        SDL_Event e{};
        e.type = kShotEvent;
        e.user.code = int32_t(g_shot_names.size());
        g_shot_names.push_back(a);
        q.push_back({t, e});
      } else if (c == "click") {
        bool right = n >= 3 && std::string(a) == "right";
        double hold = n >= 4 ? std::atof(b) : 0.08;
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.button = right ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
        e.button.down = true;
        e.button.clicks = 1;
        q.push_back({t, e});
        e.type = SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.down = false;
        q.push_back({t + hold, e});
        last_release = std::max(last_release, t + hold);
      } else if (c == "wheel" && n >= 3) {
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_WHEEL;
        e.wheel.which = kTestPointer;
        e.wheel.y = float(std::atof(a));
        e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
        q.push_back({t, e});
      } else if (c == "key" && n >= 3) {
        SDL_Scancode sc = SDL_GetScancodeFromName(a);
        if (sc == SDL_SCANCODE_UNKNOWN) {
          WarnF("TEST_INPUT: unknown key %s\n", a);
          continue;
        }
        SDL_Event e{};
        e.type = SDL_EVENT_KEY_DOWN;
        e.key.scancode = sc;
        e.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
        e.key.down = true;
        q.push_back({t, e});
        e.type = SDL_EVENT_KEY_UP;
        e.key.down = false;
        q.push_back({t + (n >= 4 ? std::atof(b) : 0.08), e});
        last_release = std::max(last_release, t + (n >= 4 ? std::atof(b) : 0.08));
      } else if (c == "wait" && n >= 4) {
        TestWait w;
        w.line = lineno;
        std::vector<std::string> tok;
        for (char* p = std::strtok(line, " \t\r\n"); p; p = std::strtok(nullptr, " \t\r\n"))
          tok.push_back(p);
        bool ok = tok.size() >= 4;
        // <ID>_TEST_WAIT_SCALE: timeouts scaled (tools/smoke.py -j N sets N:
        // with N games loading at once, each loads slower).
        static const double scale = [] {
          const char* e = Env("TEST_WAIT_SCALE");
          double v = e ? std::atof(e) : 1.0;
          return v > 0 ? v : 1.0;
        }();
        if (ok) w.timeout = std::atof(tok[2].c_str()) * scale;
        for (size_t i = 3; ok && i < tok.size(); i++) {
          int x, y;
          unsigned rgb;
          if (tok[i] == "tol" && i + 1 < tok.size()) {
            w.tolerance = std::atoi(tok[++i].c_str());
          } else if (tok[i] == "press" && i + 2 < tok.size()) {
            w.press = SDL_GetScancodeFromName(tok[i + 1].c_str());
            w.every = std::atof(tok[i + 2].c_str());
            ok = w.press != SDL_SCANCODE_UNKNOWN && w.every > 0;
            i += 2;
          } else if (std::sscanf(tok[i].c_str(), "%d,%d!=%x", &x, &y, &rgb) == 3) {
            w.points.push_back({x, y});
            w.colours.push_back(rgb & 0xFFFFFF);
            w.unlike.push_back(true);
          } else if (std::sscanf(tok[i].c_str(), "%d,%d=%x", &x, &y, &rgb) == 3) {
            w.points.push_back({x, y});
            w.colours.push_back(rgb & 0xFFFFFF);
            w.unlike.push_back(false);
          } else {
            ok = false;
          }
        }
        if (!ok || w.points.empty() || w.timeout <= 0) {
          WarnF("TEST_INPUT: bad wait (line %d)\n", lineno);
          continue;
        }
        SDL_Event e{};
        e.type = kWaitEvent;
        e.user.code = int32_t(g_waits.size());
        g_waits.push_back(std::move(w));
        q.push_back({std::max(t, last_release + 0.001), e});
      } else if (c == "exit") {
        SDL_Event e{};
        e.type = kExitEvent;
        q.push_back({t, e});
      } else {
        WarnF("TEST_INPUT: bad line: %s", line);
      }
    }
    std::fclose(f);
    std::stable_sort(q.begin(), q.end(),
                     [](const TestEvent& x, const TestEvent& y) { return x.t < y.t; });
    WarnF("TEST_INPUT: %zu events from %s\n", q.size(), path);
  }
  // Key-up of a press made while waiting.
  static double press_up_at = -1;
  static SDL_Event press_up{};
  const double now = (SDL_GetTicks() - t0) / 1000.0;
  auto send_key = [&](SDL_Event e) {
    SDL_WindowID wid = 0;
    {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      for (auto& [h, w] : g_windows)
        if (w.sdl && !w.destroyed) wid = SDL_GetWindowID(w.sdl);
    }
    // With no host window (only a dialog without an owner, say), windowID
    // 0: the key goes to the focus window.
    e.key.windowID = wid;
    HandleSdlEvent(e);
  };
  if (press_up_at >= 0 && now >= press_up_at) {
    press_up_at = -1;
    send_key(press_up);
  }
  if (next >= q.size()) return;
  // The script's clock: real time less the time spent in waits.
  static double paused = 0;
  static double wait_since = -1, last_press = -1e9;
  double t = now - paused;
  SDL_WindowID id = 0;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    for (auto& [h, w] : g_windows)
      if (w.sdl && !w.destroyed) id = SDL_GetWindowID(w.sdl);
  }
  if (!id) return;
  // At most one motion per pump, so each lands in its own game frame.
  bool moved = false;
  while (next < q.size() && q[next].t <= t) {
    SDL_Event e = q[next].e;
    if (e.type == kShotEvent) {
      gfx::RequestShot(g_shot_names[size_t(e.user.code)].c_str());
      next++;
      continue;
    }
    if (e.type == kExitEvent) {
      if (gfx::ShotPending()) break;  // after the shot is saved
      WarnF("TEST_INPUT: exit at %.1f s, %d frames presented\n", now, gfx::FramesPresented());
      std::fflush(nullptr);
      std::_Exit(0);
    }
    if (e.type == kWaitEvent) {
      const TestWait& w = g_waits[size_t(e.user.code)];
      if (wait_since < 0) {
        wait_since = now;
        last_press = -1e9;
        gfx::RequestProbe(w.points);
      }
      std::vector<uint32_t> px;
      bool matched = false;
      // A frame probed within 0.3 s of a press may not show its effect
      // yet (the press opening a dialog, say): such results don't count.
      const bool settled = w.press == SDL_SCANCODE_UNKNOWN || now - last_press >= 0.3;
      if (gfx::TakeProbe(&px) && settled) {
        matched = px.size() == w.colours.size();
        for (size_t i = 0; matched && i < px.size(); i++) {
          bool same = true;
          for (int sh = 0; sh < 24; sh += 8)
            if (std::abs(int((px[i] >> sh) & 255) - int((w.colours[i] >> sh) & 255)) > w.tolerance)
              same = false;
          if (same == w.unlike[i]) matched = false;
        }
        if (!matched && now - wait_since >= w.timeout) {
          std::string have;
          for (size_t i = 0; i < px.size(); i++) {
            char b[48];
            std::snprintf(b, sizeof b, " %d,%d=%06x", w.points[i].first, w.points[i].second,
                          px[i] & 0xFFFFFF);
            have += b;
          }
          WarnF("TEST_INPUT: wait (line %d) timed out after %.1f s; the frame has%s\n", w.line,
                now - wait_since, have.c_str());
          matched = true;  // go on: the shot will tell
        } else if (matched) {
          WarnF("TEST_INPUT: wait (line %d) matched after %.1f s\n", w.line, now - wait_since);
        }
      }
      if (matched) {
        paused += (now - wait_since);
        t = now - paused;
        wait_since = -1;
        next++;
        continue;
      }
      // Ask for the next frame's pixels, at most every 100 ms.
      static double asked = -1e9;
      if (now - asked >= 0.1) {
        asked = now;
        gfx::RequestProbe(w.points);
      }
      if (w.press != SDL_SCANCODE_UNKNOWN && now - last_press >= w.every && press_up_at < 0) {
        last_press = now;
        SDL_Event k{};
        k.type = SDL_EVENT_KEY_DOWN;
        k.key.scancode = w.press;
        k.key.key = SDL_GetKeyFromScancode(w.press, SDL_KMOD_NONE, false);
        k.key.down = true;
        send_key(k);
        k.type = SDL_EVENT_KEY_UP;
        k.key.down = false;
        press_up = k;
        press_up_at = now + 0.08;
      }
      break;
    }
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
      if (moved) break;
      moved = true;
      e.motion.windowID = id;
      if (e.motion.which == kWarp) {
        uint32_t h = HwndForSdl(id);
        int ox = 0, oy = 0;
        if (h) WindowOrigin(h, &ox, &oy);
        e.motion.which = kTestRelative;
        e.motion.xrel = e.motion.x - float(g_mouse_x - ox);
        e.motion.yrel = e.motion.y - float(g_mouse_y - oy);
      }
    } else if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) {
      e.key.windowID = id;
    } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
      e.wheel.windowID = id;
    } else {
      e.button.windowID = id;
    }
    HandleSdlEvent(e);
    next++;
  }
}

// Test hook: <ID>_TEST_STALL="at,seconds" blocks the main thread once, to
// exercise the hang watchdog.
void InjectTestStall() {
  static double at = -1, dur = 0;
  static bool done = false;
  static Uint64 t0 = SDL_GetTicks();
  if (done) return;
  if (at < 0) {
    const char* env = Env("TEST_STALL");
    if (!env || std::sscanf(env, "%lf,%lf", &at, &dur) != 2) {
      done = true;
      return;
    }
  }
  if ((SDL_GetTicks() - t0) / 1000.0 < at) return;
  done = true;
  WarnF("TEST_STALL: blocking the main thread for %.1f s\n", dur);
  std::this_thread::sleep_for(std::chrono::duration<double>(dur));
}

void (*g_host_event_sink)(const SDL_Event&) = nullptr;

}  // namespace

void SetHostEventSink(void (*sink)(const SDL_Event&)) { g_host_event_sink = sink; }

// The SDL window of a top-level `win`, hidden, sized to its client area.
void CreateHostWindow(Window& win) {
  EnsureSdlVideo();
  int cw, ch;
  ClientSize(win, &cw, &ch);
  win.sdl = SDL_CreateWindow(win32::AnsiToUtf8(win.title).c_str(), ToPoints(nullptr, cw), ToPoints(nullptr, ch),
                             SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                 gfx::ChosenWindowFlags());
  if (!win.sdl) FatalF("SDL_CreateWindow: %s", SDL_GetError());
}

uint32_t CreateSdl2Window(const std::string& title_utf8, int w, int h, SDL_WindowFlags flags,
                          SDL_Window** sdl) {
  EnsureSdlVideo();
  Window win;
  win.sdl = SDL_CreateWindow(title_utf8.c_str(), ToPoints(nullptr, w), ToPoints(nullptr, h),
                             flags | (Headless() ? SDL_WINDOW_HIDDEN : 0) |
                                 SDL_WINDOW_HIGH_PIXEL_DENSITY | gfx::ChosenWindowFlags());
  if (!win.sdl) return 0;
  win.title = title_utf8;
  win.w = w;
  win.h = h;
  win.visible = !(flags & SDL_WINDOW_HIDDEN);
  win.tid = CurrentThread()->tid;
  std::lock_guard<std::recursive_mutex> l(g_mu);
  win.hwnd = g_next_hwnd;
  g_next_hwnd += 0x10;
  *sdl = win.sdl;
  uint32_t hwnd = win.hwnd;
  g_windows[hwnd] = std::move(win);
  return hwnd;
}

void DestroySdl2Window(uint32_t hwnd) {
  SDL_Window* sdl = nullptr;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    auto it = g_windows.find(hwnd);
    if (it == g_windows.end()) return;
    sdl = it->second.sdl;
    g_windows.erase(it);
  }
  if (sdl) SDL_DestroyWindow(sdl);
}

namespace {

void DeliverHostEvent(const SDL_Event& e) {
  if (g_host_event_sink)
    g_host_event_sink(e);
  else
    HandleSdlEvent(e);
}

}  // namespace

void PumpHostEvents() {
  if (!g_sdl_up || !OnMainThread()) return;
  InjectTestStall();
  // The window system is asked for new events at most every 2 ms: games
  // of the DirectDraw era call PeekMessage in tight loops (between movie
  // frames, say), and each Cocoa pump costs about a millisecond. Events
  // already queued are delivered on every call.
  static Uint64 last_pump_ns = 0;
  Uint64 now_ns = SDL_GetTicksNS();
  if (now_ns - last_pump_ns >= 2000000) {
    last_pump_ns = now_ns;
    SDL_PumpEvents();
  }
  SDL_Event e;
  while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) > 0) {
    // Automated runs take input only from <ID>_TEST_INPUT: their windows
    // are on someone's desktop and stray clicks must not drive them.
    if (Background() && (e.type == SDL_EVENT_MOUSE_MOTION ||
                         e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                         e.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                         e.type == SDL_EVENT_MOUSE_WHEEL ||
                         e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP ||
                         e.type == SDL_EVENT_WINDOW_FOCUS_GAINED ||
                         e.type == SDL_EVENT_WINDOW_FOCUS_LOST ||
                         e.type == SDL_EVENT_WINDOW_MOUSE_LEAVE))
      continue;
    DeliverHostEvent(e);
  }
  InjectTestFocus();
  InjectTestInput();
  if (g_pump_present) g_pump_present();
  WatchdogHeartbeat();
}

void (*g_pump_present)() = nullptr;

uint32_t DcForWindow(uint32_t hwnd) { return hwnd ? 0x40000000u | hwnd : 0x00D00001u; }

uint32_t WindowForDc(uint32_t hdc) { return hdc & 0x40000000u ? hdc & ~0x40000000u : 0; }

SDL_Window* SdlWindowFor(uint32_t hwnd) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(hwnd);
  return w ? w->sdl : nullptr;
}

void SetExclusiveMode(uint32_t hwnd, int w, int h) {
  SDL_Window* sdl;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* win = Win(hwnd);
    if (!win) return;
    win->mode_w = w;
    win->mode_h = h;
    sdl = win->fullscreen ? nullptr : win->sdl;
    // A DirectDraw mode switch changes the screen the game sees
    // (GetSystemMetrics), as ChangeDisplaySettings does, and fullscreen
    // exclusive DirectDraw sizes the cooperative window to cover it.
    g_display_w = w ? w : g_desktop_w;
    g_display_h = w ? h : g_desktop_h;
    if (w && !(win->style & WS_CHILD)) {
      win->x = win->y = 0;
      win->w = w;
      win->h = h;
    }
  }
  if (sdl && w) SDL_SetWindowSize(sdl, ToPoints(sdl, w), ToPoints(sdl, h));
  UpdateFullscreen(hwnd);
}

void ShowWindowNoActivate(uint32_t hwnd) {
  SDL_Window* sdl;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* win = Win(hwnd);
    if (!win || win->visible) return;
    win->visible = true;
    sdl = win->sdl;
  }
  if (sdl && !Headless()) SDL_ShowWindow(sdl);
  UpdateFullscreen(hwnd);
}

std::vector<int> DisplayModeRates() {
  const int hz = DesktopRefreshRate();
  if (hz == 60 || hz <= 0) return {60};
  return hz < 60 ? std::vector<int>{hz, 60} : std::vector<int>{60, hz};
}

std::vector<ModeSize> DisplayModeSizes() {
  static const ModeSize kSizes[] = {
      {640, 480},   {800, 600},   {1024, 768},  {1152, 864},  {1280, 960},
      {1280, 1024}, {1600, 1200}, {1280, 720},  {1366, 768},  {1600, 900},
      {1920, 1080}, {1920, 1200}, {2560, 1080}, {2560, 1440}, {3440, 1440},
      {3840, 2160}};
  EnsureSdlVideo();
  // Never smaller than a 1024x768 desktop, so the classic modes stay.
  const int mw = std::max(g_desktop_w, 1024), mh = std::max(g_desktop_h, 768);
  std::vector<ModeSize> v{{g_desktop_w, g_desktop_h}};
  for (const ModeSize& s : kSizes)
    if (s.w <= mw && s.h <= mh) v.push_back(s);
  std::sort(v.begin(), v.end(),
            [](const ModeSize& a, const ModeSize& b) { return a.w != b.w ? a.w < b.w : a.h < b.h; });
  v.erase(std::unique(v.begin(), v.end(),
                      [](const ModeSize& a, const ModeSize& b) { return a.w == b.w && a.h == b.h; }),
          v.end());
  return v;
}

void ClientAreaSize(uint32_t hwnd, int* w, int* h) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  *w = *h = 0;
  if (Window* win = Win(hwnd)) GameClientSize(*win, w, h);
}

namespace hle {
namespace {

using namespace win32;

// ---- classes ---------------------------------------------------------------

// g_classes key of a class name argument: an atom (MAKEINTATOM) or a name.
std::string ClassKey(uint32_t name) {
  return name < 0x10000 ? "#" + std::to_string(name) : Lower(GStr(name));
}

// The system control classes every process has (see "built-in control
// classes" below). Their window procedures are host handlers behind
// thunks, so games can subclass them and chain with CallWindowProc.
constexpr uint32_t kDlgWindowExtraBytes = 30;  // DLGWINDOWEXTRA

void EnsureSystemClasses() {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (g_classes.count("edit")) return;
  for (const char* n : {"static", "edit", "button", "listbox", "combobox", "scrollbar"}) {
    WndClass k;
    k.name = n;
    k.wndproc = ThunkFor("user32.dll", std::string("<") + n + ">");
    k.atom = g_next_atom++;
    g_classes[n] = k;
  }
  // The trackbar (comctl32) and the dialog class #32770 (atom 32770).
  for (auto [name, proc] : {std::pair<const char*, const char*>{"msctls_trackbar32", "<trackbar>"},
                            {"#32770", "<dialog>"}}) {
    WndClass k;
    k.name = name;
    k.wndproc = ThunkFor("user32.dll", proc);
    k.wnd_extra = std::string(name) == "#32770" ? kDlgWindowExtraBytes : 0;
    k.atom = g_next_atom++;
    g_classes[name] = k;
  }
}

// WNDCLASSA at wc, or the same fields 4 bytes on in WNDCLASSEXA (after
// cbSize).
uint32_t RegisterWndClass(uint32_t wc) {
  WndClass k;
  k.style = Rd32(wc + 0);
  k.wndproc = Rd32(wc + 4);
  k.wnd_extra = Rd32(wc + 12);
  k.name = ClassKey(Rd32(wc + 36));
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (g_classes.count(k.name)) {
    SetLastError(kErrorClassAlreadyExists);
    return 0;
  }
  k.atom = g_next_atom++;
  g_classes[k.name] = k;
  return k.atom;
}

HLE("user32.dll", RegisterClassA, 1) { return RegisterWndClass(Arg(c, 0)); }

HLE("user32.dll", RegisterClassExA, 1) {
  uint32_t wc = Arg(c, 0);
  if (Rd32(wc) != 48) {  // cbSize: sizeof(WNDCLASSEXA)
    SetLastError(87);    // ERROR_INVALID_PARAMETER
    return 0;
  }
  return RegisterWndClass(wc + 4);
}

// GetClassInfoA(hinstance, name, WNDCLASSA*): what was registered (icons,
// cursor, brush and menu are not kept, so they read back as 0). Returns
// the class atom, or 0 and ERROR_CLASS_DOES_NOT_EXIST.
HLE("user32.dll", GetClassInfoA, 3) {
  EnsureSystemClasses();
  uint32_t name = Arg(c, 1), wc = Arg(c, 2);
  std::lock_guard<std::recursive_mutex> l(g_mu);
  const WndClass* k = nullptr;
  if (name < 0x10000) {
    for (auto& [n, cls] : g_classes)
      if (cls.atom == name) k = &cls;
  } else {
    auto it = g_classes.find(ClassKey(name));
    if (it != g_classes.end()) k = &it->second;
  }
  if (!k) {
    SetLastError(1411);  // ERROR_CLASS_DOES_NOT_EXIST
    return 0;
  }
  std::memset(GPtr(wc), 0, 40);
  Wr32(wc + 0, k->style);
  Wr32(wc + 4, k->wndproc);
  Wr32(wc + 12, k->wnd_extra);
  Wr32(wc + 16, Arg(c, 0));
  Wr32(wc + 36, name);
  return k->atom;
}

HLE("user32.dll", UnregisterClassA, 2) {
  std::string n = ClassKey(Arg(c, 0));
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return g_classes.erase(n) ? 1 : 0;
}

HLE("user32.dll", SetClassLongA, 3) {
  // GCL_HCURSOR (-12), GCL_HICON (-14)... cosmetic; remember nothing.
  return 0;
}

// ---- windows ---------------------------------------------------------------

// Set while the dialog code creates a dialog window: it gets no host window.
thread_local bool t_creating_dialog = false;

HLE("user32.dll", CreateWindowExA, 12) {
  uint32_t exstyle = Arg(c, 0), cls_va = Arg(c, 1), title_va = Arg(c, 2),
           style = Arg(c, 3), parent = Arg(c, 8), menu = Arg(c, 9),
           hinst = Arg(c, 10), param = Arg(c, 11);
  int32_t x = int32_t(Arg(c, 4)), y = int32_t(Arg(c, 5)), w = int32_t(Arg(c, 6)),
          h = int32_t(Arg(c, 7));
  std::string cls = ClassKey(cls_va);
  EnsureSystemClasses();
  Window win;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    auto it = g_classes.find(cls);
    if (it == g_classes.end()) {
      WarnF("CreateWindowExA: unknown class %s\n", cls.c_str());
      SetLastError(kErrorCannotFindWndClass);
      return 0;
    }
    win.hwnd = g_next_hwnd;
    g_next_hwnd += 0x10;
    win.cls = cls;
    win.wndproc = it->second.wndproc;
    win.class_style = it->second.style;
    win.extra.assign(it->second.wnd_extra, 0);
  }
  constexpr int32_t kUseDefault = int32_t(0x80000000);
  if (x == kUseDefault) x = y = 0;
  if (w == kUseDefault) {
    w = 640;
    h = 480;
  }
  win.style = style;
  win.exstyle = exstyle;
  win.parent = parent;
  win.id = (style & WS_CHILD) ? menu : 0;
  win.hinst = hinst;
  win.x = x;
  win.y = y;
  win.w = w;
  win.h = h;
  win.title = title_va ? GStr(title_va) : "";
  win.tid = CurrentThread()->tid;
  win.is_dialog = t_creating_dialog;
  // A top-level window gets its host window here, on the main thread.
  // Windows created by other threads (games keep hidden helper windows
  // on worker threads, as message targets) get theirs when first shown:
  // SDL, and Cocoa on macOS, create windows only on the main thread.
  if (!(style & WS_CHILD) && !win.is_dialog && OnMainThread()) CreateHostWindow(win);
  uint32_t hwnd = win.hwnd;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    g_windows[hwnd] = std::move(win);
  }
  RECOMP_TRACE("  CreateWindowExA(%s, \"%s\", style %08X, %d,%d %dx%d) -> %X\n",
            cls.c_str(), GStr(title_va).c_str(), style, x, y, w, h, hwnd);

  // CREATESTRUCTA in a process-heap block.
  uint32_t cs = HeapAllocProcess(48, true);
  Wr32(cs + 0, param);
  Wr32(cs + 4, hinst);
  Wr32(cs + 8, menu);
  Wr32(cs + 12, parent);
  Wr32(cs + 16, uint32_t(h));
  Wr32(cs + 20, uint32_t(w));
  Wr32(cs + 24, uint32_t(y));
  Wr32(cs + 28, uint32_t(x));
  Wr32(cs + 32, style);
  Wr32(cs + 36, title_va);
  Wr32(cs + 40, cls_va);
  Wr32(cs + 44, exstyle);
  // HCBT_CREATEWND (3) to the WH_CBT hooks, with a CBT_CREATEWNDA
  // {lpcs, hwndInsertAfter}: nonzero fails the creation.
  bool ok = true;
  if (uint32_t proc = NextCbtHook(0)) {
    uint32_t cbt = HeapAllocProcess(8, true);
    Wr32(cbt, cs);
    ok = CallGuest(c, proc, {3, hwnd, cbt}) == 0;
    HeapFreeProcess(cbt);
  }
  if (ok) ok = Send(hwnd, WM_NCCREATE, 0, cs) != 0;
  if (ok) ok = int32_t(Send(hwnd, WM_CREATE, 0, cs)) != -1;
  HeapFreeProcess(cs);
  if (!ok) {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (Window* wp = Win(hwnd)) {
      if (wp->sdl) SDL_DestroyWindow(wp->sdl);
      wp->destroyed = true;
    }
    return 0;
  }
  if (style & WS_VISIBLE) {
    {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      if (Window* wp = Win(hwnd)) {
        wp->visible = true;
        wp->dirty = true;
        if (wp->sdl && !Headless()) SDL_ShowWindow(wp->sdl);
      }
    }
    UpdateFullscreen(hwnd);
  }
  return hwnd;
}

HLE("user32.dll", DestroyWindow, 1) {
  uint32_t hwnd = Arg(c, 0);
  uint32_t exposed = 0;  // the parent a visible child uncovers
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return 0;
    if (w->visible && ((w->style & WS_CHILD) || w->is_dialog)) exposed = w->parent;
  }
  Send(hwnd, WM_DESTROY, 0, 0);
  Send(hwnd, WM_NCDESTROY, 0, 0);
  // As on Windows, the parent repaints what the child covered (a game's own
  // drop-down list, drawn over its dialog, goes away when it is destroyed).
  if (exposed) Invalidate(exposed, false);
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (Window* w = Win(hwnd)) {
    if (g_clip.hwnd == hwnd) {
      // The clip stays; AttachClip gives it to the next window over it.
      g_clip.hwnd = 0;
      if (w->sdl) SDL_SetWindowRelativeMouseMode(w->sdl, false);
    }
    if (w->sdl) SDL_DestroyWindow(w->sdl);
    w->sdl = nullptr;
    w->destroyed = true;
  }
  std::erase_if(g_timers, [&](const Timer& t) { return t.hwnd == hwnd; });
  if (g_focus == hwnd) g_focus = 0;
  if (g_active == hwnd) g_active = 0;
  return 1;
}

HLE("user32.dll", IsWindow, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return Win(Arg(c, 0)) ? 1 : 0;
}

void ApplyRect(uint32_t hwnd, int x, int y, int w, int h, bool move, bool size);

// SW_SHOWMAXIMIZED: the window takes the screen, its border outside it and
// its caption (if any) at the top, as Windows places a maximized window.
// A window without caption or frame then covers the whole screen, which is
// how games make a borderless fullscreen window (UpdateFullscreen). Showing
// it normal (SW_SHOWNORMAL, SW_RESTORE) puts the previous rect back.
void SetMaximized(uint32_t hwnd, bool on) {
  int x, y, w, h;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* win = Win(hwnd);
    if (!win || (win->style & WS_CHILD) || win->maximized == on) return;
    win->maximized = on;
    if (on) {
      win->nx = win->x;
      win->ny = win->y;
      win->nw = win->w;
      win->nh = win->h;
      int border, caption, sw, sh;
      FrameSize(win->style, &border, &caption);
      ScreenFor(*win, &sw, &sh);
      x = -border;
      y = -border;
      w = sw + 2 * border;
      h = sh + 2 * border;
    } else {
      x = win->nx;
      y = win->ny;
      w = win->nw;
      h = win->nh;
    }
  }
  ApplyRect(hwnd, x, y, w, h, true, true);
}

HLE("user32.dll", ShowWindow, 2) {
  uint32_t hwnd = Arg(c, 0), cmd = Arg(c, 1);
  bool was, show = cmd != 0;  // SW_HIDE = 0
  // SW_SHOWMINIMIZED, SW_MINIMIZE, SW_SHOWMINNOACTIVE
  bool minimize = cmd == 2 || cmd == 6 || cmd == 7;
  // SW_SHOWNORMAL, SW_SHOWMAXIMIZED, SW_RESTORE, SW_SHOWDEFAULT
  bool restore = cmd == 1 || cmd == 3 || cmd == 9 || cmd == 10;
  // Only these make the window the active one; minimizing hands
  // activation to some other window (and the game minimizes itself from
  // its WM_ACTIVATEAPP(0) handler, so activating here would loop).
  bool activates = restore || cmd == 5;
  SDL_Window* sdl;
  bool exclusive;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return 0;
    was = w->visible;
    w->visible = show;
    if (show && !was) Invalidate(hwnd, true);
    if (!show && was && w->parent) Invalidate(w->parent, false);
    if (minimize) w->minimized = true;
    if (restore) w->minimized = false;
    // A top-level window created off the main thread has no host window
    // yet (CreateWindowExA): it gets one when it is first shown.
    if (show && !w->sdl && !(w->style & WS_CHILD) && !w->is_dialog && !w->destroyed) {
      if (!OnMainThread())
        FatalF("ShowWindow(%08X): a window created off the main thread is shown from "
               "another thread; host windows can only be made on the main thread",
               hwnd);
      CreateHostWindow(*w);
    }
    sdl = w->sdl;
    exclusive = w->mode_w != 0;
  }
  if (sdl) {
    if (!show) {
      SDL_HideWindow(sdl);
    } else {
      if (!Headless()) SDL_ShowWindow(sdl);
      // Behind an exclusive-mode "screen" the game's minimize on
      // deactivation is not carried out: the window simply stays put
      // while the game idles, and focusing it again resumes the game.
      if (minimize && !exclusive) SDL_MinimizeWindow(sdl);
      if (restore && !exclusive) SDL_RestoreWindow(sdl);
    }
  }
  if (cmd == 3) SetMaximized(hwnd, true);              // SW_SHOWMAXIMIZED
  else if (cmd == 1 || cmd == 9) SetMaximized(hwnd, false);  // SW_SHOWNORMAL, SW_RESTORE
  UpdateFullscreen(hwnd);
  if (show != was) Send(hwnd, WM_SHOWWINDOW, show ? 1 : 0, 0);
  bool activate = false;
  {
    // A shown top-level window becomes the active app window. Record that
    // before sending: the game's activation handler calls ShowWindow
    // again, which must then see it as already active.
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (activates && sdl && g_active != hwnd) {
      g_active = g_focus = hwnd;
      activate = true;
    }
  }
  if (activate) {
    Send(hwnd, WM_ACTIVATEAPP, 1, 0);
    Send(hwnd, WM_ACTIVATE, 1, 0);
    Send(hwnd, WM_SETFOCUS, 0, 0);
  }
  return was ? 1 : 0;
}

// CloseWindow minimizes (it does not destroy).
HLE("user32.dll", CloseWindow, 1) {
  uint32_t show = ProcAddress(ModuleHandle("user32.dll"), "ShowWindow");
  CallGuest(c, show, {Arg(c, 0), 6 /* SW_MINIMIZE */});
  return 1;
}

// UpdateWindow: paints now (WM_PAINT sent) if the window needs it.
HLE("user32.dll", UpdateWindow, 1) {
  bool dirty;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(Arg(c, 0));
    dirty = w && w->dirty && w->visible;
  }
  if (dirty) Send(Arg(c, 0), 0x000F, 0, 0);
  return 1;
}
HLE("user32.dll", InvalidateRect, 3) {
  Invalidate(Arg(c, 0), false);
  return 1;
}
// GetUpdateRect(hwnd, RECT*, erase): the whole client area while the
// window needs painting (the update region is not tracked finer), else
// empty. It does not validate.
HLE("user32.dll", GetUpdateRect, 3) {
  bool dirty = false;
  int cw = 0, ch = 0;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (Window* w = Win(Arg(c, 0))) {
      dirty = w->dirty;
      ClientSize(*w, &cw, &ch);
    }
  }
  if (uint32_t rc = Arg(c, 1)) {
    Wr32(rc, 0);
    Wr32(rc + 4, 0);
    Wr32(rc + 8, dirty ? uint32_t(cw) : 0);
    Wr32(rc + 12, dirty ? uint32_t(ch) : 0);
  }
  return dirty ? 1 : 0;
}

HLE("user32.dll", ValidateRect, 2) {
  Validate(Arg(c, 0));
  return 1;
}
// Nothing is drawn through GDI, so there is nothing to repaint.
// RedrawWindow(hwnd, rect, region, flags): RDW_INVALIDATE (1) marks it,
// RDW_ALLCHILDREN (0x80) its children too, RDW_UPDATENOW (0x100) paints now.
HLE("user32.dll", RedrawWindow, 4) {
  uint32_t flags = Arg(c, 3);
  if (flags & 1) Invalidate(Arg(c, 0), flags & 0x80);
  if (flags & 0x100) {
    bool dirty;
    {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Window* w = Win(Arg(c, 0));
      dirty = w && w->dirty && w->visible;
    }
    if (dirty) Send(Arg(c, 0), 0x000F, 0, 0);
  }
  return 1;
}

HLE("user32.dll", IsWindowVisible, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  return w && w->visible ? 1 : 0;
}

HLE("user32.dll", IsWindowEnabled, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  return w && w->enabled ? 1 : 0;
}

HLE("user32.dll", EnableWindow, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  if (!w) return 0;
  bool was_disabled = !w->enabled;
  w->enabled = Arg(c, 1) != 0;
  return was_disabled ? 1 : 0;
}

HLE("user32.dll", IsIconic, 1) {
  SDL_Window* s;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(Arg(c, 0));
    if (!w) return 0;
    if (w->minimized) return 1;
    s = w->sdl;
  }
  return s && (SDL_GetWindowFlags(s) & SDL_WINDOW_MINIMIZED) ? 1 : 0;
}


// Like Windows, WM_SIZE/WM_MOVE are only sent when the size/position
// actually changes (the game re-applies its rect from its WM_SIZE handler;
// sending unconditionally recurses forever).
// A drop-down combo box's window is its face only (item height + 6): the
// height a template or MoveWindow gives it is the drop-down list's, which
// Windows keeps apart. Games centre the face's text in the window rect.
int ComboFaceHeight(const Window& w, int h) {
  if (w.cls != "combobox" || (w.style & 3) == 1 /* CBS_SIMPLE */) return h;
  uint32_t ih = w.limit == 30000 ? 16 : w.limit;
  return std::min(h, int(ih) + 6);
}

void ApplyRect(uint32_t hwnd, int x, int y, int w, int h, bool move, bool size) {
  SDL_Window* sdl = nullptr;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (Window* win = Win(hwnd)) h = ComboFaceHeight(*win, h);
  }
  int cw = 0, ch = 0;
  bool moved = false, sized = false;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* win = Win(hwnd);
    if (!win) return;
    if (move && (win->x != x || win->y != y)) {
      win->x = x;
      win->y = y;
      moved = true;
    }
    if (size && (win->w != w || win->h != h)) {
      win->w = w;
      win->h = h;
      sized = true;
    }
    ClientSize(*win, &cw, &ch);
    // In exclusive mode the host window keeps the display-mode size, and
    // a fullscreen one the screen's.
    sdl = win->mode_w || win->fullscreen ? nullptr : win->sdl;
  }
  if (sdl && sized) SDL_SetWindowSize(sdl, ToPoints(sdl, cw), ToPoints(sdl, ch));
  if (moved || sized) UpdateFullscreen(hwnd);
  if (moved || sized) ExposeParent(hwnd);

  if (sized) Send(hwnd, WM_SIZE, 0, MkLparam(cw, ch));
  if (moved) Send(hwnd, WM_MOVE, 0, MkLparam(x, y));
}

HLE("user32.dll", SetWindowPos, 7) {
  uint32_t hwnd = Arg(c, 0), flags = Arg(c, 6);
  bool nomove = flags & 0x2, nosize = flags & 0x1;
  ApplyRect(hwnd, int32_t(Arg(c, 2)), int32_t(Arg(c, 3)), int32_t(Arg(c, 4)),
            int32_t(Arg(c, 5)), !nomove, !nosize);
  if (flags & 0x40) {  // SWP_SHOWWINDOW
    if (SDL_Window* s = SdlWindowFor(hwnd); s && !Headless()) SDL_ShowWindow(s);
    {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      if (Window* w = Win(hwnd)) w->visible = true;
    }
    UpdateFullscreen(hwnd);
  }
  return 1;
}

HLE("user32.dll", MoveWindow, 6) {
  ApplyRect(Arg(c, 0), int32_t(Arg(c, 1)), int32_t(Arg(c, 2)),
            int32_t(Arg(c, 3)), int32_t(Arg(c, 4)), true, true);
  if (Arg(c, 5)) Invalidate(Arg(c, 0), true);
  return 1;
}

HLE("user32.dll", AdjustWindowRect, 3) {
  uint32_t rc = Arg(c, 0);
  int border, caption;
  FrameSize(Arg(c, 1), &border, &caption);
  Wr32(rc + 0, Rd32(rc + 0) - border);
  Wr32(rc + 4, Rd32(rc + 4) - border - caption);
  Wr32(rc + 8, Rd32(rc + 8) + border);
  Wr32(rc + 12, Rd32(rc + 12) + border);
  return 1;
}

// AdjustWindowRectEx(rect, style, menu, exstyle): the extended style adds
// nothing to the frame here.
HLE("user32.dll", AdjustWindowRectEx, 4) {
  uint32_t rc = Arg(c, 0);
  int border, caption;
  FrameSize(Arg(c, 1), &border, &caption);
  Wr32(rc + 0, Rd32(rc + 0) - border);
  Wr32(rc + 4, Rd32(rc + 4) - border - caption);
  Wr32(rc + 8, Rd32(rc + 8) + border);
  Wr32(rc + 12, Rd32(rc + 12) + border);
  return 1;
}

HLE("user32.dll", CopyRect, 2) {
  if (!Arg(c, 0) || !Arg(c, 1)) return 0;
  for (int i = 0; i < 16; i += 4) Wr32(Arg(c, 0) + i, Rd32(Arg(c, 1) + i));
  return 1;
}

HLE("user32.dll", OffsetRect, 3) {
  uint32_t rc = Arg(c, 0);
  if (!rc) return 0;
  Wr32(rc + 0, Rd32(rc + 0) + Arg(c, 1));
  Wr32(rc + 4, Rd32(rc + 4) + Arg(c, 2));
  Wr32(rc + 8, Rd32(rc + 8) + Arg(c, 1));
  Wr32(rc + 12, Rd32(rc + 12) + Arg(c, 2));
  return 1;
}

// The rest of the RECT helpers. A rect is {left, top, right, bottom},
// signed; it is empty when right <= left or bottom <= top.
namespace {
struct Rc {
  int32_t l, t, r, b;
  bool Empty() const { return r <= l || b <= t; }
};
Rc ReadRc(uint32_t p) {
  return {int32_t(Rd32(p)), int32_t(Rd32(p + 4)), int32_t(Rd32(p + 8)), int32_t(Rd32(p + 12))};
}
void WriteRc(uint32_t p, const Rc& r) {
  Wr32(p + 0, uint32_t(r.l));
  Wr32(p + 4, uint32_t(r.t));
  Wr32(p + 8, uint32_t(r.r));
  Wr32(p + 12, uint32_t(r.b));
}
}  // namespace

HLE("user32.dll", SetRect, 5) {
  if (!Arg(c, 0)) return 0;
  WriteRc(Arg(c, 0), {int32_t(Arg(c, 1)), int32_t(Arg(c, 2)), int32_t(Arg(c, 3)),
                      int32_t(Arg(c, 4))});
  return 1;
}

HLE("user32.dll", InflateRect, 3) {
  if (!Arg(c, 0)) return 0;
  Rc r = ReadRc(Arg(c, 0));
  int32_t dx = int32_t(Arg(c, 1)), dy = int32_t(Arg(c, 2));
  WriteRc(Arg(c, 0), {r.l - dx, r.t - dy, r.r + dx, r.b + dy});
  return 1;
}

HLE("user32.dll", IsRectEmpty, 1) { return !Arg(c, 0) || ReadRc(Arg(c, 0)).Empty(); }

// PtInRect(rect, POINT by value): left and top inside, right and bottom out.
HLE("user32.dll", PtInRect, 3) {
  if (!Arg(c, 0)) return 0;
  Rc r = ReadRc(Arg(c, 0));
  int32_t x = int32_t(Arg(c, 1)), y = int32_t(Arg(c, 2));
  return x >= r.l && x < r.r && y >= r.t && y < r.b;
}

// IntersectRect(dst, a, b): an empty result is stored as all zeros.
HLE("user32.dll", IntersectRect, 3) {
  if (!Arg(c, 0) || !Arg(c, 1) || !Arg(c, 2)) return 0;
  Rc a = ReadRc(Arg(c, 1)), b = ReadRc(Arg(c, 2));
  Rc d{std::max(a.l, b.l), std::max(a.t, b.t), std::min(a.r, b.r), std::min(a.b, b.b)};
  if (a.Empty() || b.Empty() || d.Empty()) {
    WriteRc(Arg(c, 0), {0, 0, 0, 0});
    return 0;
  }
  WriteRc(Arg(c, 0), d);
  return 1;
}

// UnionRect(dst, a, b): an empty source is ignored; both empty gives zeros.
HLE("user32.dll", UnionRect, 3) {
  if (!Arg(c, 0) || !Arg(c, 1) || !Arg(c, 2)) return 0;
  Rc a = ReadRc(Arg(c, 1)), b = ReadRc(Arg(c, 2));
  Rc d;
  if (a.Empty() && b.Empty()) {
    WriteRc(Arg(c, 0), {0, 0, 0, 0});
    return 0;
  }
  if (a.Empty()) d = b;
  else if (b.Empty()) d = a;
  else d = {std::min(a.l, b.l), std::min(a.t, b.t), std::max(a.r, b.r), std::max(a.b, b.b)};
  WriteRc(Arg(c, 0), d);
  return 1;
}

HLE("user32.dll", IsZoomed, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  return w && w->maximized ? 1 : 0;
}

HLE("user32.dll", GetClientRect, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  uint32_t rc = Arg(c, 1);
  if (!w) return 0;
  int cw, ch;
  ClientSize(*w, &cw, &ch);
  Wr32(rc, 0);
  Wr32(rc + 4, 0);
  Wr32(rc + 8, uint32_t(cw));
  Wr32(rc + 12, uint32_t(ch));
  return 1;
}

// GetWindowPlacement(hwnd, WINDOWPLACEMENT*): 44 bytes. showCmd is
// SW_SHOWMINIMIZED (2) while minimized, else SW_SHOWNORMAL (1); no
// minimized or maximized positions were ever set (-1, -1); the normal
// rectangle is the window's own, there being no taskbar to offset
// workspace coordinates.
HLE("user32.dll", GetWindowPlacement, 2) {
  uint32_t hwnd = Arg(c, 0), wp = Arg(c, 1);
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(hwnd);
  if (!w) return 0;
  Wr32(wp + 0, 44);
  Wr32(wp + 4, 0);
  Wr32(wp + 8, w->minimized ? 2 : 1);
  for (int i = 0; i < 4; i++) Wr32(wp + 12 + 4 * i, 0xFFFFFFFF);
  Wr32(wp + 28, uint32_t(w->x));
  Wr32(wp + 32, uint32_t(w->y));
  Wr32(wp + 36, uint32_t(w->x + w->w));
  Wr32(wp + 40, uint32_t(w->y + w->h));
  return 1;
}

HLE("user32.dll", GetWindowRect, 2) {
  uint32_t hwnd = Arg(c, 0), rc = Arg(c, 1);
  if (hwnd == 0x00010000) {  // desktop
    Wr32(rc, 0);
    Wr32(rc + 4, 0);
    Wr32(rc + 8, uint32_t(g_display_w));
    Wr32(rc + 12, uint32_t(g_display_h));
    return 1;
  }
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(hwnd);
  if (!w) return 0;
  int sx, sy;
  WindowScreenPos(*w, &sx, &sy);
  Wr32(rc, uint32_t(sx));
  Wr32(rc + 4, uint32_t(sy));
  Wr32(rc + 8, uint32_t(sx + w->w));
  Wr32(rc + 12, uint32_t(sy + w->h));
  return 1;
}

HLE("user32.dll", ClientToScreen, 2) {
  int ox, oy;
  WindowOrigin(Arg(c, 0), &ox, &oy);
  uint32_t pt = Arg(c, 1);
  Wr32(pt, Rd32(pt) + uint32_t(ox));
  Wr32(pt + 4, Rd32(pt + 4) + uint32_t(oy));
  return 1;
}

HLE("user32.dll", ScreenToClient, 2) {
  int ox, oy;
  WindowOrigin(Arg(c, 0), &ox, &oy);
  uint32_t pt = Arg(c, 1);
  Wr32(pt, Rd32(pt) - uint32_t(ox));
  Wr32(pt + 4, Rd32(pt + 4) - uint32_t(oy));
  return 1;
}

HLE("user32.dll", MapWindowPoints, 4) {
  int fx, fy, tx, ty;
  WindowOrigin(Arg(c, 0), &fx, &fy);
  WindowOrigin(Arg(c, 1), &tx, &ty);
  uint32_t pts = Arg(c, 2), n = Arg(c, 3);
  int dx = fx - tx, dy = fy - ty;
  for (uint32_t i = 0; i < n * 2; i += 2) {
    Wr32(pts + 4 * i, Rd32(pts + 4 * i) + uint32_t(dx));
    Wr32(pts + 4 * i + 4, Rd32(pts + 4 * i + 4) + uint32_t(dy));
  }
  return MkLparam(dx, dy);
}

HLE("user32.dll", GetDesktopWindow, 0) { return 0x00010000; }

HLE("user32.dll", GetParent, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  return w ? w->parent : 0;
}

HLE("user32.dll", GetWindow, 2) {
  // GW_OWNER (4) / GW_CHILD (5) / GW_HWNDNEXT (2) ... only owner is used.
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  if (!w) return 0;
  return Arg(c, 1) == 4 ? w->parent : 0;
}

HLE("user32.dll", GetLastActivePopup, 1) { return Arg(c, 0); }

HLE("user32.dll", FindWindowA, 2) {
  std::string cls = Arg(c, 0) ? Lower(GStr(Arg(c, 0))) : "";
  std::string title = Arg(c, 1) ? GStr(Arg(c, 1)) : "";
  std::lock_guard<std::recursive_mutex> l(g_mu);
  for (auto& [h, w] : g_windows) {
    if (w.destroyed || (w.style & WS_CHILD)) continue;
    if (Arg(c, 0) && w.cls != cls) continue;
    if (Arg(c, 1) && w.title != title) continue;
    return h;
  }
  return 0;
}

// FindWindowExA(parent, child after, class, title): the children of
// `parent` (top-level windows when it is 0 or HWND_MESSAGE), in creation
// order, starting after `child after`. A class given as an atom matches
// nothing (class atoms are not kept).
HLE("user32.dll", FindWindowExA, 4) {
  uint32_t parent = Arg(c, 0), after = Arg(c, 1), pcls = Arg(c, 2), ptitle = Arg(c, 3);
  if (pcls && pcls < 0x10000) return 0;
  std::string cls = pcls ? Lower(GStr(pcls)) : "";
  std::string title = ptitle ? GStr(ptitle) : "";
  bool top = parent == 0 || parent == 0xFFFFFFFD;
  std::lock_guard<std::recursive_mutex> l(g_mu);
  auto it = after ? g_windows.upper_bound(after) : g_windows.begin();
  for (; it != g_windows.end(); ++it) {
    const Window& w = it->second;
    if (w.destroyed) continue;
    if (top ? (w.style & WS_CHILD) != 0 : w.parent != parent) continue;
    if (pcls && w.cls != cls) continue;
    if (ptitle && w.title != title) continue;
    return it->first;
  }
  return 0;
}

HLE("user32.dll", SetWindowTextA, 2) {
  std::string t = GStr(Arg(c, 1));
  SDL_Window* s = nullptr;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(Arg(c, 0));
    if (!w) return 0;
    w->title = t;
    s = w->sdl;
  }
  if (s) SDL_SetWindowTitle(s, AnsiToUtf8(t).c_str());
  return 1;
}

// SetWindowTextW: the title is kept in the ANSI code page for the A
// functions; the host window gets the full Unicode text.
HLE("user32.dll", SetWindowTextW, 2) {
  std::u16string wt = Arg(c, 1) ? GWStr(Arg(c, 1)) : std::u16string();
  SDL_Window* s = nullptr;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(Arg(c, 0));
    if (!w) return 0;
    w->title = Narrow(wt);
    s = w->sdl;
  }
  bool bad;
  if (s) SDL_SetWindowTitle(s, Utf16ToUtf8(wt.data(), wt.size(), &bad).c_str());
  return 1;
}

HLE("user32.dll", GetWindowTextA, 3) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  uint32_t buf = Arg(c, 1), cap = Arg(c, 2);
  if (!w || !cap) return 0;
  std::string t = w->title.substr(0, cap - 1);
  PutStr(buf, t);
  return uint32_t(t.size());
}

HLE("user32.dll", GetWindowTextLengthA, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  return w ? uint32_t(w->title.size()) : 0;
}

HLE("user32.dll", GetWindowLongA, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  int32_t idx = int32_t(Arg(c, 1));
  if (!w) return 0;
  switch (idx) {
    case GWL_WNDPROC: return w->wndproc;
    case GWL_HINSTANCE: return w->hinst;
    case GWL_HWNDPARENT: return w->parent;
    case GWL_STYLE: return w->style | (w->visible ? WS_VISIBLE : 0);
    case GWL_EXSTYLE: return w->exstyle;
    case GWL_USERDATA: return w->userdata;
    case GWL_ID: return w->id;
  }
  if (idx >= 0 && uint32_t(idx) + 4 <= w->extra.size()) {
    uint32_t v;
    std::memcpy(&v, w->extra.data() + idx, 4);
    return v;
  }
  return 0;
}

HLE("user32.dll", SetWindowLongA, 3) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  int32_t idx = int32_t(Arg(c, 1));
  uint32_t v = Arg(c, 2), old = 0;
  if (!w) return 0;
  switch (idx) {
    case GWL_WNDPROC: old = w->wndproc; w->wndproc = v; return old;
    case GWL_HWNDPARENT: old = w->parent; w->parent = v; return old;
    case GWL_STYLE: old = w->style; w->style = v; return old;
    case GWL_EXSTYLE: old = w->exstyle; w->exstyle = v; return old;
    case GWL_USERDATA: old = w->userdata; w->userdata = v; return old;
    case GWL_ID: old = w->id; w->id = v; return old;
  }
  if (idx >= 0 && uint32_t(idx) + 4 <= w->extra.size()) {
    std::memcpy(&old, w->extra.data() + idx, 4);
    std::memcpy(w->extra.data() + idx, &v, 4);
  }
  return old;
}

// ---- focus and activation ---------------------------------------------------------

HLE("user32.dll", GetFocus, 0) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return g_focus;
}
HLE("user32.dll", SetFocus, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t old = g_focus;
  g_focus = Arg(c, 0);
  return old;
}
HLE("user32.dll", GetActiveWindow, 0) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return g_active;
}
HLE("user32.dll", SetActiveWindow, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t old = g_active;
  g_active = Arg(c, 0);
  return old;
}
HLE("user32.dll", GetForegroundWindow, 0) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return g_active;
}
// Brings a window to the front and activates it: when it was not already
// the active one, it gets WM_ACTIVATEAPP, WM_ACTIVATE and WM_SETFOCUS, as
// from ShowWindow. Games that create a visible window and only call
// SetForegroundWindow learn they are active this way.
HLE("user32.dll", SetForegroundWindow, 1) {
  uint32_t hwnd = Arg(c, 0);
  // A child window cannot be the foreground window: its top-level
  // ancestor is activated (a game's child dialogs leave it active).
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    for (int i = 0; i < 16; i++) {
      Window* w = Win(hwnd);
      if (!w || !(w->style & WS_CHILD) || !w->parent) break;
      hwnd = w->parent;
    }
  }
  // Background: the game is told it is in front, the host is not asked.
  if (SDL_Window* s = SdlWindowFor(hwnd); s && !Background()) SDL_RaiseWindow(s);
  bool activate = false;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return 0;
    if (g_active != hwnd) activate = true;
    g_active = g_focus = hwnd;
  }
  if (activate) {
    Send(hwnd, WM_ACTIVATEAPP, 1, 0);
    Send(hwnd, WM_ACTIVATE, 1, 0);
    Send(hwnd, WM_SETFOCUS, 0, 0);
  }
  return 1;
}
HLE("user32.dll", BringWindowToTop, 1) {
  if (SDL_Window* s = SdlWindowFor(Arg(c, 0)); s && !Background()) SDL_RaiseWindow(s);
  return 1;
}

// ---- messages ----------------------------------------------------------------------

HLE("user32.dll", PeekMessageA, 5) {
  uint32_t msg_va = Arg(c, 0), hwnd = Arg(c, 1), lo = Arg(c, 2), hi = Arg(c, 3),
           flags = Arg(c, 4);
  PumpHostEvents();
  Msg m;
  if (!TakeHookedMessage(c, hwnd, lo, hi, flags & 1 /* PM_REMOVE */, &m)) return 0;
  PutMsg(msg_va, m);
  return 1;
}

HLE("user32.dll", GetMessageA, 4) {
  uint32_t msg_va = Arg(c, 0), hwnd = Arg(c, 1), lo = Arg(c, 2), hi = Arg(c, 3);
  Msg m;
  for (;;) {
    PumpHostEvents();
    if (TakeHookedMessage(c, hwnd, lo, hi, true, &m)) break;
    if (OnMainThread() && g_sdl_up) {
      SDL_WaitEventTimeout(nullptr, 10);
    } else {
      std::unique_lock<std::recursive_mutex> l(g_mu);
      g_queue_cv.wait_for(l, std::chrono::milliseconds(10));
    }
  }
  PutMsg(msg_va, m);
  return m.msg == WM_QUIT ? 0 : 1;
}

// PostThreadMessageA(tid, msg, wparam, lparam): a message with no window
// in that thread's queue.
HLE("user32.dll", PostThreadMessageA, 4) {
  PostTo(uint32_t(Arg(c, 0)),
         Msg{0, uint32_t(Arg(c, 1)), uint32_t(Arg(c, 2)), uint32_t(Arg(c, 3)), TickCount(),
             g_mouse_x, g_mouse_y});
  return 1;
}

HLE("user32.dll", PostMessageA, 4) {
  uint32_t hwnd = Arg(c, 0);
  if (!hwnd) {
    PostTo(CurrentThread()->tid,
           Msg{0, uint32_t(Arg(c, 1)), uint32_t(Arg(c, 2)), uint32_t(Arg(c, 3)), TickCount(),
               g_mouse_x, g_mouse_y});
    return 1;
  }
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (!Win(hwnd)) return 0;
  }
  Post(hwnd, Arg(c, 1), Arg(c, 2), Arg(c, 3));
  return 1;
}

HLE("user32.dll", PostQuitMessage, 1) {
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Queue& q = g_queues[CurrentThread()->tid];
    q.quit = true;
    q.quit_code = Arg(c, 0);
  }
  g_queue_cv.notify_all();
  return 0;
}

HLE("user32.dll", SendMessageA, 4) {
  return Send(Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3));
}

HLE("user32.dll", DispatchMessageA, 1) {
  uint32_t m = Arg(c, 0);
  uint32_t hwnd = Rd32(m), msg = Rd32(m + 4);
  // WM_TIMER from a timer with a TIMERPROC goes to the procedure.
  if (msg == WM_TIMER && Rd32(m + 12))
    return CallGuest(c, Rd32(m + 12), {hwnd, msg, Rd32(m + 8), Rd32(m + 16)});
  if (!hwnd) return 0;
  return Send(hwnd, msg, Rd32(m + 8), Rd32(m + 12));
}

// ---- hot keys ------------------------------------------------------------------------

HLE("user32.dll", RegisterHotKey, 4) {
  uint32_t hwnd = Arg(c, 0), id = Arg(c, 1), mods = Arg(c, 2), vk = Arg(c, 3);
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t tid = CurrentThread()->tid;
  if (hwnd) {
    Window* w = Win(hwnd);
    if (!w) return 0;
    tid = w->tid;
  }
  for (const HotKey& k : g_hotkeys)
    if ((k.hwnd == hwnd && k.id == id) || (k.vk == vk && (k.mods & 7) == (mods & 7))) {
      SetLastError(1409);  // ERROR_HOTKEY_ALREADY_REGISTERED
      return 0;
    }
  g_hotkeys.push_back({hwnd, id, mods, vk, tid});
  return 1;
}

HLE("user32.dll", UnregisterHotKey, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  auto it = std::find_if(g_hotkeys.begin(), g_hotkeys.end(), [&](const HotKey& k) {
    return k.hwnd == Arg(c, 0) && k.id == Arg(c, 1);
  });
  if (it == g_hotkeys.end()) {
    SetLastError(1419);  // ERROR_HOTKEY_NOT_REGISTERED
    return 0;
  }
  g_hotkeys.erase(it);
  return 1;
}

// ---- timers ------------------------------------------------------------------------

HLE("user32.dll", SetTimer, 4) {
  uint32_t hwnd = Arg(c, 0), id = Arg(c, 1), elapse = Arg(c, 2), proc = Arg(c, 3);
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t tid = CurrentThread()->tid;
  if (hwnd) {
    Window* w = Win(hwnd);
    if (!w) return 0;
    tid = w->tid;
  } else {
    // A thread timer: the system picks the id.
    static uint32_t next_id = 0x7F00;
    id = ++next_id;
  }
  elapse = std::clamp<uint32_t>(elapse, 10, 0x7FFFFFFF);  // USER_TIMER_MINIMUM
  for (Timer& t : g_timers)
    if (t.hwnd == hwnd && t.id == id) {  // same timer: reset it
      t = Timer{hwnd, id, elapse, proc, tid, int64_t(TickCount()) + elapse};
      return hwnd ? 1 : id;
    }
  g_timers.push_back(Timer{hwnd, id, elapse, proc, tid, int64_t(TickCount()) + elapse});
  return hwnd ? 1 : id;
}

HLE("user32.dll", KillTimer, 2) {
  uint32_t hwnd = Arg(c, 0), id = Arg(c, 1);
  std::lock_guard<std::recursive_mutex> l(g_mu);
  auto it = std::find_if(g_timers.begin(), g_timers.end(),
                         [&](const Timer& t) { return t.hwnd == hwnd && t.id == id; });
  if (it == g_timers.end()) return 0;
  g_timers.erase(it);
  return 1;
}

HLE("user32.dll", TranslateMessage, 1) {
  // Generate WM_CHAR from WM_KEYDOWN using the US layout and shift/caps.
  uint32_t m = Arg(c, 0);
  uint32_t msg = Rd32(m + 4), vk = Rd32(m + 8);
  if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN) return 0;
  bool shift = g_key_down[0x10], caps = g_key_toggled[0x14];
  bool ctrl = g_key_down[0x11];
  uint32_t ch = 0;
  if (vk >= 'A' && vk <= 'Z') {
    ch = (shift != caps) ? vk : vk + 32;
    if (ctrl) ch = vk - 'A' + 1;
  } else if (vk >= 0x60 && vk <= 0x69 && g_key_toggled[0x90]) {
    ch = '0' + (vk - 0x60);
  } else {
    static const struct { uint8_t vk; char n, s; } k[] = {
        {'0', '0', ')'}, {'1', '1', '!'}, {'2', '2', '@'}, {'3', '3', '#'},
        {'4', '4', '$'}, {'5', '5', '%'}, {'6', '6', '^'}, {'7', '7', '&'},
        {'8', '8', '*'}, {'9', '9', '('}, {0xBD, '-', '_'}, {0xBB, '=', '+'},
        {0xDB, '[', '{'}, {0xDD, ']', '}'}, {0xDC, '\\', '|'},
        {0xBA, ';', ':'}, {0xDE, '\'', '"'}, {0xC0, '`', '~'},
        {0xBC, ',', '<'}, {0xBE, '.', '>'}, {0xBF, '/', '?'},
        {0x20, ' ', ' '}, {0x0D, 13, 13}, {0x08, 8, 8}, {0x09, 9, 9},
        {0x1B, 27, 27}, {0x6A, '*', '*'}, {0x6B, '+', '+'},
        {0x6D, '-', '-'}, {0x6F, '/', '/'}, {0x6E, '.', '.'}};
    for (const auto& e : k)
      if (e.vk == vk) ch = uint8_t(shift ? e.s : e.n);
  }
  if (!ch) return 0;
  Post(Rd32(m), msg == WM_SYSKEYDOWN ? WM_SYSCHAR : WM_CHAR, ch, Rd32(m + 12));
  return 1;
}

HLE("user32.dll", TranslateAcceleratorA, 3) { return 0; }
uint32_t DlgItem(uint32_t dlg, uint32_t id);
uint32_t UserProc(const char* name);

// IsDialogMessage(dlg, msg): a message for the dialog or one of its
// controls is handled here: Enter is the default button's WM_COMMAND
// (DM_GETDEFID, else IDOK), Escape IDCANCEL's; anything else is translated
// and dispatched. TRUE when it was the dialog's. (No Tab/arrow navigation
// or mnemonics.)
HLE("user32.dll", IsDialogMessageA, 2) {
  uint32_t dlg = Arg(c, 0), m = Arg(c, 1);
  uint32_t hwnd = Rd32(m), msg = Rd32(m + 4), wp = Rd32(m + 8);
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    uint32_t w = hwnd;
    while (w && w != dlg) {
      Window* win = Win(w);
      w = win && (win->style & WS_CHILD) ? win->parent : 0;
    }
    if (!w || !dlg) return 0;
  }
  if (msg == WM_KEYDOWN && (wp == 0x0D || wp == 0x1B)) {
    uint32_t id = 2;  // IDCANCEL
    if (wp == 0x0D) {
      uint32_t def = Send(dlg, 0x0400 /* DM_GETDEFID */, 0, 0);
      id = (def >> 16) == 0x534B /* DC_HASDEFID */ ? (def & 0xFFFF) : 1 /* IDOK */;
    }
    Send(dlg, 0x0111 /* WM_COMMAND */, id, DlgItem(dlg, id));
    return 1;
  }
  if (msg == WM_CHAR && (wp == 0x0D || wp == 0x1B)) return 1;  // used by the keydown
  CallGuest(c, UserProc("TranslateMessage"), {m});
  CallGuest(c, UserProc("DispatchMessageA"), {m});
  return 1;
}

HLE("user32.dll", CallWindowProcA, 5) {
  return CallGuest(c, Arg(c, 0), {Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4)});
}

HLE("user32.dll", DefWindowProcA, 4) {
  uint32_t hwnd = Arg(c, 0), msg = Arg(c, 1), wp = Arg(c, 2);
  switch (msg) {
    case WM_NCCREATE: return 1;
    case WM_CLOSE: {
      uint32_t destroy = ProcAddress(ModuleHandle("user32.dll"), "DestroyWindow");
      CallGuest(c, destroy, {hwnd});
      return 0;
    }
    case WM_ERASEBKGND: return 1;
    case 0x000F: Validate(hwnd); return 0;  // WM_PAINT
    case WM_NCHITTEST: return 1;  // HTCLIENT
    case WM_SYSCOMMAND:
      if ((wp & 0xFFF0) == 0xF060) Post(hwnd, WM_CLOSE, 0, 0);  // SC_CLOSE
      return 0;
    case WM_SYSKEYDOWN:
      if (wp == 0x73 /* VK_F4 */ && g_key_down[0x12]) Post(hwnd, WM_CLOSE, 0, 0);
      return 0;
    case WM_GETTEXTLENGTH: {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Window* w = Win(hwnd);
      return w ? uint32_t(w->title.size()) : 0;
    }
    default:
      return 0;
  }
}

uint32_t CallDefWindowProc(Ctx& c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);

// ---- dialog boxes ------------------------------------------------------------------
//
// CreateDialog*/DialogBox* build a dialog from its template: a window of
// class #32770 (or the template's class) whose procedure is DefDlgProc
// ("<dialog>"), which calls the game's dialog procedure, and one child
// window per item. Dialogs get no host window (Window::is_dialog): games
// of the DirectDraw era draw their dialogs themselves into their full-
// screen window, and RouteMouse sends the mouse to the dialog and its
// controls. A modal DialogBox runs its own message loop until EndDialog.
// Dialog units are converted with the base units of MS Sans Serif 8 (6x13)
// for DS_SETFONT templates, of the system font (8x16) otherwise.

constexpr uint32_t WM_INITDIALOG = 0x0110, WM_COMMAND = 0x0111, WM_HSCROLL = 0x0114,
                   WM_VSCROLL = 0x0115;
constexpr uint32_t DS_SETFONT = 0x40, DS_CENTER = 0x800;
constexpr uint32_t WS_TABSTOP = 0x00010000;
constexpr uint32_t kDlgWindowExtra = 30;  // DWL_MSGRESULT 0, DWL_DLGPROC 4, DWL_USER 8

std::string ReadWStrAt(uint32_t* p) {
  std::string s;
  for (;;) {
    uint16_t ch = Rd16(*p);
    *p += 2;
    if (!ch) break;
    s.push_back(ch < 256 ? char(ch) : '?');
  }
  return s;
}

// sz_Or_Ord: 0 (none), 0xFFFF then an ordinal ("#n"), or a UTF-16 string.
std::string ReadSzOrOrd(uint32_t* p) {
  uint16_t first = Rd16(*p);
  if (first == 0) {
    *p += 2;
    return "";
  }
  if (first == 0xFFFF) {
    uint16_t ord = Rd16(*p + 2);
    *p += 4;
    return "#" + std::to_string(ord);
  }
  return ReadWStrAt(p);
}

std::string ControlClassName(const std::string& k) {
  static const std::map<std::string, std::string> atoms = {
      {"#128", "button"}, {"#129", "edit"},      {"#130", "static"},
      {"#131", "listbox"}, {"#132", "scrollbar"}, {"#133", "combobox"}};
  auto it = atoms.find(k);
  return it != atoms.end() ? it->second : Lower(k);
}

uint32_t UserProc(const char* name) { return ProcAddress(ModuleHandle("user32.dll"), name); }

uint32_t GuestStr(const std::string& s) {
  uint32_t p = HeapAllocProcess(uint32_t(s.size()) + 1, false);
  PutStr(p, s);
  return p;
}

uint32_t CreateDialogFromTemplate(Ctx& c, uint32_t hinst, uint32_t t, uint32_t owner,
                                  uint32_t dlgproc, uint32_t param) {
  uint32_t p = t;
  bool ex = Rd16(p) == 1 && Rd16(p + 2) == 0xFFFF;
  uint32_t style, exstyle, count;
  if (ex) {
    exstyle = Rd32(p + 8);
    style = Rd32(p + 12);
    count = Rd16(p + 16);
    p += 18;
  } else {
    style = Rd32(p);
    exstyle = Rd32(p + 4);
    count = Rd16(p + 8);
    p += 10;
  }
  int16_t dx = int16_t(Rd16(p)), dy = int16_t(Rd16(p + 2)), dcx = int16_t(Rd16(p + 4)),
          dcy = int16_t(Rd16(p + 6));
  p += 8;
  std::string menu = ReadSzOrOrd(&p);
  std::string cls = ReadSzOrOrd(&p);
  std::string title = ReadSzOrOrd(&p);
  (void)menu;
  int font_pt = 0;
  std::string face;
  if (style & DS_SETFONT) {
    font_pt = Rd16(p);
    p += ex ? 6 : 2;  // point size (and weight, italic, charset)
    face = ReadWStrAt(&p);
  }
  RECOMP_TRACE("  dialog font: %d pt \"%s\"\n", font_pt, face.c_str());
  const int bux = (style & DS_SETFONT) ? 6 : 8, buy = (style & DS_SETFONT) ? 13 : 16;
  auto px = [&](int v) { return v * bux / 4; };
  auto py = [&](int v) { return v * buy / 8; };
  int w = px(dcx), h = py(dcy), x = px(dx), y = py(dy);
  if (style & DS_CENTER) {
    int ow = 640, oh = 480;
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (Window* o = Win(owner)) {
      int cw, ch;
      ClientSize(*o, &cw, &ch);
      GameClientSize(*o, &ow, &oh);
      (void)cw;
      (void)ch;
    }
    x = (ow - w) / 2;
    y = (oh - h) / 2;
  }
  RECOMP_TRACE("  dialog template: %u items, style %08X, %d,%d %dx%d, class \"%s\", \"%s\"\n",
               count, style, x, y, w, h, cls.c_str(), title.c_str());

  EnsureSystemClasses();
  uint32_t create = UserProc("CreateWindowExA");
  std::string dcls = cls.empty() ? "#32770" : cls;
  uint32_t cls_s = GuestStr(dcls), title_s = GuestStr(title);
  t_creating_dialog = true;
  uint32_t hdlg = uint32_t(CallGuest(c, create, {exstyle, cls_s, title_s, style & ~WS_VISIBLE,
                                                 uint32_t(x), uint32_t(y), uint32_t(w), uint32_t(h),
                                                 owner, 0, hinst, 0}));
  t_creating_dialog = false;
  HeapFreeProcess(cls_s);
  HeapFreeProcess(title_s);
  if (!hdlg) {
    WarnF("CreateDialog: the dialog window (class %s) could not be created\n", dcls.c_str());
    return 0;
  }
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* d = Win(hdlg);
    if (d->extra.size() < kDlgWindowExtra) d->extra.resize(kDlgWindowExtra, 0);
    std::memcpy(d->extra.data() + 4, &dlgproc, 4);
  }

  uint32_t focus = 0;
  for (uint32_t i = 0; i < count; i++) {
    p = (p + 3) & ~3u;
    uint32_t istyle, iexstyle, id;
    if (ex) {
      iexstyle = Rd32(p + 4);
      istyle = Rd32(p + 8);
      p += 12;
    } else {
      istyle = Rd32(p);
      iexstyle = Rd32(p + 4);
      p += 8;
    }
    int16_t ix = int16_t(Rd16(p)), iy = int16_t(Rd16(p + 2)), icx = int16_t(Rd16(p + 4)),
            icy = int16_t(Rd16(p + 6));
    p += 8;
    if (ex) {
      id = Rd32(p);
      p += 4;
    } else {
      id = Rd16(p);
      p += 2;
    }
    std::string icls = ControlClassName(ReadSzOrOrd(&p));
    std::string ititle = ReadSzOrOrd(&p);
    if (!ititle.empty() && ititle[0] == '#') ititle.clear();  // an icon/bitmap ordinal
    uint16_t extra = Rd16(p);
    uint32_t data = extra ? p + 2 : 0;
    p += 2 + extra;
    uint32_t cs = GuestStr(icls), ts = GuestStr(ititle);
    uint32_t hc = uint32_t(CallGuest(
        c, create, {iexstyle, cs, ts, istyle | WS_CHILD, uint32_t(px(ix)), uint32_t(py(iy)),
                    uint32_t(px(icx)), uint32_t(py(icy)), hdlg, id, hinst, data}));
    HeapFreeProcess(cs);
    HeapFreeProcess(ts);
    if (!hc) {
      WarnF("CreateDialog: control %u (class %s) could not be created\n", id, icls.c_str());
      continue;
    }
    if (!focus && (istyle & WS_TABSTOP)) focus = hc;
  }
  // A nonzero result: the dialog manager gives the focus to the control
  // in wParam (the first tab stop).
  if (Send(hdlg, WM_INITDIALOG, focus, param) && focus) {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    g_focus = focus;
  }
  if (style & WS_VISIBLE) CallGuest(c, UserProc("ShowWindow"), {hdlg, 5 /* SW_SHOW */});
  // GameConfig::ownerless_dialogs_take_default: the default button's
  // click, queued for the dialog's (or the caller's modal) loop.
  if (!owner && Game().ownerless_dialogs_take_default) {
    uint32_t def = Send(hdlg, 0x0400 /* DM_GETDEFID */, 0, 0);
    uint32_t id = (def >> 16) == 0x534B /* DC_HASDEFID */ ? (def & 0xFFFF) : 1 /* IDOK */;
    RECOMP_TRACE("  ownerless dialog %X: pressing its default button %u\n", hdlg, id);
    PostTo(CurrentThread()->tid,
           Msg{hdlg, WM_COMMAND, id, DlgItem(hdlg, id), TickCount(), g_mouse_x, g_mouse_y});
  }
  return hdlg;
}

// RT_DIALOG resource `name` of module `hinst` (the EXE or a loaded DLL).
uint32_t DialogResource(uint32_t hinst, uint32_t name) {
  Ctx& c = CurrentThread()->ctx;
  uint32_t r = uint32_t(CallGuest(c, ProcAddress(ModuleHandle("kernel32.dll"), "FindResourceA"),
                                  {hinst, name, 5}));
  if (!r) return 0;
  return uint32_t(CallGuest(c, ProcAddress(ModuleHandle("kernel32.dll"), "LoadResource"),
                            {hinst, r}));
}

// The modal loop of DialogBox: messages are dispatched until EndDialog.
uint32_t RunModal(Ctx& c, uint32_t hdlg) {
  if (!hdlg) return uint32_t(-1);
  CallGuest(c, UserProc("ShowWindow"), {hdlg, 5});
  uint32_t m = HeapAllocProcess(28, true);
  uint32_t dispatch = UserProc("DispatchMessageA"), translate = UserProc("TranslateMessage");
  for (;;) {
    {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Window* d = Win(hdlg);
      if (!d || d->dialog_ended) break;
    }
    PumpHostEvents();
    Msg msg;
    if (TakeHookedMessage(c, 0, 0, 0, true, &msg)) {
      PutMsg(m, msg);
      CallGuest(c, translate, {m});
      CallGuest(c, dispatch, {m});
    } else if (OnMainThread() && g_sdl_up) {
      SDL_WaitEventTimeout(nullptr, 10);
    } else {
      std::unique_lock<std::recursive_mutex> l(g_mu);
      g_queue_cv.wait_for(l, std::chrono::milliseconds(10));
    }
  }
  HeapFreeProcess(m);
  uint32_t result = 0;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (Window* d = Win(hdlg)) result = d->dialog_result;
  }
  CallGuest(c, UserProc("DestroyWindow"), {hdlg});
  return result;
}

HLE("user32.dll", CreateDialogIndirectParamA, 5) {
  return CreateDialogFromTemplate(c, Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4));
}

HLE("user32.dll", CreateDialogParamA, 5) {
  uint32_t t = DialogResource(Arg(c, 0), Arg(c, 1));
  if (!t) {
    WarnF("CreateDialogParamA: no dialog resource %X\n", Arg(c, 1));
    return 0;
  }
  return CreateDialogFromTemplate(c, Arg(c, 0), t, Arg(c, 2), Arg(c, 3), Arg(c, 4));
}

HLE("user32.dll", DialogBoxIndirectParamA, 5) {
  return RunModal(c, CreateDialogFromTemplate(c, Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3),
                                              Arg(c, 4)));
}

HLE("user32.dll", DialogBoxParamA, 5) {
  uint32_t t = DialogResource(Arg(c, 0), Arg(c, 1));
  if (!t) {
    WarnF("DialogBoxParamA: no dialog resource %X\n", Arg(c, 1));
    return uint32_t(-1);
  }
  return RunModal(c, CreateDialogFromTemplate(c, Arg(c, 0), t, Arg(c, 2), Arg(c, 3), Arg(c, 4)));
}

HLE("user32.dll", EndDialog, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* d = Win(Arg(c, 0));
  if (!d) return 0;
  d->dialog_ended = true;
  d->dialog_result = Arg(c, 1);
  d->visible = false;
  return 1;
}

// DefDlgProc: the dialog procedure first; what it does not handle gets the
// default. Its result is the message's, for the messages whose result is
// the procedure's return value, else DWL_MSGRESULT.
uint32_t DefDlg(Ctx& c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
  uint32_t proc = 0;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return 0;
    if (w->extra.size() >= 8) std::memcpy(&proc, w->extra.data() + 4, 4);
  }
  if (proc) {
    uint32_t r = uint32_t(CallGuest(c, proc, {hwnd, msg, wp, lp}));
    if (r) {
      if (msg == WM_INITDIALOG || (msg >= 0x0132 && msg <= 0x0138) || msg == 0x0039 ||
          msg == 0x002E || msg == 0x002F || msg == 0x0037)
        return r;
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Window* w = Win(hwnd);
      uint32_t res = 0;
      if (w && w->extra.size() >= 4) std::memcpy(&res, w->extra.data(), 4);
      return res;
    }
  }
  switch (msg) {
    case WM_NCCREATE: return 1;
    case WM_CLOSE: Post(hwnd, WM_COMMAND, 2 /* IDCANCEL */, 0); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_INITDIALOG: return 1;
    case 0x0400: {  // DM_GETDEFID: the first default push button's
      std::lock_guard<std::recursive_mutex> l(g_mu);
      for (auto& [h, w] : g_windows)
        if (!w.destroyed && w.parent == hwnd && w.cls == "button" && (w.style & 0xF) == 1)
          return w.id | 0x534B0000u;  // DC_HASDEFID
      return 0;
    }
    default: return CallDefWindowProc(c, hwnd, msg, wp, lp);
  }
}

HLE_NAMED("user32.dll", "<dialog>", DialogWndProc, 4) {
  return DefDlg(c, Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3));
}

HLE("user32.dll", DefDlgProcA, 4) {
  // Called by a dialog class's own window procedure: the default only.
  uint32_t hwnd = Arg(c, 0), msg = Arg(c, 1), wp = Arg(c, 2), lp = Arg(c, 3);
  switch (msg) {
    case WM_CLOSE: Post(hwnd, WM_COMMAND, 2, 0); return 0;
    case WM_INITDIALOG: return 1;
    default: return DefDlg(c, hwnd, msg, wp, lp);
  }
}

uint32_t DlgItem(uint32_t dlg, uint32_t id) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  for (auto& [h, w] : g_windows)
    if (!w.destroyed && w.parent == dlg && (w.style & WS_CHILD) && w.id == id) return h;
  return 0;
}

HLE("user32.dll", GetDlgItem, 2) { return DlgItem(Arg(c, 0), Arg(c, 1)); }

HLE("user32.dll", GetDlgCtrlID, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  return w ? w->id : 0;
}

HLE("user32.dll", SendDlgItemMessageA, 5) {
  uint32_t h = DlgItem(Arg(c, 0), Arg(c, 1));
  return h ? Send(h, Arg(c, 2), Arg(c, 3), Arg(c, 4)) : 0;
}

HLE("user32.dll", SetDlgItemTextA, 3) {
  uint32_t h = DlgItem(Arg(c, 0), Arg(c, 1));
  return h ? (Send(h, WM_SETTEXT, 0, Arg(c, 2)), 1) : 0;
}

HLE("user32.dll", GetDlgItemTextA, 4) {
  uint32_t h = DlgItem(Arg(c, 0), Arg(c, 1));
  if (!h) {
    if (Arg(c, 2) && Arg(c, 3)) Wr8(Arg(c, 2), 0);
    return 0;
  }
  return Send(h, WM_GETTEXT, Arg(c, 3), Arg(c, 2));
}

HLE("user32.dll", SetDlgItemInt, 4) {
  uint32_t h = DlgItem(Arg(c, 0), Arg(c, 1));
  if (!h) return 0;
  std::string s = Arg(c, 3) ? std::to_string(int32_t(Arg(c, 2))) : std::to_string(Arg(c, 2));
  uint32_t g = GuestStr(s);
  Send(h, WM_SETTEXT, 0, g);
  HeapFreeProcess(g);
  return 1;
}

HLE("user32.dll", GetDlgItemInt, 4) {
  uint32_t h = DlgItem(Arg(c, 0), Arg(c, 1));
  std::string t;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (Window* w = Win(h)) t = w->title;
  }
  char* end = nullptr;
  long v = std::strtol(t.c_str(), &end, 10);
  bool ok = h && !t.empty() && end && !*end;
  if (Arg(c, 2)) Wr32(Arg(c, 2), ok ? 1 : 0);
  return ok ? uint32_t(v) : 0;
}

HLE("user32.dll", CheckDlgButton, 3) {
  uint32_t h = DlgItem(Arg(c, 0), Arg(c, 1));
  return h ? (Send(h, 0x00F1 /* BM_SETCHECK */, Arg(c, 2), 0), 1) : 0;
}

HLE("user32.dll", IsDlgButtonChecked, 2) {
  uint32_t h = DlgItem(Arg(c, 0), Arg(c, 1));
  return h ? Send(h, 0x00F0 /* BM_GETCHECK */, 0, 0) : 0;
}

HLE("user32.dll", CheckRadioButton, 4) {
  for (uint32_t id = Arg(c, 1); id <= Arg(c, 2); id++)
    if (uint32_t h = DlgItem(Arg(c, 0), id)) Send(h, 0x00F1, id == Arg(c, 3) ? 1 : 0, 0);
  return 1;
}

HLE("user32.dll", GetNextDlgTabItem, 3) { return 0; }

HLE("user32.dll", MapDialogRect, 2) {
  uint32_t rc = Arg(c, 1);
  for (int i = 0; i < 4; i++) {
    int32_t v = int32_t(Rd32(rc + 4 * uint32_t(i)));
    Wr32(rc + 4 * uint32_t(i), uint32_t(i % 2 == 0 ? v * 6 / 4 : v * 13 / 8));
  }
  return 1;
}

HLE("user32.dll", GetDialogBaseUnits, 0) { return MkLparam(8, 16); }

// Children of `parent`, in creation order; with `deep`, their children
// after each.
void Children(uint32_t parent, bool deep, std::vector<uint32_t>* out) {
  std::vector<uint32_t> direct;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    for (auto& [h, w] : g_windows)
      if (!w.destroyed && w.parent == parent && (w.style & WS_CHILD)) direct.push_back(h);
  }
  for (uint32_t h : direct) {
    out->push_back(h);
    if (deep) Children(h, true, out);
  }
}

HLE("user32.dll", EnumChildWindows, 3) {
  std::vector<uint32_t> kids;
  Children(Arg(c, 0), true, &kids);
  for (uint32_t h : kids)
    if (!CallGuest(c, Arg(c, 1), {h, Arg(c, 2)})) break;
  return 1;
}

HLE("user32.dll", GetClassNameA, 3) {
  std::string n;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(Arg(c, 0));
    if (!w) return 0;
    static const std::map<std::string, std::string> nice = {
        {"button", "Button"}, {"edit", "Edit"}, {"static", "Static"}, {"listbox", "ListBox"},
        {"combobox", "ComboBox"}, {"scrollbar", "ScrollBar"}};
    auto it = nice.find(w->cls);
    n = it != nice.end() ? it->second : w->cls;
  }
  return CopyOut(Arg(c, 1), Arg(c, 2), n.substr(0, Arg(c, 2) ? Arg(c, 2) - 1 : 0));
}

HLE("user32.dll", GetTopWindow, 1) {
  std::vector<uint32_t> kids;
  Children(Arg(c, 0), false, &kids);
  return kids.empty() ? 0 : kids.front();
}

HLE("user32.dll", ChildWindowFromPoint, 3) {
  int32_t x = int32_t(Arg(c, 1)), y = int32_t(Arg(c, 2));
  int ox, oy;
  WindowOrigin(Arg(c, 0), &ox, &oy);
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t best = Arg(c, 0);
  for (auto& [h, w] : g_windows)
    if (!w.destroyed && w.parent == Arg(c, 0) && (w.style & WS_CHILD) &&
        ContainsScreen(w, ox + x, oy + y))
      best = h;
  return best;
}

// ChildWindowFromPointEx(parent, x, y, flags): as ChildWindowFromPoint.
HLE("user32.dll", ChildWindowFromPointEx, 4) {
  int32_t x = int32_t(Arg(c, 1)), y = int32_t(Arg(c, 2));
  int ox, oy;
  WindowOrigin(Arg(c, 0), &ox, &oy);
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t best = Arg(c, 0);
  for (auto& [h, w] : g_windows)
    if (!w.destroyed && w.parent == Arg(c, 0) && (w.style & WS_CHILD) &&
        (!(Arg(c, 3) & 1) || w.visible) && ContainsScreen(w, ox + x, oy + y))
      best = h;
  return best;
}

HLE("user32.dll", WindowFromPoint, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t top = 0;
  for (auto& [h, w] : g_windows)
    if (!w.destroyed && w.sdl && w.visible) top = h;
  if (!top) return 0;
  int cx, cy;
  return RouteMouse(top, int32_t(Arg(c, 0)), int32_t(Arg(c, 1)), &cx, &cy);
}

// ---- built-in control classes ------------------------------------------------------
//
// "static", "edit" and "button" exist in every Windows process. Nothing is
// drawn; the text (the window title) and state are kept so the game can
// read them back. A read-only multi-line edit is a log the game appends to
// (a dedicated console window): appended text is echoed to stdout,
// which is where that log is visible here.

constexpr uint32_t ES_MULTILINE = 0x0004, ES_READONLY = 0x0800;
constexpr uint32_t EM_GETSEL = 0x00B0, EM_SETSEL = 0x00B1, EM_LINESCROLL = 0x00B6,
                   EM_SCROLLCARET = 0x00B7, EM_GETLINECOUNT = 0x00BA,
                   EM_REPLACESEL = 0x00C2, EM_SETLIMITTEXT = 0x00C5,
                   EM_SETREADONLY = 0x00CF;
constexpr uint32_t BM_GETCHECK = 0x00F0, BM_SETCHECK = 0x00F1, BM_GETSTATE = 0x00F2;
constexpr uint32_t STM_SETICON = 0x0170, STM_GETICON = 0x0171, STM_SETIMAGE = 0x0172,
                   STM_GETIMAGE = 0x0173;

uint32_t CallDefWindowProc(Ctx& c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
  uint32_t def = ProcAddress(ModuleHandle("user32.dll"), "DefWindowProcA");
  return CallGuest(c, def, {hwnd, msg, wp, lp});
}

// Messages every control handles the same way; false if `msg` is not one.
bool CommonControlMsg(Window& w, uint32_t msg, uint32_t wp, uint32_t lp, uint32_t* r) {
  switch (msg) {
    case WM_NCCREATE: *r = 1; return true;
    case WM_CREATE: *r = 0; return true;
    case WM_SETTEXT: w.title = lp ? GStr(lp) : ""; *r = 1; return true;
    case WM_GETTEXT: {
      if (!wp) { *r = 0; return true; }
      std::string t = w.title.substr(0, wp - 1);
      PutStr(lp, t);
      *r = uint32_t(t.size());
      return true;
    }
    case WM_GETTEXTLENGTH: *r = uint32_t(w.title.size()); return true;
    case WM_SETFONT: w.font = wp; *r = 0; return true;
    case WM_GETFONT: *r = w.font; return true;
    default: return false;
  }
}

// Writes s to stdout with CRLF and lone CR as plain newlines.
void EchoLog(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\r') {
      if (i + 1 < s.size() && s[i + 1] == '\n') continue;
      out += '\n';
    } else {
      out += s[i];
    }
  }
  std::fwrite(out.data(), 1, out.size(), stdout);
  std::fflush(stdout);
}

// Unknown control messages are reported once each and return 0.
void UnhandledControlMsg(const char* cls, uint32_t msg) {
  static std::set<std::pair<std::string, uint32_t>> seen;
  if (seen.insert({cls, msg}).second)
    WarnF("%s control: message %04X not modelled, returning 0\n", cls, msg);
}

HLE_NAMED("user32.dll", "<static>", StaticWndProc, 4) {
  uint32_t hwnd = Arg(c, 0), msg = Arg(c, 1), wp = Arg(c, 2), lp = Arg(c, 3), r = 0;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return 0;
    if (CommonControlMsg(*w, msg, wp, lp, &r)) return r;
    switch (msg) {
      case STM_SETIMAGE: case STM_SETICON: r = w->image; w->image = msg == STM_SETICON ? wp : lp; return r;
      case STM_GETIMAGE: case STM_GETICON: return w->image;
    }
    if (msg >= 0x0170 && msg <= 0x017F) {
      UnhandledControlMsg("static", msg);
      return 0;
    }
  }
  return CallDefWindowProc(c, hwnd, msg, wp, lp);
}

HLE_NAMED("user32.dll", "<button>", ButtonWndProc, 4) {
  uint32_t hwnd = Arg(c, 0), msg = Arg(c, 1), wp = Arg(c, 2), lp = Arg(c, 3), r = 0;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return 0;
    if (CommonControlMsg(*w, msg, wp, lp, &r)) return r;
    switch (msg) {
      case BM_GETCHECK: return w->check;
      case BM_SETCHECK: w->check = wp; return 0;
      case BM_GETSTATE: return w->check;
      case 0x00F4: return 0;  // BM_SETSTYLE
      case 0x00F5:            // BM_CLICK
      case WM_LBUTTONUP: {
        // A click: auto check boxes toggle, auto radio buttons check (and
        // their siblings uncheck); then BN_CLICKED to the parent.
        uint32_t type = w->style & 0xF;
        if (type == 3 /* BS_AUTOCHECKBOX */) w->check = !w->check;
        if (type == 6 /* BS_AUTO3STATE */) w->check = (w->check + 1) % 3;
        if (type == 9 /* BS_AUTORADIOBUTTON */) {
          for (auto& [h, o] : g_windows)
            if (!o.destroyed && o.parent == w->parent && o.cls == "button" && (o.style & 0xF) == 9)
              o.check = 0;
          w->check = 1;
        }
        if (type == 7 /* BS_GROUPBOX */) return 0;
        // Posted (the lock is held): it arrives through the game's loop.
        if (w->parent) Post(w->parent, 0x0111 /* WM_COMMAND */, w->id & 0xFFFF, hwnd);
        return 0;
      }
      case WM_LBUTTONDOWN: return 0;
    }
    if (msg >= 0x00F0 && msg <= 0x00FF) {
      UnhandledControlMsg("button", msg);
      return 0;
    }
  }
  return CallDefWindowProc(c, hwnd, msg, wp, lp);
}

HLE_NAMED("user32.dll", "<edit>", EditWndProc, 4) {
  uint32_t hwnd = Arg(c, 0), msg = Arg(c, 1), wp = Arg(c, 2), lp = Arg(c, 3), r = 0;
  std::string echo;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return 0;
    if (msg == WM_SETTEXT) w->sel_lo = w->sel_hi = 0;
    if (CommonControlMsg(*w, msg, wp, lp, &r)) return r;
    const uint32_t len = uint32_t(w->title.size());
    w->sel_lo = std::min(w->sel_lo, len);
    w->sel_hi = std::min(w->sel_hi, len);
    // Replaces the selection with t (cut to the text limit), caret after it.
    auto replace = [&](const std::string& t) {
      uint32_t room = w->limit - std::min(w->limit, len - (w->sel_hi - w->sel_lo));
      std::string ins = t.substr(0, room);
      w->title.replace(w->sel_lo, w->sel_hi - w->sel_lo, ins);
      w->sel_lo = w->sel_hi = w->sel_lo + uint32_t(ins.size());
      return ins;
    };
    switch (msg) {
      case EM_GETSEL:
        if (wp) Wr32(wp, w->sel_lo);
        if (lp) Wr32(lp, w->sel_hi);
        return MkLparam(int(std::min(w->sel_lo, 0xFFFFu)), int(std::min(w->sel_hi, 0xFFFFu)));
      case EM_SETSEL: {
        int32_t a = int32_t(wp), b = int32_t(lp);
        if (a == -1) {  // remove the selection, keep the caret
          w->sel_lo = w->sel_hi;
          return 1;
        }
        uint32_t ua = a < 0 ? len : std::min(uint32_t(a), len);
        uint32_t ub = b < 0 ? len : std::min(uint32_t(b), len);
        w->sel_lo = std::min(ua, ub);
        w->sel_hi = std::max(ua, ub);
        return 1;
      }
      case EM_REPLACESEL: {
        std::string ins = replace(lp ? GStr(lp) : "");
        if ((w->style & (ES_MULTILINE | ES_READONLY)) == (ES_MULTILINE | ES_READONLY))
          echo = ins;
        break;
      }
      case EM_LINESCROLL: return (w->style & ES_MULTILINE) ? 1 : 0;
      case EM_SCROLLCARET: return 1;
      case EM_GETLINECOUNT:
        return 1 + uint32_t(std::count(w->title.begin(), w->title.end(), '\n'));
      case EM_SETLIMITTEXT:
        w->limit = wp ? wp : ((w->style & ES_MULTILINE) ? 0xFFFFFFFFu : 0x7FFFFFFEu);
        return 0;
      case EM_SETREADONLY:
        w->style = wp ? (w->style | ES_READONLY) : (w->style & ~ES_READONLY);
        return 1;
      case WM_CHAR:
        if (w->style & ES_READONLY) return 0;
        if (wp == 8) {  // backspace: the selection, or the character before
          if (w->sel_lo == w->sel_hi && w->sel_lo > 0) w->sel_lo--;
          replace("");
        } else if (wp >= 32 && wp < 256) {
          replace(std::string(1, char(wp)));
        }
        return 0;
      default:
        if (msg >= 0x00B0 && msg <= 0x00DF) {
          UnhandledControlMsg("edit", msg);
          return 0;
        }
        break;
    }
  }
  if (msg == EM_REPLACESEL) {
    if (!echo.empty()) EchoLog(echo);
    return 0;
  }
  return CallDefWindowProc(c, hwnd, msg, wp, lp);
}

// ---- list boxes, combo boxes, trackbars, scroll bars ---------------------------------
//
// Their items, selection, range and position are kept; nothing is drawn
// (games that use them in their own dialogs draw them, owner-draw or from
// the state). A click selects (a list box's row from its item height, a
// combo box's next item, a trackbar's position from x) and notifies the
// parent as the real controls do.

constexpr uint32_t LBS_NOTIFY = 0x1, LBS_SORT = 0x2, LBS_OWNERDRAWFIXED = 0x10,
                   LBS_OWNERDRAWVARIABLE = 0x20, LBS_HASSTRINGS = 0x40;
constexpr uint32_t CBS_SORT = 0x100, CBS_OWNERDRAWFIXED = 0x10, CBS_OWNERDRAWVARIABLE = 0x20,
                   CBS_HASSTRINGS = 0x200;
constexpr uint32_t LB_ERR = 0xFFFFFFFF;

void NotifyParent(uint32_t hwnd, uint32_t code) {
  uint32_t parent = 0, id = 0;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return;
    parent = w->parent;
    id = w->id;
  }
  if (parent) Send(parent, WM_COMMAND, (id & 0xFFFF) | (code << 16), hwnd);
}

// Item messages shared by list and combo boxes, given each one's codes.
struct ItemMsgs {
  uint32_t add, insert, del, reset, setsel, getsel, gettext, gettextlen, count, findstr,
      findexact, selectstr, getdata, setdata;
  bool strings, sort;
};

bool ItemMsg(Window& w, const ItemMsgs& k, uint32_t msg, uint32_t wp, uint32_t lp, uint32_t* r) {
  auto text_of = [&](uint32_t v) { return k.strings ? (v ? GStr(v) : std::string()) : std::string(); };
  auto data_of = [&](uint32_t v) { return k.strings ? 0u : v; };
  auto find = [&](uint32_t start, const std::string& s, bool exact) -> uint32_t {
    size_t n = w.items.size();
    for (size_t i = 0; i < n; i++) {
      size_t j = (size_t(int32_t(start) + 1) + i) % n;
      std::string a = Lower(w.items[j].first), b = Lower(s);
      if (exact ? a == b : a.compare(0, b.size(), b) == 0) return uint32_t(j);
    }
    return LB_ERR;
  };
  if (msg == k.add) {
    std::pair<std::string, uint32_t> it{text_of(lp), data_of(lp)};
    size_t at = w.items.size();
    if (k.sort && k.strings)
      at = size_t(std::upper_bound(w.items.begin(), w.items.end(), it,
                                   [](auto& a, auto& b) { return Lower(a.first) < Lower(b.first); }) -
                  w.items.begin());
    w.items.insert(w.items.begin() + long(at), it);
    *r = uint32_t(at);
  } else if (msg == k.insert) {
    size_t at = int32_t(wp) < 0 || wp > w.items.size() ? w.items.size() : wp;
    w.items.insert(w.items.begin() + long(at), {text_of(lp), data_of(lp)});
    *r = uint32_t(at);
  } else if (msg == k.del) {
    if (wp >= w.items.size()) { *r = LB_ERR; return true; }
    w.items.erase(w.items.begin() + long(wp));
    if (w.cur_sel >= int32_t(w.items.size())) w.cur_sel = -1;
    *r = uint32_t(w.items.size());
  } else if (msg == k.reset) {
    w.items.clear();
    w.cur_sel = -1;
    w.top_index = 0;
    *r = 0;
  } else if (msg == k.setsel) {
    w.cur_sel = int32_t(wp) < int32_t(w.items.size()) ? int32_t(wp) : -1;
    if (w.cur_sel >= 0 && w.cls == "combobox") w.title = w.items[size_t(w.cur_sel)].first;
    *r = w.cur_sel < 0 ? LB_ERR : uint32_t(w.cur_sel);
  } else if (msg == k.getsel) {
    *r = uint32_t(w.cur_sel);
  } else if (msg == k.gettext) {
    if (wp >= w.items.size()) { *r = LB_ERR; return true; }
    if (k.strings) {
      PutStr(lp, w.items[wp].first);
      *r = uint32_t(w.items[wp].first.size());
    } else {
      Wr32(lp, w.items[wp].second);
      *r = 4;
    }
  } else if (msg == k.gettextlen) {
    *r = wp < w.items.size() ? (k.strings ? uint32_t(w.items[wp].first.size()) : 4) : LB_ERR;
  } else if (msg == k.count) {
    *r = uint32_t(w.items.size());
  } else if (msg == k.findstr || msg == k.findexact || msg == k.selectstr) {
    *r = w.items.empty() ? LB_ERR : find(wp, lp ? GStr(lp) : "", msg == k.findexact);
    if (msg == k.selectstr && *r != LB_ERR) w.cur_sel = int32_t(*r);
  } else if (msg == k.getdata) {
    *r = wp < w.items.size() ? w.items[wp].second : LB_ERR;
  } else if (msg == k.setdata) {
    if (wp >= w.items.size()) { *r = LB_ERR; return true; }
    w.items[wp].second = lp;
    *r = 0;
  } else {
    return false;
  }
  return true;
}

uint32_t ItemHeight(const Window& w) { return w.limit == 30000 ? 16 : w.limit; }  // reused field

// Owner-drawn list and combo boxes: WM_MEASUREITEM to the parent when the
// control is created (its item height), WM_DRAWITEM for each visible item
// (a combo box: its selection, in the face) when it paints.
constexpr uint32_t WM_DRAWITEM = 0x002B, WM_MEASUREITEM = 0x002C;
constexpr uint32_t ODT_LISTBOX = 2, ODT_COMBOBOX = 3;

bool IsOwnerDraw(const Window& w) {
  if (w.cls == "listbox") return w.style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE);
  if (w.cls == "combobox") return w.style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE);
  return false;
}

void MeasureOwnerDraw(uint32_t hwnd) {
  uint32_t parent, id, type;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w || !IsOwnerDraw(*w) || !w->parent) return;
    parent = w->parent;
    id = w->id;
    type = w->cls == "listbox" ? ODT_LISTBOX : ODT_COMBOBOX;
  }
  uint32_t mi = HeapAllocProcess(24, true);
  Wr32(mi + 0, type);
  Wr32(mi + 4, id);
  Wr32(mi + 8, 0);
  Wr32(mi + 16, 16);  // itemHeight: the default
  Send(parent, WM_MEASUREITEM, id, mi);
  uint32_t h = Rd32(mi + 16);
  HeapFreeProcess(mi);
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (Window* w = Win(hwnd)) w->limit = h ? h : 16;
}

void PaintOwnerDraw(uint32_t hwnd) {
  struct Item { uint32_t id, state, data; int top, bottom; };
  std::vector<Item> items;
  uint32_t parent, ctl, type;
  int width;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w || !w->parent) return;
    parent = w->parent;
    ctl = w->id;
    width = w->w;
    int ih = int(ItemHeight(*w));
    if (w->cls == "listbox") {
      type = ODT_LISTBOX;
      for (int i = w->top_index; i < int(w->items.size()); i++) {
        int y = (i - w->top_index) * ih;
        if (y >= w->h) break;
        items.push_back({uint32_t(i), i == w->cur_sel ? 1u : 0u, w->items[size_t(i)].second, y, y + ih});
      }
    } else {
      type = ODT_COMBOBOX;
      uint32_t data = w->cur_sel >= 0 && w->cur_sel < int(w->items.size())
                          ? w->items[size_t(w->cur_sel)].second
                          : 0;
      // The face (selection field); ODS_COMBOBOXEDIT (0x1000).
      items.push_back({uint32_t(w->cur_sel), 0x1000, data, 0, std::min(w->h, ih + 4)});
      width = std::max(0, w->w - 16);  // left of the drop-down button
    }
  }
  uint32_t dis = HeapAllocProcess(48, true);
  for (const Item& it : items) {
    Wr32(dis + 0, type);
    Wr32(dis + 4, ctl);
    Wr32(dis + 8, it.id);
    Wr32(dis + 12, 1);  // ODA_DRAWENTIRE
    Wr32(dis + 16, it.state);
    Wr32(dis + 20, hwnd);
    Wr32(dis + 24, DcForWindow(hwnd));
    Wr32(dis + 28, 0);
    Wr32(dis + 32, uint32_t(it.top));
    Wr32(dis + 36, uint32_t(width));
    Wr32(dis + 40, uint32_t(it.bottom));
    Wr32(dis + 44, it.data);
    Send(parent, WM_DRAWITEM, ctl, dis);
  }
  HeapFreeProcess(dis);
}

HLE_NAMED("user32.dll", "<listbox>", ListBoxWndProc, 4) {
  uint32_t hwnd = Arg(c, 0), msg = Arg(c, 1), wp = Arg(c, 2), lp = Arg(c, 3), r = 0;
  if (msg == WM_CREATE) {
    MeasureOwnerDraw(hwnd);
    return 0;
  }
  if (msg == 0x000F /* WM_PAINT */) {
    bool od;
    {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Window* w = Win(hwnd);
      od = w && IsOwnerDraw(*w);
    }
    if (od) PaintOwnerDraw(hwnd);
    Validate(hwnd);
    return 0;
  }
  bool notify = false, dbl = false;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return 0;
    if (CommonControlMsg(*w, msg, wp, lp, &r)) return r;
    bool strings = (w->style & LBS_HASSTRINGS) ||
                   !(w->style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE));
    static const ItemMsgs kLb{0x180, 0x181, 0x182, 0x184, 0x186, 0x188, 0x189, 0x18A,
                              0x18B, 0x18F, 0x1A2, 0x18C, 0x199, 0x19A, true, false};
    ItemMsgs k = kLb;
    k.strings = strings;
    k.sort = w->style & LBS_SORT;
    if (ItemMsg(*w, k, msg, wp, lp, &r)) return r;
    switch (msg) {
      case 0x185:  // LB_SETSEL (multi-select): one selection kept
        if (wp) w->cur_sel = int32_t(lp);
        return 0;
      case 0x187: return int32_t(wp) == w->cur_sel ? 1 : 0;           // LB_GETSEL
      case 0x190: return w->cur_sel >= 0 ? 1 : 0;                      // LB_GETSELCOUNT
      case 0x191:                                                       // LB_GETSELITEMS
        if (w->cur_sel < 0 || !wp) return 0;
        Wr32(lp, uint32_t(w->cur_sel));
        return 1;
      case 0x18E: return uint32_t(w->top_index);                       // LB_GETTOPINDEX
      case 0x197: w->top_index = int32_t(wp); return 0;                // LB_SETTOPINDEX
      case 0x19E: w->cur_sel = int32_t(wp); return 0;                  // LB_SETCARETINDEX
      case 0x19F: return uint32_t(w->cur_sel);                         // LB_GETCARETINDEX
      case 0x1A0: w->limit = lp & 0xFFFF; return 0;                    // LB_SETITEMHEIGHT
      case 0x1A1: return ItemHeight(*w);                               // LB_GETITEMHEIGHT
      case 0x198: {                                                    // LB_GETITEMRECT
        int32_t y = (int32_t(wp) - w->top_index) * int32_t(ItemHeight(*w));
        Wr32(lp, 0);
        Wr32(lp + 4, uint32_t(y));
        Wr32(lp + 8, uint32_t(w->w));
        Wr32(lp + 12, uint32_t(y + int32_t(ItemHeight(*w))));
        return 0;
      }
      case 0x1A9: return 0;                                             // LB_ITEMFROMPOINT
      case 0x194: case 0x195: case 0x196: return 0;                    // horizontal extent
      case WM_LBUTTONDOWN:
      case WM_LBUTTONDBLCLK: {
        int32_t row = w->top_index + int32_t(int16_t(lp >> 16)) / int32_t(ItemHeight(*w));
        if (row >= 0 && row < int32_t(w->items.size())) {
          w->cur_sel = row;
          notify = w->style & LBS_NOTIFY;
          dbl = msg == WM_LBUTTONDBLCLK;
        }
        break;
      }
      default:
        if (msg >= 0x0180 && msg <= 0x01AF) {
          UnhandledControlMsg("listbox", msg);
          return 0;
        }
        return CallDefWindowProc(c, hwnd, msg, wp, lp);
    }
  }
  if (notify) NotifyParent(hwnd, dbl ? 2 /* LBN_DBLCLK */ : 1 /* LBN_SELCHANGE */);
  return 0;
}

HLE_NAMED("user32.dll", "<combobox>", ComboBoxWndProc, 4) {
  uint32_t hwnd = Arg(c, 0), msg = Arg(c, 1), wp = Arg(c, 2), lp = Arg(c, 3), r = 0;
  if (msg == WM_CREATE) {
    MeasureOwnerDraw(hwnd);
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (Window* w = Win(hwnd)) w->h = ComboFaceHeight(*w, w->h);
    return 0;
  }
  if (msg == 0x000F /* WM_PAINT */) {
    bool od;
    {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Window* w = Win(hwnd);
      od = w && IsOwnerDraw(*w);
    }
    if (od) PaintOwnerDraw(hwnd);
    Validate(hwnd);
    return 0;
  }
  bool notify = false;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return 0;
    if (CommonControlMsg(*w, msg, wp, lp, &r)) return r;
    bool strings = (w->style & CBS_HASSTRINGS) ||
                   !(w->style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE));
    static const ItemMsgs kCb{0x143, 0x14A, 0x144, 0x14B, 0x14E, 0x147, 0x148, 0x149,
                              0x146, 0x14C, 0x158, 0x14D, 0x150, 0x151, true, false};
    ItemMsgs k = kCb;
    k.strings = strings;
    k.sort = w->style & CBS_SORT;
    if (ItemMsg(*w, k, msg, wp, lp, &r)) return r;
    switch (msg) {
      case 0x14F: w->dropped = wp != 0; return 1;                     // CB_SHOWDROPDOWN
      case 0x157: return w->dropped ? 1 : 0;                           // CB_GETDROPPEDSTATE
      case 0x153: w->limit = lp & 0xFFFF; return 0;                    // CB_SETITEMHEIGHT
      case 0x154: return ItemHeight(*w);                               // CB_GETITEMHEIGHT
      case 0x141: case 0x142: case 0x155: case 0x156: return 1;        // limit, edit sel, ext UI
      case 0x140: return 0;                                             // CB_GETEDITSEL
      case 0x152: {                                                     // CB_GETDROPPEDCONTROLRECT
        int sx, sy;
        WindowScreenPos(*w, &sx, &sy);
        Wr32(lp, uint32_t(sx));
        Wr32(lp + 4, uint32_t(sy));
        Wr32(lp + 8, uint32_t(sx + w->w));
        Wr32(lp + 12, uint32_t(sy + w->h));
        return 1;
      }
      case WM_LBUTTONDOWN:
        if (!w->items.empty()) {
          w->cur_sel = (w->cur_sel + 1) % int32_t(w->items.size());
          w->title = w->items[size_t(w->cur_sel)].first;
          notify = true;
        }
        break;
      default:
        if (msg >= 0x0140 && msg <= 0x016F) {
          UnhandledControlMsg("combobox", msg);
          return 0;
        }
        return CallDefWindowProc(c, hwnd, msg, wp, lp);
    }
  }
  if (notify) NotifyParent(hwnd, 1 /* CBN_SELCHANGE */);
  return 0;
}

HLE_NAMED("user32.dll", "<trackbar>", TrackbarWndProc, 4) {
  uint32_t hwnd = Arg(c, 0), msg = Arg(c, 1), wp = Arg(c, 2), lp = Arg(c, 3), r = 0;
  uint32_t parent = 0, pos = 0;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return 0;
    if (CommonControlMsg(*w, msg, wp, lp, &r)) return r;
    switch (msg) {
      case 0x400: return uint32_t(w->pos);                              // TBM_GETPOS
      case 0x401: return uint32_t(w->range_lo);                         // TBM_GETRANGEMIN
      case 0x402: return uint32_t(w->range_hi);                         // TBM_GETRANGEMAX
      case 0x405: w->pos = std::clamp(int32_t(lp), w->range_lo, w->range_hi); return 0;
      case 0x406: w->range_lo = int16_t(lp); w->range_hi = int16_t(lp >> 16); return 0;
      case 0x407: w->range_lo = int32_t(lp); return 0;
      case 0x408: w->range_hi = int32_t(lp); return 0;
      case WM_LBUTTONDOWN: {
        int32_t x = int16_t(lp);
        int32_t span = std::max(1, w->w - 1);
        w->pos = std::clamp(w->range_lo + x * (w->range_hi - w->range_lo) / span, w->range_lo,
                            w->range_hi);
        parent = w->parent;
        pos = uint32_t(w->pos);
        break;
      }
      default:
        if (msg >= 0x0400 && msg <= 0x0430) return 0;  // tics, page/line size, selection
        return CallDefWindowProc(c, hwnd, msg, wp, lp);
    }
  }
  if (parent) Send(parent, WM_HSCROLL, 4 /* TB_THUMBPOSITION */ | (pos << 16), hwnd);
  return 0;
}

HLE_NAMED("user32.dll", "<scrollbar>", ScrollBarWndProc, 4) {
  uint32_t hwnd = Arg(c, 0), msg = Arg(c, 1), wp = Arg(c, 2), lp = Arg(c, 3), r = 0;
  uint32_t parent = 0, code = 0, sbmsg = WM_VSCROLL;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return 0;
    if (CommonControlMsg(*w, msg, wp, lp, &r)) return r;
    switch (msg) {
      case 0xE0: r = uint32_t(w->pos); w->pos = int32_t(wp); return r;  // SBM_SETPOS
      case 0xE1: return uint32_t(w->pos);                               // SBM_GETPOS
      case 0xE2: case 0xE6: w->range_lo = int32_t(wp); w->range_hi = int32_t(lp); return 0;
      case 0xE3:                                                        // SBM_GETRANGE
        if (wp) Wr32(wp, uint32_t(w->range_lo));
        if (lp) Wr32(lp, uint32_t(w->range_hi));
        return 1;
      case 0xE9: case 0xEA: {  // SBM_SETSCROLLINFO / SBM_GETSCROLLINFO: SCROLLINFO at lp
        uint32_t mask = Rd32(lp + 4);
        if (msg == 0xE9) {
          if (mask & 1) { w->range_lo = int32_t(Rd32(lp + 8)); w->range_hi = int32_t(Rd32(lp + 12)); }
          if (mask & 4) w->pos = int32_t(Rd32(lp + 20));
          return uint32_t(w->pos);
        }
        if (mask & 1) { Wr32(lp + 8, uint32_t(w->range_lo)); Wr32(lp + 12, uint32_t(w->range_hi)); }
        if (mask & 2) Wr32(lp + 16, 1);
        if (mask & 4) Wr32(lp + 20, uint32_t(w->pos));
        if (mask & 0x10) Wr32(lp + 24, uint32_t(w->pos));
        return 1;
      }
      case WM_LBUTTONDOWN: {
        bool horz = !(w->style & 1);  // SBS_VERT
        int32_t at = horz ? int16_t(lp) : int16_t(lp >> 16);
        int32_t len = horz ? w->w : w->h;
        code = at < len / 2 ? 0 /* SB_LINEUP */ : 1 /* SB_LINEDOWN */;
        sbmsg = horz ? WM_HSCROLL : WM_VSCROLL;
        parent = w->parent;
        break;
      }
      default:
        return CallDefWindowProc(c, hwnd, msg, wp, lp);
    }
  }
  if (parent) Send(parent, sbmsg, code, hwnd);
  return 0;
}

HLE("user32.dll", MsgWaitForMultipleObjects, 5) {
  // Wait for objects or for any queued input/message (wake mask ignored).
  uint32_t n = Arg(c, 0), arr = Arg(c, 1), wait_all = Arg(c, 2),
           timeout = Arg(c, 3);
  uint32_t wfm = ProcAddress(ModuleHandle("kernel32.dll"), "WaitForMultipleObjects");
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
  for (;;) {
    PumpHostEvents();
    {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Queue& q = g_queues[CurrentThread()->tid];
      if (!q.msgs.empty() || q.quit) return n;  // WAIT_OBJECT_0 + n
    }
    if (n) {
      uint32_t r = CallGuest(c, wfm, {n, arr, wait_all, 0});
      if (r != 0x102) return r;
    }
    if (timeout != 0xFFFFFFFF && std::chrono::steady_clock::now() >= deadline)
      return 0x102;  // WAIT_TIMEOUT
    if (OnMainThread() && g_sdl_up)
      SDL_WaitEventTimeout(nullptr, 1);
    else
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// ---- hooks ---------------------------------------------------------------------------
// WH_CBT (5) hooks are called for HCBT_CREATEWND (CbtCreateWindow): class
// libraries attach their window objects to new windows there. Other hooks
// get a handle and are never called: WH_KEYBOARD_LL (13), installed to
// swallow the Windows keys (our input never produces system hotkeys), and
// WH_MSGFILTER (-1), called only by user32's own modal loops (menus,
// message boxes), which do not run guest code here.

HLE64("user32.dll", SetWindowsHookExA, 4) {
  static uint32_t next = 0x00F00010;
  int32_t type = int32_t(Arg(c, 0));
  uint32_t h = next += 0x10;
  if (type == kWhCbt || type == kWhKeyboard) {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    uint32_t tid = uint32_t(Arg(c, 3));
    g_hooks.insert(g_hooks.begin(), Hook{type, h, uint32_t(Arg(c, 1)), tid});
  } else {
    RECOMP_TRACE("  SetWindowsHookExA(type %d, proc %08llX): never called\n", type,
                 (unsigned long long)Arg(c, 1));
  }
  return HandleRet(h);
}
HLE64("user32.dll", UnhookWindowsHookEx, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  for (auto it = g_hooks.begin(); it != g_hooks.end(); ++it)
    if (it->handle == uint32_t(Arg(c, 0))) {
      g_hooks.erase(it);
      break;
    }
  return 1;
}
// CallNextHookEx(hook, code, wp, lp): the next hook of `hook`'s type in
// the chain (for this thread or global); 0 at its end.
HLE64("user32.dll", CallNextHookEx, 4) {
  int32_t type = 0;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    for (const Hook& k : g_hooks)
      if (k.handle == uint32_t(Arg(c, 0))) type = k.type;
  }
  uint32_t proc = type ? NextHook(type, uint32_t(Arg(c, 0))) : 0;
  if (!proc) return 0;
  return CallGuest(c, proc, {Arg(c, 1), Arg(c, 2), Arg(c, 3)});
}

// ---- cursor ------------------------------------------------------------------------

HLE("user32.dll", ShowCursor, 1) {
  g_cursor_count += Arg(c, 0) ? 1 : -1;
  if (g_sdl_up && !Background()) {
    if (g_cursor_count >= 0)
      SDL_ShowCursor();
    else
      SDL_HideCursor();
  }
  return uint32_t(g_cursor_count);
}

// Some games re-centre the pointer every frame and move their own cursor
// by trunc((pos - ref) * sensitivity), clamped to the screen, where ref is
// the last position they read or set (some do in fullscreen). Over
// a desktop window that drifts from the host pointer, so while the pointer
// is over the window and the game is re-centring, GetCursorPos returns the
// position that lands the game's cursor exactly under the pointer.
// GameConfig::game_cursor says where that cursor is.
bool SyncedCursorPos(int32_t* sx, int32_t* sy) {
  if (!g_pointer_inside || !Game().game_cursor) return false;
  // Only while the game is re-centring (otherwise it reads absolute
  // positions and the plain pointer position is right).
  if (int64_t(TickCount()) - g_last_recentre_ms > 250) return false;
  uint32_t app = 0;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    for (auto& [h, w] : g_windows)
      if (w.mode_w && !w.destroyed) app = w.userdata;
  }
  int32_t gx, gy;
  float kx = 1, ky = 1;
  if (!app || !Game().game_cursor(app, &gx, &gy, &kx, &ky)) return false;
  int32_t dx = int32_t(std::lround((g_real_x - gx) / kx));
  int32_t dy = int32_t(std::lround((g_real_y - gy) / ky));
  *sx = g_recentre_x + dx;
  *sy = g_recentre_y + dy;
  return true;
}

HLE("user32.dll", GetCursorPos, 1) {
  int32_t x = g_mouse_x, y = g_mouse_y;
  SyncedCursorPos(&x, &y);
  Wr32(Arg(c, 0), uint32_t(x));
  Wr32(Arg(c, 0) + 4, uint32_t(y));
  return 1;
}

// mouse_event(flags, dx, dy, data, extra): synthesised input. Movement
// (MOUSEEVENTF_MOVE 1) moves the cursor as a real mouse would: by dx/dy
// mickeys (taken as pixels: no acceleration), or with MOUSEEVENTF_ABSOLUTE
// (0x8000) to dx/dy in 0-65535 across the screen; DirectInput sees the
// relative motion. Games nudge the mouse by +1 and -1 to keep the
// screensaver away. Synthesised buttons and wheel are not provided yet.
HLE("user32.dll", mouse_event, 5) {
  uint32_t flags = Arg(c, 0);
  int32_t dx = int32_t(Arg(c, 1)), dy = int32_t(Arg(c, 2));
  if (flags & ~0x8001u)
    FatalF("mouse_event(flags %X): synthesised buttons and wheel are not supported yet", flags);
  if (!(flags & 1)) return 0;
  int32_t ox = g_mouse_x, oy = g_mouse_y;
  if (flags & 0x8000) {
    g_mouse_x = int32_t(int64_t(dx) * g_display_w / 65536);
    g_mouse_y = int32_t(int64_t(dy) * g_display_h / 65536);
  } else {
    g_mouse_x = std::clamp(g_mouse_x + dx, 0, g_display_w - 1);
    g_mouse_y = std::clamp(g_mouse_y + dy, 0, g_display_h - 1);
  }
  if (g_mouse_x != ox) EmitRawInput(RawInput::MouseX, 0, g_mouse_x - ox);
  if (g_mouse_y != oy) EmitRawInput(RawInput::MouseY, 0, g_mouse_y - oy);
  return 0;
}

HLE("user32.dll", SetCursorPos, 2) {
  g_mouse_x = g_recentre_x = int32_t(Arg(c, 0));
  g_mouse_y = g_recentre_y = int32_t(Arg(c, 1));
  g_last_recentre_ms = TickCount();
  return 1;
}

// ClipCursor(RECT const*): NULL frees the cursor. A rectangle inside one
// of our windows makes its cursor virtual and grabs the host pointer.
HLE("user32.dll", ClipCursor, 1) {
  uint32_t rc = Arg(c, 0);
  SDL_Window* release = nullptr;
  SDL_Window* grab = nullptr;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (g_clip.on && g_clip.hwnd)
      if (Window* w = Win(g_clip.hwnd)) release = w->sdl;
    g_clip = {};
    if (rc) {
      g_clip.on = true;
      g_clip.l = int32_t(Rd32(rc));
      g_clip.t = int32_t(Rd32(rc + 4));
      g_clip.r = int32_t(Rd32(rc + 8));
      g_clip.b = int32_t(Rd32(rc + 12));
      g_clip.hwnd = ClipWindow();
      if (Window* w = Win(g_clip.hwnd)) grab = w->sdl;
      g_mouse_x = std::clamp(g_mouse_x, g_clip.l, std::max(g_clip.l, g_clip.r - 1));
      g_mouse_y = std::clamp(g_mouse_y, g_clip.t, std::max(g_clip.t, g_clip.b - 1));
    }
  }
  if (release && release != grab) SDL_SetWindowRelativeMouseMode(release, false);
  if (grab && !Background()) SDL_SetWindowRelativeMouseMode(grab, true);
  return 1;
}

// Mouse capture: there is one game window, so mouse messages go to it
// either way; the capture window is only remembered.
HLE("user32.dll", SetCapture, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t old = g_capture;
  g_capture = Arg(c, 0);
  return old;
}

HLE("user32.dll", GetCapture, 0) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return g_capture;
}

HLE("user32.dll", ReleaseCapture, 0) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  g_capture = 0;
  return 1;
}

// ---- display modes -------------------------------------------------------------------

struct Mode {
  int w, h, bpp, hz;
};

std::vector<Mode> Modes() {
  std::vector<Mode> m;
  for (int bpp : {16, 32})
    for (const ModeSize& s : DisplayModeSizes())
      for (int hz : DisplayModeRates()) m.push_back({s.w, s.h, bpp, hz});
  return m;
}

void PutDevMode(uint32_t dm, const Mode& m) {
  std::memset(GPtr(dm), 0, 156);
  PutStr(dm, win32::Upper(Game().id) + " Display");
  Wr16(dm + 32, 0x0401);  // dmSpecVersion
  Wr16(dm + 36, 156);     // dmSize
  Wr32(dm + 40, 0x001C0000 | 0x00400000);  // BITSPERPEL|PELSWIDTH|PELSHEIGHT|DISPLAYFREQUENCY
  Wr32(dm + 104, uint32_t(m.bpp));
  Wr32(dm + 108, uint32_t(m.w));
  Wr32(dm + 112, uint32_t(m.h));
  Wr32(dm + 120, uint32_t(m.hz));
}

HLE("user32.dll", EnumDisplaySettingsA, 3) {
  EnsureSdlVideo();
  uint32_t idx = Arg(c, 1), dm = Arg(c, 2);
  if (idx == 0xFFFFFFFF || idx == 0xFFFFFFFE) {  // ENUM_CURRENT/REGISTRY_SETTINGS
    PutDevMode(dm, {g_display_w, g_display_h, 32, DesktopRefreshRate()});
    return 1;
  }
  auto modes = Modes();
  if (idx >= modes.size()) return 0;
  PutDevMode(dm, modes[idx]);
  return 1;
}

// ---- monitors -----------------------------------------------------------------
// One monitor, the primary, covering the screen the game sees (the
// virtual display mode); its work area is all of it (no taskbar).

// kPrimaryMonitor (user32_window.h) is its HMONITOR.

// MonitorFromWindow(hwnd, flags), MonitorFromPoint(POINT by value, flags),
// MonitorFromRect(rect, flags): the one monitor, whatever the flags ask
// for a window or point off every monitor (nearest or primary are it).
HLE("user32.dll", MonitorFromWindow, 2) { return kPrimaryMonitor; }
HLE("user32.dll", MonitorFromPoint, 3) { return kPrimaryMonitor; }
HLE("user32.dll", MonitorFromRect, 2) { return kPrimaryMonitor; }

HLE("user32.dll", SetProcessDPIAware, 0) { return 1; }

// EnumDisplayMonitors(hdc, clip, proc, data): proc(hmonitor, hdc, &rect,
// data) for the one monitor; stops early if it returns FALSE.
HLE("user32.dll", EnumDisplayMonitors, 4) {
  EnsureSdlVideo();
  gaddr rc = HeapAllocProcess(16, true);
  Wr32(rc + 8, uint32_t(g_display_w));
  Wr32(rc + 12, uint32_t(g_display_h));
  CallGuest(c, Arg(c, 2), {kPrimaryMonitor, Arg(c, 0), rc, Arg(c, 3)});
  HeapFreeProcess(rc);
  return 1;
}

// GetMonitorInfoA(hmonitor, MONITORINFO(EXA)*): cbSize 40, or 72 with the
// device name.
HLE("user32.dll", GetMonitorInfoA, 2) {
  EnsureSdlVideo();
  gaddr mi = Arg(c, 1);
  uint32_t cb = Rd32(mi);
  if (Arg(c, 0) != kPrimaryMonitor || (cb != 40 && cb != 72)) return 0;
  for (gaddr r : {mi + 4, mi + 20}) {
    Wr32(r + 0, 0);
    Wr32(r + 4, 0);
    Wr32(r + 8, uint32_t(g_display_w));
    Wr32(r + 12, uint32_t(g_display_h));
  }
  Wr32(mi + 36, 1);  // MONITORINFOF_PRIMARY
  if (cb == 72) {
    std::memset(GPtr(mi + 40), 0, 32);
    PutStr(mi + 40, "\\\\.\\DISPLAY1");
  }
  return 1;
}

// EnumDisplayDevicesA(device, index, DISPLAY_DEVICEA*, flags): with no
// device, the adapters (one, "\\.\DISPLAY1"); with an adapter's name,
// its monitors (one). DISPLAY_DEVICEA: cb, DeviceName[32],
// DeviceString[128], StateFlags, DeviceID[128], DeviceKey[128].
HLE("user32.dll", EnumDisplayDevicesA, 4) {
  std::string dev = Arg(c, 0) ? GStr(Arg(c, 0)) : "";
  gaddr dd = Arg(c, 2);
  if (Arg(c, 1) != 0 || (!dev.empty() && Lower(dev) != "\\\\.\\display1")) return 0;
  uint32_t cb = Rd32(dd);
  if (cb < 168) return 0;
  std::memset(GPtr(dd + 4), 0, std::min<uint32_t>(cb, 424) - 4);
  if (dev.empty()) {
    PutStr(dd + 4, "\\\\.\\DISPLAY1");
    PutStr(dd + 36, "Display Adapter");
    Wr32(dd + 164, 0x1 | 0x4);  // ATTACHED_TO_DESKTOP | PRIMARY_DEVICE
  } else {
    PutStr(dd + 4, "\\\\.\\DISPLAY1\\Monitor0");
    PutStr(dd + 36, "Generic PnP Monitor");
    Wr32(dd + 164, 0x1 | 0x2);  // DISPLAY_DEVICE_ACTIVE | ATTACHED
  }
  return 1;
}

// DwmSetWindowAttribute(hwnd, attribute, data, size): frame colours,
// rounded corners, dark title bars; nothing the host window takes.
HLE("dwmapi.dll", DwmSetWindowAttribute, 4) { return 0; }  // S_OK

// Display modes are virtual: the host's mode never changes here. A mode
// the game switches to becomes the screen it sees (GetSystemMetrics,
// EnumDisplaySettings), as after a real switch; a window covering it goes
// host fullscreen (UpdateFullscreen). NULL restores the desktop size.
HLE("user32.dll", ChangeDisplaySettingsA, 2) {
  EnsureSdlVideo();
  uint32_t dm = Arg(c, 0), flags = Arg(c, 1);
  if (!dm) {
    g_display_w = g_desktop_w;
    g_display_h = g_desktop_h;
    UpdateFullscreenAll();
    return 0;  // DISP_CHANGE_SUCCESSFUL
  }
  uint32_t fields = Rd32(dm + 40);
  int w = (fields & 0x00080000) ? int(Rd32(dm + 108)) : g_display_w;  // DM_PELSWIDTH
  int h = (fields & 0x00100000) ? int(Rd32(dm + 112)) : g_display_h;  // DM_PELSHEIGHT
  int bpp = (fields & 0x00040000) ? int(Rd32(dm + 104)) : 32;         // DM_BITSPERPEL
  bool known = false;
  for (const Mode& m : Modes()) known |= m.w == w && m.h == h && m.bpp == bpp;
  if (!known) return uint32_t(-2);  // DISP_CHANGE_BADMODE
  if (flags & 2) return 0;          // CDS_TEST
  g_display_w = w;
  g_display_h = h;
  UpdateFullscreenAll();
  return 0;
}

// ---- painting and GDI device contexts (no GDI drawing reaches the screen;
// the game renders through DirectDraw) -------------------------------------------------

// Device contexts are named after their window (see DcForWindow); the
// screen DC (GetDC(NULL)) is a constant.
HLE("user32.dll", GetDC, 1) { return DcForWindow(Arg(c, 0)); }
HLE("user32.dll", ReleaseDC, 2) { return 1; }
HLE("user32.dll", BeginPaint, 2) {
  uint32_t ps = Arg(c, 1), dc = DcForWindow(Arg(c, 0));
  std::memset(GPtr(ps), 0, 64);
  Wr32(ps, dc);
  // rcPaint: the whole client area.
  int cw = 0, ch = 0;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (Window* w = Win(Arg(c, 0))) ClientSize(*w, &cw, &ch);
  }
  Wr32(ps + 16, uint32_t(cw));
  Wr32(ps + 20, uint32_t(ch));
  Validate(Arg(c, 0));
  return dc;
}
HLE("user32.dll", EndPaint, 2) { return 1; }
HLE("user32.dll", DrawTextA, 5) { return 16; }
HLE("user32.dll", DrawFrameControl, 4) { return 1; }

// ---- clipboard (not wired to the host yet) --------------------------------------------

HLE("user32.dll", OpenClipboard, 1) { return 0; }
HLE("user32.dll", CloseClipboard, 0) { return 1; }
HLE("user32.dll", EmptyClipboard, 0) { return 0; }
HLE("user32.dll", GetClipboardData, 1) { return 0; }
HLE("user32.dll", SetClipboardData, 2) { return 0; }

}  // namespace
}  // namespace hle

bool PostWindowMessage(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam) {
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (!Win(hwnd)) return false;
  }
  Post(hwnd, msg, wparam, lparam);
  return true;
}

void ScreenPointToClient(uint32_t hwnd, int* x, int* y) {
  int ox, oy;
  WindowOrigin(hwnd, &ox, &oy);
  *x -= ox;
  *y -= oy;
}

void SetPointerRemap(uint32_t hwnd, int shown_l, int shown_t, int shown_w, int shown_h, int asked_l,
                     int asked_t, int asked_w, int asked_h) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (shown_w <= 0 || shown_h <= 0)
    g_pointer_remap.erase(hwnd);
  else
    g_pointer_remap[hwnd] = {shown_l, shown_t, shown_w, shown_h, asked_l, asked_t, asked_w, asked_h};
}

void CursorInClient(uint32_t hwnd, int* x, int* y) {
  int32_t sx = g_mouse_x, sy = g_mouse_y;
  hle::SyncedCursorPos(&sx, &sy);
  int ox, oy;
  WindowOrigin(hwnd, &ox, &oy);
  *x = sx - ox;
  *y = sy - oy;
}

}  // namespace recomp
