// FramePacer (runtime/pacing.h).
//
// Its rules, on a simulated clock (exact, whatever the host is doing):
//   - frame starts lie on a grid of exact periods, so the average rate is
//     the one asked for and wake-up lateness does not add up;
//   - a frame that ends late (by less than a period) is followed by a
//     shorter wait, back on the grid;
//   - one more than a period late moves the grid: the next frame starts at
//     once and the one after a full period later (no catch-up burst);
//   - a change of rate, or a frame with no limit, starts a new grid.
//
// On the host's clock: waiting sleeps instead of spinning (true under any
// load, as load only lowers the CPU share), and, when the host is quiet
// enough to tell, wake-ups are punctual. On a loaded host the punctuality
// checks are skipped (said so in the output): there they measure the
// scheduler, not the pacer.

#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#include "runtime/pacing.h"
#include "runtime/platform.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    g_failures++;
  }
}

// ---- the simulated clock: time moves only when the pacer sleeps or the
// test "works"
uint64_t g_now = 0;
uint64_t SimNow() { return g_now; }
void SimSleepUntil(uint64_t t) {
  if (t > g_now) g_now = t;
}
recomp::FramePacer SimPacer() {
  g_now = 1000000000;  // any start
  return recomp::FramePacer({SimNow, SimSleepUntil});
}

void Grid() {
  for (double fps : {60.0, 144.0, 240.0}) {
    recomp::FramePacer p = SimPacer();
    const uint64_t period = uint64_t(1e9 / fps + 0.5);
    p.Wait(fps);
    const uint64_t t0 = g_now;
    bool on_grid = true;
    for (int i = 1; i <= 1000; i++) {
      g_now += period * uint64_t(i % 6) / 10;  // work: 0 to half a period
      p.Wait(fps);
      on_grid &= g_now == t0 + uint64_t(i) * period;
    }
    Check(on_grid, "every frame starts on the grid of exact periods");
  }
}

void SlightlyLate() {
  recomp::FramePacer p = SimPacer();
  const double fps = 100;
  const uint64_t period = 10000000;
  p.Wait(fps);
  const uint64_t t0 = g_now;
  p.Wait(fps);
  g_now += period * 3 / 2;  // work: one and a half periods
  p.Wait(fps);              // late by half a period: no wait
  Check(g_now == t0 + period * 5 / 2, "a frame late by less than a period starts at once");
  p.Wait(fps);
  Check(g_now == t0 + period * 3, "the frame after it is back on the grid (a shorter wait)");
}

void VeryLate() {
  recomp::FramePacer p = SimPacer();
  const double fps = 200;
  const uint64_t period = 5000000;
  for (int i = 0; i < 5; i++) p.Wait(fps);
  g_now += period * 3;  // a frame three periods long
  const uint64_t t0 = g_now;
  p.Wait(fps);
  Check(g_now == t0, "a frame more than a period late is followed at once");
  p.Wait(fps);
  Check(g_now == t0 + period, "then a full period: no burst to catch up");
}

void NewGrid() {
  recomp::FramePacer p = SimPacer();
  p.Wait(100);
  p.Wait(100);
  g_now += 1234;
  const uint64_t t0 = g_now;
  p.Wait(50);  // a new rate: a new grid, from now
  Check(g_now == t0, "a change of rate starts a new grid at once");
  p.Wait(50);
  Check(g_now == t0 + 20000000, "at the new period");
  p.Wait(0);  // no limit: no wait
  Check(g_now == t0 + 20000000, "no wait without a limit");
  g_now += 777;
  const uint64_t t1 = g_now;
  p.Wait(50);
  Check(g_now == t1, "a limit after none starts a new grid at once");
}

// ---- the host's clock

uint64_t CpuNs() {
  timespec ts;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  return uint64_t(ts.tv_sec) * 1000000000u + uint64_t(ts.tv_nsec);
}

// Quiet enough that wake-up lateness measures the pacer: the last
// minute's load average under half the cores.
bool Quiet(double* load) {
  double l[1] = {0};
  const long cores = sysconf(_SC_NPROCESSORS_ONLN);
  if (getloadavg(l, 1) != 1 || cores <= 0) return false;
  *load = l[0];
  return l[0] < 0.5 * double(cores);
}

// One second of frames at fps with "work" (a sleep) of a varying part of
// the period: mean period, lateness behind the grid, CPU share.
void Host(double fps, bool quiet) {
  recomp::FramePacer p;
  const int n = int(fps);
  const double period = 1e9 / fps;
  std::vector<uint64_t> start(static_cast<size_t>(n));
  const uint64_t cpu0 = CpuNs();
  for (int i = 0; i < n; i++) {
    p.Wait(fps);
    start[size_t(i)] = recomp::platform::MonotonicNs();
    std::this_thread::sleep_for(std::chrono::nanoseconds(int64_t(period * 0.1 * (i % 6))));
  }
  const double cpu = double(CpuNs() - cpu0);
  const double wall = double(start[size_t(n - 1)] - start[0]);
  const double mean = wall / (n - 1);
  std::vector<double> lateness(static_cast<size_t>(n));
  for (int i = 0; i < n; i++) lateness[size_t(i)] = double(start[size_t(i)] - start[0]) - i * period;
  std::nth_element(lateness.begin(), lateness.begin() + n / 2, lateness.end());
  const double median = lateness[size_t(n / 2)];
  std::printf("host clock, %.0f fps: mean period %.4f ms (%+.3f%%), lateness median %.3f ms, "
              "CPU %.1f%%\n",
              fps, mean / 1e6, (mean / period - 1) * 100, median / 1e6, cpu / wall * 100);
  Check(cpu / wall < 0.25, "waiting does not spin");
  if (!quiet) return;
  Check(std::fabs(mean / period - 1) < 0.01, "mean period within 1%");
  // Wake-ups land some tens of microseconds late; timer coalescing (a
  // sleep with leeway) made it milliseconds.
  Check(median < 0.5e6, "median lateness under 0.5 ms");
}

}  // namespace

int main() {
  Grid();
  SlightlyLate();
  VeryLate();
  NewGrid();
  double load = 0;
  const bool quiet = Quiet(&load);
  if (!quiet) std::printf("host loaded (load average %.1f): punctuality checks skipped\n", load);
  Host(144, quiet);
  Host(240, quiet);
  if (g_failures) return 1;
  std::printf("ok\n");
  return 0;
}
