import ApplicationServices
import Foundation

/// Reads our own process's accessibility tree.
///
/// An application may probe ITS OWN tree without any TCC permission —
/// the Accessibility trust boundary exists to gate other processes'
/// trees (and screen contents), not self-inspection. The same technique
/// the round-7 probe exercised (torn down with the verification), kept
/// as a runtime capability.
///
/// Round 11: the Return claim in `SidebarMainWindow` reads the
/// SYSTEM-WIDE focus track (below) — the app-level answer is our own
/// `@AccessibilityFocusState` vend, which the focus engine never
/// updates (round-10 probe fact) and which therefore only ever
/// reported the last anchor. The app-level readers remain for the
/// headless keyboard probe's diagnostics.
enum SelfAccessibility {

#if DEBUG
    /// Probe override for the pane toggle
    /// (`TETHERNET_FKA_OVERRIDE=0|1`). Headless keyboard probes must
    /// pin the pane state regardless of what System Settings currently
    /// shows; DEBUG-only so shipped builds always read the real
    /// preference.
    private static var fullKeyboardAccessOverride: Bool? {
        guard let raw = ProcessInfo.processInfo.environment["TETHERNET_FKA_OVERRIDE"] else {
            return nil
        }
        return raw == "1"
    }
#endif

    /// The Accessibility pane's Full Keyboard Access toggle
    /// (universalaccessd's `keyboardAccessEnabled` preference), read
    /// fresh at every call. This is the system state the sidebar's
    /// Space/Return actuation is keyed to — NOT
    /// `NSApp.isFullKeyboardAccessEnabled`, which reports the legacy
    /// `AppleKeyboardUIMode` bit (Keyboard ▸ Keyboard Navigation) and
    /// stays `true` while the pane toggle is off (both probed on
    /// macOS 26.5, round 7). Keyboard focus on a row without the pane
    /// toggle is stale-browsing state (round 7), so plain Space/Return
    /// anywhere must stay inert.
    static var isFullKeyboardAccessOn: Bool {
#if DEBUG
        if let forced = fullKeyboardAccessOverride { return forced }
#endif
        return (CFPreferencesCopyAppValue("keyboardAccessEnabled" as CFString,
                                          "com.apple.universalaccess" as CFString) as? Bool) ?? false
    }

#if DEBUG
    /// The description attribute of the keyboard-focused element of our
    /// own application, or `nil` when focus is unset or the focused
    /// element carries no description. Synchronous, and only as
    /// expensive as the self-AX round-trip. Probe-only: the sole
    /// caller is `KeyboardProbe.observe`, which samples on a timer.
    static func focusedDescription() -> String? {
        guard let element = focusedElement() else { return nil }
        return stringAttribute(kAXDescriptionAttribute, on: element)
    }

    /// The AX role of the focused element — probe discriminator for
    /// opaque AppKit key-view proxies (they answer neither role nor
    /// title, SwiftUI buttons answer `AXButton` plus their title).
    static func focusedRole() -> String? {
        guard let element = focusedElement() else { return nil }
        return stringAttribute(kAXRoleAttribute, on: element)
    }

    /// The AX title of the focused element — probe discriminator (see
    /// `focusedRole`).
    static func focusedTitle() -> String? {
        guard let element = focusedElement() else { return nil }
        return stringAttribute(kAXTitleAttribute, on: element)
    }

    /// The on-screen frame of the keyboard-focused element, or `nil`
    /// when unfocused or geometry is unavailable. The travel companion
    /// of the attribute readers: opaque key-view proxies answer no
    /// role / title / description (run-1 fact), but their geometry
    /// still tells WHERE focus sits — sidebar column vs page content.
    /// Probe-only discriminator.
    static func focusedFrame() -> CGRect? {
        guard let element = focusedElement() else { return nil }
        var positionRef: CFTypeRef?
        guard AXUIElementCopyAttributeValue(element,
                                            kAXPositionAttribute as CFString,
                                            &positionRef) == .success,
              let positionValue = positionRef else {
            return nil
        }
        var sizeRef: CFTypeRef?
        guard AXUIElementCopyAttributeValue(element,
                                            kAXSizeAttribute as CFString,
                                            &sizeRef) == .success,
              let sizeValue = sizeRef else {
            return nil
        }
        var point = CGPoint.zero
        var size = CGSize.zero
        guard CFGetTypeID(positionValue) == AXValueGetTypeID(),
              AXValueGetValue(positionValue as! AXValue, .cgPoint, &point),
              CFGetTypeID(sizeValue) == AXValueGetTypeID(),
              AXValueGetValue(sizeValue as! AXValue, .cgSize, &size) else {
            return nil
        }
        return CGRect(origin: point, size: size)
    }

    private static func focusedElement() -> AXUIElement? {
        let app = AXUIElementCreateApplication(ProcessInfo.processInfo.processIdentifier)
        var ref: CFTypeRef?
        guard AXUIElementCopyAttributeValue(app,
                                            kAXFocusedUIElementAttribute as CFString,
                                            &ref) == .success,
              let value = ref,
              CFGetTypeID(value) == AXUIElementGetTypeID() else {
            return nil
        }
        return value as! AXUIElement
    }
#endif

    // MARK: - System-wide focus track (round 11)

    /// The keyboard-focused element as the SYSTEM reports it — read on
    /// the system-wide element, and accepted ONLY when it belongs to
    /// this process. This is the channel the Full Keyboard Access
    /// focus track answers through: a Return press (which
    /// universalaccessd passes through untouched, round 6) must
    /// actuate what this read resolves, or nothing.
    ///
    /// Note on permissions: unlike inspecting our own application's
    /// AX tree, querying `AXUIElementCreateSystemWide()` for
    /// `kAXFocusedUIElement` is a cross-process system-wide read that
    /// requires Accessibility trust (TCC). When untrusted (`!AXIsProcessTrusted()`),
    /// this returns nil (avoiding `kAXErrorCannotComplete`), and the
    /// window-level claim yields to the focused row's `.onKeyPress(.return)`
    /// backstop.
    ///
    /// Deliberately NOT the app-level `kAXFocusedUIElement` answer:
    /// that is the `@AccessibilityFocusState` vend (see the type doc).
    /// The PID acceptance also keeps foreign elements out — if the
    /// focused element lives in another process, this answers nil and
    /// the key keeps its normal behavior.
    private static func systemWideFocusedElementOfOwnProcess() -> AXUIElement? {
        guard AXIsProcessTrusted() else {
#if DEBUG
            print("PROBE [syswide-read] process untrusted for system-wide AX read (falling back to row backstop)")
#endif
            return nil
        }
        let systemWide = AXUIElementCreateSystemWide()
        var ref: CFTypeRef?
        let result = AXUIElementCopyAttributeValue(systemWide,
                                                   kAXFocusedUIElementAttribute as CFString,
                                                   &ref)
        guard result == .success else {
#if DEBUG
            print("PROBE [syswide-read] error=\(result.rawValue)")
#endif
            return nil
        }
        guard let value = ref,
              CFGetTypeID(value) == AXUIElementGetTypeID() else {
#if DEBUG
            print("PROBE [syswide-read] focus unset or not an AX element")
#endif
            return nil
        }
        let element = value as! AXUIElement
        var pid: Int32 = 0
        AXUIElementGetPid(element, &pid)
        guard pid == ProcessInfo.processInfo.processIdentifier else {
#if DEBUG
            print("PROBE [syswide-read] foreign pid=\(pid)")
#endif
            return nil
        }
        return element
    }

    /// The description attribute of the system's keyboard-focused
    /// element when it sits in our own process, or `nil`. Synchronous;
    /// callers gate it behind a key event, so the one AX round-trip is
    /// acceptable.
    static func systemWideFocusedDescription() -> String? {
        guard let element = systemWideFocusedElementOfOwnProcess() else { return nil }
        return stringAttribute(kAXDescriptionAttribute, on: element)
    }

#if DEBUG
    /// The AX role of the system's focused element — probe
    /// discriminator (see `focusedRole`).
    static func systemWideFocusedRole() -> String? {
        guard let element = systemWideFocusedElementOfOwnProcess() else { return nil }
        return stringAttribute(kAXRoleAttribute, on: element)
    }

    /// The AX title of the system's focused element — probe
    /// discriminator (see `focusedRole`).
    static func systemWideFocusedTitle() -> String? {
        guard let element = systemWideFocusedElementOfOwnProcess() else { return nil }
        return stringAttribute(kAXTitleAttribute, on: element)
    }
#endif

    private static func stringAttribute(_ attribute: String, on element: AXUIElement) -> String? {
        var value: CFTypeRef?
        guard AXUIElementCopyAttributeValue(element,
                                            attribute as CFString,
                                            &value) == .success,
              let text = value as? String,
              !text.isEmpty else {
            return nil
        }
        return text
    }
}
