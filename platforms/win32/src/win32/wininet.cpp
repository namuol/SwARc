// wininet.dll (HTTP client) as on a machine with no network connection.
//
// WinINet does no network I/O until a request is sent: InternetOpen,
// InternetConnect and HttpOpenRequest only build handles, on Windows too,
// so they succeed here. Sending fails as when the host name cannot be
// resolved (ERROR_INTERNET_NAME_NOT_RESOLVED), and what needs a sent
// request (reading, writing, querying headers) fails as for a handle in
// the wrong state. Games that fetch news or check for updates take their
// offline path.

#include <mutex>
#include <set>

#include "runtime/log.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"

namespace recomp::hle {
namespace {

constexpr const char* kDll = "wininet.dll";
constexpr uint32_t kErrorNameNotResolved = 12007;
constexpr uint32_t kErrorIncorrectHandleState = 12019;
constexpr uint32_t kErrorInvalidHandle = 6;

// HINTERNETs are opaque to their users; these are not kernel handles
// (CloseHandle does not take them on Windows either).
std::mutex g_mu;
std::set<uint32_t> g_handles;
uint32_t g_next = 0x00CC0004;

uint32_t NewHandle() {
  std::lock_guard<std::mutex> l(g_mu);
  uint32_t h = g_next;
  g_next += 4;
  g_handles.insert(h);
  return h;
}

bool Valid(uint32_t h) {
  std::lock_guard<std::mutex> l(g_mu);
  return g_handles.count(h) != 0;
}

// A handle derived from `parent`, or 0 and ERROR_INVALID_HANDLE.
uint32_t Child(uint32_t parent) {
  if (!Valid(parent)) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  return NewHandle();
}

// Fails a call that needs a request to have been sent.
uint32_t NotSent(uint32_t h) {
  SetLastError(Valid(h) ? kErrorIncorrectHandleState : kErrorInvalidHandle);
  return 0;
}

// InternetOpenA(agent, access type, proxy, proxy bypass, flags).
HLE(kDll, InternetOpenA, 5) { return NewHandle(); }

// InternetConnectA(session, server, port, user, password, service, flags,
// context).
HLE(kDll, InternetConnectA, 8) {
  RECOMP_TRACE("  wininet: connect %s (offline)\n", Arg(c, 1) ? GStr(Arg(c, 1)).c_str() : "");
  return Child(Arg(c, 0));
}

// HttpOpenRequestA(connection, verb, object, version, referrer, accept
// types, flags, context).
HLE(kDll, HttpOpenRequestA, 8) { return Child(Arg(c, 0)); }

// HttpAddRequestHeadersA(request, headers, length, modifiers): headers are
// kept by the request until it is sent, so adding them succeeds.
HLE(kDll, HttpAddRequestHeadersA, 4) {
  if (!Valid(Arg(c, 0))) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  return 1;
}

// HttpSendRequestExA(request, buffers in, buffers out, flags, context): the
// host name is resolved here, and that fails.
HLE(kDll, HttpSendRequestExA, 5) {
  if (!Valid(Arg(c, 0))) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  SetLastError(kErrorNameNotResolved);
  return 0;
}

// HttpEndRequestA(request, buffers out, flags, context).
HLE(kDll, HttpEndRequestA, 4) { return NotSent(Arg(c, 0)); }

// HttpQueryInfoA(request, level, buffer, length*, index*).
HLE(kDll, HttpQueryInfoA, 5) { return NotSent(Arg(c, 0)); }

// InternetReadFile(handle, buffer, size, read*).
HLE(kDll, InternetReadFile, 4) {
  if (Arg(c, 3)) Wr32(Arg(c, 3), 0);
  return NotSent(Arg(c, 0));
}

// InternetWriteFile(handle, buffer, size, written*).
HLE(kDll, InternetWriteFile, 4) {
  if (Arg(c, 3)) Wr32(Arg(c, 3), 0);
  return NotSent(Arg(c, 0));
}

HLE(kDll, InternetCloseHandle, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_handles.erase(Arg(c, 0))) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  return 1;
}

}  // namespace
}  // namespace recomp::hle
