# src -- the feature modules
#
# The dependency direction is strictly one-way; reverse or circular
# dependencies are forbidden:
#
#   tn_common  <- no dependencies (logging, errors, lock-free containers,
#                 memory pool, statistics)
#      |
#      v
#   tn_rndis   <- common           (RNDIS wire format and state machine, pure
#                                   logic, no I/O)
#   tn_net     <- common           (feth virtual interface + BPF link, macOS
#                                   syscalls only)
#   tn_usb     <- common           (libusb wrapper)
#      |
#      v
#   tn_core    <- common+rndis+net+usb (data-path bridging and runtime
#                                      orchestration)
#      |
#      v
#   tethernet  <- core             (command-line executable)
#   tn_capi    <- core             (C ABI, consumed by the Swift GUI)

# ------------------------------------------------------------------------------
# Common compile properties inherited by every target in this project.
# ------------------------------------------------------------------------------
add_library(tethernet_base INTERFACE)
target_include_directories(tethernet_base INTERFACE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(tethernet_base INTERFACE tethernet_warnings tethernet_sanitizers
                                               tethernet_optimizations)

# ------------------------------------------------------------------------------
# Version: generated from the template so that a version bump does not force a
# full rebuild.
# ------------------------------------------------------------------------------
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/version.cc.in" "${CMAKE_CURRENT_BINARY_DIR}/version.cc"
               @ONLY)

add_library(tn_version STATIC "${CMAKE_CURRENT_BINARY_DIR}/version.cc")
target_link_libraries(tn_version PUBLIC tethernet_base)

add_subdirectory(common)
add_subdirectory(rndis)
add_subdirectory(net)
add_subdirectory(usb)
add_subdirectory(core)
add_subdirectory(capi)
add_subdirectory(app)
