import XCTest

@testable import TetherKitIPC

/// Encoding/decoding and comparison of the helper version string.
///
/// What these cases pin down:
///   "Which version of the component is installed" is the only fact the App can
///   ask for (`helperVersion` is the one method an old component will still
///   answer), and the next two decisions both depend on it: a protocol mismatch
///   must be blocked on the onboarding page, and a version mismatch must light
///   up "update privileged component". Getting the parsing wrong fails silently
///   -- either an old component is treated as current (the user keeps running the
///   pre-upgrade library) or a useless update prompt appears every day that never
///   goes away when clicked. Neither produces any error; only tests can guard
///   this.
final class HelperConstantsTests: XCTestCase {

    // MARK: - Encode/decode

    func testEncodeDecodeRoundTrip() {
        let encoded = HelperConstants.encodeVersion("TetherKit 0.1.4 (C++23, macOS 13.3+)")
        let (revision, version) = HelperConstants.decodeVersion(encoded)

        XCTAssertEqual(revision, HelperConstants.protocolRevision)
        XCTAssertEqual(version, "TetherKit 0.1.4 (C++23, macOS 13.3+)")
    }

    /// A string returned by an old component has no separator -- exactly the kind
    /// of component that must be identified, recorded as revision 0.
    func testDecodeLegacyVersionWithoutRevision() {
        let (revision, version) =
            HelperConstants.decodeVersion("TetherKit 0.1.2 (C++23, macOS 13.3+)")

        XCTAssertEqual(revision, 0)
        XCTAssertEqual(version, "TetherKit 0.1.2 (C++23, macOS 13.3+)")
        XCTAssertNotEqual(revision, HelperConstants.protocolRevision,
                          "0 must differ from every real revision, otherwise an old component would be treated as matching")
    }

    // MARK: - Version number extraction

    func testSemanticVersionFromLibraryVersionString() {
        XCTAssertEqual(
            HelperConstants.semanticVersion(of: "TetherKit 0.1.4 (C++23, macOS 13.3+)"), "0.1.4")
    }

    /// The other two numbers in the string (C++ standard, minimum macOS version)
    /// must not be taken as the version number: the former has no dot, the latter
    /// comes after the version number.
    func testSemanticVersionIgnoresBuildConfigurationNumbers() {
        let text = "TetherKit 1.0.0 (C++23, macOS 13.3+)"
        XCTAssertEqual(HelperConstants.semanticVersion(of: text), "1.0.0")
    }

    /// A difference in build configuration only (the same version rebuilt with
    /// different compile options) does not count as a version mismatch -- this is
    /// why the whole string is not compared directly: otherwise a spurious
    /// warning would appear that never goes away when clicked.
    func testSemanticVersionEqualAcrossBuildConfigurations() {
        let installed = HelperConstants.semanticVersion(of: "TetherKit 0.1.4 (C++23, macOS 13.3+)")
        let bundled = HelperConstants.semanticVersion(of: "TetherKit 0.1.4 (C++26, macOS 15.0+)")

        XCTAssertEqual(installed, bundled)
    }

    func testSemanticVersionDetectsDifferentReleases() {
        let installed = HelperConstants.semanticVersion(of: "TetherKit 0.1.3 (C++23, macOS 13.3+)")
        let bundled = HelperConstants.semanticVersion(of: "TetherKit 0.1.4 (C++23, macOS 13.3+)")

        XCTAssertNotEqual(installed, bundled)
        XCTAssertEqual(installed, "0.1.3")
        XCTAssertEqual(bundled, "0.1.4")
    }

    /// Falls back to the whole string when nothing can be extracted: better a
    /// false positive (a spurious update prompt) than a missed one (the user keeps
    /// running an old component without knowing it).
    func testSemanticVersionFallsBackToWholeString() {
        XCTAssertEqual(HelperConstants.semanticVersion(of: "  TetherKit dev  "), "TetherKit dev")
        XCTAssertNotEqual(HelperConstants.semanticVersion(of: "TetherKit dev-a"),
                          HelperConstants.semanticVersion(of: "TetherKit dev-b"))
    }

    /// Comparing a version to itself after installation must match -- if this
    /// turns red, the UI will show a "component needs updating" prompt that stays
    /// in an endless loop even after updating.
    func testSemanticVersionIsStableForSameInput() {
        let text = "TetherKit 0.1.4 (C++23, macOS 13.3+)"
        XCTAssertEqual(HelperConstants.semanticVersion(of: text),
                       HelperConstants.semanticVersion(of: text))
    }
}
