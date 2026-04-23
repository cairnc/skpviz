// ISurfaceComposer shim — FE references ISurfaceComposer::eAnimation and
// ISurfaceComposerClient::e* flags. Real upstream also surfaces gui::
// FrameTimelineInfo + utils::Vector from this header, so mirror those.
#pragma once
#include <android/gui/FrameTimelineInfo.h>
#include <binder/Binder.h>
#include <cstdint>
#include <utils/Vector.h>
namespace android {
class ISurfaceComposer : public IBinder {
public:
  enum : uint32_t {
    eAnimation = 0x1,
    eSynchronous = 0x2,
    eExplicitEarlyWakeupStart = 0x4,
    eExplicitEarlyWakeupEnd = 0x8,
    eOneWay = 0x10,
  };
};
} // namespace android

// LayerState.h references gui::ISurfaceComposerClient::e* constants; mirror
// the same flag set under the gui namespace.
namespace android::gui {
class ISurfaceComposerClient : public IBinder {
public:
  enum : uint32_t {
    eHidden = 0x01,
    eOpaque = 0x02,
    eSecure = 0x80,
    eSkipScreenshot = 0x40,
    eNonPremultiplied = 0x100,
    eCursorWindow = 0x200,
    eProtectedByApp = 0x400,
    eNoColorFill = 0x800,
    eFXSurfaceEffect = 0x1,
    eFXSurfaceBufferState = 0x2,
    eFXSurfaceContainer = 0x4,
  };
};
} // namespace android::gui
// Keep the android::ISurfaceComposerClient alias for code that still uses
// the unqualified form.
namespace android {
using ISurfaceComposerClient = ::android::gui::ISurfaceComposerClient;
using gui::FrameTimelineInfo;
} // namespace android
// Upstream LayerState.h uses SpHash<T> unqualified in android:: scope.
// Drag the gui:: one up so that works.
#include <gui/SpHash.h>
namespace android {
using gui::SpHash;
}
