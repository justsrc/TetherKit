import AppKit
import SwiftUI
import TethernetIPC
import TethernetUIModel

/// The main window. `performKeyEquivalent` is AppKit's window-level
/// key-equivalent phase: it runs BEFORE the raw key reaches the first
/// responder, so Return can be claimed for the sidebar with no
/// lifecycle bookkeeping (a local NSEvent monitor would need explicit
/// install/removal across the menu-bar show/hide cycles instead).
/// Return passes through FKA untouched (research round 6: Full
/// Keyboard Access converts Space and the arrows — never Return — to
/// accessibility commands), so this is the one reliable in-app site
/// for that key; Space's real-FKA path is the rows' own
/// `.accessibilityAction(.default)` instead. Round 11: the claim
/// resolves the row through the SYSTEM-WIDE focused element (the real
/// FKA focus track) — the app's own `kAXFocusedUIElement` answer was
/// retired as a claim source after the user hardware pass showed it
/// permanently answering the last-anchored page; the focused row also
/// carries a read-free `.onKeyPress` backstop (SidebarRow), which
/// never fires when the system-wide read resolves (the claim consumes
/// the key first), and covers the setups where it does not.

private final class SidebarMainWindow: NSWindow {
    /// Virtual keycodes (macOS `keyCode` values): Return and keypad
    /// Enter activate under Full Keyboard Access; arrows step pages
    /// with the pane toggle off.
    private enum KeyCode {
        static let returnKey = 36
        static let keypadEnter = 76
        static let downArrow = 125
        static let upArrow = 126
    }

    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        // Bare keys only: modifier chords (⌘⏎ and friends) fall
        // through to normal shortcut handling for everything below.
        // (Arrows carry macOS's `.function` device-independent bit
        // even with no keys held — that bit is deliberately NOT
        // banned; the ban is an explicit set, probed r7. Note: .capsLock
        // is included so that engaged Caps Lock defers navigation to
        // standard system handling rather than trapping bare navigation).
        let isActivationKey = event.keyCode == KeyCode.returnKey
            || event.keyCode == KeyCode.keypadEnter
        let isArrow = event.keyCode == KeyCode.downArrow
            || event.keyCode == KeyCode.upArrow
        guard isActivationKey || isArrow,
              event.modifierFlags
                  .intersection([.capsLock, .shift, .control, .option, .command]).isEmpty
        else {
            return super.performKeyEquivalent(with: event)
        }
        if isArrow {
            return handleArrowKey(event)
        }
        // Return actuation is Full Keyboard Access-only (see
        // SelfAccessibility.isFullKeyboardAccessOn): with the pane
        // toggle off, AX focus on a row is stale-browsing state and
        // Return must keep its normal behavior everywhere.
        guard SelfAccessibility.isFullKeyboardAccessOn else {
            return super.performKeyEquivalent(with: event)
        }
        // Deliberately narrow: Return yields only to the system field
        // editor (the round 6 claim condition). Checked first to avoid
        // any synchronous AX call when typing in text fields. Arrows
        // use the broader editable-text predicate in `isFirstResponderEditableText`
        // because caret movement applies to any editable text view.
        let fieldEditorActive = (firstResponder as? NSTextView)?.isFieldEditor == true
        guard !fieldEditorActive else {
            return super.performKeyEquivalent(with: event)
        }

        // Claim the key when the SYSTEM's focused element — the
        // Full Keyboard Access focus track, read system-wide — is one
        // of the sidebar rows. Round 11: NOT the app's own
        // `kAXFocusedUIElement` answer. That answer is our
        // `@AccessibilityFocusState` binding, and the round-10 probe
        // fact bites: the SwiftUI focus engine never fires for
        // Tab-loop/FKA landings, so the binding only ever says what
        // the last mouse click anchored (user hardware pass: Return
        // on ANY row selected the last-clicked page, network). The
        // app-level answer is a lie by construction; the system-wide
        // read is the actual FKA focus track.
        //
        // Note: system-wide element reading requires Accessibility trust;
        // if untrusted or if it resolves to nothing, the key passes
        // through to the focused row's own `.onKeyPress(.return)` backstop
        // (SidebarRow) — read-free, engine-fact only.
        let (systemWideFocusedPage, claimDescription) =
            SidebarPage.fromSystemWideFocusedDescription()
#if DEBUG
        // The description the claim resolved against — captured in the
        // destructuring above, not a second AX read — reported for the
        // user hardware pass (probe conventions: `PROBE [return-claim]`).
        if let page = systemWideFocusedPage {
            print("PROBE [return-claim] sys-wide matched page=\(page.rawValue) "
                  + "desc=\(claimDescription ?? "nil")")
        } else {
            print("PROBE [return-claim] sys-wide matched nothing "
                  + "(desc=\(claimDescription ?? "nil")) "
                  + "- key passes through to row backstop")
        }
#endif
        if let page = systemWideFocusedPage {
            // A real key event turn: the standard hop applies. The
            // write goes straight to the storage key, which the
            // shell's `@AppStorage` observes — but unlike the shell's
            // `select`, it does not re-anchor AX focus (`syncAXFocus`).
            ButtonTurn.write {
                UserDefaults.standard.set(page.rawValue, forKey: SidebarPage.storageKey)
            }
            return true
        }
        return super.performKeyEquivalent(with: event)
    }

    /// True when the first responder is any editable text view — the
    /// system field editor or an editable `NSTextView` — whose arrow
    /// keys belong to caret movement, so the arrow claim must yield
    /// and pass the key through to it.
    private var isFirstResponderEditableText: Bool {
        if let text = firstResponder as? NSTextView {
            return text.isFieldEditor || text.isEditable
        }
        return false
    }

    /// Bare Arrow Up/Down stepping of the sidebar pages, claimed at the
    /// key-equivalent phase — before any first-responder dispatch, and
    /// therefore before AppKit's Keyboard Navigation key-nav engine.
    ///
    /// Claimed when the Accessibility-pane FKA toggle is off and the user
    /// is not editing a text field. A focused button, card, or scroll
    /// area does not consume arrow keys, so page stepping is always
    /// available unless an active text editor owns the keys for caret
    /// movement.
    ///
    /// Stepping wraps in line with the shell's `stepSelection`, and
    /// the write goes through `ButtonTurn.write`.
    private func handleArrowKey(_ event: NSEvent) -> Bool {
        guard !SelfAccessibility.isFullKeyboardAccessOn else {
            return super.performKeyEquivalent(with: event)
        }
        guard !isFirstResponderEditableText else {
            return super.performKeyEquivalent(with: event)
        }
        let pages = SidebarPage.allCases
        let currentRaw = UserDefaults.standard.string(forKey: SidebarPage.storageKey)
        let current = pages.first(where: { $0.rawValue == currentRaw }) ?? pages[0]
        let index = pages.firstIndex(of: current) ?? 0
        let nextIndex = event.keyCode == KeyCode.downArrow
            ? (index + 1) % pages.count
            : (index + pages.count - 1) % pages.count
        ButtonTurn.write {
            UserDefaults.standard.set(pages[nextIndex].rawValue,
                                      forKey: SidebarPage.storageKey)
        }
        return true
    }
}

/// Owns the main window.
///
/// Why not SwiftUI's `Window` scene: it defers the first window and
/// rebuilds it when the app drops to menu-bar mode; owning one window
/// object from the app delegate is deterministic and exposes the
/// reopen hook. Chrome (traffic-light size and spacing) is decided by
/// the executable's LC_BUILD_VERSION `sdk` stamp, not by the layer
/// owning the window; see the linker settings in Package.swift.
///
/// One window object, reused forever: the menu bar's "Open Main
/// Window" re-shows it, the red close button hides it, and closing it
/// drops the app back to menu-bar-only mode.

@MainActor
final class MainWindowManager: NSObject {
    static let shared = MainWindowManager()

    /// Held strongly so closing the window does not destroy it.
    private var window: NSWindow?
    private weak var model: AppModel?

    /// Creates the window on first use, then brings it to the front.
    func present(_ model: AppModel) {
        self.model = model
        // Restore .regular BEFORE ordering front. Coming back from
        // menu-bar mode, the app still carries the accessory policy;
        // ordering front first can leave the window behind other apps
        // even though activate(ignoringOtherApps:) follows.
        NSApp.setActivationPolicy(.regular)
        if window == nil {
            window = makeWindow(model)
        }
        // Whether makeKeyAndOrderFront restores a miniaturized window
        // is version-dependent (observed once on 26.5, folklore says
        // no on older releases), so ask for the restore directly:
        // deminiaturize is a no-op when not mini.
        if window?.isMiniaturized == true {
            window?.deminiaturize(nil)
        }
        window?.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        model.windowDidAppear()
#if DEBUG
        KeyboardProbe.armIfNeeded()
#endif
    }

    /// Whether the main window itself is on screen. The menu-bar panel
    /// is also a window, so `NSApp.hasVisibleWindows` cannot answer
    /// this: with the main window hidden but the panel open, Dock
    /// reopen must still present the main window.
    var mainWindowIsVisible: Bool { window?.isVisible == true }

    /// Hides the main window (File ▸ Close / ⌘W and the red close
    /// button land on the same path). performClose fires windowWillClose
    /// (policy flip, poll cadence) even on an off-screen window, so a
    /// hidden window gets a true no-op instead.
    func closeMainWindow() {
        // A miniaturized window reports isVisible == false (probed on
        // macOS 26.5), so the isVisible guard alone would silently
        // dead-end cmd-W on a dock-minimized window. performClose
        // works from that state and deminiaturizes on the way to close.
        guard mainWindowIsVisible || window?.isMiniaturized == true else { return }
        window?.performClose(nil)
    }

    private func makeWindow(_ model: AppModel) -> NSWindow {
        let window = SidebarMainWindow(contentRect: NSRect(x: 0, y: 0,
                                                           width: Design.Window.width,
                                                           height: Design.Window.height),
                              styleMask: [.titled, .closable, .miniaturizable, .fullSizeContentView],
                              backing: .buffered,
                              defer: false)
        window.title = ""
        window.titleVisibility = .hidden
        window.titlebarAppearsTransparent = true
        window.isReleasedWhenClosed = false
        // Zoom does nothing on a fixed-size window; disable it
        // explicitly rather than leave a live-looking button.
        window.standardWindowButton(.zoomButton)?.isEnabled = false
        // The disabled zoom button is only one entry point: Window ▸
        // Zoom, Dock menus, and the full-screen affordance all route
        // through window APIs. fullScreenNone rejects full screen; the
        // delegate's windowShouldZoom rejects zoom requests.
        window.collectionBehavior = .fullScreenNone
        // Hosting sizing propagation OFF: the hosting view must not
        // drive the window size from page content. Default sizing
        // options push min/intrinsic/max into the window as pages swap
        // (page heights differ), and on macOS 26 content-size changes
        // synchronize with window-frame animation (WWDC25-256), the
        // click's "bulge".
        let hosting = NSHostingView(rootView: MainWindowRoot(model: model))
        hosting.sizingOptions = []
        window.contentView = hosting
        window.minSize = NSSize(width: Design.Window.width, height: Design.Window.height)
        window.maxSize = window.minSize
        window.delegate = self
        window.center()
        return window
    }
}

extension MainWindowManager: NSWindowDelegate {
    func windowWillClose(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)
        model?.windowDidDisappear()
        model = nil
    }

    func windowShouldZoom(_ window: NSWindow, toFrame newFrame: NSRect) -> Bool {
        false
    }

    // The window has no `.resizable` style bit, so min/max only guard
    // programmatic resizes. Observed on macOS 26: AppKit resets both
    // shortly after presentation (a one-time +32pt chrome settle via
    // `_setFrameCommon` — this is also what changes the recorded
    // frame), so the pin is reasserted on these two delegate hooks.
    // A SwiftUI-facing redundancy: the hosting view runs with
    // `sizingOptions = []`, so page swaps cannot resize the window in
    // either direction.
    func windowDidBecomeMain(_ notification: Notification) {
        pinFixedSize()
    }

    func windowDidResize(_ notification: Notification) {
        pinFixedSize()
    }

    private func pinFixedSize() {
        let fixed = NSSize(width: Design.Window.width, height: Design.Window.height)
        // Idempotent writes: setting min/max must never itself post a
        // resize event (windowDidResize hand-off → infinite loop).
        if window?.minSize != fixed { window?.minSize = fixed }
        if window?.maxSize != fixed { window?.maxSize = fixed }
    }
}
