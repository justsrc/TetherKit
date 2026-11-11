# tn_capi -- the C ABI layer
#
# The **only** extern "C" boundary of the whole project; it exists for one
# reason: to serve the Swift GUI.
# It sits at the top of the dependency chain (same level as app) and depends
# on core and everything below it.
#
# Built as an OBJECT library rather than a STATIC one, so that the shared
# library libtethernet picks up **all** the object files. With a static
# library the linker only pulls in the members somebody references, and the
# exported C ABI functions are referenced by nobody inside the library -- the
# result is an empty dylib, with no error message whatsoever.
# (The other route is -force_load, but an OBJECT library is the more direct
# way to say it in CMake.)

add_library(
  tn_capi OBJECT
  capi_support.cc
  core_foundation_support.cc
  environment.cc
  log_ring.cc
  net_config.cc
  orphan_cleanup.cc
  process_runner.cc
  session.cc)

target_link_libraries(tn_capi PUBLIC tethernet_base tn_version tn_core)

# The interface IP configuration has to read back configd's dynamic store
# (the gateway and DNS that are actually in effect), and that means going
# through SystemConfiguration. This is a dependency of tn_capi alone -- the
# lower layers never touch configd.
target_link_libraries(tn_capi PUBLIC "-framework CoreFoundation"
                                     "-framework SystemConfiguration")

# ------------------------------------------------------------------------------
# libtethernet -- the shared library we distribute
#
# Swift can only link shared libraries or system libraries, not a pile of CMake
# static targets, so this layer has to exist.
# ------------------------------------------------------------------------------
if(TETHERNET_BUILD_SHARED_LIB)
  add_library(tethernet_shared SHARED)
  target_link_libraries(tethernet_shared PRIVATE tn_capi)

  set_target_properties(
    tethernet_shared
    PROPERTIES OUTPUT_NAME tethernet
               VERSION "${PROJECT_VERSION}"
               SOVERSION "${PROJECT_VERSION_MAJOR}"
               # Let the dependents load it by @rpath instead of baking in an
               # absolute path -- otherwise the .app has to run install_name_tool
               # on the embedded dylib afterwards (and re-sign it after that,
               # see the pitfall documented in docs/GUI-SPIKE.md section 7).
               INSTALL_NAME_DIR "@rpath")

  # Export only the C ABI; every other symbol (including the C++ mangled
  # names) is hidden. The reason is documented in the allowlist file.
  target_link_options(tethernet_shared PRIVATE
                      "-Wl,-exported_symbols_list,${PROJECT_SOURCE_DIR}/cmake/tethernet_exports.txt")

  install(TARGETS tethernet_shared LIBRARY DESTINATION lib)
endif()

# The C ABI header is installed for the outside: once the Homebrew formula is
# installed, other projects can bind to it too.
install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/tethernet/capi"
        DESTINATION include/tethernet
        FILES_MATCHING
        PATTERN "*.h")
