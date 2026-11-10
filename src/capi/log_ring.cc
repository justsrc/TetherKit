// Log capture: buffers the library's internal log lines for the GUI to poll.
//
// Why not a callback straight into Swift: the logs arrive from the libusb
// event thread, the control thread and the two data path threads, and they
// are produced **inside** the log mutex. Running a Swift closure in that
// context means both marshalling across threads and the landmine of "the
// closure logs one more line -> self-deadlock". Buffering plus polling kills
// both problems at once.
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>

#include "capi_support.h"
#include "tetherkit/capi/tetherkit_c.h"
#include "tetherkit/common/logging.h"

namespace {

using tetherkit::capi::CopyText;
using tetherkit::capi::WallNanos;

/// Capacity of the ring buffer.
///
/// Why 256: the GUI pulls every 500 ms, while the library logs one statistics
/// line every 5 seconds in steady state. 256 entries are enough to absorb the
/// dense burst of the startup sequence (about 30 lines) and any sudden flood
/// of warnings, while keeping the resident memory at 256 x 552 B ~ 138 KiB.
constexpr std::size_t kLogRingCapacity = 256;

/// Fixed-size ring buffer. When it fills, the **oldest** entry is dropped --
/// when the host is stuck, the newest scene is more useful than the beginning.
class LogRing {
 public:
  void Push(tk_log_level_t level, std::string_view thread_name,
            std::string_view message) noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (count_ == kLogRingCapacity) {
      // Overwrite the oldest entry: advance the read pointer, which is
      // logically the same as dropping it.
      head_ = (head_ + 1) % kLogRingCapacity;
      --count_;
      ++dropped_;
    }
    tk_log_record_t& record = records_[(head_ + count_) % kLogRingCapacity];
    record.level = static_cast<std::int32_t>(level);
    record.wall_nanos = WallNanos();
    CopyText(record.thread, thread_name);
    CopyText(record.message, message);
    ++count_;
  }

  std::size_t Drain(tk_log_record_t* out_records, std::size_t capacity,
                    std::uint64_t* out_dropped) noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (out_dropped != nullptr) {
      *out_dropped = dropped_;
      dropped_ = 0;
    }
    std::size_t taken = 0;
    while (taken < capacity && count_ > 0) {
      if (out_records != nullptr) {
        out_records[taken] = records_[head_];
      }
      head_ = (head_ + 1) % kLogRingCapacity;
      --count_;
      ++taken;
    }
    return taken;
  }

  void Clear() noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    head_ = 0;
    count_ = 0;
    dropped_ = 0;
  }

 private:
  std::mutex mutex_;
  std::array<tk_log_record_t, kLogRingCapacity> records_{};
  /// Index of the next record to read.
  std::size_t head_ = 0;
  std::size_t count_ = 0;
  std::uint64_t dropped_ = 0;
};

/// A process-wide singleton. A function-local static rather than a global
/// object, to avoid static initialization order problems -- the log sink may
/// be triggered during the static construction of any translation unit.
LogRing& Ring() noexcept {
  static LogRing ring;
  return ring;
}

/// Installs the callback for tetherkit::SetLogSink.
///
/// It runs inside the log mutex, so the implementation may only "copy plus
/// take one lock of its own", and must never log again (that would deadlock
/// on itself).
void SinkTrampoline(tetherkit::LogLevel level, std::string_view thread_name,
                    std::string_view message, void* /*user*/) noexcept {
  Ring().Push(static_cast<tk_log_level_t>(level), thread_name, message);
}

}  // namespace

void tk_set_log_level(int32_t level) {
  if (level < TK_LOG_TRACE || level > TK_LOG_OFF) {
    return;
  }
  tetherkit::SetLogLevel(static_cast<tetherkit::LogLevel>(level));
}

void tk_enable_log_capture(bool enabled) {
  if (enabled) {
    tetherkit::SetLogSink(&SinkTrampoline, nullptr);
    return;
  }

  // The order matters: **first** remove the log sink, **then** clear the
  // buffer.
  //
  // The other way round, a thread that was logging before the removal pushes
  // a new record into the just-cleared buffer, and a few leftovers survive
  // the disabling of the capture.
  //
  // Also, this deliberately does not hold the LogRing lock while calling
  // SetLogSink -- SetLogSink takes the log mutex, and the log sink path is
  // "log lock -> LogRing lock", so taking them in the reverse order would
  // invert the lock order.
  tetherkit::SetLogSink(nullptr, nullptr);
  Ring().Clear();
}

size_t tk_drain_logs(tk_log_record_t* out_records, size_t capacity, uint64_t* out_dropped) {
  if (out_dropped != nullptr) {
    *out_dropped = 0;
  }
  if (capacity == 0) {
    return 0;
  }
  return Ring().Drain(out_records, capacity, out_dropped);
}
