# third_party -- vendored third-party dependencies
#
# Vendoring is a deliberate choice over FetchContent: the configure step stays
# offline, so `cmake -B build` succeeds reproducibly at any time and under any
# network conditions.

# doctest 2.4.12 -- a single-header test framework.
# Why it over GoogleTest/Catch2: one header, fast to compile, expressive enough
# assertions, native SUBCASE support for parameterized cases, and safe to use
# for assertions in multithreaded tests.
add_library(doctest INTERFACE)
target_include_directories(doctest SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/doctest")
