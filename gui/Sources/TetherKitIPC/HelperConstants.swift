import Foundation

/// A set of constants agreed on between the App and the helper.
///
/// Kept in a single file so that "changing a name means synchronizing which
/// places" has exactly one answer -- these strings appear simultaneously in the
/// LaunchDaemon plist, install scripts, and code on both sides, and any place
/// left out of sync manifests as "cannot connect to helper" with no useful error.
public enum HelperConstants {
    /// Mach service name. Must match the MachServices key in
    /// /Library/LaunchDaemons/*.plist.
    public static let machServiceName = "com.tetherkit.helper"

    /// Install path of the helper executable.
    ///
    /// /Library/PrivilegedHelperTools is Apple's conventional location for
    /// privileged helpers; only root can write there.
    public static let helperExecutablePath = "/Library/PrivilegedHelperTools/com.tetherkit.helper"

    /// LaunchDaemon configuration file path.
    public static let launchDaemonPlistPath = "/Library/LaunchDaemons/com.tetherkit.helper.plist"

    /// Authorization right required for privileged operations.
    ///
    /// Why the built-in system.privilege.admin, not a custom right:
    ///   A custom right must first be written into the policy database with
    ///   AuthorizationRightSet, which itself requires administrator privileges
    ///   -- creating a chicken-and-egg problem of "authorization needs
    ///   authorization to install". The rule for system.privilege.admin is
    ///   authenticate-admin, which shows exactly the password / Touch ID prompt
    ///   we want, and its semantics fit: "this is an operation that needs an
    ///   administrator identity".
    public static let privilegedRightName = "system.privilege.admin"

    /// XPC interface revision. Increment this on every change to
    /// TetherKitHelperProtocol.
    ///
    /// Why this is needed: the helper is installed in a system directory, so if
    /// the App is upgraded but the helper is not reinstalled, the method
    /// signatures on the two sides no longer match -- manifesting as calls that
    /// hang or crash outright, with nothing indicating a version problem. With
    /// this number, the App can detect the mismatch as soon as it connects and
    /// tell the user clearly to reinstall the privileged component.
    ///
    /// Revision history:
    ///   1 -- initial version
    ///   2 -- privileged method replies changed from (String?) to (String?, Bool)
    ///        to distinguish authorization failures
    ///   3 -- added setLanguage so helper notices and library logs follow the UI
    ///        language
    public static let protocolRevision = 3

    /// Encode the revision into the version string.
    ///
    /// Intentionally reuses the existing `helperVersion` method instead of
    /// adding a new one -- adding a method is itself a protocol change, and an
    /// old helper does not have it, which would bring back "mismatch but no way
    /// to detect it". Using this method, which any old helper will still answer,
    /// is the only way to reliably identify an old helper.
    public static func encodeVersion(_ version: String) -> String {
        "\(protocolRevision)|\(version)"
    }

    /// Parse a version string. Strings returned by old helpers have no separator,
    /// in which case the revision is recorded as 0.
    public static func decodeVersion(_ encoded: String) -> (revision: Int, version: String) {
        guard let separator = encoded.firstIndex(of: "|"),
              let revision = Int(encoded[encoded.startIndex..<separator]) else {
            return (0, encoded)
        }
        return (revision, String(encoded[encoded.index(after: separator)...]))
    }

    /// Extract the semantic version number from the library's version string:
    /// `"TetherKit 0.1.4 (C++23, macOS 13.3+)"` -> `"0.1.4"`.
    ///
    /// Why not compare the whole string:
    ///   Besides the version number, the string also carries the C++ standard and
    ///   minimum macOS version -- that is build configuration, not the version.
    ///   Using the whole string as the criterion would produce a spurious
    ///   "component needs updating" warning after a rebuild with different
    ///   compile options, and nothing would change when the user clicked it.
    ///
    /// Falls back to the whole string when nothing can be extracted (no dotted
    /// number in the string): better a false positive than a missed one -- a
    /// missed one means the user keeps running the pre-upgrade library without
    /// knowing it.
    public static func semanticVersion(of text: String) -> String {
        // Must contain at least one dot, otherwise values like "C++23" would be
        // mistaken for a version number.
        guard let range = text.range(of: "[0-9]+(\\.[0-9]+)+", options: .regularExpression) else {
            return text.trimmingCharacters(in: .whitespacesAndNewlines)
        }
        return String(text[range])
    }

    /// XPC call timeout (seconds).
    ///
    /// 30 seconds because the slowest call is DHCP configuration: the library
    /// waits up to 10 seconds for a lease internally; adding USB handshake and
    /// interface creation, this leaves a 3x margin.
    public static let requestTimeout: TimeInterval = 30
}
