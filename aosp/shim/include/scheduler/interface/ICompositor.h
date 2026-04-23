// SF scheduler interface shim — CE references a few types in this header.
// Matching real AOSP's shape: TimePoint inherits from steady_clock::time_point,
// Duration inherits from TimePoint::duration, Period aliases Duration.
#pragma once

#include <chrono>
#include <cstdint>
#include <ftl/optional.h>
#include <ui/DisplayId.h>
#include <ui/DisplayMap.h>
#include <utils/Timers.h> // nsecs_t

namespace android {

namespace scheduler {
using SchedulerClock = std::chrono::steady_clock;
} // namespace scheduler

struct Duration;

struct TimePoint : scheduler::SchedulerClock::time_point {
  constexpr TimePoint() = default;
  explicit constexpr TimePoint(const Duration &);
  constexpr TimePoint(scheduler::SchedulerClock::time_point p)
      : scheduler::SchedulerClock::time_point(p) {}

  static constexpr TimePoint fromNs(nsecs_t);
  static TimePoint now() { return scheduler::SchedulerClock::now(); }

  nsecs_t ns() const;
};

struct Duration : TimePoint::duration {
  constexpr Duration() = default;
  template <typename R, typename P>
  constexpr Duration(std::chrono::duration<R, P> d) : TimePoint::duration(d) {}

  static constexpr Duration fromNs(nsecs_t ns) {
    return Duration(std::chrono::nanoseconds(ns));
  }

  nsecs_t ns() const { return std::chrono::nanoseconds(*this).count(); }
};

using Period = Duration;

constexpr TimePoint::TimePoint(const Duration &d)
    : scheduler::SchedulerClock::time_point(d) {}

constexpr TimePoint TimePoint::fromNs(nsecs_t ns) {
  return TimePoint(Duration::fromNs(ns));
}

inline nsecs_t TimePoint::ns() const {
  return Duration(time_since_epoch()).ns();
}

// Duration → numeric ticks helper.
template <typename P, typename Rep = Duration::rep>
constexpr Rep ticks(Duration d) {
  using D = std::chrono::duration<Rep, P>;
  return std::chrono::duration_cast<D>(d).count();
}

namespace compositionengine {
using ::android::Duration;
using ::android::Period;
using ::android::TimePoint;
} // namespace compositionengine

namespace scheduler {
using TimePoint = ::android::TimePoint;
using Duration = ::android::Duration;
using Period = ::android::Period;

class FrameTarget {
public:
  TimePoint expectedPresentTime() const { return {}; }
  TimePoint earliestPresentTime() const { return {}; }
  std::optional<TimePoint> debugPresentDelay() const { return std::nullopt; }
  int64_t frameId() const { return 0; }
  Period frameInterval() const { return {}; }
  bool wouldBackpressureHwc() const { return false; }
};

using FrameTargets =
    ui::PhysicalDisplayMap<PhysicalDisplayId, const FrameTarget *>;

class ICompositor {
public:
  virtual ~ICompositor() = default;
};
} // namespace scheduler

} // namespace android
