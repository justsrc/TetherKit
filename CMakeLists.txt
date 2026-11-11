cmake_minimum_required(VERSION 3.24)

project(
  Tethernet
  VERSION 0.1.0
  DESCRIPTION "macOS userspace RNDIS driver: libusb + feth + BPF"
  LANGUAGES CXX)

# ------------------------------------------------------------------------------
# Platform prerequisites
#
# This project depends on the macOS-private feth (if_fake) virtual interface
# and Darwin BPF; it cannot be portable.
# ------------------------------------------------------------------------------
if(NOT APPLE)
  message(FATAL_ERROR "Tethernet supports macOS only: it depends on feth (if_fake) and Darwin BPF.")
endif()

# ------------------------------------------------------------------------------
# Global language standard
#
# Why C++23 (measured on Apple clang 21 / macOS 26.5):
#   * std::expected -- the primary error-propagation mechanism on control and
#     init paths, avoiding a mix of exceptions and error codes.
#   * std::byteswap -- the RNDIS wire format is little-endian, so byte order
#     flips need explicit conversion.
# ------------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Deployment target:
#   * Do not set it proactively; let CMake take the host macOS version --
#     otherwise linking against Homebrew's libusb (built for the host version)
#     produces the "building for macOS-13.3, but linking with dylib which was
#     built for newer version" warning.
#   * But the 13.3 floor must be enforced: libc++'s std::to_chars (the
#     underlying implementation of std::format's float formatting) carries
#     availability annotations, and anything lower simply fails to compile.
set(TETHERNET_MIN_MACOS_VERSION "13.3")

if(CMAKE_OSX_DEPLOYMENT_TARGET AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS
                                   TETHERNET_MIN_MACOS_VERSION)
  message(
    FATAL_ERROR
      "CMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET} is too low; "
      "at least ${TETHERNET_MIN_MACOS_VERSION} is required (libc++ std::to_chars carries availability annotations).")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE
      "RelWithDebInfo"
      CACHE STRING "Build type" FORCE)
endif()

# Always export compile_commands.json, for clangd / clang-tidy.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Position-independent code, globally.
#
# The feature modules are all static libraries, but they end up linked into
# the shared lib libtethernet -- non-PIC object files cannot link into a
# dylib. On macOS/clang, PIC is the default anyway; spelling it out here keeps
# the "why is PIC mandatory" reasoning documented in the build script itself.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Keep executables and libraries under build/bin and build/lib for easy script reference.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ------------------------------------------------------------------------------
# Build options
# ------------------------------------------------------------------------------
option(TETHERNET_BUILD_TESTS "Build unit tests" ON)
option(TETHERNET_BUILD_BENCHMARKS "Build performance benchmarks" ON)
option(TETHERNET_BUILD_SHARED_LIB "Build shared lib libtethernet (needed by the GUI and other language bindings)" ON)
option(TETHERNET_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(TETHERNET_NATIVE_ARCH "Optimize for the host CPU microarchitecture (-mcpu=native; binary not portable)" OFF)
option(TETHERNET_ENABLE_LTO "Enable link-time optimization (Release-style builds)" OFF)
option(TETHERNET_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(TETHERNET_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(TETHERNET_ENABLE_TSAN "Enable ThreadSanitizer (validates the lock-free queues and thread model)" OFF)

include(CompilerWarnings)
include(Sanitizers)
include(Optimizations)

# ------------------------------------------------------------------------------
# Dependencies
# ------------------------------------------------------------------------------
find_package(LibUSB REQUIRED)

# ------------------------------------------------------------------------------
# Subdirectories
#
# The dependency direction is strictly one-way: common <- rndis/net <- usb <- core <- app
# ------------------------------------------------------------------------------
add_subdirectory(third_party)
add_subdirectory(src)

if(TETHERNET_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(TETHERNET_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

# ------------------------------------------------------------------------------
# Graphical interface
#
# The Swift part is built by SwiftPM (see gui/Package.swift for why CMake's
# Swift support is not used); this only provides a convenient entry point that
# chains "build C++ first, then Swift":
#
#   cmake --build build --target gui
#
# Deliberately a **manual** target (the effect of EXCLUDE_FROM_ALL): the GUI
# needs the Xcode toolchain, and someone who only wants the command line should
# not be forced to install it.
# ------------------------------------------------------------------------------
if(TETHERNET_BUILD_SHARED_LIB AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/gui/Package.swift")
  # Pass the current build directory to the script explicitly (by default it
  # looks for <repo>/build), so libtethernet is also found correctly in
  # non-default build dirs such as build-rel / build-tsan.
  add_custom_target(
    gui
    COMMAND "${CMAKE_COMMAND}" -E env "TETHERNET_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/gui/Scripts/build-gui.sh"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS tethernet_shared
    COMMENT "Building Tethernet.app and tethernet-helper"
    USES_TERMINAL
    VERBATIM)
  set_property(TARGET gui APPEND PROPERTY
               ADDITIONAL_CLEAN_FILES "${CMAKE_CURRENT_SOURCE_DIR}/dist")
endif()

# ------------------------------------------------------------------------------
# Build configuration summary
# ------------------------------------------------------------------------------
message(STATUS "")
message(STATUS "==== Tethernet ${PROJECT_VERSION} build configuration ====")
message(STATUS "  Build type        : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard      : C++${CMAKE_CXX_STANDARD}")
if(CMAKE_OSX_DEPLOYMENT_TARGET)
  set(_tn_deploy "macOS ${CMAKE_OSX_DEPLOYMENT_TARGET}")
else()
  set(_tn_deploy "host default")
endif()
message(STATUS "  Deployment target : ${_tn_deploy} (floor ${TETHERNET_MIN_MACOS_VERSION})")
message(STATUS "  Compiler          : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  libusb            : ${LibUSB_VERSION} (${LibUSB_LIBRARY})")
message(STATUS "  Unit tests        : ${TETHERNET_BUILD_TESTS}")
message(STATUS "  Benchmarks        : ${TETHERNET_BUILD_BENCHMARKS}")
message(STATUS "  Host microarch    : ${TETHERNET_NATIVE_ARCH}")
message(STATUS "  LTO               : ${TETHERNET_ENABLE_LTO}")
message(STATUS "  ASan/UBSan/TSan   : ${TETHERNET_ENABLE_ASAN}/${TETHERNET_ENABLE_UBSAN}/${TETHERNET_ENABLE_TSAN}")
message(STATUS "=========================================")
message(STATUS "")
