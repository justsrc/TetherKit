#include "tetherkit/common/scheduling.h"

#include <pthread/qos.h>

#include "tetherkit/common/logging.h"

namespace tetherkit {

unsigned int QosClassFor(ThreadRole role) noexcept {
  switch (role) {
    case ThreadRole::kDataPath:
      return QOS_CLASS_USER_INTERACTIVE;
    case ThreadRole::kControl:
      return QOS_CLASS_USER_INITIATED;
    case ThreadRole::kAuxiliary:
      return QOS_CLASS_UTILITY;
  }
  return QOS_CLASS_DEFAULT;
}

void ConfigureCurrentThread(std::string_view name, ThreadRole role) noexcept {
  SetCurrentThreadName(name);

  const auto qos = static_cast<qos_class_t>(QosClassFor(role));
  // relative_priority 0 means the default priority within that QoS class.
  const int rc = ::pthread_set_qos_class_self_np(qos, 0);
  if (rc != 0) {
    // A QoS failure does not affect functionality, only performance, so this
    // just warns instead of returning an error.
    TETHERKIT_WARN_TR(Msg::kCommonThreadQosFailed, static_cast<int>(qos), rc);
  }
}

}  // namespace tetherkit
