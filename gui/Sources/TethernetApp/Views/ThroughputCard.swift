import Charts
import SwiftUI
import TethernetIPC

/// Throughput and counters.
///
/// The chart deliberately plots the **rate** and not the cumulative
/// total: a cumulative curve only ever climbs and shows nothing; the
/// rate makes stutter, jitter and throttling visible at a glance. The
/// cumulative totals sit in the metric tiles below, readable when
/// needed.
///
/// The card carries no title row: it is the body of the "Throughput"
/// page and the page header already says it (with the LIVE badge).
struct ThroughputCard: View {
    var model: AppModel

    private var hasData: Bool { !model.throughputHistory.isEmpty }

    var body: some View {
        Card {
            VStack(alignment: .leading, spacing: Design.Spacing.medium) {
                rateRow
                chart
                Divider()
                counterRow
                if hasDrops {
                    dropHint
                }
            }
        }
    }

    // 2x2 rather than one row: down and up keep their columns aligned
    // across the rate row and the pps row.
    private var rateRow: some View {
        Grid(alignment: .leading,
             horizontalSpacing: Design.Spacing.medium,
             verticalSpacing: Design.Spacing.small) {
            GridRow {
                MetricTile(caption: L(.throughputDownstream),
                           value: Format.bitrate(model.throughput.receiveBitsPerSecond),
                           systemImage: "arrow.down",
                           tint: .blue)
                MetricTile(caption: L(.throughputUpstream),
                           value: Format.bitrate(model.throughput.transmitBitsPerSecond),
                           systemImage: "arrow.up",
                           tint: .purple)
            }
            GridRow {
                MetricTile(caption: L(.throughputDownstreamFPS),
                           value: "\(Format.packetsPerSecond(model.throughput.receivePacketsPerSecond)) pps",
                           systemImage: "square.stack.3d.up")
                MetricTile(caption: L(.throughputUpstreamFPS),
                           value: "\(Format.packetsPerSecond(model.throughput.transmitPacketsPerSecond)) pps",
                           systemImage: "square.stack.3d.up")
            }
        }
    }

    @ViewBuilder
    private var chart: some View {
        if hasData {
            Chart {
                ForEach(model.throughputHistory) { sample in
                    AreaMark(x: .value(L(.chartTime), sample.timestamp),
                             y: .value(L(.chartRate), sample.receiveBitsPerSecond))
                        .foregroundStyle(
                            .linearGradient(colors: [.blue.opacity(0.35), .blue.opacity(0.02)],
                                            startPoint: .top, endPoint: .bottom))
                        .interpolationMethod(.monotone)

                    LineMark(x: .value(L(.chartTime), sample.timestamp),
                             y: .value(L(.chartRate), sample.receiveBitsPerSecond),
                             series: .value(L(.chartDirection), L(.downstreamShort)))
                        .foregroundStyle(.blue)
                        .interpolationMethod(.monotone)

                    LineMark(x: .value(L(.chartTime), sample.timestamp),
                             y: .value(L(.chartRate), sample.transmitBitsPerSecond),
                             series: .value(L(.chartDirection), L(.upstreamShort)))
                        .foregroundStyle(.purple)
                        .interpolationMethod(.monotone)
                }
            }
            .chartYAxis {
                AxisMarks(position: .leading) { value in
                    AxisGridLine()
                    AxisValueLabel {
                        if let bits = value.as(Double.self) {
                            Text(Format.bitrate(bits)).font(.caption2)
                        }
                    }
                }
            }
            // The x axis carries no ticks: this is a rolling curve over
            // "the last 60 seconds", the exact moments mean nothing, and
            // drawing them would only eat into an already short plot
            // area.
            .chartXAxis(.hidden)
            .frame(height: 120)
        } else {
            RoundedRectangle(cornerRadius: Design.Radius.control)
                .fill(.quaternary.opacity(0.35))
                .frame(height: 120)
                .overlay(
                    Text(L(model.status.runState == .running ? .throughputCollecting : .throughputPlaceholder))
                        .font(.callout)
                        .foregroundStyle(.secondary))
        }
    }

    private var counterRow: some View {
        HStack(spacing: Design.Spacing.medium) {
            MetricTile(caption: L(.totalDownstream), value: Format.bytes(model.status.rxBytes))
            MetricTile(caption: L(.totalUpstream), value: Format.bytes(model.status.txBytes))
            MetricTile(caption: L(.downstreamFrames), value: Format.count(model.status.rxFrames))
            MetricTile(caption: L(.upstreamFrames), value: Format.count(model.status.txFrames))
        }
    }

    private var hasDrops: Bool {
        model.status.rxDropped > 0 || model.status.txDropped > 0
            || model.status.linkKernelDrops > 0 || model.status.txBackpressure > 0
    }

    /// The drop hint.
    ///
    /// Four numbers are listed separately instead of being merged into one
    /// "drop rate": they point at completely different bottlenecks -- a
    /// full queue means the downstream side cannot keep up, kernel drops
    /// mean we are not reading fast enough, and backpressure means the
    /// USB side cannot send. Once they are merged, there is no way to
    /// tell which parameter to turn.
    private var dropHint: some View {
        HStack(spacing: Design.Spacing.medium) {
            MetricTile(caption: L(.downstreamDropped), value: Format.count(model.status.rxDropped), tint: .orange)
            MetricTile(caption: L(.upstreamDropped), value: Format.count(model.status.txDropped), tint: .orange)
            MetricTile(caption: L(.kernelDrops), value: Format.count(model.status.linkKernelDrops), tint: .orange)
            MetricTile(caption: L(.transmitBackpressure), value: Format.count(model.status.txBackpressure), tint: .orange)
        }
    }
}
