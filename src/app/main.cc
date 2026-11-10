// TetherKit command-line entry point.
//
// Kept deliberately thin: parse arguments -> assemble a RuntimeConfig -> hand
// it to core::Runtime -> handle signals. All substantive logic lives in the
// library so it can be covered by unit tests.
#include "tetherkit/common/i18n.h"
#include "tetherkit/common/logging.h"
#include "tetherkit/core/runtime.h"
#include "tetherkit/version.h"

#include <signal.h>  // NOLINT(modernize-deprecated-headers) -- sigaction lives only in this header
#include <unistd.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <thread>

namespace {

using tetherkit::Error;
using tetherkit::Language;
using tetherkit::LogLevel;
using tetherkit::Msg;
using tetherkit::Result;
using tetherkit::Status;
using tetherkit::Text;
using tetherkit::Tr;

/// Global shutdown flag.
///
/// Process-level signal state is inherently global and unavoidable; with a
/// lock-free atomic making it safe, no further wrapping is needed.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
/// **Only volatile sig_atomic_t or lock-free atomics may be touched from a
/// signal handler** -- never locks, allocation or logging: none of those are
/// async-signal-safe. We write a single atomic here; the real shutdown runs on
/// the main thread once it observes it.
std::atomic<bool> g_stop_requested{false};

extern "C" void HandleSignal(int /*signal_number*/) {
  g_stop_requested.store(true, std::memory_order_release);
}

/// Install the signal handlers.
///
/// sigaction rather than signal(): the latter has inconsistent semantics
/// across Unixes and resets to SIG_DFL by default while the handler runs.
[[nodiscard]] Status InstallSignalHandlers() {
  struct sigaction action{};
  action.sa_handler = &HandleSignal;
  // Note: on macOS sigemptyset is a **macro**
  // (#define sigemptyset(set) (*(set)=0,0)), so it cannot be written as
  // ::sigemptyset -- macro names cannot be scope-qualified.
  sigemptyset(&action.sa_mask);
  // Deliberately **do not set** SA_RESTART: we want signals to interrupt
  // blocking syscalls (BPF's read() for example) so threads get a chance to
  // observe the shutdown flag.
  action.sa_flags = 0;

  for (const int signal_number : {SIGINT, SIGTERM, SIGHUP}) {
    if (::sigaction(signal_number, &action, nullptr) != 0) {
      return std::unexpected(
          Error::FromErrno(0, Tr(Msg::kCliInstallSignalHandlerFailed, signal_number)));
    }
  }

  // Ignore SIGPIPE: writing to a closed BPF descriptor triggers it and the
  // default action terminates the process.
  struct sigaction ignore{};
  ignore.sa_handler = SIG_IGN;
  sigemptyset(&ignore.sa_mask);
  if (::sigaction(SIGPIPE, &ignore, nullptr) != 0) {
    return std::unexpected(Error::FromErrno(0, Tr(Msg::kCliIgnoreSigpipeFailed)));
  }
  return tetherkit::Ok();
}

// =============================================================================
// Command-line parsing
// =============================================================================

/// Write a parameterless user-facing string verbatim to stdout.
///
/// fwrite rather than fputs: the string_view from `Text()` points at literals
/// in the table, and although they happen to be NUL-terminated, that is not
/// part of the string_view contract -- do not form bad habits.
void PrintText(Msg id) {
  const std::string_view text = Text(id);
  std::fwrite(text.data(), 1, text.size(), stdout);
}

void PrintUsage() {
  PrintText(Msg::kCliUsage);
}

/// Pull `--lang` out and apply it **before** formal argument parsing.
///
/// Why a separate pass: the help text, and the errors ParseArguments itself
/// emits along the way, must all be in the language the user asked for. By the
/// time ParseArguments reaches `--lang` in order, messages for earlier options
/// have already gone out in the old language.
///
/// Only the `--lang <value>` spelling is accepted (no option in this project
/// supports `--opt=value`, and making an exception here would only confuse
/// ParseArguments, which would treat `--lang=en` as an unknown option). An
/// invalid value is not reported here -- ParseArguments reports it, by which
/// point the error itself is already in the user's language.
void ApplyLanguageOption(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string_view{argv[i]} != "--lang") {
      continue;
    }
    if (Language language{}; tetherkit::ParseLanguage(argv[i + 1], &language)) {
      tetherkit::SetLanguage(language);
    }
    return;
  }
}

/// Parse an unsigned integer argument.
[[nodiscard]] Result<std::uint32_t> ParseUint(std::string_view text, int base = 10) {
  std::uint32_t value = 0;
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto [ptr, error_code] = std::from_chars(begin, end, value, base);
  if (error_code != std::errc{} || ptr != end) {
    return std::unexpected(Error::Generic(Tr(Msg::kCliParseNumberFailed, text)));
  }
  return value;
}

[[nodiscard]] Result<LogLevel> ParseLogLevel(std::string_view text) {
  if (text == "trace") {
    return LogLevel::kTrace;
  }
  if (text == "debug") {
    return LogLevel::kDebug;
  }
  if (text == "info") {
    return LogLevel::kInfo;
  }
  if (text == "warn") {
    return LogLevel::kWarn;
  }
  if (text == "error") {
    return LogLevel::kError;
  }
  if (text == "off") {
    return LogLevel::kOff;
  }
  return std::unexpected(Error::Generic(Tr(Msg::kCliUnknownLogLevel, text)));
}

/// Result of parsing.
struct ParsedArguments {
  tetherkit::core::RuntimeConfig config;
  bool show_help = false;
  bool show_version = false;
  bool list_devices = false;
};

[[nodiscard]] Result<ParsedArguments> ParseArguments(int argc, char** argv) {
  ParsedArguments parsed;

  // Take the value of an option that requires one.
  const auto take_value = [argc, argv](int& index,
                                       std::string_view option) -> Result<std::string_view> {
    if (index + 1 >= argc) {
      return std::unexpected(Error::Generic(Tr(Msg::kCliMissingOptionValue, option)));
    }
    ++index;
    return std::string_view{argv[index]};
  };

  for (int i = 1; i < argc; ++i) {
    const std::string_view argument{argv[i]};

    if (argument == "-h" || argument == "--help") {
      parsed.show_help = true;
      return parsed;
    }
    if (argument == "-V" || argument == "--version") {
      parsed.show_version = true;
      return parsed;
    }
    if (argument == "--list") {
      parsed.list_devices = true;
      continue;
    }
    if (argument == "--keep-feth-mac") {
      parsed.config.adopt_device_mac = false;
      continue;
    }
    if (argument == "--no-color") {
      tetherkit::SetLogColorEnabled(false);
      continue;
    }

    if (argument == "--vid") {
      TETHERKIT_ASSIGN_OR_RETURN(const auto text, take_value(i, argument));
      TETHERKIT_ASSIGN_OR_RETURN(const auto value, ParseUint(text, 16));
      parsed.config.device_filter.vendor_id = static_cast<std::uint16_t>(value);
      continue;
    }
    if (argument == "--pid") {
      TETHERKIT_ASSIGN_OR_RETURN(const auto text, take_value(i, argument));
      TETHERKIT_ASSIGN_OR_RETURN(const auto value, ParseUint(text, 16));
      parsed.config.device_filter.product_id = static_cast<std::uint16_t>(value);
      continue;
    }
    if (argument == "--mtu") {
      TETHERKIT_ASSIGN_OR_RETURN(const auto text, take_value(i, argument));
      TETHERKIT_ASSIGN_OR_RETURN(parsed.config.mtu, ParseUint(text));
      continue;
    }
    if (argument == "--rx-transfers") {
      TETHERKIT_ASSIGN_OR_RETURN(const auto text, take_value(i, argument));
      TETHERKIT_ASSIGN_OR_RETURN(parsed.config.data_channel.rx_transfer_count, ParseUint(text));
      continue;
    }
    if (argument == "--tx-transfers") {
      TETHERKIT_ASSIGN_OR_RETURN(const auto text, take_value(i, argument));
      TETHERKIT_ASSIGN_OR_RETURN(parsed.config.data_channel.tx_transfer_count, ParseUint(text));
      continue;
    }
    if (argument == "--rx-transfer-kb") {
      TETHERKIT_ASSIGN_OR_RETURN(const auto text, take_value(i, argument));
      TETHERKIT_ASSIGN_OR_RETURN(const auto kilobytes, ParseUint(text));
      parsed.config.data_channel.rx_transfer_bytes = kilobytes * 1024;
      continue;
    }
    if (argument == "--max-transfer-kb") {
      TETHERKIT_ASSIGN_OR_RETURN(const auto text, take_value(i, argument));
      TETHERKIT_ASSIGN_OR_RETURN(const auto kilobytes, ParseUint(text));
      parsed.config.rndis.host_max_transfer_size = kilobytes * 1024;
      continue;
    }
    if (argument == "--max-tx-packets") {
      TETHERKIT_ASSIGN_OR_RETURN(const auto text, take_value(i, argument));
      TETHERKIT_ASSIGN_OR_RETURN(parsed.config.rndis.max_tx_packets_per_message, ParseUint(text));
      continue;
    }
    if (argument == "--bpf-buffer-kb") {
      TETHERKIT_ASSIGN_OR_RETURN(const auto text, take_value(i, argument));
      TETHERKIT_ASSIGN_OR_RETURN(const auto kilobytes, ParseUint(text));
      parsed.config.bpf.kernel_buffer_bytes = kilobytes * 1024;
      continue;
    }
    if (argument == "--stats") {
      TETHERKIT_ASSIGN_OR_RETURN(const auto text, take_value(i, argument));
      TETHERKIT_ASSIGN_OR_RETURN(parsed.config.stats_interval_millis, ParseUint(text));
      continue;
    }
    if (argument == "--log") {
      TETHERKIT_ASSIGN_OR_RETURN(const auto text, take_value(i, argument));
      TETHERKIT_ASSIGN_OR_RETURN(const auto level, ParseLogLevel(text));
      tetherkit::SetLogLevel(level);
      continue;
    }
    if (argument == "--lang") {
      // The value was already applied by ApplyLanguageOption before parsing
      // began (so that even this round's errors come out in the target
      // language). Here we only consume it and re-check the spelling.
      TETHERKIT_ASSIGN_OR_RETURN(const auto text, take_value(i, argument));
      Language ignored{};
      if (!tetherkit::ParseLanguage(text, &ignored)) {
        return std::unexpected(Error::Generic(Tr(Msg::kCliUnknownLanguage, text)));
      }
      continue;
    }

    return std::unexpected(Error::Generic(Tr(Msg::kCliUnknownOption, argument)));
  }
  return parsed;
}

/// `--list`: print the recognized RNDIS devices. **Does not need root**.
[[nodiscard]] Status ListDevices(const tetherkit::usb::DeviceFilter& filter) {
  TETHERKIT_ASSIGN_OR_RETURN(const auto context, tetherkit::usb::Context::Create());
  TETHERKIT_ASSIGN_OR_RETURN(const auto candidates,
                             tetherkit::usb::FindRndisDevices(*context, filter));

  if (candidates.empty()) {
    PrintText(Msg::kCliNoDevices);
    PrintText(Msg::kCliNoDevicesHint);
    return tetherkit::Ok();
  }

  PrintText(Msg::kCliDevicesHeader);
  for (const auto& candidate : candidates) {
    const std::string line =
        Tr(Msg::kCliDeviceLine, candidate.Describe(), candidate.control_interface,
           candidate.data_interface, candidate.signature.interface_class,
           candidate.signature.interface_subclass, candidate.signature.interface_protocol,
           candidate.used_android_quirk ? Text(Msg::kCliDeviceAndroidQuirk) : std::string_view{});
    std::fputs(line.c_str(), stdout);
  }
  return tetherkit::Ok();
}

}  // namespace

int main(int argc, char** argv) {
  // The language must be settled **before any** output. Infer from the
  // environment first, then let an explicit --lang override -- that way even
  // argument parsing's own errors come out in the user's language.
  tetherkit::SetLanguage(tetherkit::DetectLanguageFromEnvironment());
  ApplyLanguageOption(argc, argv);

  auto parsed = ParseArguments(argc, argv);
  if (!parsed) {
    const std::string message = Tr(Msg::kCliArgumentError, parsed.error().ToString());
    std::fputs(message.c_str(), stderr);
    return 2;
  }

  if (parsed->show_help) {
    PrintUsage();
    return 0;
  }
  if (parsed->show_version) {
    const std::string line = std::format("{}\n{}\n", tetherkit::GetVersionString(),
                                         tetherkit::GetBuildDescription());
    std::fputs(line.c_str(), stdout);
    return 0;
  }

  if (parsed->list_devices) {
    if (const auto status = ListDevices(parsed->config.device_filter); !status) {
      const std::string message = Tr(Msg::kCliEnumerateFailed, status.error().ToString());
      std::fputs(message.c_str(), stderr);
      return 1;
    }
    return 0;
  }

  if (const auto status = InstallSignalHandlers(); !status) {
    TETHERKIT_ERROR("{}", status.error().ToString());
    return 1;
  }

  TETHERKIT_INFO("{}", tetherkit::GetVersionString());

  auto runtime = tetherkit::core::Runtime::Create(parsed->config);
  if (!runtime) {
    TETHERKIT_ERROR("{}", runtime.error().ToString());
    return 1;
  }

  if (const auto status = (*runtime)->Start(); !status) {
    TETHERKIT_ERROR_TR(Msg::kCliStartFailed, status.error().ToString());
    return 1;
  }

  // A signal handler may only set one atomic (the only async-signal-safe
  // option), so someone has to relay it to the runtime. A lightweight watcher
  // thread rather than letting Runtime read the process-level global directly
  // -- that way Runtime depends on no global state and stays testable.
  std::thread signal_watcher([&runtime] {
    while (!g_stop_requested.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    TETHERKIT_INFO_TR(Msg::kCliStopSignalReceived);
    (*runtime)->RequestStop();
  });

  // The control loop runs on Runtime's own thread (see the thread model notes
  // in core/runtime.h); the main thread only parks here until it ends.
  (*runtime)->WaitUntilStopped();

  // The control loop may have exited because of an internal fatal error
  // rather than a signal; the watcher thread must still be released.
  g_stop_requested.store(true, std::memory_order_release);
  signal_watcher.join();

  (*runtime)->Stop();

  // The start sequence runs asynchronously, so failures do not show up in
  // Start()'s return value -- take them from the snapshot. The error text has
  // already been logged; here we only carry the exit code out so scripts can
  // tell.
  const auto snapshot = (*runtime)->Snapshot();
  return snapshot.fatal_message.empty() ? 0 : 1;
}
