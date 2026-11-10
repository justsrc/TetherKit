# tests -- unit tests
#
# Every test is linked into a single executable, tetherkit_tests, because:
#  * doctest registration is global, and one binary has the lowest startup
#    overhead;
#  * the module can be run via doctest's --test-suite filter alone;
#  * CTest registers several cases per test-suite, so failing cases still
#    pinpoint exactly which module broke.

add_executable(
  tetherkit_tests
  test_main.cc
  test_version.cc
  test_common_byte_order.cc
  test_common_error.cc
  test_common_frame_ring.cc
  test_common_i18n.cc
  test_common_spsc_ring.cc
  test_common_stats.cc
  test_rndis_messages.cc
  test_rndis_packet_codec.cc
  test_rndis_state_machine.cc
  test_net_link.cc
  test_core_bridge.cc
  test_capi.cc)

# tk_rndis / tk_net both PUBLIC-link tk_common, so it does not need repeating
# (otherwise the linker prints "ignoring duplicate libraries").
# tk_core PUBLIC-links tk_common/tk_rndis/tk_net/tk_usb, so listing it alone is
# enough.
# tk_capi PUBLIC-links tk_version and tk_core, so neither needs repeating.
target_link_libraries(tetherkit_tests PRIVATE tetherkit_base doctest tk_capi)
# The src/capi directory: capi_support.h is an implementation-internal header,
# not installed publicly, but it must be includable directly by the tests.
target_include_directories(tetherkit_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                                                   "${PROJECT_SOURCE_DIR}/src/capi")

# ------------------------------------------------------------------------------
# CTest registration
#
#  This repo once had a pitfall where the whole suite ran zero cases and read
# green. Two things to watch:
#
#  1. **The filter argument must not be quoted.** Written as
#     `--test-suite="${_suite}"`, CMake passes the quote marks through as
#     argument content (verified with ctest -V: it passes
#     `"--test-suite="rndis.state_machine""`), doctest matches zero cases --
#     so "0 cases ran", and doctest returns exit code 0 on zero matches, so
#     ctest reports "Passed". The real failures were swallowed entirely.
#
#  2. Hence an extra safety net: each case sets **FAIL_REGULAR_EXPRESSION** so
#     that any "assertions: 0" in the output fails the test. A mistyped suite
#     name will go red immediately from now on.
#     This **must** be FAIL_ rather than PASS_REGULAR_EXPRESSION -- the latter
#     **replaces** the exit-code check (documented CMake behavior), so once set,
#     a genuine assertion failure would be ignored instead, digging the pit
#     right back open. FAIL_ is additive on top of the exit code.
#
# A separate `all` case that runs the entire binary is registered as the
# **authoritative result** -- even if a suite gets forgotten in the list below,
# it will expose the failure.
# ------------------------------------------------------------------------------
add_test(NAME all COMMAND tetherkit_tests)

set(TETHERKIT_TEST_SUITES
    version
    common.byte_order
    common.error
    common.frame_ring
    common.frame_ring_batch
    common.i18n
    common.spsc_ring
    common.stats
    rndis.messages
    rndis.packet_codec
    rndis.state_machine
    rndis.state_machine_timeout
    net.abi
    net.loopback
    net.feth
    core.bridge
    capi.support
    capi.basics
    capi.log_ring
    capi.session
    capi.process
    capi.net_config)

foreach(_suite IN LISTS TETHERKIT_TEST_SUITES)
  # Note: do **not** put quotes after --test-suite=, see the explanation above.
  add_test(NAME "${_suite}" COMMAND tetherkit_tests --test-suite=${_suite})
  set_tests_properties("${_suite}" PROPERTIES FAIL_REGULAR_EXPRESSION "assertions: +0 \\|")
endforeach()
