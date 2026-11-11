// Runs a system tool (ipconfig / route) and collects its output.
//
//  Why not system() / popen()
//   Both of those go through /bin/sh. The arguments -- interface names, IP
//   addresses -- are all validated by us, but as long as they pass through a
//   shell the whole defense rests on "the validation has no hole"; passing
//   the argv array straight to posix_spawn means shell metacharacters have
//   nowhere to be interpreted in the first place.
//
//  Why external tools instead of issuing the ioctls ourselves
//   `ipconfig set` goes through IPConfiguration's proper registration path,
//   and the service it creates is adopted by IPMonitor (DNS takes effect,
//   the default route gets installed); an address configured by our own
//   SIOCAIFADDR is not recognized by configd at all. This was confirmed by
//   measurement in docs/GUI-SPIKE.md sections 3.2 / 6.3, so it must not be
//   reverted to bare ioctls just to save one fork.
#pragma once

#include "tethernet/common/error.h"

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace tethernet::capi {

struct ProcessResult {
  int exit_code = 0;
  /// The child's stdout and stderr merged together.
  ///
  /// The merge is deliberate: some of these tools report errors on stdout and
  /// some on stderr, and collecting them separately only makes "it failed
  /// and we never got the reason" more likely.
  std::string output;

  [[nodiscard]] bool Succeeded() const noexcept { return exit_code == 0; }
};

/// Runs an executable and waits for it to finish.
///
/// @param executable Absolute path. It must be an absolute path -- relying on
///                   PATH makes the behavior depend on the caller's
///                   environment, and the helper is launched by launchd,
///                   whose PATH differs from the one in a terminal.
/// @param arguments  Without argv[0]; the function appends it.
///
/// An error return means only "could not start the process, or did not wait
/// for it to finish"; a process that starts and exits non-zero is a success
/// that returns a ProcessResult with exit_code != 0 -- the caller has to
/// judge that itself.
[[nodiscard]] Result<ProcessResult> RunTool(std::string_view executable,
                                            std::initializer_list<std::string_view> arguments);

/// The overload for a dynamically built argument list.
[[nodiscard]] Result<ProcessResult> RunTool(std::string_view executable,
                                            const std::vector<std::string>& arguments);

}  // namespace tethernet::capi
