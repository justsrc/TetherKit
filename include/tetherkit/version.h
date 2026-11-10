// TetherKit version information.
//
// The version number is injected into version.cc by CMake at configure time;
// this header only exposes accessors, so bumping the version does not trigger
// a full project rebuild.
#pragma once

#include <cstdint>
#include <string_view>

namespace tetherkit {

/// The three components of the semantic version number.
struct Version {
  std::uint16_t major;
  std::uint16_t minor;
  std::uint16_t patch;
};

/// Returns the version number burned in at compile time.
Version GetVersion() noexcept;

/// Returns a human-readable version string like "TetherKit 0.1.0 (C++23, macOS 13.3+)".
std::string_view GetVersionString() noexcept;

/// Returns a build description like
/// "RelWithDebInfo, AppleClang 21.0.0, C++23, LTO=off, sanitizers=none".
///
/// Benchmark reports must include it -- an ns/op number without the build
/// configuration is meaningless.
std::string_view GetBuildDescription() noexcept;

}  // namespace tetherkit
