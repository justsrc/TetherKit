#include "tethernet/usb/context.h"

#include "tethernet/common/i18n.h"
#include "tethernet/common/logging.h"
#include "tethernet/common/scheduling.h"

#include <format>

namespace tethernet::usb {
namespace {

/// Upper bound on how long a single handle_events call may block in the event
/// loop.
///
/// Why such a long timeout is fine: the darwin backend marks every bulk
/// transfer with USBI_TRANSFER_OS_HANDLES_TIMEOUT (IOKit owns the timeout),
/// and libusb_get_next_timeout skips transfers carrying that flag -- so as
/// long as everything in flight is bulk, it always reports "no timeout" and
/// the event thread can rely purely on the event pipe to wake it.
/// The 250 ms here exists only so that RequestStop() is seen within one
/// cycle.
constexpr long kEventLoopTimeoutSeconds = 0;
constexpr long kEventLoopTimeoutMicros = 250'000;

}  // namespace

Result<std::unique_ptr<Context>> Context::Create() {
  auto context = std::unique_ptr<Context>(new Context());

  const int rc = ::libusb_init(&context->context_);
  if (rc != LIBUSB_SUCCESS) {
    return std::unexpected(Error::FromLibUsb(rc, Tr(Msg::kUsbInitFailed)));
  }

  TETHERNET_INFO_TR(
      Msg::kUsbInitialized, VersionString(),
      Text(SupportsHotplug() ? Msg::kUsbHotplugSupported : Msg::kUsbHotplugUnsupported));

  context->running_.store(true, std::memory_order_release);
  context->event_thread_ = std::thread([raw = context.get()] { raw->RunEventLoop(); });
  return context;
}

Context::~Context() {
  RequestStop();

  if (event_thread_.joinable()) {
    // libusb_interrupt_event_handler writes one byte to the event pipe so the
    // thread blocked in handle_events returns immediately. Without it we
    // would have to wait out a full timeout period.
    if (context_ != nullptr) {
      ::libusb_interrupt_event_handler(context_);
    }
    event_thread_.join();
  }

  if (context_ != nullptr) {
    ::libusb_exit(context_);
    context_ = nullptr;
  }
  TETHERNET_DEBUG_TR(Msg::kUsbContextReleased);
}

std::string Context::VersionString() {
  const ::libusb_version* version = ::libusb_get_version();
  if (version == nullptr) {
    return std::string{Text(Msg::kUsbUnknownVersion)};
  }
  return std::format("{}.{}.{}.{}", version->major, version->minor, version->micro,
                     version->nano);
}

bool Context::SupportsHotplug() noexcept {
  return ::libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG) != 0;
}

void Context::RequestStop() noexcept {
  const bool already = stop_requested_.exchange(true, std::memory_order_acq_rel);
  if (!already && context_ != nullptr) {
    ::libusb_interrupt_event_handler(context_);
  }
}

void Context::RunEventLoop() noexcept {
  ConfigureCurrentThread("usb-event", ThreadRole::kDataPath);
  TETHERNET_DEBUG_TR(Msg::kUsbEventThreadStarted);

  ::timeval timeout{};
  timeout.tv_sec = kEventLoopTimeoutSeconds;
  timeout.tv_usec = kEventLoopTimeoutMicros;

  while (!stop_requested_.load(std::memory_order_acquire)) {
    // Use the timeout-taking variant rather than libusb_handle_events(): the
    // latter's internal timeout is fixed at 60 seconds, far too slow for
    // shutdown. Not libusb_handle_events_completed() either, because our exit
    // condition is our own atomic flag, not some transfer's completion.
    const int rc = ::libusb_handle_events_timeout_completed(context_, &timeout, nullptr);

    if (rc == LIBUSB_SUCCESS || rc == LIBUSB_ERROR_INTERRUPTED) {
      continue;
    }
    if (rc == LIBUSB_ERROR_NO_DEVICE) {
      // The device was unplugged. Not the event loop's fault; upper-level
      // reconnect logic handles it, so keep running here.
      TETHERNET_DEBUG_TR(Msg::kUsbEventLoopNoDevice);
      continue;
    }
    TETHERNET_ERROR_TR(Msg::kUsbHandleEventsFailed, ::libusb_error_name(rc), rc);
    break;
  }

  running_.store(false, std::memory_order_release);
  TETHERNET_DEBUG_TR(Msg::kUsbEventThreadExited);
}

}  // namespace tethernet::usb
