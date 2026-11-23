#pragma once
// Frame pacing for game patches that replace a busy-waiting frame limiter
// (ARCHITECTURE.md, "Frame pacing").

#include <cstdint>

namespace recomp {

// Paces a loop to a frame rate on the host's monotonic clock. Wait()
// sleeps, without spinning, until the next frame's start. Frame starts
// lie on a grid of exact periods (not rounded to milliseconds), so
// wake-up latency does not add up and the average rate is the one asked
// for. A frame that ends late is followed by a shorter wait; one that is
// more than a whole period late moves the grid instead of being followed
// by a burst of frames.
class FramePacer {
 public:
  // The clock it reads and sleeps on: the platform layer's by default;
  // tests give their own.
  struct Clock {
    uint64_t (*now)();
    void (*sleep_until)(uint64_t t);
  };
  FramePacer();
  explicit FramePacer(Clock clock) : clock_(clock) {}

  // Call at the start of every frame. fps <= 0: no limit (and the grid
  // starts again at the next limited frame).
  void Wait(double fps);

 private:
  Clock clock_;
  uint64_t next_ = 0;    // start of the next frame (MonotonicNs), 0: none
  uint64_t period_ = 0;  // ns
};

}  // namespace recomp
