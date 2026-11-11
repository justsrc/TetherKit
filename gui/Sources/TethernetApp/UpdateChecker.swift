import Foundation
import TethernetIPC

/// Ask GitHub Releases whether a newer version exists.
///
/// Only "check and point the way", never download and replace:
///   This project is distributed without a certificate (ad-hoc signing).
///   A self-updater (Sparkle and the like) has to download a new .app and
///   replace itself; the download carries a quarantine attribute and the
///   replaced app is blocked outright by Gatekeeper -- the same wall that
///   rules out a Cask. The real update channel is `brew upgrade` or a
///   rebuild from source; the app only discovers a new version and hands
///   the command to the user.
///
/// Privacy:
///   It only queries GitHub's public REST API (releases/latest) and
///   carries no information about this machine. The automatic check runs
///   at most once a day and is silent on failure;
///   `defaults write com.tethernet.app updateCheckDisabled -bool YES` turns
///   it off completely.
enum UpdateChecker {
    struct Release: Equatable {
        /// The version number with the v prefix removed, for example
        /// "0.2.0".
        let version: String
        /// The release page, where the user reads the release notes or
        /// downloads.
        let pageURL: URL
    }

    enum Failure: LocalizedError {
        case noReleases
        case badStatus(Int)
        case malformedPayload

        var errorDescription: String? {
            switch self {
            case .noReleases:
                return L(.updateNoReleases)
            case .badStatus(let code):
                return L(.updateHTTPStatus, code)
            case .malformedPayload:
                return L(.updateBadResponse)
            }
        }
    }

    /// The current app's version number, from Info.plist (build-gui.sh
    /// injects it from CMakeLists). The bare executable from `swift run`
    /// has no bundle and returns nil -- a development build does not
    /// check.
    static var currentVersion: String? {
        Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String
    }

    /// Fetch the latest release. 404 = never released; every other
    /// non-200 is treated as a failure.
    static func fetchLatestRelease() async throws -> Release {
        var request = URLRequest(url: endpoint)
        request.timeoutInterval = 10
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")

        let (data, response) = try await URLSession.shared.data(for: request)
        guard let http = response as? HTTPURLResponse else { throw Failure.malformedPayload }
        guard http.statusCode != 404 else { throw Failure.noReleases }
        guard http.statusCode == 200 else { throw Failure.badStatus(http.statusCode) }

        let payload = try JSONDecoder().decode(Payload.self, from: data)
        guard let pageURL = URL(string: payload.htmlURL) else { throw Failure.malformedPayload }
        return Release(version: normalize(payload.tagName), pageURL: pageURL)
    }

    /// Whether candidate is newer than current. The segments are compared
    /// numerically, a missing segment counts as 0 and a non-numeric one as
    /// 0 -- an unparseable version is better judged "not new" than worth
    /// an update popup for a malformed tag.
    static func isNewer(_ candidate: String, than current: String) -> Bool {
        let lhs = components(of: candidate)
        let rhs = components(of: current)
        for index in 0..<max(lhs.count, rhs.count) {
            let l = index < lhs.count ? lhs[index] : 0
            let r = index < rhs.count ? rhs[index] : 0
            if l != r { return l > r }
        }
        return false
    }

    // MARK: - Implementation

    private static let endpoint =
        URL(string: "https://api.github.com/repos/justsrc/Tethernet/releases/latest")!

    private struct Payload: Decodable {
        let tagName: String
        let htmlURL: String

        enum CodingKeys: String, CodingKey {
            case tagName = "tag_name"
            case htmlURL = "html_url"
        }
    }

    /// Remove the v/V prefix that tags commonly carry.
    private static func normalize(_ tag: String) -> String {
        var tag = tag
        if tag.hasPrefix("v") || tag.hasPrefix("V") { tag.removeFirst() }
        return tag
    }

    private static func components(of version: String) -> [Int] {
        normalize(version).split(separator: ".").map { Int($0) ?? 0 }
    }
}
