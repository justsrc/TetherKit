# app -- the command-line executable tethernet-cli
#
# The CMake target name stays tethernet (the usage
# `cmake --build build --target tethernet` and the CMake cache are unaffected);
# the **output file name** is tethernet-cli: on Homebrew the `tethernet` name
# was claimed by the GUI formula, so the CLI ships as tethernet-cli.
add_executable(tethernet main.cc)
set_target_properties(tethernet PROPERTIES OUTPUT_NAME tethernet-cli)

# tn_core PUBLIC-links all of the other modules.
target_link_libraries(tethernet PRIVATE tethernet_base tn_version tn_core)

install(TARGETS tethernet RUNTIME DESTINATION bin)
