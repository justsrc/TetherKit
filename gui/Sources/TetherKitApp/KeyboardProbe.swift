#if DEBUG
import AppKit
import TetherKitUIModel

/// Headless keyboard probe (DEBUG only; opt-in via
/// `TETHERKIT_SB_PROBE=walk|matrix|fka_on|full`; any other value runs
/// the FKA-off contract suite).
///
/// Walks the keyboard contract through the REAL dispatch chain —
/// synthesized `NSEvent`s fed to `NSApp.sendEvent`, no private APIs,
/// no permissions — and samples, after each step: the persisted page
/// selection, the window first responder, and the AX answers (self
/// app-level and the round-11 system-wide track, plus on-screen
/// geometry). Every line is prefixed `PROBE` for easy capture.
///
/// Modes:
///
///   * `walk` — facts only: presses successive Tabs and records where
///     each lands. No assertions. (Round 10 — AGENTS.md pitfall 24 — used a
///     DEBUG style lever to walk each row style; that lever has since been
///     removed.)
///   * `matrix` — one pass per sidebar page: two Tabs (recorded), a
///     bare Down that must step to the next page, an Up that must step
///     back, then one Shift+Tab (recorded).
///   * `fka_on` — FKA forced on via `TETHERKIT_FKA_OVERRIDE=1`: one
///     Return then one Tab, recorded; only the override's liveness is
///     asserted.
///   * `full` — the FKA-off contract suite (FKA forced off via
///     `TETHERKIT_FKA_OVERRIDE=0`), round 11. Space and Return are sent before any Tab, so no Tab can
///     first park a content button under them (the run-1 hazard: Space
///     landing on a real button once hijacked the whole probe run):
///
///       - bare Down arrow steps the page             → round 7 claim
///       - bare Space is inert                        → FKA-off
///         contract (no stale-row actuation)
///       - bare Return is inert                       → FKA-off
///         contract
///       - bare Tab changes nothing in the sidebar    → the self-driven
///         ring is REMOVED (round 11): the system key-view loop cannot
///         reach the rows, so Tab must leave the selection alone
///       - a second Tab is equally inert              → no state
///
/// Exits 0 on PASS (or walk completion), 1 on FAIL, 3 on the 20 s
/// watchdog, 4 if event synthesis failed. Armed once, from
/// `MainWindowManager.present`.
@MainActor
enum KeyboardProbe {
    private static var armed = false
    private static var failures: [String] = []

    /// Raw `kVK_*` key codes for the probe's synthesized keyDowns.
    private enum KeyCode {
        static let downArrow: UInt16 = 125
        static let upArrow: UInt16 = 126
        static let space: UInt16 = 49
        static let returnKey: UInt16 = 36
        static let tab: UInt16 = 48
    }

    /// The private-use glyphs NSEvent delivers for the Down (0xF701)
    /// and Up (0xF700) arrow keys — sent as the bare `characters:` of
    /// the synthesized arrow keyDowns.
    private static let downArrowGlyph = "\u{F701}"
    private static let upArrowGlyph = "\u{F700}"

    /// Wait after arming before the first synthetic key: the main
    /// window must be on screen and past its first SwiftUI render.
    private static let initialSettleSeconds: Double = 0.8

    static func armIfNeeded() {
        guard !armed,
              let mode = ProcessInfo.processInfo.environment["TETHERKIT_SB_PROBE"] else { return }
        armed = true
        print("PROBE armed mode=\(mode) fka=\(SelfAccessibility.isFullKeyboardAccessOn)")
        scheduleWatchdog()
        switch mode {
        case "walk":
            after(initialSettleSeconds) {
                _ = observe("base")
                walk(1)
            }
        case "matrix":
            runMatrixSuite()
        case "fka_on":
            runFkaOnSuite()
        default:
            runFullSuite()
        }
    }

    private static func runFkaOnSuite() {
        if !SelfAccessibility.isFullKeyboardAccessOn {
            failures.append("TETHERKIT_FKA_OVERRIDE did not force FKA on")
        }
        after(initialSettleSeconds) {
            _ = observe("fka-on-base")
            sendKey(keyCode: KeyCode.returnKey, characters: "\r")
            after(0.3) {
                _ = observe("fka-on-return")
                sendKey(keyCode: KeyCode.tab, characters: "\t")
                after(0.3) {
                    _ = observe("fka-on-tab")
                    finish()
                }
            }
        }
    }

    /// Tests keyboard behavior across all sidebar pages.
    private static func runMatrixSuite() {
        print("PROBE [matrix] starting multi-page matrix tests across all \(SidebarPage.allCases.count) pages...")
        after(initialSettleSeconds) {
            testPage(index: 0)
        }
    }

    private static func testPage(index: Int) {
        let pages = SidebarPage.allCases
        guard index < pages.count else {
            print("PROBE [matrix] all \(pages.count) pages tested successfully!")
            finish()
            return
        }
        let page = pages[index]
        print("\nPROBE [matrix] page \(page.rawValue) ---")
        ButtonTurn.write {
            UserDefaults.standard.set(page.rawValue, forKey: SidebarPage.storageKey)
        }
        after(0.3) {
            let base = observe("matrix-\(page.rawValue)-base")
            if base != page.rawValue {
                failures.append("page select failed for \(page.rawValue)")
            }
            // Tab 1: landing sampled, not assumed
            sendKey(keyCode: KeyCode.tab, characters: "\t")
            after(0.25) {
                _ = observe("matrix-\(page.rawValue)-tab1-sidebar")
                // Tab 2: landing sampled, not assumed
                sendKey(keyCode: KeyCode.tab, characters: "\t")
                after(0.25) {
                    _ = observe("matrix-\(page.rawValue)-tab2-content")
                    // Down Arrow while inside content: must step to next page!
                    let expectedNext = pages[(index + 1) % pages.count].rawValue
                    sendKey(keyCode: KeyCode.downArrow, characters: downArrowGlyph)
                    after(0.25) {
                        let afterDown = observe("matrix-\(page.rawValue)-down-from-content")
                        if afterDown != expectedNext {
                            failures.append("down arrow from content on \(page.rawValue) expected \(expectedNext) but got \(afterDown)")
                        }
                        // Up Arrow: must step back to original page!
                        sendKey(keyCode: KeyCode.upArrow, characters: upArrowGlyph)
                        after(0.25) {
                            let afterUp = observe("matrix-\(page.rawValue)-up-back")
                            if afterUp != page.rawValue {
                                failures.append("up arrow back on \(page.rawValue) expected \(page.rawValue) but got \(afterUp)")
                            }
                            // Shift+Tab: backwards traversal
                            sendKey(keyCode: KeyCode.tab, characters: "\t", modifierFlags: .shift)
                            after(0.25) {
                                _ = observe("matrix-\(page.rawValue)-shifttab")
                                // Move to next page in matrix
                                testPage(index: index + 1)
                            }
                        }
                    }
                }
            }
        }
    }

    /// Number of Tabs the walk mode sends before finishing.
    private static let walkTabCount = 4

    /// Facts only: send one Tab, then observe where it landed; chain to
    /// the next Tab instead of scheduling sibling timers.
    private static func walk(_ step: Int) {
        sendKey(keyCode: KeyCode.tab, characters: "\t")
        after(0.35) {
            _ = observe("tab\(step)")
            if step < walkTabCount {
                walk(step + 1)
            } else {
                finishWalkFacts()
            }
        }
    }

    private static func runFullSuite() {
        if SelfAccessibility.isFullKeyboardAccessOn {
            failures.append("full suite requires FKA off (set TETHERKIT_FKA_OVERRIDE=0 or disable Full Keyboard Access in System Settings)")
        }
        after(initialSettleSeconds) {
            // Capture the pinned selection BEFORE sending any key: the
            // timer fires after the previous step's key already moved
            // the page, so reading it inside the nested closure would
            // compare the stepped state against itself (round-11
            // probe rewrite: first run failed exactly that way).
            let base = observe("base")
            sendKey(keyCode: KeyCode.downArrow, characters: downArrowGlyph)
            after(0.3) {
                let afterDown = observe("down")
                if afterDown == base {
                    failures.append("bare down arrow did not step the page")
                }
                sendKey(keyCode: KeyCode.space, characters: " ")
                after(0.3) {
                    let afterSpace = observe("space")
                    if afterSpace != afterDown {
                        failures.append("space actuated a row (must stay inert under fka-off)")
                    }
                    sendKey(keyCode: KeyCode.returnKey, characters: "\r")
                    after(0.3) {
                        let afterReturn = observe("return")
                        if afterReturn != afterDown {
                            failures.append("return actuated a row (must stay inert under fka-off)")
                        }
                        sendKey(keyCode: KeyCode.tab, characters: "\t")
                        after(0.3) {
                            let tab1 = observe("tab1")
                            if tab1 != afterDown {
                                failures.append(
                                    "tab1 moved the selection (ring removed; tab must be a no-op)")
                            }
                            sendKey(keyCode: KeyCode.tab, characters: "\t")
                            after(0.3) {
                                let tab2 = observe("tab2")
                                if tab2 != afterDown {
                                    failures.append(
                                        "tab2 moved the selection (tab must stay a no-op)")
                                }
                                sendKey(keyCode: KeyCode.downArrow, characters: downArrowGlyph)
                                after(0.3) {
                                    let down2 = observe("downAfterButton")
                                    if down2 == tab2 {
                                        failures.append("down arrow did not step page while button was focused")
                                    }
                                    finish()
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // MARK: - plumbing

    private static func scheduleWatchdog() {
        after(20) {
            print("PROBE VERDICT: FAIL (watchdog timeout)")
            exit(3)
        }
    }

    private static func finish() {
        if failures.isEmpty {
            print("PROBE VERDICT: PASS")
            exit(0)
        } else {
            for failure in failures {
                print("PROBE VERDICT: FAIL — \(failure)")
            }
            exit(1)
        }
    }

    /// Walk mode reports facts and always exits 0.
    private static func finishWalkFacts() {
        print("PROBE VERDICT: PASS (walk)")
        exit(0)
    }

    /// Samples the observables and returns the persisted selection.
    ///
    /// Additional facts printed for offline analysis: the window first
    /// responder, the app-level AX answer (role / title / description
    /// / on-screen frame — SwiftUI's own vend), and the SYSTEM-WIDE
    /// answer (the Full Keyboard Access focus track the round-11
    /// Return claim reads). Geometry is printed alongside `win=` for
    /// column math: a landing site can answer no AX attributes
    /// (opaque AppKit key-view proxies — run-1 fact), but its frame
    /// still says WHERE focus sits.
    /// `x,y wxh` for a frame, for the `axFrame=` / `win=` columns.
    private static func rectText(_ rect: CGRect) -> String {
        "\(Int(rect.minX)),\(Int(rect.minY)) \(Int(rect.width))x\(Int(rect.height))"
    }

    private static func observe(_ label: String) -> String {
        let selection = UserDefaults.standard.string(forKey: SidebarPage.storageKey) ?? "-"
        let firstResponder = NSApp.keyWindow?.firstResponder
        let responder = firstResponder.map { String(describing: type(of: $0)) } ?? "nil"
        let role = SelfAccessibility.focusedRole() ?? "nil"
        let title = SelfAccessibility.focusedTitle() ?? "nil"
        let description = SelfAccessibility.focusedDescription() ?? "nil"
        let frameText = SelfAccessibility.focusedFrame().map(rectText) ?? "nil"
        let sysRole = SelfAccessibility.systemWideFocusedRole() ?? "nil"
        let sysTitle = SelfAccessibility.systemWideFocusedTitle() ?? "nil"
        let sysDesc = SelfAccessibility.systemWideFocusedDescription() ?? "nil"
        let windowText = (NSApp.keyWindow?.frame).map(rectText) ?? "nil"
        print("PROBE [\(label)] sel=\(selection) fr=\(responder) role=\(role) "
              + "title=\(title) desc=\(description) axFrame=\(frameText) "
              + "sysRole=\(sysRole) sysTitle=\(sysTitle) sysDesc=\(sysDesc) "
              + "win=\(windowText)")
        return selection
    }

    private static func after(_ seconds: Double, _ body: @escaping @MainActor () -> Void) {
        DispatchQueue.main.asyncAfter(deadline: .now() + seconds) {
            MainActor.assumeIsolated { body() }
        }
    }

    /// Synthesizes a bare keyDown and feeds it through the real
    /// dispatch chain. `sendEvent` puts the event through the window's
    /// key-equivalent phase first, exactly like a hardware key.
    private static func sendKey(keyCode: UInt16,
                                characters: String,
                                modifierFlags: NSEvent.ModifierFlags = []) {
        guard let window = NSApp.keyWindow,
              let event = NSEvent.keyEvent(with: .keyDown,
                                           location: NSPoint.zero,
                                           modifierFlags: modifierFlags,
                                           timestamp: ProcessInfo.processInfo.systemUptime,
                                           windowNumber: window.windowNumber,
                                           context: nil,
                                           characters: characters,
                                           charactersIgnoringModifiers: characters,
                                           isARepeat: false,
                                           keyCode: keyCode) else {
            failures.append("event synthesis failed (keyCode \(keyCode))")
            exit(4)
        }
        NSApp.sendEvent(event)
    }
}
#endif
