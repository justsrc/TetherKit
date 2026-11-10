// doctest's main() lives in its own translation unit.
//
// This way, editing any test file never recompiles the implementation part of
// the 7000-line doctest header, which significantly shortens incremental build
// times.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
