// TransactionTracing shim — FE's LayerLog.h reaches this header for the
// global TransactionTraceWriter + SFTRACE_NAME + FlagManager + __predict_false
// macros, so consolidate all of them into the transitive chain here.
#pragma once
#include <common/trace.h>
#include <string>

namespace android {

class TransactionTraceWriter {
public:
  static TransactionTraceWriter &getInstance() {
    static TransactionTraceWriter w;
    return w;
  }
  void enable() {}
  void disable() {}
  void invoke(const std::string & /*prefix*/, bool /*overwrite*/) {}
  void invokeForTest(const std::string & /*filename*/, bool /*overwrite*/) {}
};

} // namespace android
