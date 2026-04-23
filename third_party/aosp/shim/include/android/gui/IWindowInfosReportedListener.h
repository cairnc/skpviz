// Stub — FE holds sp<IWindowInfosReportedListener> fields; needs RefBase for
// sp<> to work. Real impl is AIDL-generated.
#pragma once
#include <utils/RefBase.h>
namespace android::gui {
class IWindowInfosReportedListener : public virtual RefBase {};
} // namespace android::gui
