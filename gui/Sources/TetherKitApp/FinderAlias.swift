import Foundation
import TetherKitIPC

/// Maintain a Finder alias pointing at this .app in the Applications
/// directory, so Spotlight can find it.
///
/// Why an alias (bookmark) and not a symbolic link:
///   The Homebrew formula installs the .app into the Cellar, and Spotlight
///   does not index that; a **symbolic link** pointing at the App is not
///   recognized by Spotlight either. A Finder alias is a real bookmark
///   file: Spotlight indexes it by file name and Return launches the
///   target. It is created with the URL bookmark API
///   (.suitableForBookmarkFile), which needs no help from Finder and
///   triggers no permission dialog.
///
/// Why it is created at first launch and not at install time:
///   Measured: brew runs even its postinstall inside a sandbox, and
///   writing /Applications is silently blocked -- so "create it right
///   after installing" does not work. Instead the app checks it on every
///   launch as a side task: add it if missing, rewrite it if the target
///   drifted (brew upgrade changed the Cellar version path). The
///   `--install-finder-alias` mode (create and exit) is kept for the CI
///   smoke test and for manual rebuilds.
enum FinderAlias {
    /// The alias file name. Without .app -- it is a bookmark file, not a
    /// bundle; the name Spotlight shows is exactly this.
    private static let aliasName = "TetherKit"

    /// Make sure the alias exists and points at bundleURL. Returns a
    /// plain-language result (the --install-finder-alias mode prints it
    /// verbatim). Failures do not throw -- the alias is a bonus and must
    /// not block any flow.
    @discardableResult
    static func ensure(for bundleURL: URL) -> String {
        guard bundleURL.pathExtension == "app" else {
            return L(.aliasSkippedNotBundle)
        }
        let target = bundleURL.standardizedFileURL.resolvingSymlinksInPath()
        guard !target.path.hasPrefix("/Applications/") else {
            return L(.aliasSkippedAlreadyInApplications)
        }

        // Try the global /Applications first (writable by the admin
        // group, no root needed); if it cannot be written (a managed
        // machine, a non-admin account) fall back to ~/Applications --
        // Spotlight indexes that one too.
        let candidates = [
            URL(fileURLWithPath: "/Applications", isDirectory: true),
            FileManager.default.homeDirectoryForCurrentUser
                .appendingPathComponent("Applications", isDirectory: true),
        ]
        for directory in candidates {
            do {
                if let message = try ensure(in: directory, target: target) {
                    return message
                }
            } catch {
                continue
            }
        }
        return L(.aliasFailed)
    }

    /// Create or repair the alias in the given directory. Returns nil
    /// when the name is taken by a file that is not an alias (try the next
    /// candidate directory); other failures throw.
    private static func ensure(in directory: URL, target: URL) throws -> String? {
        let aliasURL = directory.appendingPathComponent(aliasName)
        let path = aliasURL.path

        if FileManager.default.fileExists(atPath: path) {
            let isAlias = (try? aliasURL.resourceValues(forKeys: [.isAliasFileKey]))?
                .isAliasFile ?? false
            // Not our alias (the user copied a file with the same
            // name?) -- never touch it.
            guard isAlias else { return nil }

            if let resolved = try? URL(resolvingAliasFileAt: aliasURL,
                                       options: [.withoutUI, .withoutMounting]),
               resolved.standardizedFileURL.path == target.path {
                return L(.aliasAlreadyPresent, path)
            }
            // The target drifted (an upgrade changed the path) --
            // rewrite it.
        }

        let bookmark = try target.bookmarkData(options: .suitableForBookmarkFile,
                                               includingResourceValuesForKeys: nil,
                                               relativeTo: nil)
        try URL.writeBookmarkData(bookmark, to: aliasURL)
        return L(.aliasCreated, path, target.path)
    }
}
