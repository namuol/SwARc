#pragma once
// Hang watchdog. The guest main thread reports a heartbeat each time it
// pumps host events, and one per presented frame; if either stops for 5 s
// (<ID>_WATCHDOG_SECS), every registered
// guest thread is asked (SIGUSR2) to write its host backtrace (lifted
// functions are named recomp::gen::f_<guest VA>) to a dump file in the
// per-user data directory and to stderr. The process keeps running.

#include <cstdint>

namespace recomp {

void StartWatchdog();
// Call on each host thread that runs guest code, once, at its start (it
// leaves the list when the thread exits).
void RegisterGuestHostThread(const char* name, unsigned tid);
// Called by the main thread whenever it services the window system.
void WatchdogHeartbeat();
// Called on every presented frame. A minimized/inactive game presents
// nothing, so this only counts while the game is active.
void WatchdogFrame();
// The game was (de)activated; frames are only expected while active.
void WatchdogAppActive(bool active);

// Per-thread HLE state, maintained by hle::CallThunk, in one thread-local
// so a call touches it once (on macOS every thread-local access from a
// library is a call):
// - name, ret: the import the thread is executing (innermost) and its
//   guest return address; null outside imports. Exception unwinds through
//   an import leave them stale, so dumps say "last".
// - depth: >0 while an HLE handler runs (0 again inside CallGuest). A
//   host fault then is a runtime bug, not a guest access violation.
struct HleThreadState {
  const char* name = nullptr;
  uint64_t ret = 0;
  int depth = 0;
};
extern thread_local HleThreadState t_hle_state;

}  // namespace recomp
