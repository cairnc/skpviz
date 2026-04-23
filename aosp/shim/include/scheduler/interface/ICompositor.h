// SF scheduler ICompositor shim — CE's CompositionRefreshArgs.h references it
// for timing fields only.
#pragma once
#include <chrono>

namespace android {

// AOSP's scheduler uses a TimePoint class (not a bare
// std::chrono::time_point). SF code calls TimePoint::now(), min operators,
// and conversion to the underlying std::chrono::time_point. Keep a
// derived-like wrapper that round-trips.
class TimePoint {
public:
  using duration = std::chrono::nanoseconds;
  using clock = std::chrono::steady_clock;
  using std_time_point = clock::time_point;

  constexpr TimePoint() = default;
  constexpr TimePoint(std_time_point tp) : mTp(tp) {}
  explicit constexpr TimePoint(duration d) : mTp(std_time_point(d)) {}

  static TimePoint now() { return TimePoint(clock::now()); }
  static TimePoint fromNs(int64_t ns) { return TimePoint(duration(ns)); }
  int64_t ns() const {
    return std::chrono::duration_cast<duration>(mTp.time_since_epoch()).count();
  }
  constexpr operator std_time_point() const { return mTp; }
  constexpr bool operator<(const TimePoint &o) const { return mTp < o.mTp; }
  constexpr bool operator>(const TimePoint &o) const { return mTp > o.mTp; }
  constexpr bool operator==(const TimePoint &o) const { return mTp == o.mTp; }
  constexpr bool operator!=(const TimePoint &o) const { return mTp != o.mTp; }
  constexpr TimePoint operator+(duration d) const { return TimePoint(mTp + d); }
  constexpr duration operator-(TimePoint o) const { return mTp - o.mTp; }

private:
  std_time_point mTp{};
};

using Period = std::chrono::nanoseconds;
namespace compositionengine {
using ::android::Period;
using ::android::TimePoint;
} // namespace compositionengine
} // namespace android

namespace android::scheduler {
using TimePoint = ::android::TimePoint;
using Period = ::android::Period;

class FrameTarget {
public:
  TimePoint expectedPresentTime() const { return {}; }
  TimePoint earliestPresentTime() const { return {}; }
  int64_t frameId() const { return 0; }
  Period frameInterval() const { return {}; }
  bool wouldBackpressureHwc() const { return false; }
};
class FrameTargets {
public:
  const FrameTarget *get(int32_t /*displayId*/) const { return nullptr; }
};

class ICompositor {
public:
  virtual ~ICompositor() = default;
};
} // namespace android::scheduler
