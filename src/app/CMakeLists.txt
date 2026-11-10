# app -- the command-line executable tetherkit-cli
#
# The CMake target name stays tetherkit (the usage
# `cmake --build build --target tetherkit` and the CMake cache are unaffected);
# the **output file name** is tetherkit-cli: on Homebrew the `tetherkit` name
# was claimed by the GUI formula, so the CLI ships as tetherkit-cli.
add_executable(tetherkit main.cc)
set_target_properties(tetherkit PROPERTIES OUTPUT_NAME tetherkit-cli)

# tk_core PUBLIC-links all of the other modules.
target_link_libraries(tetherkit PRIVATE tetherkit_base tk_version tk_core)

install(TARGETS tetherkit RUNTIME DESTINATION bin)
