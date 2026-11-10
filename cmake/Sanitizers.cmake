# Sanitizers.cmake -- sanitizer switches.
#
# Provides the INTERFACE target tetherkit_sanitizers.
#
# Notes:
#  * ASan and TSan are mutually exclusive; they cannot be on together.
#  * TSan is this project's most important sanitizer -- memory-ordering
#    correctness between the lock-free SPSC ring, the libusb event thread, and
#    the BPF reader/writer threads can only be validated by it. Run at least:
#      cmake -B build-tsan -DTETHERKIT_ENABLE_TSAN=ON && ctest --test-dir build-tsan
#  * Sanitizers cut throughput substantially; benchmarks must run on a
#    Release build without any sanitizer enabled.

add_library(tetherkit_sanitizers INTERFACE)

if(TETHERKIT_ENABLE_ASAN AND TETHERKIT_ENABLE_TSAN)
  message(FATAL_ERROR "AddressSanitizer and ThreadSanitizer cannot be enabled together.")
endif()

set(_tk_san_list "")

if(TETHERKIT_ENABLE_ASAN)
  list(APPEND _tk_san_list address)
endif()

if(TETHERKIT_ENABLE_TSAN)
  list(APPEND _tk_san_list thread)
endif()

if(TETHERKIT_ENABLE_UBSAN)
  list(APPEND _tk_san_list undefined)
  # Do we ever make unaligned accesses on hand-packed wire-format structs?
  # No -- every read is byte-wise, so the alignment check stays on to catch
  # genuine bugs.
endif()

set(_tk_ubsan_recover_opts "")
if(TETHERKIT_ENABLE_UBSAN)
  # UBSan defaults to report-and-continue: a diagnostic prints and the
  # process keeps running, so ctest still exits 0 and a sanitizer job stays
  # green through real UB. Compile every UBSan diagnostic fatal instead;
  # the CI job that enables UBSan additionally sets UBSAN_OPTIONS=
  # halt_on_error=1 to cover sanitizers inside code we cannot recompile.
  list(APPEND _tk_ubsan_recover_opts -fno-sanitize-recover=undefined)
endif()

if(_tk_san_list)
  list(JOIN _tk_san_list "," _tk_san_flags)
  set(TETHERKIT_SANITIZER_SUMMARY "${_tk_san_flags}")
  target_compile_options(tetherkit_sanitizers INTERFACE -fsanitize=${_tk_san_flags}
                                                        -fno-omit-frame-pointer -g
                                                        ${_tk_ubsan_recover_opts})
  target_link_options(tetherkit_sanitizers INTERFACE -fsanitize=${_tk_san_flags})
  message(STATUS "Sanitizers enabled: ${_tk_san_flags}")
else()
  set(TETHERKIT_SANITIZER_SUMMARY "none")
endif()
