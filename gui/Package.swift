// swift-tools-version: 5.9
//
// The graphical interface of Tethernet.
//
// Why SwiftPM rather than CMake or an Xcode project:
//
//   * CMake's Swift support only works with the Ninja / Xcode generators,
//     while this repository uses Unix Makefiles, and switching generators
//     would disturb the existing C++ build flow;
//   * An Xcode project file is XML that cannot be read or written
//     reliably by hand, and putting it into version control only brings
//     merge conflicts;
//   * SwiftPM only needs a Package.swift, `swift build` is enough, and it
//     matches the source distribution route (the Homebrew formula).
//
//   The .app bundle is assembled by Scripts/build-gui.sh -- SwiftPM only
//   produces the executable; the Info.plist, the icon and the embedded
//   dylib are all put together in the script.
//
// How libtethernet is found:
//
//   The C++ side is built first, and its product lands in
//   <repo>/build/lib. The path comes in through an environment variable:
//
//     export TETHERNET_LIB_DIR=$PWD/build/lib
//     swift build --package-path gui
//
//   Scripts/build-gui.sh sets it for you. Without it the repository
//   default build/lib is used, so `swift build` inside the repository
//   works too.
import Foundation
import PackageDescription

/// The directory libtethernet lives in.
///
/// An absolute path: SwiftPM's working directory depends on how it is
/// invoked, and a relative path would give different results between
/// `swift build --package-path gui` and `cd gui && swift build`.
let libraryDirectory: String = {
    if let fromEnvironment = ProcessInfo.processInfo.environment["TETHERNET_LIB_DIR"],
       !fromEnvironment.isEmpty {
        return fromEnvironment
    }
    // Package.swift sits in <repo>/gui, so ../build/lib is the default
    // product directory.
    let packageDirectory = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
    return packageDirectory
        .deletingLastPathComponent()
        .appendingPathComponent("build/lib")
        .path
}()

/// All the flags needed to link libtethernet.
///
/// This has to use unsafeFlags -- SwiftPM has no safe interface for
/// "add a library search path". This package is a root package and will
/// not be depended on by others, so the unsafeFlags restriction does not
/// apply.
let tethernetLinkerSettings: [LinkerSetting] = [
    .unsafeFlags([
        "-L\(libraryDirectory)",
        // The rpath has to be passed to the linker in pieces via -Xlinker.
        // Written in the gcc style "-Wl,-rpath,...", swiftc takes it as
        // its own argument and reports "unknown argument" -- that is not
        // a link error, the driver layer simply refuses it.
        //
        // The three rpaths each have a purpose, and the **order matters**
        // (dyld tries them one by one in declaration order):
        //   1. ../Frameworks -- that is where the dylib is once the app
        //      is installed into a .app;
        //   2. next to the executable -- the helper is a bare executable
        //      and the dylib sits right next to it;
        //   3. the build product directory -- so `swift run` works
        //      directly during development.
        //
        // The build directory has to come **last**: it is an absolute
        // path and therefore always exists on the development machine.
        // Put it first and a packaged .app still loads the copy in the
        // build directory on this machine, so the embedded copy never
        // gets verified -- that only shows up on another machine, and by
        // then it is too late.
        // (Scripts/build-gui.sh also removes this rpath from the release
        // artifacts entirely.)
        "-Xlinker", "-rpath", "-Xlinker", "@executable_path/../Frameworks",
        "-Xlinker", "-rpath", "-Xlinker", "@executable_path",
        "-Xlinker", "-rpath", "-Xlinker", libraryDirectory,
    ]),
    .linkedLibrary("tethernet"),
]

/// Tahoe (macOS 26+) selects the window chrome from the executable's
/// LC_BUILD_VERSION `sdk` stamp. SwiftPM links with
/// `-target arm64-apple-macos14.0`, which stamps sdk 14.0 and forces
/// the legacy chrome (compact, flat traffic lights) on every Tahoe
/// machine. Xcode-linked apps stamp the SDK in use -- Hand Mirror is
/// minos 14.4 / sdk 26.5 -- with the deployment target untouched.
/// Do the same here; minos stays 14.0 and the sdk stamp gates only
/// the Tahoe appearance (verified: sdk 14.0 -> legacy lights,
/// sdk 26.5 -> full-size glossy ones, same minos).
let tethernetAppLinkerSettings: [LinkerSetting] = [
    .unsafeFlags([
        "-Xlinker", "-platform_version", "-Xlinker", "macos",
        "-Xlinker", "14.0.0", "-Xlinker", "26.5.0",
    ]),
]

let package = Package(
    name: "TethernetGUI",
    // macOS 14: @Observable and ContentUnavailableView need it. The
    // command line part still supports 13.3; the two are independent
    // artifacts and do not have to be aligned.
    platforms: [.macOS(.v14)],
    products: [
        .executable(name: "TethernetApp", targets: ["TethernetApp"]),
        .executable(name: "tethernet-helper", targets: ["TethernetHelper"]),
    ],
    targets: [
        // The module map for the C ABI. The header is a symbolic link
        // to include/tethernet/capi/tethernet_c.h, so it stays in sync
        // with the C++ side forever and needs no generation step.
        .target(name: "CTethernet"),

        // The XPC protocol and the data models shared by the app and
        // the helper. Both sides stay consistent through one shared
        // source rather than each keeping its own copy.
        .target(name: "TethernetIPC"),

        // The Swift wrapper of the C ABI: it turns tn_* into Swift types
        // and errors.
        .target(name: "TethernetCore",
                dependencies: ["CTethernet", "TethernetIPC"],
                linkerSettings: tethernetLinkerSettings),

        // The privileged helper, running as root.
        .executableTarget(name: "TethernetHelper",
                          dependencies: ["TethernetCore", "TethernetIPC"]),

        // The SwiftUI app the user sees (running as an ordinary user).
        .executableTarget(name: "TethernetApp",
                          dependencies: ["TethernetCore", "TethernetIPC", "TethernetUIModel"],
                          linkerSettings: tethernetAppLinkerSettings),

        // Sidebar page model (SidebarPage: titles, icons, fixed and
        // session-state tints, persisted-selection resolution). A
        // library target of its own rather than folded into
        // TethernetApp: an executable target cannot be
        // @testable-imported, so the tint semantics are only lockable
        // from outside it (see TethernetUIModelTests).
        .target(name: "TethernetUIModel", dependencies: ["TethernetIPC"]),

        // TethernetIPC: pure logic layer, no hardware access, no root.
        // Session and interface config tests live on the C++ side
        // (tests/test_capi.cc), not duplicated here.
        .testTarget(name: "TethernetIPCTests", dependencies: ["TethernetIPC"]),

        // Pure logic of the page model (TethernetUIModel): tint
        // decision and persisted selection.
        .testTarget(name: "TethernetUIModelTests", dependencies: ["TethernetUIModel"]),
    ])
