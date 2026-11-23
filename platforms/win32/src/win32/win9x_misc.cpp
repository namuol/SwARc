// The odds and ends of the Windows 9x-era API that games of that time
// import but use only on paths with no host counterpart here: serial
// ports and consoles (modem play, debug consoles), dialog boxes (setup
// and error dialogs; the game's own UI is drawn with DirectDraw), OLE
// automation and structured storage (a game registering itself as a COM
// server for a launcher), common-control image lists, and Winsock 1.1
// sockets (LAN/online play; there is no network here, so sockets cannot
// be created). Cheap ones are real (rects, string and byte-order helpers,
// DOS dates); the rest report "not available" the way Windows does when
// the device or feature is absent.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "runtime/log.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "win32/win32.h"

namespace recomp::hle {
namespace {

using namespace win32;

constexpr uint32_t kErrorNotSupported = 50;
constexpr uint32_t kEFail = 0x80004005, kENotImpl = 0x80004001;

// ---- kernel32 ------------------------------------------------------------------------

HLE("kernel32.dll", lstrcatA, 2) {
  std::string s = GStr(Arg(c, 0)) + GStr(Arg(c, 1));
  PutStr(Arg(c, 0), s);
  return Arg(c, 0);
}

HLE("kernel32.dll", lstrlenW, 1) {
  if (!Arg(c, 0)) return 0;
  uint32_t n = 0;
  while (Rd16(Arg(c, 0) + 2 * n)) n++;
  return n;
}

// GetDiskFreeSpaceA(root, &sectors_per_cluster, &bytes_per_sector,
// &free_clusters, &total_clusters): a 2 GB disk with 1 GB free, the most a
// FAT32-era API reports without overflowing callers' arithmetic.
uint64_t GetDiskFreeSpace(Ctx& c) {
  const uint32_t v[4] = {8, 512, (1u << 30) / 4096, (2u << 30) / 4096 - 1};
  for (int i = 0; i < 4; i++)
    if (Arg(c, i + 1)) Wr32(Arg(c, i + 1), v[i]);
  return 1;
}
HLE64("kernel32.dll", GetDiskFreeSpaceA, 5) { return GetDiskFreeSpace(c); }
HLE64("kernel32.dll", GetDiskFreeSpaceW, 5) { return GetDiskFreeSpace(c); }

// DosDateTimeToFileTime(date, time, FILETIME*): local time, as FAT stores it.
HLE("kernel32.dll", DosDateTimeToFileTime, 3) {
  uint32_t d = Arg(c, 0) & 0xFFFF, t = Arg(c, 1) & 0xFFFF;
  std::tm tm{};
  tm.tm_year = int((d >> 9) + 80);
  tm.tm_mon = int(((d >> 5) & 15) - 1);
  tm.tm_mday = int(d & 31);
  tm.tm_hour = int(t >> 11);
  tm.tm_min = int((t >> 5) & 63);
  tm.tm_sec = int((t & 31) * 2);
  int64_t secs = int64_t(timegm(&tm));
  uint64_t ft = uint64_t(secs + 11644473600LL) * 10000000ULL;
  Wr32(Arg(c, 2), uint32_t(ft));
  Wr32(Arg(c, 2) + 4, uint32_t(ft >> 32));
  return 1;
}

HLE("kernel32.dll", FatalAppExitA, 2) {
  FatalF("FatalAppExit: %s", GStr(Arg(c, 1)).c_str());
}

HLE64("kernel32.dll", HeapValidate, 3) { return 1; }

// Consoles: none is attached; output goes to stderr.
HLE("kernel32.dll", AllocConsole, 0) { return 1; }
HLE("kernel32.dll", FreeConsole, 0) { return 1; }
HLE("kernel32.dll", SetConsoleMode, 2) { return 1; }
HLE("kernel32.dll", GetNumberOfConsoleInputEvents, 2) {
  Wr32(Arg(c, 1), 0);
  return 1;
}
HLE("kernel32.dll", PeekConsoleInputA, 4) {
  Wr32(Arg(c, 3), 0);
  return 1;
}
HLE("kernel32.dll", ReadConsoleInputA, 4) {
  Wr32(Arg(c, 3), 0);
  return 1;
}
HLE("kernel32.dll", WriteConsoleA, 5) {
  std::string s(reinterpret_cast<const char*>(GPtr(Arg(c, 1))), Arg(c, 2));
  std::fputs(s.c_str(), stderr);
  if (Arg(c, 3)) Wr32(Arg(c, 3), Arg(c, 2));
  return 1;
}

// Serial ports: there are none (CreateFile of COMn fails first; these
// only see invalid handles).
#define COMM_FAILS(name, n)                  \
  HLE("kernel32.dll", name, n) {             \
    SetLastError(kErrorInvalidHandle);       \
    return 0;                                \
  }
COMM_FAILS(ClearCommBreak, 1)
COMM_FAILS(ClearCommError, 3)
COMM_FAILS(EscapeCommFunction, 2)
COMM_FAILS(GetCommMask, 2)
COMM_FAILS(GetCommModemStatus, 2)
COMM_FAILS(GetCommState, 2)
COMM_FAILS(PurgeComm, 2)
COMM_FAILS(SetCommBreak, 1)
COMM_FAILS(SetCommState, 2)
COMM_FAILS(SetCommTimeouts, 2)
COMM_FAILS(SetupComm, 3)
#undef COMM_FAILS

// ---- user32 ------------------------------------------------------------------------

// SetRect, IntersectRect and the other rectangle helpers: user32_window.cpp.


HLE("user32.dll", CharToOemBuffA, 3) {
  if (Arg(c, 0) != Arg(c, 1)) std::memmove(GPtr(Arg(c, 1)), GPtr(Arg(c, 0)), Arg(c, 2));
  return 1;
}

HLE("user32.dll", MessageBoxIndirectA, 1) {
  // MSGBOXPARAMSA: cbSize, hwndOwner, hInstance, lpszText, lpszCaption, ...
  uint32_t p = Arg(c, 0);
  std::fprintf(stderr, "MessageBoxIndirect: %s: %s\n", GStr(Rd32(p + 16)).c_str(),
               GStr(Rd32(p + 12)).c_str());
  return 1;  // IDOK
}

HLE("user32.dll", GetMenu, 1) { return 0; }
HLE("user32.dll", LockWindowUpdate, 1) { return 1; }

// ---- imm32: no input method is ever active --------------------------------------------

HLE("imm32.dll", ImmGetContext, 1) { return 0; }
HLE("imm32.dll", ImmAssociateContext, 2) { return 0; }
HLE("imm32.dll", ImmNotifyIME, 4) { return 0; }
HLE("imm32.dll", ImmGetCandidateListA, 4) { return 0; }
HLE("imm32.dll", ImmGetCompositionStringA, 4) { return 0; }
HLE("imm32.dll", ImmSetOpenStatus, 2) { return 0; }
HLE("user32.dll", GetWindowContextHelpId, 1) { return 0; }
HLE("user32.dll", WaitForInputIdle, 2) { return 0; }
HLE("user32.dll", WinHelpA, 4) { return 1; }

// ---- gdi32 -------------------------------------------------------------------------
// Text (fonts, colours, measuring, drawing) is in gdi_draw.cpp.

HLE("gdi32.dll", SaveDC, 1) { return 1; }
HLE("gdi32.dll", RestoreDC, 2) { return 1; }
HLE("gdi32.dll", SetGraphicsMode, 2) { return 1; }
HLE("gdi32.dll", ModifyWorldTransform, 3) { return 1; }
HLE("gdi32.dll", SetViewportOrgEx, 4) {
  if (Arg(c, 3)) Wr32(Arg(c, 3), 0), Wr32(Arg(c, 3) + 4, 0);
  return 1;
}
HLE("gdi32.dll", SetWindowOrgEx, 4) {
  if (Arg(c, 3)) Wr32(Arg(c, 3), 0), Wr32(Arg(c, 3) + 4, 0);
  return 1;
}

// ---- comctl32 ----------------------------------------------------------------------

HLE_NAMED("comctl32.dll", "#17", InitCommonControls, 0) { return 0; }
// DllGetVersion(DLLVERSIONINFO*): cbSize, major, minor, build, platform.
// 5.81, the Internet Explorer 5 common controls games ask for at most.
HLE("comctl32.dll", DllGetVersion, 1) {
  uint32_t p = Arg(c, 0);
  if (Rd32(p) < 20) return 0x80070057;  // E_INVALIDARG
  Wr32(p + 4, 5);
  Wr32(p + 8, 81);
  Wr32(p + 12, 4704);
  Wr32(p + 16, 1);  // DLLVER_PLATFORM_WINDOWS
  return 0;
}
HLE("comctl32.dll", ImageList_Destroy, 1) { return 1; }
HLE("comctl32.dll", ImageList_BeginDrag, 4) { return 0; }
HLE("comctl32.dll", ImageList_EndDrag, 0) { return 0; }
HLE("comctl32.dll", ImageList_DragEnter, 3) { return 0; }
HLE("comctl32.dll", ImageList_DragMove, 2) { return 0; }
HLE("comctl32.dll", ImageList_DragShowNolock, 1) { return 0; }

// ---- shell32, winmm ----------------------------------------------------------------

HLE("shell32.dll", FindExecutableA, 3) {
  if (Arg(c, 2)) Wr8(Arg(c, 2), 0);
  return 31;  // SE_ERR_NOASSOC
}

// timeGetDevCaps(TIMECAPS*, size): 1 ms to 1,000,000 ms.
HLE64("winmm.dll", timeGetDevCaps, 2) {
  Wr32(Arg(c, 0), 1);
  Wr32(Arg(c, 0) + 4, 1000000);
  return 0;
}

// ---- ole32 / oleaut32 --------------------------------------------------------------
// The game may register itself as a COM server (for a launcher or an
// online service); nothing here ever connects to it.

HLE("ole32.dll", OleInitialize, 1) { return 0; }
HLE("ole32.dll", OleUninitialize, 0) { return 0; }
HLE("ole32.dll", CoDisconnectObject, 2) { return 0; }
HLE("ole32.dll", OleRun, 1) { return 0; }
HLE("ole32.dll", OleSaveToStream, 2) { return kENotImpl; }
HLE("ole32.dll", OleLoadFromStream, 3) { return kENotImpl; }
HLE("ole32.dll", StgCreateDocfile, 4) { return 0x80030003; }  // STG_E_PATHNOTFOUND
HLE("ole32.dll", StgOpenStorage, 6) { return 0x80030002; }    // STG_E_FILENOTFOUND

HLE("ole32.dll", CoFileTimeNow, 1) {
  uint64_t ft = (uint64_t(std::time(nullptr)) + 11644473600ULL) * 10000000ULL;
  Wr32(Arg(c, 0), uint32_t(ft));
  Wr32(Arg(c, 0) + 4, uint32_t(ft >> 32));
  return 0;
}

// StringFromCLSID(clsid, LPOLESTR*): "{XXXXXXXX-XXXX-...}" in task memory.
HLE64("ole32.dll", StringFromCLSID, 2) {
  gaddr g = Arg(c, 0);
  char s[40];
  std::snprintf(s, sizeof s, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", Rd32(g),
                Rd16(g + 4), Rd16(g + 6), Rd8(g + 8), Rd8(g + 9), Rd8(g + 10), Rd8(g + 11),
                Rd8(g + 12), Rd8(g + 13), Rd8(g + 14), Rd8(g + 15));
  gaddr out = HeapAllocProcess(2 * 39, false);
  for (int i = 0; i < 39; i++) Wr16(out + 2 * uint32_t(i), uint8_t(s[i]));
  WrPtr(Arg(c, 1), out);
  return 0;
}

// VariantInit, VariantClear, GetErrorInfo, SetErrorInfo: advapi32.cpp.
HLE_NAMED("oleaut32.dll", "#12", VariantChangeType, 4) { return kENotImpl; }
// RegisterActiveObject(punk, clsid, flags, &cookie) / RevokeActiveObject(
// cookie, reserved): the running object table nobody here reads.
HLE_NAMED("oleaut32.dll", "#33", RegisterActiveObject, 4) {
  if (Arg(c, 3)) Wr32(Arg(c, 3), 1);
  return 0;
}
HLE_NAMED("oleaut32.dll", "#34", RevokeActiveObject, 2) { return 0; }
HLE_NAMED("oleaut32.dll", "#161", LoadTypeLib, 2) {
  Wr32(Arg(c, 1), 0);
  return 0x80029C4A;  // TYPE_E_CANTLOADLIBRARY
}
HLE_NAMED("oleaut32.dll", "#202", CreateErrorInfo, 1) {
  Wr32(Arg(c, 0), 0);
  return kEFail;
}

// ---- wsock32 (Winsock 1.1) -----------------------------------------------------------
// WSAStartup succeeds (shell_net.cpp) but no socket can be created: the
// network is down (WSAENETDOWN), which games report as "no network".

constexpr uint32_t kWsaENetDown = 10050;
uint32_t g_wsa_error = 0;

uint32_t SockFail() {
  g_wsa_error = kWsaENetDown;
  return 0xFFFFFFFF;  // INVALID_SOCKET / SOCKET_ERROR
}

HLE_NAMED("wsock32.dll", "#23", wsock_socket, 3) { return SockFail(); }
HLE_NAMED("wsock32.dll", "#2", wsock_bind, 3) { return SockFail(); }
HLE_NAMED("wsock32.dll", "#3", wsock_closesocket, 1) { return 0; }
HLE_NAMED("wsock32.dll", "#7", wsock_getsockopt, 5) { return SockFail(); }
HLE_NAMED("wsock32.dll", "#21", wsock_setsockopt, 5) { return SockFail(); }
HLE_NAMED("wsock32.dll", "#17", wsock_recvfrom, 6) { return SockFail(); }
HLE_NAMED("wsock32.dll", "#20", wsock_sendto, 6) { return SockFail(); }
HLE_NAMED("wsock32.dll", "#101", wsock_WSAAsyncSelect, 4) { return SockFail(); }
HLE_NAMED("wsock32.dll", "#108", wsock_WSACancelAsyncRequest, 1) { return SockFail(); }
HLE_NAMED("wsock32.dll", "#111", wsock_WSAGetLastError, 0) { return g_wsa_error; }
// EnumProtocolsA(protocols, buffer, &length): no transport protocols.
HLE_NAMED("wsock32.dll", "#1111", wsock_EnumProtocolsA, 3) {
  if (Arg(c, 2)) Wr32(Arg(c, 2), 0);
  return 0;
}
HLE_NAMED("wsock32.dll", "#8", wsock_htonl, 1) { return __builtin_bswap32(Arg(c, 0)); }
HLE_NAMED("wsock32.dll", "#9", wsock_htons, 1) { return __builtin_bswap16(uint16_t(Arg(c, 0))); }
HLE_NAMED("wsock32.dll", "#15", wsock_ntohs, 1) { return __builtin_bswap16(uint16_t(Arg(c, 0))); }
// wsock32 numbers its first 23 exports in name order, so 10 is inet_addr,
// 11 inet_ntoa and 12 ioctlsocket (ws2_32 numbers those three otherwise).
// inet_addr("a.b.c.d"), network byte order; INADDR_NONE otherwise.
HLE_NAMED("wsock32.dll", "#10", wsock_inet_addr, 1) {
  unsigned a, b, cc, d;
  char tail;
  if (std::sscanf(GStr(Arg(c, 0)).c_str(), "%u.%u.%u.%u%c", &a, &b, &cc, &d, &tail) != 4 ||
      a > 255 || b > 255 || cc > 255 || d > 255)
    return 0xFFFFFFFF;
  return a | b << 8 | cc << 16 | d << 24;
}
// inet_ntoa(struct in_addr, by value): "a.b.c.d" in a buffer per thread.
HLE_NAMED("wsock32.dll", "#11", wsock_inet_ntoa, 1) {
  static thread_local gaddr buf = 0;
  if (!buf) buf = HeapAllocProcess(16, false);
  uint32_t a = Arg(c, 0);
  char s[16];
  std::snprintf(s, sizeof s, "%u.%u.%u.%u", a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF, a >> 24);
  PutStr(buf, s);
  return buf;
}
// With no socket to be had, the calls that take one fail (WSAENETDOWN as
// the rest), and select finds nothing ready.
HLE_NAMED("wsock32.dll", "#1", wsock_accept, 3) { return SockFail(); }
HLE_NAMED("wsock32.dll", "#4", wsock_connect, 3) { return SockFail(); }
HLE_NAMED("wsock32.dll", "#6", wsock_getsockname, 3) { return SockFail(); }
HLE_NAMED("wsock32.dll", "#13", wsock_listen, 2) { return SockFail(); }
HLE_NAMED("wsock32.dll", "#16", wsock_recv, 4) { return SockFail(); }
HLE_NAMED("wsock32.dll", "#18", wsock_select, 5) { return SockFail(); }
HLE_NAMED("wsock32.dll", "#19", wsock_send, 4) { return SockFail(); }
HLE_NAMED("wsock32.dll", "#22", wsock_shutdown, 2) { return SockFail(); }
HLE_NAMED("wsock32.dll", "#112", wsock_WSASetLastError, 1) {
  g_wsa_error = Arg(c, 0);
  return 0;
}
// __WSAFDIsSet(socket, fd_set* {u_int count; SOCKET array[]}).
HLE_NAMED("wsock32.dll", "#151", wsock___WSAFDIsSet, 2) {
  gaddr set = Arg(c, 1);
  uint32_t n = Rd32(set);
  for (uint32_t i = 0; i < n && i < 64; i++)
    if (Rd32(set + 4 + 4 * i) == Arg(c, 0)) return 1;
  return 0;
}

// The rest of the socket calls fail the same way (no socket exists to use).
HLE_NAMED("wsock32.dll", "#5", wsock_getpeername, 3) { return SockFail(); }
HLE_NAMED("wsock32.dll", "#12", wsock_ioctlsocket, 3) { return SockFail(); }

// Lookups find nothing: NULL with WSANO_DATA (a valid name, no record).
constexpr uint32_t kWsaNoData = 11004;
uint32_t NoRecord() {
  g_wsa_error = kWsaNoData;
  return 0;
}
HLE_NAMED("wsock32.dll", "#51", wsock_gethostbyaddr, 3) { return NoRecord(); }
HLE_NAMED("wsock32.dll", "#55", wsock_getservbyname, 2) { return NoRecord(); }
HLE_NAMED("wsock32.dll", "#56", wsock_getservbyport, 2) { return NoRecord(); }
// WSAAsyncGetHostByName(hwnd, msg, name, buf, buflen): no task handle (0).
HLE_NAMED("wsock32.dll", "#103", wsock_WSAAsyncGetHostByName, 5) {
  SockFail();
  return 0;
}

// The Microsoft extensions: AcceptEx(listen, accept, buf, recv len, local
// len, remote len, &bytes, overlapped) fails; GetAcceptExSockaddrs(buf,
// recv len, local len, remote len, &local, &local len, &remote, &remote
// len) has no addresses to give.
HLE_NAMED("wsock32.dll", "#1141", wsock_AcceptEx, 8) {
  SockFail();
  return 0;
}
HLE_NAMED("wsock32.dll", "#1142", wsock_GetAcceptExSockaddrs, 8) {
  for (int i : {4, 6})
    if (Arg(c, i)) Wr32(Arg(c, i), 0);
  for (int i : {5, 7})
    if (Arg(c, i)) Wr32(Arg(c, i), 0);
  return 0;
}

}  // namespace
}  // namespace recomp::hle
