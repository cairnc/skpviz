// ComposerHal.h shim — CE's OutputLayerCompositionState references it.
// The real file wires up the HAL composer client; our layerviewer uses no
// HAL so we only need the `hal` namespace alias it establishes.
#pragma once
#include "Hal.h"
