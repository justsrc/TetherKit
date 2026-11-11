import AppKit
import SwiftUI
import TethernetIPC

/// The permanent menu bar label: a status icon plus live rates.
///
/// The rate text appears only while a session is running -- when idle a
/// string of "0K" is just noise, and the icon itself already says what
/// the state is. The digits are monospaced, so a changing rate does not
/// shove the other menu bar items to the right around.
struct MenuBarLabel: View {
    var model: AppModel

    var body: some View {
        HStack(spacing: 2) {
            Image(systemName: Design.statusSymbol(for: model.status))
            if model.status.runState == .running {
                Text(speedText)
                    .font(.system(size: 11, weight: .medium).monospacedDigit())
            }
        }
    }

    private var speedText: String {
        "↓\(Format.compactBitrate(model.throughput.receiveBitsPerSecond))"
            + " ↑\(Format.compactBitrate(model.throughput.transmitBitsPerSecond))"
    }
}

/// The small panel that opens when the menu bar item is clicked.
///
/// The content is chosen by "what do you need to know at a glance": the
/// state, the rates, the interface and the address, plus two actions
/// (open the main window, quit). Configuration, charts and logs stay in
/// the main window -- the panel is not a second interface.
struct MenuBarPanel: View {
    var model: AppModel
    @Environment(\.dismiss) private var dismiss

    private var status: SessionStatus { model.status }

    var body: some View {
        VStack(alignment: .leading, spacing: Design.Spacing.small) {
            header

            switch status.runState {
            case .running:
                runningDetails
            case .failed:
                if !status.fatalMessage.isEmpty {
                    Text(status.fatalMessage)
                        .font(.caption)
                        .foregroundStyle(.red)
                        .lineLimit(3)
                        .fixedSize(horizontal: false, vertical: true)
                }
            default:
                idleHint
            }

            Divider()
            actions
        }
        .padding(Design.Spacing.medium)
        .frame(width: 280)
        // Same reasoning as the main window: the strings come from a
        // global table lookup, so changing the language makes no
        // @Observable property "look" changed, and an explicit identity is
        // needed to force a rebuild. The panel itself refreshes with the
        // polling, so without this line the language would catch up on
        // the next cycle anyway -- but when the user changes the language
        // from inside the panel, the panel is exactly what they are
        // staring at, and half a beat of delay reads as "it did not take
        // effect".
        .id(model.languageRevision)
    }

    private var header: some View {
        HStack(spacing: Design.Spacing.tight) {
            Circle()
                .fill(Design.accent(for: status.runState))
                .frame(width: 8, height: 8)
            Text(Design.statusLabel(for: status))
                .font(.headline)
            Spacer()
            if let duration = model.connectedDuration {
                Text(Format.duration(duration))
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.secondary)
            }
        }
    }

    @ViewBuilder
    private var runningDetails: some View {
        speedRow(symbol: "arrow.down", caption: L(.downstreamShort),
                 bitsPerSecond: model.throughput.receiveBitsPerSecond, tint: .blue)
        speedRow(symbol: "arrow.up", caption: L(.upstreamShort),
                 bitsPerSecond: model.throughput.transmitBitsPerSecond, tint: .purple)

        if !status.systemInterface.isEmpty {
            Text("\(status.systemInterface) · "
                 + (model.networkState.hasAddress ? model.networkState.address
                                                  : L(.noIPConfigured)))
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(.secondary)
        }
    }

    private var idleHint: some View {
        Text(model.devices.isEmpty
             ? L(.menuBarNoDevice)
             : L(.menuBarReady, model.devices.first?.displayName ?? ""))
            .font(.caption)
            .foregroundStyle(.secondary)
            .lineLimit(2)
            .fixedSize(horizontal: false, vertical: true)
    }

    private var actions: some View {
        VStack(alignment: .leading, spacing: Design.Spacing.tight) {
            HStack {
                Button(L(.openMainWindow)) {
                    dismiss()
                    MainWindowManager.shared.present(model)
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.small)

                Spacer()

                // In menu-bar-only mode this panel is the only entry
                // point, so the language switch has to be reachable here
                // as well -- otherwise a user who cannot read the
                // interface has to know that they should "go to the app
                // menu" before they can change the language, and in this
                // mode the app menu is only visible once the window is
                // opened.
                languageMenu

                Button(L(.quit)) {
                    NSApp.terminate(nil)
                }
                .controlSize(.small)
            }

            if status.runState == .running {
                // The session belongs to the helper and quitting the app
                // does not affect it -- this has to be said out loud,
                // otherwise users think "quitting = losing the network"
                // and hesitate to click.
                Text(L(.quitTooltip))
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    /// The language toggle. Options show native names, not translations --
    /// see LanguageMenu for why.
    private var languageMenu: some View {
        Menu {
            Picker(L(.languageLabel), selection: Bindable(model).languagePreference) {
                Text(L(.languageSystem)).tag(LanguagePreference.system)
                Text(verbatim: "English").tag(LanguagePreference.english)
            }
            .pickerStyle(.inline)
            .labelsHidden()
        } label: {
            Image(systemName: "globe")
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.hidden)
        .fixedSize()
        .help(L(.languageLabel))
    }

    private func speedRow(symbol: String, caption: String, bitsPerSecond: Double,
                          tint: Color) -> some View {
        HStack(spacing: Design.Spacing.small) {
            Image(systemName: symbol)
                .font(.caption)
                .foregroundStyle(tint)
                .frame(width: 14)
            Text(caption)
                .font(.callout)
                .foregroundStyle(.secondary)
            Spacer()
            Text(Format.bitrate(bitsPerSecond))
                .font(.callout.monospacedDigit())
                .foregroundStyle(tint)
        }
    }
}
