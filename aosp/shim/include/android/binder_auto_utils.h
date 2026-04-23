// NDK binder ScopedAStatus / ScopedFileDescriptor shim. Lives in its own
// header to avoid a cycle between HWC2.h and Luts.h.
#pragma once

namespace android::ndk {

class ScopedAStatus {
public:
  ScopedAStatus() = default;
  bool isOk() const { return true; }
};

class ScopedFileDescriptor {
public:
  ScopedFileDescriptor() = default;
  explicit ScopedFileDescriptor(int fd) : mFd(fd) {}
  ScopedFileDescriptor(ScopedFileDescriptor &&o) noexcept : mFd(o.mFd) {
    o.mFd = -1;
  }
  ScopedFileDescriptor &operator=(ScopedFileDescriptor &&o) noexcept {
    mFd = o.mFd;
    o.mFd = -1;
    return *this;
  }
  ScopedFileDescriptor(const ScopedFileDescriptor &) = delete;
  ScopedFileDescriptor &operator=(const ScopedFileDescriptor &) = delete;
  int get() const { return mFd; }
  int release() {
    int f = mFd;
    mFd = -1;
    return f;
  }

private:
  int mFd = -1;
};

} // namespace android::ndk
