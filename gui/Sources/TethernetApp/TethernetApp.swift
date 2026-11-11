import AppKit
import SwiftUI
import TethernetIPC

/// The graphical interface entry point of Tethernet.
///
/// The app runs as an **ordinary user** and needs no privileges of its own.
/// Everything that requires root (creating the virtual interface, opening
/// BPF, configuring the IP) is handed to tethernet-helper, each call
/// carrying an authorization credential the user confirmed. See
/// docs/GUI-ARCHITECTURE.md.
///
/// Background running mode:
///
///   Closing the main window does not quit: the app drops to "menu bar
///   only" (the accessory activation policy hides the dock icon too), and
///   the menu bar item keeps showing live rates. The session runs in the
///   helper anyway, so whether the app exists does not affect it -- but as
///   long as the app lives, the user has an entry point they can glance at
///   any time.
///
///   Polling does **not** stop when the window closes (the menu bar feeds
///   on it); it only slows down to once every 2 seconds while "the window
///   is closed and no session is running", see AppModel.pollDelay.
///
/// Window ownership
///
///   The main window is an AppKit-managed window (`MainWindowManager`):
///   a plain NSWindow (fullSizeContentView, transparent, title hidden)
///   hosting SwiftUI. The window chrome comes from the executable's
///   `sdk` stamp, not from which layer owns the window; see
///   `MainWindowManager` and the linker settings in `Package.swift`.
@main
struct TethernetApplication: App {
    @State private var model = AppModel()
    @NSApplicationDelegateAdaptor(TethernetAppDelegate.self) private var appDelegate

    init() {
        // The mode used by the CI smoke test and for manual rebuilds:
        // create the Finder alias and exit, never entering the GUI.
        // Always exit 0 -- failing to create the alias (a managed machine
        // and the like) must not fail the flow, and the result goes to
        // stdout. (The brew postinstall also runs in a sandbox and cannot
        // write /Applications -- confirmed by measurement -- so the normal
        // install path relies on the automatic creation at first launch
        // below.)
        if CommandLine.arguments.dropFirst().contains("--install-finder-alias") {
            print(FinderAlias.ensure(for: Bundle.main.bundleURL))
            exit(0)
        }
        // Bare executables (swift build output, no bundle id) launch
        // with the .prohibited activation policy. The main window is
        // ours (MainWindowManager) and is presented by the delegate
        // once launch finishes; the policy still gates whether the app
        // can become key, so normalize it here. A packaged .app gets
        // .regular from LaunchServices and never hits this branch.
        if NSApplication.shared.activationPolicy() == .prohibited {
            NSApplication.shared.setActivationPolicy(.regular)
        }
        TethernetAppDelegate.launchModel = model
        // Normal launch: check it in the background as a side task -- add
        // it if missing, and rewrite it if the target drifted after brew
        // upgrade changed the Cellar path.
        let bundleURL = Bundle.main.bundleURL
        Task.detached(priority: .utility) {
            FinderAlias.ensure(for: bundleURL)
        }
    }

    var body: some Scene {
        // Persistent menu bar item: icon + live rate.
        // The .window style gives it a small panel, which can hold a
        // Language submenu and live values that menu items cannot.
        // Commands live on this scene -- there is no Window scene
        // anymore; the main window belongs to AppKit (see
        // MainWindowManager).
        MenuBarExtra {
            MenuBarPanel(model: model)
        } label: {
            MenuBarLabel(model: model)
        }
        .menuBarExtraStyle(.window)
        .commands {
            // "New Window" makes no sense for a single-window app; drop
            // it so the user does not click something that does nothing.
            CommandGroup(replacing: .newItem) {}
            // Cmd-W: no Window scene means no built-in system Close, so
            // this menu item hides the main window and drops to
            // menu-bar mode (same as the red close button).
            CommandGroup(after: .newItem) { FileMenuItems(model: model) }
            // "Check for Updates..." goes in the customary place under
            // "About" in the app menu. It only checks and never replaces --
            // the reason is in the type notes of UpdateChecker.swift.
            CommandGroup(after: .appInfo) { AppMenuItems(model: model) }
        }
    }
}

/// Tethernet's own items in the app menu.
///
/// Why this is wrapped in a View instead of spreading the Buttons directly
/// inside the CommandGroup:
///
///   Spread out, each Button's label is an independent expression, and
///   SwiftUI only re-evaluates it when "something it depends on changed".
///   The strings come from a global table lookup, so
///   `L(.menuCheckForUpdates)` depends on no @Observable property -- after
///   a language switch the language menu updated itself (it reads
///   `languagePreference`) while the "Check for Updates..." next to it
///   kept the old language. Confirmed by measurement.
///
///   Wrapping the group in a View and reading `languageRevision` once in
///   its body re-evaluates the whole group together.
private struct AppMenuItems: View {
    var model: AppModel

    var body: some View {
        // Read it to establish the dependency; the value itself is not
        // used.
        let _ = model.languageRevision

        Button(L(.menuCheckForUpdates)) {
            Task { await model.checkForUpdates() }
        }
        // Language lives in the App menu instead of the main window:
        // switching is a set-it-once action, and the main window has no
        // spare vertical space for another row.
        LanguageMenu(model: model)
    }
}

/// The language choice in the App menu.
///
/// Every option shows its native name ("English") and is deliberately not
/// translated with the interface: the menu exists for the person who cannot
/// read the interface right now, so the way out must stay readable.
/// "Follow system" is the one exception: it describes behavior, not a
/// language, so it follows the interface.
private struct LanguageMenu: View {
    var model: AppModel

    var body: some View {
        Menu(L(.languageMenuTitle)) {
            Picker(L(.languageLabel), selection: Bindable(model).languagePreference) {
                Text(L(.languageSystem)).tag(LanguagePreference.system)
                Text(verbatim: "English").tag(LanguagePreference.english)
            }
            .pickerStyle(.inline)
            .labelsHidden()
        }
    }
}

/// File > Close Window (Cmd-W).
///
/// Same reason as AppMenuItems above: a flat Button in a CommandGroup
/// picks its label from the global string table, which depends on no
/// @Observable, so a language switch never re-evaluates it (AGENTS.md
/// pitfall 18). Reading languageRevision here re-labels the item.
private struct FileMenuItems: View {
    var model: AppModel

    var body: some View {
        let _ = model.languageRevision
        Button(L(.closeWindow)) { MainWindowManager.shared.closeMainWindow() }
            .keyboardShortcut("w")
    }
}

/// Main window root: fixed-size content + poll startup.
///
/// The window itself is AppKit-managed (`MainWindowManager`); closing
/// it hides the window (orderOut, not destroy) and the app drops to
/// menu-bar mode.
struct MainWindowRoot: View {
    var model: AppModel

    var body: some View {
        ContentView(model: model)
            .frame(width: Design.Window.width, height: Design.Window.height)
            .handleSelfCaptureRequest()
            // The strings come from a global table lookup, so changing the
            // language makes no @Observable property "look" changed and
            // SwiftUI therefore does not re-evaluate the view body. An
            // explicit revision number is used as the identity to force the
            // whole tree to rebuild -- a language switch is an extremely
            // rare action, so the cost does not matter.
            .id(model.languageRevision)
            .task {
                // Polling starts here (idempotent). It keeps running
                // after the window closes -- that is exactly what lets the
                // menu bar update in real time.
                model.start()
            }
    }
}

// MARK: - Launch-time window presentation
//
//   The main window is created and presented by the app delegate
//   (MainWindowManager), not by a SwiftUI scene. The model crosses
//   to the delegate through the static slot below: the adaptor hands
//   us an instance, but setting instance state on it from App.init
//   is init-order dependent, so the launch slot is the deterministic
//   channel.
final class TethernetAppDelegate: NSObject, NSApplicationDelegate {
    static var launchModel: AppModel?

    func applicationDidFinishLaunching(_ notification: Notification) {
        guard let model = Self.launchModel else { return }
        MainWindowManager.shared.present(model)
    }

    /// Double-clicking the app icon (e.g. in Finder) asks to reopen
    /// the window. SwiftUI's scene lifecycle never delivered this
    /// callback; reopen shows the main window.
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows: Bool) -> Bool {
        // hasVisibleWindows cannot decide this: the menu-bar panel is
        // a window too, so a hidden main window behind an open panel
        // still counts as "visible". Ask about the main window itself.
        if !MainWindowManager.shared.mainWindowIsVisible, let model = Self.launchModel {
            MainWindowManager.shared.present(model)
        }
        return true
    }

    /// Never quit just because the last window closed -- the app is a
    /// menu-bar accessory by design. Close hides the window; the
    /// menu-bar item stays live.
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        false
    }
}
