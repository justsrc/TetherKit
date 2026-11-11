// Smoke tests for the version info: confirms the CMake version-injection
// pipeline is wired up.
#include "tethernet/version.h"

#include <doctest.h>

TEST_SUITE("version") {
  TEST_CASE("Version number is not all zeros") {
    const auto v = tethernet::GetVersion();
    CHECK((v.major != 0 || v.minor != 0 || v.patch != 0));
  }

  TEST_CASE("Version string contains the project name") {
    const std::string_view s = tethernet::GetVersionString();
    CHECK(s.find("Tethernet") != std::string_view::npos);
  }

}  // TEST_SUITE("version")
