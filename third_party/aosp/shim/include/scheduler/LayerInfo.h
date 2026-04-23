// LayerInfo shim — FrameRate struct that FE uses as `Layer::FrameRate`.
// Enums come verbatim from upstream headers:
//   - scheduler/Fps.h  (Fps, FrameRateCategory)
//   - Scheduler/FrameRateCompatibility.h (FrameRateCompatibility)
//   - scheduler/Seamlessness.h (Seamlessness)
#pragma once

#include <Scheduler/FrameRateCompatibility.h>
#include <cstdint>
#include <gui/PidUid.h>
#include <map>
#include <scheduler/Fps.h>
#include <scheduler/Seamlessness.h>
#include <ui/FenceTime.h>

namespace android::scheduler {

class LayerInfo {
public:
  enum class FrameRateSelectionStrategy : uint32_t {
    Propagate,
    OverrideChildren,
    Self,
    ftl_last = Self,
  };
  static FrameRateSelectionStrategy convertFrameRateSelectionStrategy(int8_t) {
    return FrameRateSelectionStrategy::Propagate;
  }

  struct FrameRate {
    struct FrameRateVote {
      Fps rate;
      FrameRateCompatibility type = FrameRateCompatibility::Default;
      Seamlessness seamlessness = Seamlessness::Default;

      bool operator==(const FrameRateVote &o) const {
        return rate.getValue() == o.rate.getValue() && type == o.type &&
               seamlessness == o.seamlessness;
      }
      FrameRateVote() = default;
      FrameRateVote(Fps r, FrameRateCompatibility t,
                    Seamlessness s = Seamlessness::Default)
          : rate(r), type(t), seamlessness(s) {}
    };

    FrameRateVote vote;
    FrameRateCategory category = FrameRateCategory::Default;
    bool categorySmoothSwitchOnly = false;

    FrameRate() = default;
    FrameRate(Fps r, FrameRateCompatibility t,
              Seamlessness s = Seamlessness::Default,
              FrameRateCategory c = FrameRateCategory::Default)
        : vote(r, t, s), category(c) {}

    bool operator==(const FrameRate &o) const {
      return vote == o.vote && category == o.category;
    }
    bool isValid() const { return true; }

    // HAL-int → enum converters — replay path doesn't need the real
    // mapping, collapse to Default.
    static FrameRateCompatibility convertCompatibility(int8_t) {
      return FrameRateCompatibility::Default;
    }
    static Seamlessness convertChangeFrameRateStrategy(int8_t) {
      return Seamlessness::Default;
    }
    static FrameRateCategory convertCategory(int8_t) {
      return FrameRateCategory::Default;
    }
  };
};

} // namespace android::scheduler
