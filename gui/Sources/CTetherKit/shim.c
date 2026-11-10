// SwiftPM requires every C target to have at least one source file, otherwise
// it refuses to build.
//
// The entire content of this target is actually include/tetherkit_c.h (a
// symlink to the C++-side header) -- the real implementation lives in
// libtetherkit.dylib. This empty translation unit exists only to satisfy
// SwiftPM's formal requirement.
//
// It also does something useful along the way: wrapping the header catches
// problems like "the header contains syntax only C++ accepts" at compile time,
// rather than waiting for the Swift-side import to fail.
#include "tetherkit_c.h"

// A non-exported dummy symbol to prevent some linkers from warning about a
// completely empty object file.
static const int tetherkit_c_shim_anchor = 0;
const int* tetherkit_c_shim_unused(void);
const int* tetherkit_c_shim_unused(void) { return &tetherkit_c_shim_anchor; }
