// TracedOrdinal shim — SF's tracing helper that stamps atraces on value
// changes. For layerviewer it collapses to a plain value holder.
#pragma once
#include <string>
#include <utility>

template <typename T> class TracedOrdinal {
public:
  TracedOrdinal(std::string /*name*/, T initial) : mValue(std::move(initial)) {}
  TracedOrdinal(const TracedOrdinal &) = default;
  TracedOrdinal(TracedOrdinal &&) = default;
  TracedOrdinal &operator=(T v) {
    mValue = std::move(v);
    return *this;
  }
  TracedOrdinal &operator=(const TracedOrdinal &) = default;
  TracedOrdinal &operator=(TracedOrdinal &&) = default;
  operator const T &() const { return mValue; }
  const T &get() const { return mValue; }

private:
  T mValue;
};
