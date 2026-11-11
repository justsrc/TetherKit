import SwiftUI
import TethernetIPC

/// Main window root view: gates on helper availability, then shows
/// the sidebar shell once the helper is ready.
///
/// `SidebarShell` is the skeleton; this file keeps two jobs:
///
///   1. Gate on helper availability: probing / missing / outdated /
///      ready. The first three are the screens new users hit first,
///      so they must take over the entire window instead of appearing
///      inside one sidebar page (nothing is tappable there anyway,
///      and the sidebar would just be noise).
///   2. Window-level alerts: operation failures and update-check
///      results. They belong to the window, whatever page is showing.
struct ContentView: View {
    var model: AppModel

    var body: some View {
        GeometryReader { proxy in
            switch model.helperAvailability {
            case .unknown:
                gatingPage(ProbingCard(), proxy)
            case .missing(let reason):
                gatingPage(HelperMissingCard(model: model, reason: reason), proxy)
            case .outdated(let installed, let expected):
                gatingPage(HelperOutdatedCard(model: model,
                                              installed: installed,
                                              expected: expected), proxy)
            case .available:
                SidebarShell(model: model)
            }
        }
        .background(backgroundGradient)
        .animation(.smooth(duration: 0.25), value: model.status.runState)
        .animation(.smooth(duration: 0.25), value: model.helperAvailability)
        .alert(L(.alertOperationFailed),
               isPresented: Binding(get: { model.alertMessage != nil },
                                    set: { if !$0 { model.alertMessage = nil } })) {
            Button(L(.ok)) { model.alertMessage = nil }
        } message: {
            Text(model.alertMessage ?? "")
        }
        .alert(L(.updateCheckTitle),
               isPresented: Binding(get: { model.updateCheckResult != nil },
                                    set: { if !$0 { model.updateCheckResult = nil } })) {
            if case .updateAvailable(let release) = model.updateCheckResult {
                Button(L(.openReleasePage)) { NSWorkspace.shared.open(release.pageURL) }
                Button(L(.copyBrewUpgradeCommand)) {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString("brew upgrade tethernet", forType: .string)
                }
                Button(L(.ok), role: .cancel) {}
            } else {
                Button(L(.ok), role: .cancel) {}
            }
        } message: {
            Text(updateCheckDescription)
        }
    }

    /// Full-page container for the probing / missing / outdated
    /// helper states.
    ///
    /// All three states show a single card at the top of the window,
    /// on the shared background. `minHeight` fills the visible area
    /// and `basedOnSize` suppresses scrolling and bounce while the
    /// content fits.
    private func gatingPage(_ card: some View, _ proxy: GeometryProxy) -> some View {
        ScrollView {
            card
                .padding(Design.Spacing.large)
                .frame(minHeight: proxy.size.height, alignment: .top)
        }
        .scrollBounceBehavior(.basedOnSize)
    }

    /// The body of the "Check for Updates..." popup.
    ///
    /// The brew command name matches the GUI formula in the tap
    /// (tethernet) -- when the CLI was renamed to tethernet-cli this name
    /// went to the GUI; users who build by hand follow the second
    /// sentence.
    private var updateCheckDescription: String {
        switch model.updateCheckResult {
        case .upToDate(let current):
            return L(.updateUpToDate, current)
        case .updateAvailable(let release):
            return L(.updateAvailable, release.version)
        case .failed(let reason):
            return L(.updateCheckFailed, reason)
        case .unavailable:
            return L(.updateDevBuild)
        case nil:
            return ""
        }
    }

    /// A very faint gradient behind everything, tinted by session
    /// state.
    ///
    /// Kept deliberately faint (0.10 / 0.04): its job is to make
    /// "connected" perceptible in peripheral vision, not to compete
    /// with content. Sidebar pages carry their own background
    /// material; the gradient remains visible only on the
    /// helper-not-ready full pages and as fallback behind the sidebar.
    private var backgroundGradient: some View {
        let accent = Design.accent(for: model.status.runState)
        return LinearGradient(
            colors: [accent.opacity(0.10), accent.opacity(0.04), .clear],
            startPoint: .top,
            endPoint: .bottom)
        .ignoresSafeArea()
    }
}

/// The placeholder shown while the helper is being probed.
private struct ProbingCard: View {
    var body: some View {
        Card {
            HStack(spacing: Design.Spacing.small) {
                ProgressView()
                    .controlSize(.small)
                Text(L(.checkingHelper))
                    .foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, alignment: .center)
            .padding(.vertical, Design.Spacing.large)
        }
    }
}

/// The guide shown when the helper is not installed.
///
/// This is the screen a new user is most likely to hit, so after
/// explaining "why it is needed" it goes straight to an "Install" button
/// -- one system authorization dialog and the helper is in. The terminal
/// route is demoted to a fallback inside a disclosure group.
private struct HelperMissingCard: View {
    var model: AppModel
    let reason: String

    var body: some View {
        Card(title: L(.needInstallTitle), systemImage: "lock.shield") {
            VStack(alignment: .leading, spacing: Design.Spacing.medium) {
                Text(L(.needInstallBody))
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                HelperInstallSection(
                    model: model,
                    buttonTitle: L(.installHelperButton),
                    detail: L(.installHelperDetail))

                DisclosureGroup(L(.showConnectFailureDetail)) {
                    Text(reason)
                        .font(.system(.caption, design: .monospaced))
                        .textSelection(.enabled)
                        .foregroundStyle(.secondary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(.top, Design.Spacing.tight)
                }
                .font(.caption)
            }
        }
    }
}

/// The installed helper's XPC interface does not match the current app.
///
/// A screen of its own rather than reusing "not installed": the user has
/// to do different things in the two cases, and unless it is spelled out
/// the symptom shows up as calls hanging or crashing outright -- the
/// hardest kind of problem to diagnose on one's own.
private struct HelperOutdatedCard: View {
    var model: AppModel
    let installed: Int
    let expected: Int

    var body: some View {
        Card(title: L(.helperNeedsUpdateTitle), systemImage: "arrow.triangle.2.circlepath") {
            VStack(alignment: .leading, spacing: Design.Spacing.medium) {
                Text(L(.helperNeedsUpdateBody, String(installed), String(expected)))
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                HelperInstallSection(
                    model: model,
                    buttonTitle: L(.updateHelperButton),
                    detail: L(.updateHelperDetail))
            }
        }
    }
}

/// The "one-click install" button plus the terminal fallback. Shared by
/// the missing and the outdated cards.
///
/// The terminal route has to be kept rather than dropped entirely: the
/// bare executable from `swift run` has no embedded payload, and the
/// button reports that clearly and points at the script; and there are
/// always people who simply will not type their password into a dialog
/// the app pops up.
private struct HelperInstallSection: View {
    var model: AppModel
    let buttonTitle: String
    let detail: String

    var body: some View {
        VStack(alignment: .leading, spacing: Design.Spacing.small) {
            HStack(spacing: Design.Spacing.small) {
                Button {
                    Task { await model.installHelper() }
                } label: {
                    HStack(spacing: Design.Spacing.tight) {
                        if model.isBusy {
                            ProgressView().controlSize(.small)
                        } else {
                            Image(systemName: "arrow.down.circle.fill")
                        }
                        Text(model.isBusy ? L(.installingProgress) : buttonTitle)
                    }
                    .frame(minWidth: 132)
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.large)
                .disabled(model.isBusy)

                Text(L(.authorizationPromptHint))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Text(detail)
                .font(.caption)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            DisclosureGroup(L(.installViaTerminal)) {
                VStack(alignment: .leading, spacing: Design.Spacing.tight) {
                    CopyableCommand(command: "sudo ./gui/Scripts/install-helper.sh")
                    Text(L(.installViaTerminalHint))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                .padding(.top, Design.Spacing.tight)
            }
            .font(.caption)
        }
    }
}

/// A command that can be copied.
struct CopyableCommand: View {
    let command: String
    @State private var copied = false

    var body: some View {
        HStack(spacing: Design.Spacing.small) {
            Text(command)
                .font(.system(.callout, design: .monospaced))
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)

            Button {
                NSPasteboard.general.clearContents()
                NSPasteboard.general.setString(command, forType: .string)
                copied = true
                // Restore after 2 seconds. It gives the "copied"
                // confirmation, not a state that needs the user to act on
                // it.
                Task {
                    try? await Task.sleep(for: .seconds(2))
                    copied = false
                }
            } label: {
                Label(L(copied ? .copied : .copy), systemImage: copied ? "checkmark" : "doc.on.doc")
                    .labelStyle(.iconOnly)
            }
            .buttonStyle(.borderless)
            .help(L(copied ? .copiedToClipboard : .copyCommand))
        }
        .padding(Design.Spacing.small)
        .background(.quaternary.opacity(0.5), in: RoundedRectangle(cornerRadius: Design.Radius.control))
    }
}
