// ITransactionCompletedListener shim. In real AOSP this also transitively
// surfaces gui::CachingHint + onTrustedPresentationChanged signatures, so
// we pull them into the chain here.
#pragma once
#include <android/gui/CachingHint.h>
#include <binder/Binder.h>
#include <cstdint>

namespace android {

class ITransactionCompletedListener : public IBinder {
public:
  virtual void onTrustedPresentationChanged(int32_t /*windowId*/,
                                            bool /*entered*/) {}
};

struct CallbackId {
  int64_t id = 0;
  uint64_t sp = 0;
  enum class Type : int32_t { NORMAL = 0, ONCOMMIT = 1 };
  Type type = Type::NORMAL;
  bool operator==(const CallbackId &o) const {
    return id == o.id && sp == o.sp && type == o.type;
  }
};
struct ListenerCallbacks {};

// Hash functor for sp<IBinder> keys (used by TransactionHandler).
struct IListenerHash {
  size_t operator()(const sp<IBinder> &ptr) const {
    return std::hash<IBinder *>{}(ptr.get());
  }
};

} // namespace android
