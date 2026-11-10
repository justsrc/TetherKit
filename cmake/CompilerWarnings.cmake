# CompilerWarnings.cmake -- centralized warning options.
#
# Provides the INTERFACE target tetherkit_warnings; every project-owned target
# links it, while third_party/ code does not (keeps the noise down).

add_library(tetherkit_warnings INTERFACE)

set(_tk_warnings
    -Wall
    -Wextra
    -Wpedantic
    # Implicit conversions are the most error-prone spots in protocol parsing
    # code; this one is mandatory.
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
    # Implicit heap allocation on the data path is a performance killer; let
    # the compiler police VLAs.
    -Wvla
    # Struct padding affects our hand-packed wire-format structs and deserves a
    # static_assert backstop, but -Wpadded is far too noisy -- instead the code
    # pins sizes with static_assert(sizeof(...)).
)

target_compile_options(tetherkit_warnings INTERFACE ${_tk_warnings})

if(TETHERKIT_WARNINGS_AS_ERRORS)
  target_compile_options(tetherkit_warnings INTERFACE -Werror)
endif()
