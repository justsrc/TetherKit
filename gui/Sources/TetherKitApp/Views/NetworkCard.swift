import SwiftUI
import TetherKitCore
import TetherKitIPC
import TetherKitUIModel

/// Network access mode configuration: DHCP or a static IP.
///
/// Why the "currently effective" row exists on its own:
///   It shows the state **read back from the system**, not the value we
///   pushed. The two can differ -- the most typical case is DNS in static
///   mode: IPConfiguration only publishes DNS in DHCP mode, and in static
///   mode all we can do is add a key on a best-effort basis; whether the
///   system adopts it depends on IPMonitor. Rather than promise the user
///   a result that may not hold, show the real state.
///
/// This card lives on the "Network" page with the full content width
/// to itself. The two-column grid works on a full-width page: a single
/// column would stretch the form sparse and tall (IPv4 values are
/// short), and long explanations stay in tooltips.
struct NetworkCard: View {
    @Bindable var model: AppModel

    /// The whole block is disabled while the interface does not exist yet
    /// -- with no interface to configure, making the user fill in the form
    /// and only then report an error is the worst possible order.
    private var interfaceReady: Bool { !model.status.systemInterface.isEmpty }

    private var canAddDNS: Bool {
        // The cap matches TK_DNS_MAX in the C ABI; extra entries would
        // be dropped, so it is better not to allow adding them at all.
        model.networkConfiguration.dnsServers.count < 4
    }

    /// The tooltip for static DNS. It is "when do I need to care" level
    /// information and does not deserve a permanent row.
    /// A computed property rather than `static let`: the latter is
    /// evaluated only once and stops updating after a language switch.
    private static var dnsHint: String { L(.dnsEffectivenessTooltip) }

    var body: some View {
        Card(title: L(.ipModeLabel), systemImage: "network", accessory: AnyView(interfaceBadge)) {
            VStack(alignment: .leading, spacing: Design.Spacing.small) {
                // The "connect first" hint sits inline above the form
                // instead of floating over it; at full width the old
                // centered capsule covered the DHCP explanation.
                if !interfaceReady {
                    Label(L(.connectBeforeConfiguring), systemImage: "cable.connector")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }

                Group {
                    modePicker

                    switch model.networkConfiguration.mode {
                    case .dhcp:
                        dhcpExplanation
                    case .manual:
                        manualForm
                    case .none:
                        EmptyView()
                    }

                    defaultRouteToggle
                    actionRow

                    if interfaceReady {
                        Divider()
                        EffectiveStateView(state: model.networkState,
                                           interface: model.status.systemInterface)
                    }
                }
                .disabled(!interfaceReady)
                .opacity(interfaceReady ? 1 : 0.55)
            }
        }
    }

    @ViewBuilder
    private var interfaceBadge: some View {
        if interfaceReady {
            StatusBadge(text: model.status.systemInterface, color: .accentColor)
        }
    }

    private var modeBinding: Binding<IPMode> {
        ButtonTurn.routed(
            get: { model.networkConfiguration.mode },
            set: { model.networkConfiguration.mode = $0 }
        )
    }

    private var modePicker: some View {
        Picker(L(.ipModeLabel), selection: modeBinding) {
            // Deliberately keep "Not configured" out of the picker: it
            // is an action (revert), not a network mode. Mixed in, it
            // would look like selecting it has already taken effect.
            Text(IPMode.dhcp.displayName).tag(IPMode.dhcp)
            Text(IPMode.manual.displayName).tag(IPMode.manual)
        }
        // Segmented pickers are keyboard-inert under Keyboard
        // Navigation and the Accessibility pane's FKA toggle; `.menu`
        // is the keyboard-operable replacement (AGENTS.md pitfall 24,
        // round 8). Same style choice as LogCard's level filter.
        .pickerStyle(.menu)
        .labelsHidden()
    }

    private var dhcpExplanation: some View {
        VStack(alignment: .leading, spacing: Design.Spacing.tight) {
            Label(L(.dhcpHelp), systemImage: "wand.and.stars")
                .font(.callout)
            Text(L(.dhcpTooltip))
                .font(.caption)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private var manualForm: some View {
        Grid(alignment: .leading,
             horizontalSpacing: Design.Spacing.medium,
             verticalSpacing: Design.Spacing.small) {
            GridRow {
                AddressField(label: L(.ipAddress),
                             placeholder: "192.168.42.100",
                             text: $model.networkConfiguration.address,
                             isValid: NetworkValidator.isValidIPv4(model.networkConfiguration.address))
                AddressField(label: L(.netmask),
                             placeholder: "255.255.255.0",
                             text: $model.networkConfiguration.netmask,
                             isValid: NetworkValidator.isValidNetmask(model.networkConfiguration.netmask))
            }
            GridRow {
                AddressField(label: L(.router),
                             placeholder: L(.routerOptional),
                             text: $model.networkConfiguration.router,
                             isValid: model.networkConfiguration.router.isEmpty
                                 || NetworkValidator.isValidIPv4(model.networkConfiguration.router))
                if model.networkConfiguration.dnsServers.isEmpty {
                    emptyDNSCell
                } else {
                    dnsField(at: 0)
                }
            }
            // From the 2nd entry on, each DNS row occupies the right
            // cell of a row and the left cell stays empty -- so they line
            // up with the DNS field above.
            ForEach(Array(model.networkConfiguration.dnsServers.indices.dropFirst()),
                    id: \.self) { index in
                GridRow {
                    Color.clear.gridCellUnsizedAxes([.horizontal, .vertical])
                    dnsField(at: index)
                }
            }
        }
    }

    /// One DNS entry: the input field plus a delete button, and the last
    /// entry also carries an "add" button.
    private func dnsField(at index: Int) -> some View {
        HStack(spacing: Design.Spacing.tight) {
            AddressField(label: index == 0 ? "DNS" : "",
                         placeholder: "223.5.5.5",
                         text: $model.networkConfiguration.dnsServers[index],
                         isValid: model.networkConfiguration.dnsServers[index].isEmpty
                             || NetworkValidator.isValidIPv4(model.networkConfiguration.dnsServers[index]))

            Button {
                model.networkConfiguration.dnsServers.remove(at: index)
            } label: {
                Image(systemName: "minus.circle")
            }
            .buttonStyle(.borderless)
            .help(L(.deleteThisEntry))

            if index == model.networkConfiguration.dnsServers.count - 1, canAddDNS {
                Button {
                    model.networkConfiguration.dnsServers.append("")
                } label: {
                    Image(systemName: "plus.circle")
                }
                .buttonStyle(.borderless)
                .help(L(.addDNSServer))
            }
        }
        .help(Self.dnsHint)
    }

    /// The "add" that occupies the cell when there is no DNS entry at
    /// all.
    private var emptyDNSCell: some View {
        HStack(spacing: Design.Spacing.tight) {
            Text("DNS")
                .font(.callout)
                .foregroundStyle(.secondary)
                .frame(width: AddressField.labelWidth, alignment: .leading)
            Button {
                model.networkConfiguration.dnsServers.append("")
            } label: {
                Label(L(.add), systemImage: "plus.circle")
            }
            .buttonStyle(.borderless)
            .font(.callout)
            Spacer(minLength: 0)
        }
        .help(Self.dnsHint)
    }

    private var defaultRouteToggle: some View {
        Toggle(isOn: $model.networkConfiguration.setDefaultRoute) {
            VStack(alignment: .leading, spacing: 1) {
                Text(L(.setDefaultRoute))
                Text(L(.setDefaultRouteHelp))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .help(L(.setDefaultRouteTooltip))
    }

    private var actionRow: some View {
        HStack(spacing: Design.Spacing.small) {
            Button {
                Task { await model.applyNetworkConfiguration() }
            } label: {
                HStack(spacing: Design.Spacing.tight) {
                    if model.isBusy {
                        ProgressView().controlSize(.small)
                    }
                    Text(L(.apply))
                }
                .frame(minWidth: 56)
            }
            .buttonStyle(.borderedProminent)
            .disabled(model.isBusy)

            Button(L(.clearConfiguration)) {
                Task { await model.clearNetworkConfiguration() }
            }
            .disabled(model.isBusy || !model.networkState.hasAddress)
            .help(L(.clearConfigurationTooltip))

            Spacer()
        }
    }
}

/// A compact address input field with immediate validation feedback: the
/// label on the left, the validation icon overlaid on the inside edge of
/// the field (in a two-column layout every pixel of width is precious).
///
/// Validation happens while typing rather than on submit: a mistyped
/// address is the most common mistake, and reporting it only after
/// pressing "Apply" makes the user guess back and forth which field is
/// wrong.
private struct AddressField: View {
    /// The label column width. The readback area uses it too, so the
    /// vertical alignment line of the two blocks is the same one.
    static let labelWidth: CGFloat = 56

    let label: String
    let placeholder: String
    @Binding var text: String
    let isValid: Bool

    var body: some View {
        HStack(spacing: Design.Spacing.tight) {
            Text(label)
                .font(.callout)
                .foregroundStyle(.secondary)
                .frame(width: Self.labelWidth, alignment: .leading)

            TextField(placeholder, text: $text)
                .textFieldStyle(.roundedBorder)
                .font(.system(.callout, design: .monospaced))
                .overlay(alignment: .trailing) {
                    // An empty input is not marked red: an unfinished
                    // entry is not a mistake yet.
                    if !text.isEmpty {
                        Image(systemName: isValid ? "checkmark.circle.fill"
                                                  : "exclamationmark.circle.fill")
                            .foregroundStyle(isValid ? Color.green : Color.orange)
                            .font(.caption)
                            .padding(.trailing, 4)
                            .allowsHitTesting(false)
                    }
                }
        }
    }
}

/// The real effective state, read back from the system.
private struct EffectiveStateView: View {
    let state: NetworkState
    let interface: String

    var body: some View {
        VStack(alignment: .leading, spacing: Design.Spacing.tight) {
            HStack(spacing: Design.Spacing.small) {
                Text(L(.currentlyEffective))
                    .font(.subheadline.weight(.medium))
                if !state.method.isEmpty {
                    StatusBadge(text: state.method, color: .accentColor)
                }
                if !state.serviceState.isEmpty {
                    StatusBadge(text: state.serviceState,
                                color: state.serviceState == "BOUND" ? .green : .orange)
                }
                if state.isPrimaryDefaultRoute {
                    StatusBadge(text: L(.primaryDefaultRoute), color: .green)
                }
            }

            if state.hasAddress {
                // Two columns of four cells rather than four rows: this
                // is a pure display area, compactness first. Values that
                // do not fit (multiple DNS entries) are shown truncated,
                // and the tooltip shows the whole thing and allows
                // selecting and copying it.
                Grid(alignment: .leading,
                     horizontalSpacing: Design.Spacing.medium,
                     verticalSpacing: Design.Spacing.tight) {
                    GridRow {
                        readbackCell(L(.ipAddress), state.address)
                        readbackCell(L(.netmask), state.netmask)
                    }
                    GridRow {
                        readbackCell(L(.router), state.router)
                        readbackCell("DNS", state.dnsServers.isEmpty
                                     ? L(.notEffective)
                                     : state.dnsServers.joined(separator: L(.listSeparator)))
                    }
                }
            } else {
                Text(L(.noAddressYet, interface))
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }
        }
    }

    private func readbackCell(_ label: String, _ value: String) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: Design.Spacing.tight) {
            Text(label)
                .font(.caption)
                .foregroundStyle(.secondary)
                .frame(width: AddressField.labelWidth, alignment: .leading)
            Text(value.isEmpty ? "—" : value)
                .font(.system(.caption, design: .monospaced))
                .textSelection(.enabled)
                .lineLimit(1)
                .truncationMode(.middle)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .help(value)
    }
}
