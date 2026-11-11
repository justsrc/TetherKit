import SwiftUI
import TethernetIPC

/// The design language of the UI.
///
/// Spacing, corner radii, colors and a few reusable containers are defined
/// in one place for a single purpose: **make every visual decision
/// exactly once**. Magic numbers scattered across the views slowly drift
/// apart as new panels are added, and no automated means can detect that
/// kind of drift.
enum Design {
    // MARK: - Sizes

    /// The spacing ladder. It progresses in multiples of 4 -- the same
    /// origin as the internal padding of system controls, so mixed layouts
    /// do not misalign.
    enum Spacing {
        static let tight: CGFloat = 6
        static let small: CGFloat = 10
        /// Gutter between cards. Spacing carries no information, so it
        /// is the first knob to turn when a page must gain room.
        static let gutter: CGFloat = 12
        static let medium: CGFloat = 16
        static let large: CGFloat = 24
        /// The card padding.
        static let section: CGFloat = 16
    }

    enum Radius {
        static let card: CGFloat = 14
        static let control: CGFloat = 8
    }

    /// Fixed window size.
    ///
    /// Settings-style, non-resizable window at a compact footprint.
    /// User resizing buys nothing: the Connection page fits one screen,
    /// and the Network page scrolls only with the static form expanded.
    enum Window {
        static let width: CGFloat = 800
        static let height: CGFloat = 560
    }

    // MARK: - State colors

    /// The accent color for a session state. The whole UI is driven by it
    /// -- a glance at the color tells the user what is going on, without
    /// reading any text.
    static func accent(for state: RunState) -> Color {
        switch state {
        case .idle, .stopped: return .secondary
        case .starting, .stopping: return .orange
        case .running: return .green
        case .failed: return .red
        }
    }

    static func statusLabel(for status: SessionStatus) -> String {
        switch status.runState {
        case .idle: return L(.statusDisconnected)
        case .starting: return L(.statusConnecting)
        case .running: return L(status.linkUp ? .statusConnected : .statusReadyLinkDown)
        case .stopping: return L(.statusDisconnecting)
        case .stopped: return L(.statusStopped)
        case .failed: return L(.statusFailed)
        }
    }

    /// The SF Symbol inside the status ring.
    static func statusSymbol(for status: SessionStatus) -> String {
        switch status.runState {
        case .idle, .stopped: return "bolt.horizontal"
        case .starting, .stopping: return "arrow.triangle.2.circlepath"
        case .running: return status.linkUp ? "bolt.horizontal.fill" : "bolt.horizontal"
        case .failed: return "exclamationmark.triangle.fill"
        }
    }

    static func logColor(for level: LogLevel) -> Color {
        switch level {
        case .trace, .debug: return .secondary
        case .info: return .primary
        case .warning: return .orange
        case .error: return .red
        }
    }
}

// MARK: - Reusable containers

/// The card container. Every content panel wraps itself in one, so the
/// corner radius, the padding and the border stay consistent.
struct Card<Content: View>: View {
    var title: String?
    var systemImage: String?
    /// The accessory view to the right of the title (a refresh button, a
    /// status badge, and the like).
    var accessory: AnyView?
    @ViewBuilder var content: () -> Content

    init(title: String? = nil,
         systemImage: String? = nil,
         accessory: AnyView? = nil,
         @ViewBuilder content: @escaping () -> Content) {
        self.title = title
        self.systemImage = systemImage
        self.accessory = accessory
        self.content = content
    }

    var body: some View {
        VStack(alignment: .leading, spacing: Design.Spacing.medium) {
            if let title {
                HStack(spacing: Design.Spacing.tight) {
                    if let systemImage {
                        Image(systemName: systemImage)
                            .foregroundStyle(.secondary)
                    }
                    Text(title)
                        .font(.headline)
                    Spacer(minLength: Design.Spacing.small)
                    accessory
                }
            }
            content()
        }
        .padding(Design.Spacing.section)
        .frame(maxWidth: .infinity, alignment: .leading)
        // Use material rather than a flat color: it follows the
        // light/dark appearance and the desktop background, which is the
        // least-effort and least error-prone way to look "modern" on
        // macOS.
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: Design.Radius.card))
        .overlay(
            RoundedRectangle(cornerRadius: Design.Radius.card)
                .strokeBorder(.separator.opacity(0.6), lineWidth: 0.5))
    }
}

/// One metric tile: the caption on top, the value below.
struct MetricTile: View {
    let caption: String
    let value: String
    var systemImage: String?
    var tint: Color = .primary

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 4) {
                if let systemImage {
                    Image(systemName: systemImage)
                        .font(.caption2)
                }
                Text(caption)
                    .font(.caption)
            }
            .foregroundStyle(.secondary)

            Text(value)
                // Monospaced digits: the characters do not jitter
                // sideways when the value changes.
                .font(.system(.title3, design: .rounded).weight(.medium))
                .monospacedDigit()
                .foregroundStyle(tint)
                .lineLimit(1)
                .minimumScaleFactor(0.7)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

/// A small dot badge, for binary states such as "link is up / link is
/// down".
struct StatusBadge: View {
    let text: String
    let color: Color

    var body: some View {
        HStack(spacing: 5) {
            Circle()
                .fill(color)
                .frame(width: 7, height: 7)
            Text(text)
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .padding(.horizontal, Design.Spacing.small)
        .padding(.vertical, 4)
        .background(color.opacity(0.12), in: Capsule())
    }
}

// MARK: - Formatting

/// Every number the UI shows is formatted here, so the same quantity
/// never looks different in two panels.
enum Format {
    /// A bit rate. The unit is picked automatically among bit/s, Kbps, Mbps
    /// and Gbps.
    ///
    /// Bits rather than bytes: the industry convention for network
    /// throughput is bits, which matches what the user sees on a router or
    /// at an ISP.
    static func bitrate(_ bitsPerSecond: Double) -> String {
        let units: [(threshold: Double, suffix: String, divisor: Double)] = [
            (1_000_000_000, "Gbps", 1_000_000_000),
            (1_000_000, "Mbps", 1_000_000),
            (1_000, "Kbps", 1_000),
        ]
        for unit in units where bitsPerSecond >= unit.threshold {
            return String(format: "%.1f %@", bitsPerSecond / unit.divisor, unit.suffix)
        }
        return String(format: "%.0f bps", max(0, bitsPerSecond))
    }

    /// A cumulative byte count.
    static func bytes(_ value: UInt64) -> String {
        // ByteCountFormatter renders 0 as "Zero KB" (its localized
        // behavior), which stands out badly in a row of numbers, so 0 is
        // handled separately.
        guard value > 0 else { return "0 B" }
        let formatter = ByteCountFormatter()
        formatter.countStyle = .binary
        return formatter.string(fromByteCount: Int64(clamping: value))
    }

    /// Large integers get thousands separators, so "12,345,678 frames" is
    /// easy to read.
    static func count(_ value: UInt64) -> String {
        let formatter = NumberFormatter()
        formatter.numberStyle = .decimal
        return formatter.string(from: NSNumber(value: value)) ?? "\(value)"
    }

    /// The compact bit rate for the menu bar (for example "12.3M", "999K").
    ///
    /// The menu bar has very little room: the unit is squeezed to a single
    /// letter, and anything below 1 Kbps always shows as "0K" -- in the
    /// "glance at the menu bar" context that much traffic simply does not
    /// exist.
    static func compactBitrate(_ bitsPerSecond: Double) -> String {
        let value = max(0, bitsPerSecond)
        if value >= 1_000_000_000 { return String(format: "%.1fG", value / 1_000_000_000) }
        if value >= 1_000_000 { return String(format: "%.1fM", value / 1_000_000) }
        if value >= 1_000 { return String(format: "%.0fK", value / 1_000) }
        return "0K"
    }

    static func packetsPerSecond(_ value: Double) -> String {
        value >= 10_000
            ? String(format: "%.1f k", value / 1000)
            : String(format: "%.0f", max(0, value))
    }

    private static let timeFormatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss.SSS"
        return formatter
    }()

    static func time(_ date: Date) -> String {
        timeFormatter.string(from: date)
    }

    /// Render a number of seconds as "1:02:03" / "2:03".
    static func duration(_ seconds: TimeInterval) -> String {
        let total = Int(max(0, seconds))
        let hours = total / 3600
        let minutes = (total % 3600) / 60
        let secs = total % 60
        return hours > 0
            ? String(format: "%d:%02d:%02d", hours, minutes, secs)
            : String(format: "%d:%02d", minutes, secs)
    }
}
