import AppKit
import SwiftUI

/// Escape hatch for state writes that run inside a control-activation
/// turn.
///
/// macOS 26 wraps the view updates caused by activating a button,
/// picker segment, or toggle in an **animated transaction** (the
/// "activation turn"), so a state write in the action closure
/// interpolates: pages zoom, rows fade in and out, heights breathe.
/// Timer/polling-driven writes never run in such a turn and are
/// pixel-clean as-is — only activation-driven writes need this escape.
///
/// Two runtime behaviors (probed on macOS 26.5 and bisected in
/// standalone repros; AGENTS.md pitfalls 22 / 24,
/// docs/GUI-ARCHITECTURE.md 4.11) shape the cure:
///
///   * Mouse-click and key-press turns: the write must *leave* the
///     turn — hop to the next runloop tick, then write with an instant
///     transaction. A direct write during a key event re-renders
///     mid-event and re-delivers the same key (the sidebar
///     double-stepped its selection that way).
///   * Accessibility press turns (VoiceOver's AXPress et al.): every
///     asynchronous block scheduled from that turn is dropped before
///     its body runs — `DispatchQueue.main.async` and
///     `Task { @MainActor }` alike — and `NSApp.currentEvent` is not a
///     reliable discriminator there: it can be nil, but out-of-process
///     AX turns leave a stale event from an earlier click (AGENTS.md
///     pitfall 24 round 6). Such turns must route through
///     `writeDirect`, never through the hop.
///
/// The write itself always carries an instant `Transaction`, so
/// whatever turn the body lands in, the flush interpolates nothing.
///
/// Lives in this target (the sidebar page model) so both its callers
/// and its commit semantics are unit-testable: the App target is an
/// executable, which `swift test` cannot import (see TethernetUIModelTests).
public enum ButtonTurn {

    /// Event types that identify an activation turn requiring the
    /// runloop hop. `NSApp.currentEvent` is `nil` outside user turns
    /// (programmatic writes therefore land direct), but accessibility
    /// press turns are not reliably nil — AppKit can leave a stale
    /// event (pitfall 24 round 6) — so those route through
    /// `writeDirect`, never through this nil check.
    private static let hopTurns: Set<NSEvent.EventType> = [
        .leftMouseDown, .leftMouseUp,
        .rightMouseDown, .rightMouseUp,
        .otherMouseDown, .otherMouseUp,
        .keyDown, .keyUp,
    ]

    /// Runs `body` in a fresh instant transaction (`animation = nil`,
    /// `disablesAnimations = true`), so nothing it changes is
    /// interpolated. The transaction is built per call, not shared.
    private static func withInstantTransaction(_ body: () -> Void) {
        var instant = Transaction()
        instant.animation = nil
        instant.disablesAnimations = true
        withTransaction(instant) { body() }
    }

    /// Pluggable event type provider for testing the runloop hop branch
    /// outside an interactive AppKit environment. Defaults to
    /// `NSApp?.currentEvent?.type`.
    internal static var currentEventTypeProvider: () -> NSEvent.EventType? = {
        NSApp?.currentEvent?.type
    }

    /// Runs `mutation` cleanly out of the current activation turn.
    ///
    /// Must run on the main thread: the type carries no `@MainActor`,
    /// so caller discipline is the contract. It reads
    /// `currentEventTypeProvider()` and schedules on the main queue. Callers
    /// are control action / binding setters, which are main-thread by
    /// construction. `NSApp` is nil before the application object
    /// exists (notably in unit tests) — treated as no turn and the
    /// write lands direct, which is what those tests assert.
    public static func write(_ mutation: @escaping () -> Void) {
        let body = { withInstantTransaction { mutation() } }
        if let type = currentEventTypeProvider(), hopTurns.contains(type) {
            DispatchQueue.main.async(execute: body)
        } else {
            body()
        }
    }

    /// Direct-write variant for accessibility action turns.
    ///
    /// Remote actuation (VoiceOver, Full Keyboard Access's Space, any
    /// out-of-process AX client) arrives as `AXUIElementPerformAction`
    /// IPC, not as an `NSEvent` (research round 6). AppKit does not
    /// clear `NSApp.currentEvent` for such turns, so the generic
    /// `write` sees a stale event from an earlier click and hops to
    /// the next runloop tick — where macOS 26.5 drops every
    /// asynchronous block scheduled during an accessibility turn
    /// (AGENTS.md pitfall 24). Such a write would silently vanish on
    /// real hardware. Accessibility press turns must write direct;
    /// that is what this entry point does.
    public static func writeDirect(_ mutation: @escaping () -> Void) {
        withInstantTransaction { mutation() }
    }

    /// A `Binding` whose writes route through `write`.
    ///
    /// Factory for control bindings (picker selections, toggles) that
    /// mutate state inside the control's activation turn; routing the
    /// write through `write` escapes the turn, so the affected
    /// subtrees swap without interpolation. The commit semantics are
    /// this escape's whole contract and are asserted in
    /// TethernetUIModelTests: outside a user turn (where a unit test
    /// runs, `NSApp.currentEvent == nil`) the write lands synchronously.
    public static func routed<T>(get: @escaping () -> T,
                                 set: @escaping (T) -> Void) -> Binding<T> {
        Binding(get: get, set: { value in
            write { set(value) }
        })
    }
}
