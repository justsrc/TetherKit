# src -- the feature modules
#
# The dependency direction is strictly one-way; reverse or circular
# dependencies are forbidden:
#
#   tk_common  <- no dependencies (logging, errors, lock-free containers,
#                 memory pool, statistics)
#      |
#      v
#   tk_rndis   <- common           (RNDIS wire format and state machine, pure
#                                   logic, no I/O)
#   tk_net     <- common           (feth virtual interface + BPF link, macOS
#                                   syscalls only)
#   tk_usb     <- common           (libusb wrapper)
#      |
#      v
#   tk_core    <- common+rndis+net+usb (data-path bridging and runtime
#                                      orchestration)
#      |
#      v
#   tetherkit  <- core             (command-line executable)
#   tk_capi    <- core             (C ABI, consumed by the Swift GUI)

# ------------------------------------------------------------------------------
# Common compile properties inherited by every target in this project.
# ------------------------------------------------------------------------------
add_library(tetherkit_base INTERFACE)
target_include_directories(tetherkit_base INTERFACE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(tetherkit_base INTERFACE tetherkit_warnings tetherkit_sanitizers
                                               tetherkit_optimizations)

# ------------------------------------------------------------------------------
# Version: generated from the template so that a version bump does not force a
# full rebuild.
# ------------------------------------------------------------------------------
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/version.cc.in" "${CMAKE_CURRENT_BINARY_DIR}/version.cc"
               @ONLY)

add_library(tk_version STATIC "${CMAKE_CURRENT_BINARY_DIR}/version.cc")
target_link_libraries(tk_version PUBLIC tetherkit_base)

add_subdirectory(common)
add_subdirectory(rndis)
add_subdirectory(net)
add_subdirectory(usb)
add_subdirectory(core)
add_subdirectory(capi)
add_subdirectory(app)
