import SwiftUI
import TetherKitIPC
import TetherKitUIModel

/// Main window frame: sidebar plus content area.
///
/// Styling follows the reference UI set (settings-style windows like
/// Hand Mirror / Dropover): a left column of "colored tile icon + page
/// name" entries, and headered scrolling content on the right. The
/// title bar is transparent with the title hidden (`MainWindowManager`
/// creates the window that way), so the three traffic lights float
/// above the sidebar. The sidebar's top inset (`trafficLightInset`)
/// keeps clear of them.
///
/// Why a hand-rolled sidebar instead of NavigationSplitView:
///
///   NavigationSplitView demands collapsibility and the system's
///   selection styling, and cannot produce the fixed-width,
///   colored-tile look of the reference UIs. This app does not need
///   collapsing (5 pages, all shown). Handling width, selected and
///   hover states by hand costs little and stays in control.
struct SidebarShell: View {
    var model: AppModel

    /// Persisted selection, restored on launch. `@AppStorage` because
    /// the AppKit-owned window has no scene for `@SceneStorage`.
    @AppStorage(SidebarPage.storageKey) private var storedPage: String = ""

    /// Keyboard-focus engine state for the row group. Under Full
    /// Keyboard Access the rows participate, and whatever the engine
    /// advances to is where AX focus must answer — AGENTS.md pitfall 24
    /// round 8.
    @FocusState private var focusedRow: SidebarPage?
    /// The accessibility-focus engine state: writing it posts
    /// `kAXFocusedUIElementChanged`, so the app's answer to
    /// `kAXFocusedUIElement` tracks what this shell writes (the user's
    /// selection). This is the ONLY sanctioned way to move AX focus
    /// in-process — the outside-facing `AXUIElementSetAttributeValue`
    /// write is a confirmed no-op on our own element (AGENTS.md pitfall 24
    /// round 8; probe-confirmed). Round 11: no key claim reads this vend
    /// any more; the window's Return claim reads the system-wide focus
    /// track.
    @AccessibilityFocusState private var axFocusedRow: SidebarPage?

    private var selection: SidebarPage {
        SidebarPage.resolveStored(storedPage)
    }

    init(model: AppModel) {
        self.model = model
#if DEBUG
        // Keyboard-probe pin (`TETHERKIT_SB_SELECT=<raw page>`): the
        // probe must not depend on the persisted selection — real pages
        // carry destructive buttons a stray synthetic Space would fire.
        if let pinned = ProcessInfo.processInfo.environment["TETHERKIT_SB_SELECT"],
           UserDefaults.standard.string(forKey: SidebarPage.storageKey) != pinned {
            UserDefaults.standard.set(pinned, forKey: SidebarPage.storageKey)
        }
#endif
    }

    /// Re-aims the app's `kAXFocusedUIElement` vend answer at a row
    /// after a selection, so the answered element is the one the user
    /// just chose.
    ///
    /// Round 11: the vend is NOT a claim source any more — the window's
    /// Return claim reads the system-wide focus track instead (the
    /// engine never updates the vend; the user hardware pass showed a
    /// vend-reading claim selecting the last-anchored page from every
    /// row). What remains is honest bookkeeping: anything ASKING the
    /// app for its focused element gets the row the user actually
    /// chose, not a stale anchor.
    ///
    /// Window claim paths (the window's arrow / Return claims) bypass
    /// this on purpose: they write the selection without re-anchoring,
    /// so after such a switch the vend stays pinned to the pre-switch
    /// row by design.
    private func syncAXFocus(to page: SidebarPage) {
        guard SelfAccessibility.isFullKeyboardAccessOn else { return }
        axFocusedRow = page
    }

    private func select(_ page: SidebarPage) {
        // Deliberately instant. The turn-conditional hop (mouse/key
        // turns leave the animated activation transaction; accessibility
        // presses write direct because macOS 26.5 drops every async
        // block scheduled from that turn) and the instant transaction
        // all live in ButtonTurn.write — AGENTS.md pitfalls 22 / 24.
        //
        // The sidebar adds two local clamps on top:
        //
        //   1. content/pageBody attach .transition(.identity) so a page's
        //      enter/exit is never interpolated, whatever drives it.
        //   2. In-page animations (LogCard auto-scroll, StatusHeroCard
        //      pulse) own their transactions and are untouched. Rows also
        //      bypass the system activation machinery entirely
        //      (InstantButtonStyle).
        //
        // The window's arrow and Return claims do NOT go through
        // `select` — they write the storage key directly.
        ButtonTurn.write {
            self.storedPage = page.rawValue
        }
        syncAXFocus(to: page)
    }

    /// Accessibility press write (the row `.accessibilityAction`
    /// target): the caller is an AX action turn, which must write
    /// direct — see `ButtonTurn.writeDirect` and AGENTS.md pitfall 24.
    private func selectDirect(_ page: SidebarPage) {
        ButtonTurn.writeDirect { self.storedPage = page.rawValue }
        syncAXFocus(to: page)
    }

    /// The row Button's plain activation entry.
    ///
    /// Under FKA every turn actuates — mouse clicks, native keyboard
    /// activation showed up through the focus engine, and the AXPress
    /// path (which enters via `selectDirect`, never here). With the
    /// Accessibility pane's FKA toggle off, a key-driven turn is
    /// dropped: the FKA-off keyboard surface of the sidebar is the
    /// bare arrows only (window claim; the system key-view loop moves
    /// focus across controls but must not activate rows without FKA).
    /// Key-driven activation must stay inert while mouse clicks work.
    ///
    /// In this view, `NSApp.currentEvent` is read only behind the FKA-off
    /// gate with a strict mouse allow-list: out-of-process accessibility
    /// turns (e.g. AXPress) can leave a stale event from an earlier user
    /// interaction, so key-driven activation is dropped. (Note:
    /// `ButtonTurn.write` reads `currentEvent` independently to decide
    /// the runloop hop).
    private func rowSelect(_ page: SidebarPage) {
        if SelfAccessibility.isFullKeyboardAccessOn {
            select(page)
            return
        }
        switch NSApp.currentEvent?.type {
        case .leftMouseDown, .leftMouseUp:
            select(page)
        default:
            break // inert: key-driven activation with the pane toggle off
        }
    }

    private func stepSelection(forward: Bool) {
        let all = SidebarPage.allCases
        guard let index = all.firstIndex(of: selection) else { return }
        let next = forward ? (index + 1) % all.count : (index + all.count - 1) % all.count
        select(all[next])
    }

    var body: some View {
        HStack(spacing: 0) {
            sidebar
            // Thin separator between sidebar and content. Both areas
            // read as almost the same material shade; a hairline beats
            // relying on color contrast.
            Rectangle()
                .fill(.separator.opacity(0.6))
                .frame(width: 1)
                .ignoresSafeArea()
            content
        }
        .background(Color(nsColor: .windowBackgroundColor), ignoresSafeAreaEdges: .all)
    }

    // MARK: - Sidebar

    /// The traffic lights float here. Their centers sit about 14pt below
    /// the window top; starting the first row at 44pt reads as "the
    /// lights belong to the sidebar's breathing room", which is what the
    /// reference windows do.
    private static let trafficLightInset: CGFloat = 44
    /// Sidebar width. Proportioned after the reference windows
    /// (Hand Mirror about 1/3, Dropover about 1/4).
    private static let sidebarWidth: CGFloat = 204

    /// The branding row's app icon. A bare `swift run` binary carries
    /// no icon asset, and `NSApp.applicationIconImage` then falls back
    /// to the generic "generic app" folder-style image (it read as a
    /// blue folder in self-captures). Brand with the real icon only
    /// when the bundle actually ships one; otherwise the row is
    /// text-only.
    private static let brandIcon: NSImage? = {
        let shipsIcon = Bundle.main.object(forInfoDictionaryKey: "CFBundleIconName") != nil
            || Bundle.main.path(forResource: "AppIcon", ofType: "icns") != nil
        return shipsIcon ? NSApp.applicationIconImage : nil
    }()

    private var sidebar: some View {
        VStack(alignment: .leading, spacing: 0) {
            rows
                .padding(.top, Self.trafficLightInset)
                .padding(.horizontal, Design.Spacing.small)

            Spacer(minLength: 0)

            // A bare `swift run` binary has no version; the row then
            // renders just the name, which is fine.
            HStack(alignment: .center, spacing: Design.Spacing.small) {
                if let icon = Self.brandIcon {
                    Image(nsImage: icon)
                        .resizable()
                        .frame(width: 28, height: 28)
                }
                VStack(alignment: .leading, spacing: 1) {
                    Text(verbatim: "TetherKit")
                        .font(.callout.weight(.medium))
                    if let version = Self.bundleVersion {
                        Text(verbatim: version)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .monospacedDigit()
                    }
                }
            }
            .foregroundStyle(.secondary)
            .padding(Design.Spacing.medium)
        }
        .frame(width: Self.sidebarWidth)
        .frame(maxHeight: .infinity)
        .background(.thinMaterial, ignoresSafeAreaEdges: .all)
    }

    /// The page rows.
    ///
    /// The sidebar's keys are claimed at the WINDOW level
    /// (`SidebarMainWindow.performKeyEquivalent`): bare arrows step the
    /// pages with the Accessibility-pane FKA toggle off (round 11
    /// contract; with FKA on the arrows belong to the system focus
    /// engine), and bare Return is resolved against the system-wide
    /// focus track with FKA on (round 11). The rows ARE reachable
    /// through the system key-view loop — each row declares
    /// `.focusable(FKA || isSelected)` — so Tab lands on them in both
    /// modes.
    /// FKA Space actuates the row the focus engine landed on through
    /// the row's `.accessibilityAction` — it arrives as an AXPress,
    /// never an NSEvent (round 6). The container keeps arrow handlers
    /// as a bubble-up fallback; Space/Return need no container path any
    /// more (the vend answer they used to read was retired from the
    /// claim in round 11).
    /// The system's focus chrome is suppressed
    /// (`.focusEffectDisabled()`), the selected row's fill is the
    /// visible cursor.
    ///
    /// Selected state is a rounded gray fill. Uses `primary` at low
    /// opacity instead of `.quaternary` because quaternary is nearly
    /// invisible on the material background (measured by eye in
    /// screenshots, not taste).
    private var rows: some View {
        VStack(alignment: .leading, spacing: 3) {
            ForEach(SidebarPage.allCases) { page in
                SidebarRow(
                    page: page,
                    isSelected: selection == page,
                    iconTint: SidebarPage.tint(for: page, status: model.status),
                    focusedRow: $focusedRow,
                    axFocusedRow: $axFocusedRow,
                    onSelect: { rowSelect(page) },
                    onAccessibilityPress: { selectDirect(page) },
                    onStep: stepSelection
                )
            }
        }
        .focusEffectDisabled()
        .onKeyPress(.upArrow) {
            stepSelection(forward: false)
            return .handled
        }
        .onKeyPress(.downArrow) {
            stepSelection(forward: true)
            return .handled
        }
        .onChange(of: storedPage) { _, newRaw in
            // Re-anchor keyboard focus only when the engine already holds a row;
            // never steal focus from content. Window-level arrow/Return claims
            // write the storage key directly and must not pull focus back to the
            // sidebar (AGENTS.md pitfall 24 round 11).
            if focusedRow != nil, let page = SidebarPage(rawValue: newRaw) {
                focusedRow = page
            }
        }
        .onChange(of: focusedRow) { _, row in
            guard SelfAccessibility.isFullKeyboardAccessOn,
                  let row, axFocusedRow != row else { return }
            axFocusedRow = row
        }
    }

    // MARK: - Content area

    @ViewBuilder
    private var content: some View {
        if selection == .log {
            // The log page takes no page-level scroll: its list fills
            // the page height (see LogPage). Height semantics,
            // including this page's padding, belong to the page.
            LogPage(model: model)
                .transition(.identity)
        } else {
            scrollablePage
                .transition(.identity)
        }
    }

    /// Heading for the current page. Throughput carries the LIVE badge
    /// in the trailing accessory (the card no longer renders one).
    @ViewBuilder
    private var pageHeading: some View {
        switch selection {
        case .throughput:
            SidebarPageHeading(title: selection.title) { throughputLiveBadge }
        default:
            SidebarPageHeading(title: selection.title)
        }
    }

    @ViewBuilder
    private var throughputLiveBadge: some View {
        if model.status.runState == .running {
            StatusBadge(text: L(.throughputLive), color: .green)
        }
    }

    /// Every page except the log shares this "heading + scrolling body"
    /// container.
    private var scrollablePage: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: Design.Spacing.large) {
                pageHeading
                pageBody
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, Design.Spacing.large)
            .padding(.top, Design.Spacing.medium + 4)
            .padding(.bottom, Design.Spacing.large)
            .frame(maxWidth: .infinity, alignment: .topLeading)
        }
        // No scrolling or bounce when the content fits.
        .scrollBounceBehavior(.basedOnSize)
    }

    @ViewBuilder
    private var pageBody: some View {
        // Every branch gets .transition(.identity): pages entering or
        // leaving here must not be interpolated (see select() for why,
        // and content above for the sibling boundary).
        switch selection {
        case .connection: ConnectionPage(model: model).transition(.identity)
        case .network: NetworkCard(model: model).transition(.identity)
        case .throughput: ThroughputCard(model: model).transition(.identity)
        // The log page branches at the content level above (no outer
        // scroll); this case only satisfies exhaustiveness.
        case .log: EmptyView()
        case .manage: ManagePage(model: model).transition(.identity)
        }
    }

    private static var bundleVersion: String? {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String
    }
}

/// Page heading: page title plus an optional trailing accessory view
/// (the LIVE badge, for one).
///
/// Shared by the shell's generic container and the log page (which
/// owns its own layout). Both must render the identical heading, so it
/// lives here instead of two hand-rolled HStacks.
struct SidebarPageHeading<Trailing: View>: View {
    let title: String
    @ViewBuilder var trailing: () -> Trailing

    init(title: String,
         @ViewBuilder trailing: @escaping () -> Trailing = { EmptyView() }) {
        self.title = title
        self.trailing = trailing
    }

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: Design.Spacing.small) {
            Text(title)
                .font(.title2.weight(.semibold))
            Spacer(minLength: Design.Spacing.small)
            trailing()
        }
    }
}

/// One sidebar entry row.
///
/// Drawn the way native sidebars behave: light gray on hover, deeper
/// gray when selected. The icon is a white 12pt symbol inside a 22pt
/// rounded colored tile, the signature visual element of the
/// reference UIs.
private struct SidebarRow: View {
    let page: SidebarPage
    let isSelected: Bool
    let iconTint: Color
    let focusedRow: FocusState<SidebarPage?>.Binding
    let axFocusedRow: AccessibilityFocusState<SidebarPage?>.Binding
    let onSelect: () -> Void
    /// Accessibility press path (AXPress: VoiceOver activation, Full
    /// Keyboard Access Space). Writes direct — see the
    /// `.accessibilityAction(.default)` attachment below and
    /// `ButtonTurn.writeDirect`.
    let onAccessibilityPress: () -> Void
    /// Arrow-press handler for the focused row; `forward` = Down.
    let onStep: (Bool) -> Void

    /// Partial-strength accent for the key-focus outline.
    private static let keyFocusRingOpacity = 0.55

    @Environment(\.controlActiveState) private var controlActiveState
    @State private var isHovering = false

    private var hasKeyboardFocus: Bool {
        focusedRow.wrappedValue == page && controlActiveState == .key
    }

    var body: some View {
        rowButton
        .onHover { isHovering = $0 }
        .focusable(SelfAccessibility.isFullKeyboardAccessOn || isSelected)
        .focused(focusedRow, equals: page)
        .accessibilityFocused(axFocusedRow, equals: page)
        .accessibilityAddTraits(isSelected ? .isSelected : [])
        .accessibilityAction(.default) {
            onAccessibilityPress()
        }
        .onKeyPress(.upArrow) {
            onStep(false)
            return .handled
        }
        .onKeyPress(.downArrow) {
            onStep(true)
            return .handled
        }
        .onKeyPress(.return) {
            guard SelfAccessibility.isFullKeyboardAccessOn else { return .ignored }
            onSelect()
            return .handled
        }
    }

    private var rowButton: some View {
        Button(action: onSelect) {
            HStack(spacing: Design.Spacing.small) {
                icon
                Text(page.title)
                    .font(.body)
                    .foregroundStyle(.primary)
                    .lineLimit(1)
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 7)
            .background(rowBackground, in: RoundedRectangle(cornerRadius: 7))
            .overlay {
                if hasKeyboardFocus {
                    RoundedRectangle(cornerRadius: 7)
                        .strokeBorder(Color.accentColor.opacity(Self.keyFocusRingOpacity), lineWidth: 1)
                }
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(InstantButtonStyle())
    }

    private var icon: some View {
        Image(systemName: page.symbol)
            .font(.system(size: 12, weight: .semibold))
            .foregroundStyle(.white)
            .frame(width: 22, height: 22)
            .background(iconTint, in: RoundedRectangle(cornerRadius: 6, style: .continuous))
            .accessibilityHidden(true)
    }

    private var rowBackground: Color {
        if isSelected {
            return Color.primary.opacity(0.08)
        }
        if hasKeyboardFocus || isHovering {
            return Color.primary.opacity(0.04)
        }
        return .clear
    }
}

/// A primitive button style that only performs the action: no activation
/// highlight machinery, no animated transaction around the action's view
/// updates. macOS 26's default activation wraps button actions in an
/// animated transaction; that is the click-only context the page-insertion
/// interpolation escaped through. Selected and hover styling is hand-drawn
/// in the row already.
private struct InstantButtonStyle: PrimitiveButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .transaction { t in t.animation = nil }
            .contentShape(Rectangle())
            .simultaneousGesture(TapGesture().onEnded { configuration.trigger() })
    }
}

/// The persisted-selection key, shared by the shell's `@AppStorage`
/// and the window-level Return handler in MainWindowManager — one
/// literal, two writers of the same default.
extension SidebarPage {
    static let storageKey = "tetherkit.sidebar.page"

    /// The page whose row the SYSTEM currently reports as the keyboard
    /// focused element — read on the system-wide element and accepted
    /// only for our own process — or `nil` when focus is unset, sits
    /// outside this process, or is a non-row element.
    ///
    /// Round 11: this replaced `fromAXFocusAnswer` as the window's
    /// Return-claim resolver. The app-level answer is our own
    /// `@AccessibilityFocusState` vend: the SwiftUI focus engine does
    /// not drive it (round-10 probe fact — `@FocusState` never fires
    /// for FKA/loop landings), so the vend only ever reported the last
    /// anchor, and the user hardware pass showed a claim built on it
    /// selecting the same page from every row. The system-wide track
    /// is the actual Full Keyboard Access focus; if it resolves
    /// nothing, the claim passes the key through to the focused row's
    /// `.onKeyPress` backstop. Resolution reads the focused element's
    /// AX description and matches it against each page's localized
    /// title — the row buttons vend their title as the description.
    ///
    /// The matched description is handed back alongside the page so the
    /// caller's DEBUG report can print what this resolution actually
    /// matched against without issuing a second system-wide AX read.
    static func fromSystemWideFocusedDescription() -> (page: SidebarPage?, description: String?) {
        guard let text = SelfAccessibility.systemWideFocusedDescription() else { return (nil, nil) }
        return (allCases.first { $0.title == text }, text)
    }
}
