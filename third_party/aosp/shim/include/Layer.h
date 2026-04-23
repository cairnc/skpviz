// Layer.h shim — FE only references 4 static/nested members of SF's real
// Layer class (`Layer::PRIORITY_UNSET`, `Layer::translateDataspace`, and
// converters under `Layer::FrameRate`). Skip the full Layer port; alias
// FrameRate to the scheduler-side type so `Layer::FrameRate::convertX`
// works unchanged.
#pragma once

#include <Scheduler/LayerInfo.h>
#include <cstdint>
#include <ui/GraphicTypes.h>
#include <utils/RefBase.h>

// Upstream Layer.h transitively pulls in FrontEnd/LayerSnapshot.h (through
// the SF service headers). Replicate that so RequestedLayerState.cpp can
// use LayerSnapshot::isOpaqueFormat unchanged.
#include "FrontEnd/LayerSnapshot.h"

namespace android {

class Layer : public virtual RefBase {
public:
  int32_t getSequence() const { return 0; }

  static constexpr int32_t PRIORITY_UNSET = -1;
  static constexpr int32_t PRIORITY_FOCUSED_WITH_MODE = 0;
  static constexpr int32_t PRIORITY_FOCUSED_WITHOUT_MODE = 1;
  static constexpr int32_t PRIORITY_NOT_FOCUSED_WITH_MODE = 2;

  using FrameRate = scheduler::LayerInfo::FrameRate;

  // Real impl widens V0 dataspaces / re-tags legacy colour spaces. Trace
  // replay doesn't need the remap — pass-through.
  static ui::Dataspace translateDataspace(ui::Dataspace d) { return d; }
};

} // namespace android
