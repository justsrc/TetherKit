import Foundation

// All user-facing strings for the GUI and the helper.
//
// Adding one: add a case to `L10nKey`, then its text to the `localizations`
// switch below. The switch is exhaustive, so a missing entry is a compile
// error.
//
// How to add a language (the wiring deliberately stays in place with one
// language, so this is a bounded change, not a re-architecture):
//
//   1. Mirror the C++ side: add the value to the `Language` enum here in the
//      package, to tn_language_t (include/tethernet/capi/tethernet_c.h), and
//      to the add-a-language checklist at the top of messages.def.
//   2. Give every row of the `localizations` switch one more labeled argument.
//   3. Dispatch on the language in L10n.text(_:in:). The helper already
//      follows the App through the existing setLanguage plumbing.
//   4. Extend LocalizationTests so every table covers every message: parity
//      across languages checks placeholder positions and conversions.
//
// Placeholder rules (checked entry by entry by LocalizationTests):
//
//   Strings go through `String(format:)`, printf style: `%@` strings, `%ld`
//   integers, `%.1f` floats. Positions must be contiguous (1...n). Call sites
//   convert to `Int` / `Double` / `String` before passing: feeding a `UInt32`
//   to `%ld` behaves inconsistently between 32/64-bit, one explicit
//   conversion is the safe path. Reorder with positional forms (`%1$@`,
//   `%2$ld`) when the word order differs.

/// The identifier of one user-facing message.
public enum L10nKey: String, CaseIterable, Sendable {

    // MARK: - Common

    case ok
    case cancel
    case copy
    case copied
    case add
    case apply
    case connect
    case disconnect
    case uninstall
    case ipAddress
    case netmask
    case router
    case notConfigured
    case listSeparator

    // MARK: - Language settings

    case languageLabel
    case languageSystem
    case languageEnglish
    case languageMenuTitle

    // MARK: - Data model (Models.swift)

    case usbDeviceFallbackName
    case ipModeDhcp
    case ipModeManual
    case ipModeNone

    // MARK: - Authorization (Authorization.swift)

    case authorizationCancelled
    case authorizationDenied
    case authorizationSessionFailed
    case authorizationBlobMalformed
    case authorizationRestoreFailed
    case authorizationRightMissing

    // MARK: - Library errors (TethernetError.swift)

    case libraryGenericFailure

    // MARK: - Session events (TethernetSession.swift)

    case eventLinkUp
    case eventLinkDown
    case eventDeviceResetReplayed
    case eventDeviceReset
    case eventNegotiated

    // MARK: - Network validation (NetworkConfigurator.swift)

    case invalidIPAddress
    case invalidNetmask
    case invalidRouter
    case routerRequiredForDefaultRoute
    case invalidDNSServer

    // MARK: - Helper service (HelperService.swift / main.swift / InstallerMode.swift)

    case helperSessionAlreadyRunning
    case helperSessionStopped
    case helperNetworkApplied
    case helperRequestDecodeFailed
    case helperReplyEncodeFailed
    case helperOrphansCleaned
    case helperOrphanCleanupFailed
    case helperSigtermReceived
    case helperReady
    case installerNeedsRoot
    case installerUseTerminal
    case installerScriptMissing

    // MARK: - Helper client (HelperClient.swift / HelperInstaller.swift)

    case helperConnectFailed
    case helperReplyUnparsable
    case installerNoPayload
    case installerAPIUnavailable
    case installerLaunchFailed

    // MARK: - Update check (UpdateChecker.swift)

    case updateNoReleases
    case updateHTTPStatus
    case updateBadResponse

    // MARK: - Finder alias (FinderAlias.swift)

    case aliasSkippedNotBundle
    case aliasSkippedAlreadyInApplications
    case aliasFailed
    case aliasAlreadyPresent
    case aliasCreated

    // MARK: - Connection status (DesignSystem.swift)

    case statusDisconnected
    case statusConnecting
    case statusConnected
    case statusReadyLinkDown
    case statusDisconnecting
    case statusStopped
    case statusFailed

    // MARK: - App model (AppModel.swift)

    case authPromptSession
    case authPromptInstall
    case authPromptUninstall
    case interfaceNotReadyYet
    case installScriptRanButUnreachable
    case uninstallScriptRanButStillAlive
    case scriptOutput
    case scriptNoOutput

    // MARK: - Main window, dialogs and helper management

    case closeWindow
    case menuCheckForUpdates
    case alertOperationFailed
    case confirmUninstallTitle
    case updateCheckTitle
    case openReleasePage
    case copyBrewUpgradeCommand
    case updateUpToDate
    case updateAvailable
    case updateCheckFailed
    case updateDevBuild
    case uninstallExplanation
    case uninstallWhileRunningWarning
    case helperComponentVersion
    case helperVersionMismatch
    case helperVersionMismatchTooltip
    case confirmHelperUpdateTitle
    case helperUpdateWhileRunningWarning
    case helperUpdateBadge
    case uninstallHelperMenuItem
    case checkingHelper
    case needInstallTitle
    case needInstallBody
    case installHelperButton
    case installHelperDetail
    case showConnectFailureDetail
    case helperNeedsUpdateTitle
    case helperNeedsUpdateBody
    case updateHelperButton
    case updateHelperDetail
    case installingProgress
    case authorizationPromptHint
    case installViaTerminal
    case installViaTerminalHint
    case copiedToClipboard
    case copyCommand
    case sysctlNeedsFixTitle
    case sysctlNeedsFixBody

    // MARK: - Device card (DeviceCard.swift)

    case usbDeviceSectionTitle
    case cannotRescanWhileRunning
    case rescanUSBDevices
    case noDeviceDetected
    case deviceChecklistCable
    case deviceChecklistTethering
    case deviceChecklistUnlocked
    case mtuBytes
    case mtuHelp
    case mtuHelpWithLimit
    case mtuTooltip
    case adoptDeviceMAC
    case adoptDeviceMACHelp
    case adoptDeviceMACTooltip
    case compatibilityMode
    case compatibilityModeTooltip

    // MARK: - Log card (LogCard.swift)

    case logDroppedNotice
    case logLevelLabel
    case logLevelAll
    case logLevelDebug
    case logLevelInfo
    case logLevelWarning
    case logLevelError
    case logAutoScroll
    case logCopyAll
    case logClear
    case logRepeatSuffix
    case logRepeatTooltip

    // MARK: - Sidebar pages (SidebarPage.swift)

    case pageConnectionTitle
    case pageNetworkTitle
    case pageThroughputTitle
    case pageLogTitle
    case pageManageTitle
    case manageHelperSectionTitle

    // MARK: - Menu bar panel (MenuBarPanel.swift)

    case downstreamShort
    case upstreamShort
    case noIPConfigured
    case menuBarNoDevice
    case menuBarReady
    case openMainWindow
    case quit
    case quitTooltip

    // MARK: - Network card (NetworkCard.swift)

    case dnsEffectivenessTooltip
    case ipModeLabel
    case connectBeforeConfiguring
    case dhcpHelp
    case dhcpTooltip
    case routerOptional
    case deleteThisEntry
    case addDNSServer
    case setDefaultRoute
    case setDefaultRouteHelp
    case setDefaultRouteTooltip
    case clearConfiguration
    case clearConfigurationTooltip
    case currentlyEffective
    case primaryDefaultRoute
    case notEffective
    case noAddressYet

    // MARK: - Status card (StatusHeroCard.swift)

    case linkUp
    case linkDown
    case transferPaused
    case pendingDevice
    case noRNDISDeviceDetected
    case interfaceLabel
    case interfaceCreating
    case connectDisabledHint

    // MARK: - Throughput card (ThroughputCard.swift)

    case throughputLive
    case throughputDownstream
    case throughputUpstream
    case throughputDownstreamFPS
    case throughputUpstreamFPS
    case chartTime
    case chartRate
    case chartDirection
    case throughputCollecting
    case throughputPlaceholder
    case totalDownstream
    case totalUpstream
    case downstreamFrames
    case upstreamFrames
    case downstreamDropped
    case upstreamDropped
    case kernelDrops
    case transmitBackpressure
}

extension L10nKey {

    /// The text of the message in the shipped language. Adding a language
    /// turns this into a multi-element labeled tuple and gives every case one
    /// more labeled argument; see the checklist at the top of this file.
    // swiftlint:disable:next function_body_length cyclomatic_complexity
    public var localizations: String {
        switch self {

        // MARK: Common

        case .ok: return "OK"
        case .cancel: return "Cancel"
        case .copy: return "Copy"
        case .copied: return "Copied"
        case .add: return "Add"
        case .apply: return "Apply"
        case .connect: return "Connect"
        case .disconnect: return "Disconnect"
        case .uninstall: return "Uninstall"
        case .ipAddress: return "IP address"
        case .netmask: return "Subnet mask"
        case .router: return "Router"
        case .notConfigured: return "Not configured"
        case .listSeparator: return ", "

        // MARK: Language settings

        case .languageLabel: return "Language"
        case .languageSystem: return "Follow system"
        case .languageEnglish: return "English"
        case .languageMenuTitle: return "Language"

        // MARK: Data model

        case .usbDeviceFallbackName: return "USB device %04lx:%04lx"
        case .ipModeDhcp: return "Automatic (DHCP)"
        case .ipModeManual: return "Static IP"
        case .ipModeNone: return "None"

        // MARK: Authorization

        case .authorizationCancelled: return "Authorization cancelled"
        case .authorizationDenied: return "Authorization was denied (%ld)"
        case .authorizationSessionFailed:
            return "Cannot create an authorization session (%ld)"
        case .authorizationBlobMalformed:
            return "The authorization credential is malformed"
        case .authorizationRestoreFailed:
            return "Cannot restore the authorization credential (%ld)"
        case .authorizationRightMissing:
            return "The caller lacks the authorization this operation requires (%ld)"

        // MARK: Library errors

        case .libraryGenericFailure:
            return "The operation failed (error code %ld)"

        // MARK: Session events

        case .eventLinkUp: return "Link is up"
        case .eventLinkDown: return "Link is down"
        case .eventDeviceResetReplayed:
            return "The device soft-reset; addressing information has been replayed"
        case .eventDeviceReset: return "The device soft-reset"
        case .eventNegotiated:
            return "RNDIS negotiation complete: MTU %1$ld, link %2$ld Mbps"

        // MARK: Network validation

        case .invalidIPAddress: return "The IP address is not valid"
        case .invalidNetmask:
            return "The subnet mask is not valid (it must be contiguous, e.g. 255.255.255.0)"
        case .invalidRouter: return "The router address is not valid"
        case .routerRequiredForDefaultRoute:
            return "Routing traffic through this interface by default requires a router address"
        case .invalidDNSServer:
            return "DNS server %@ is not a valid address"

        // MARK: Helper service

        case .helperSessionAlreadyRunning: return "A session is already running"
        case .helperSessionStopped: return "Session stopped"
        case .helperNetworkApplied:
            return "Applied the network configuration: %@"
        case .helperRequestDecodeFailed:
            return "Cannot decode the request arguments: %@"
        case .helperReplyEncodeFailed: return "Cannot encode the reply: %@"
        case .helperOrphansCleaned:
            return "Cleaned up %ld leftover virtual interface(s) from a previous run"
        case .helperOrphanCleanupFailed:
            return "Failed to clean up leftover virtual interfaces: %@"
        case .helperSigtermReceived: return "Received SIGTERM; shutting down"
        case .helperReady: return "tethernet-helper ready: %@"
        case .installerNeedsRoot:
            return "Error: cannot obtain root (euid=%1$ld, ruid=%2$ld)."
        case .installerUseTerminal:
            return "Use the terminal instead: sudo %@"
        case .installerScriptMissing: return "Error: script %@ not found"

        // MARK: Helper client

        case .helperConnectFailed:
            return "Cannot reach the privileged helper: %@"
        case .helperReplyUnparsable:
            return "The helper's reply could not be parsed; the versions may not match"
        // Both of these two messages end with "use the terminal instead", so they
        // share one row instead of repeating the same closing clause.
        case .installerNoPayload:
            return "This copy of Tethernet carries no installer payload (likely a dev build). "
                    + "Run this in a terminal: sudo ./gui/Scripts/install-helper.sh"
        case .installerAPIUnavailable:
            return "This version of macOS no longer offers the API in-app installation needs. "
                    + "Run this in a terminal: sudo ./gui/Scripts/install-helper.sh"
        case .installerLaunchFailed:
            return "Cannot launch the install/uninstall program (%ld)"

        // MARK: Update check

        case .updateNoReleases: return "The repository has no releases yet"
        case .updateHTTPStatus: return "GitHub returned %ld"
        case .updateBadResponse: return "The response was not in the expected format"

        // MARK: Finder alias

        case .aliasSkippedNotBundle:
            return "Skipped: not an .app bundle (dev build)"
        case .aliasSkippedAlreadyInApplications:
            return "Skipped: the app already lives in /Applications"
        case .aliasFailed:
            return "Failed: could not write the alias to /Applications or ~/Applications"
        case .aliasAlreadyPresent: return "Alias already in place: %@"
        case .aliasCreated: return "Created alias: %1$@ -> %2$@"

        // MARK: Connection status

        case .statusDisconnected: return "Not connected"
        case .statusConnecting: return "Connecting"
        case .statusConnected: return "Connected"
        case .statusReadyLinkDown: return "Ready (link down)"
        case .statusDisconnecting: return "Disconnecting"
        case .statusStopped: return "Disconnected"
        case .statusFailed: return "Connection failed"

        // MARK: App model

        case .authPromptSession:
            return "Tethernet needs administrator privileges to create the virtual interface, "
                    + "open the data link and configure IP addresses."
        case .authPromptInstall:
            return "Tethernet needs administrator privileges to install the privileged helper "
                    + "(copy it to /Library/PrivilegedHelperTools and register the system "
                    + "service)."
        case .authPromptUninstall:
            return "Tethernet needs administrator privileges to uninstall the privileged helper "
                    + "(unregister the system service and delete its files under /Library)."
        case .interfaceNotReadyYet:
            return "The virtual interface does not exist yet -- connect a device first"
        case .installScriptRanButUnreachable:
            return "The install script finished, but the helper is still unreachable."
        case .uninstallScriptRanButStillAlive:
            return "The uninstall script finished, but the helper is still responding."
        case .scriptOutput: return "%1$@Script output:\n%2$@"
        case .scriptNoOutput: return "(no output)"

        // MARK: Main window, dialogs and helper management

        case .closeWindow: return "Close Window"
        case .menuCheckForUpdates: return "Check for Updates…"
        case .alertOperationFailed: return "Operation failed"
        case .confirmUninstallTitle: return "Uninstall the privileged helper?"
        case .updateCheckTitle: return "Check for updates"
        case .openReleasePage: return "Open the release page"
        case .copyBrewUpgradeCommand: return "Copy the brew upgrade command"
        case .updateUpToDate:
            return "You are on the latest version (v%@)."
        case .updateAvailable:
            return "Version v%@ is available. If you installed via Homebrew, run "
                    + "brew upgrade tethernet in a terminal; if you build from source, pull the "
                    + "latest code and rebuild."
        case .updateCheckFailed: return "The check could not be completed: %@"
        case .updateDevBuild:
            return "This is a development build with no version number, so there is nothing to "
                    + "compare against."
        case .uninstallExplanation:
            return "This unregisters the system service and deletes the helper files under "
                    + "/Library. You can reinstall it at any time."
        case .uninstallWhileRunningWarning:
            return "The current connection will be dropped and the virtual interface destroyed."
        case .helperComponentVersion: return "Helper · %@"
        case .helperVersionMismatch: return "Helper %1$@ · app %2$@"
        case .helperVersionMismatchTooltip:
            return "The installed helper is still %1$@ while the app is %2$@. The library inside "
                    + "the helper is what does the actual work, so anything the new version fixes "
                    + "stays unfixed until the helper is updated too. One click reinstalls the "
                    + "helper the app ships with; macOS will ask for an administrator password "
                    + "once."
        case .confirmHelperUpdateTitle: return "Update the privileged helper?"
        case .helperUpdateWhileRunningWarning:
            return "Updating unregisters the old helper first, so the current connection will be "
                    + "dropped and the virtual interface destroyed. Just reconnect once it is "
                    + "installed."
        case .helperUpdateBadge: return "%@ available"
        case .uninstallHelperMenuItem: return "Uninstall the helper…"
        case .checkingHelper: return "Checking the privileged helper…"
        case .needInstallTitle: return "The privileged helper must be installed"
        case .needInstallBody:
            return "Creating the virtual interface and opening the data link require\n"
                    + "administrator privileges. Tethernet keeps that work in a separate\n"
                    + "background component so the app itself runs as a normal user."
        case .installHelperButton: return "Install the helper"
        case .installHelperDetail:
            return "It is installed to /Library/PrivilegedHelperTools and registered\n"
                    + "as a LaunchDaemon. This page recovers on its own afterwards --\n"
                    + "no need to restart the app."
        case .showConnectFailureDetail:
            return "Show why the connection failed"
        case .helperNeedsUpdateTitle: return "The privileged helper needs updating"
        case .helperNeedsUpdateBody:
            return "The installed helper predates this app and speaks a different protocol "
                    + "(helper v%1$@, app needs v%2$@)."
        case .updateHelperButton: return "Update the helper"
        case .updateHelperDetail:
            return "Updating unregisters the old version before installing the new one. "
                    + "This page recovers on its own afterwards."
        case .installingProgress: return "Installing…"
        case .authorizationPromptHint:
            return "macOS will ask once for an administrator password"
        case .installViaTerminal: return "Install from the terminal instead"
        case .installViaTerminalHint:
            return "Run this from the repository root; it does exactly what the button does."
        case .copiedToClipboard: return "Copied to the clipboard"
        case .copyCommand: return "Copy the command"
        case .sysctlNeedsFixTitle: return "System parameters need adjusting"
        case .sysctlNeedsFixBody:
            return "These switches are snapshotted into the virtual interface **when it is\n"
                    + "created**, so changing them afterwards has no effect. Fix them before\n"
                    + "connecting:"

        // MARK: Device card

        case .usbDeviceSectionTitle: return "USB device"
        case .cannotRescanWhileRunning:
            return "The device list cannot be refreshed while running"
        case .rescanUSBDevices: return "Rescan USB devices"
        case .noDeviceDetected: return "No device detected"
        case .deviceChecklistCable:
            return "1. The cable carries data and is not charge-only"
        case .deviceChecklistTethering:
            return "2. \"USB tethering\" is switched on for the phone"
        case .deviceChecklistUnlocked:
            return "3. The phone is unlocked and trusts this Mac"
        case .mtuBytes: return "%@ bytes"
        case .mtuHelp:
            return "Values beyond the device's capability are lowered during negotiation."
        case .mtuHelpWithLimit:
            return "Values beyond the device's capability are lowered during negotiation "
                    + "(this Mac allows up to %ld)."
        case .mtuTooltip:
            return "Negotiation honours whatever the device reports, so an oversized value is "
                    + "lowered automatically. The ceiling comes from the system's "
                    + "net.link.fake.max_mtu."
        case .adoptDeviceMAC: return "Adopt the MAC address the device reports"
        case .adoptDeviceMACHelp:
            return "Only turn this off when investigating a MAC address conflict."
        case .adoptDeviceMACTooltip:
            return "Under RNDIS the device *is* the network interface, so the peer's ARP table "
                    + "and DHCP leases are all keyed on the device's MAC."
        case .compatibilityMode: return "Compatibility mode"
        case .compatibilityModeTooltip:
            return "This device's CDC descriptors are non-standard, so the interface numbers "
                    + "were inferred using the usual Android layout"

        // MARK: Log card

        case .logDroppedNotice:
            return "%ld log lines were dropped because the buffer filled up"
        case .logLevelLabel: return "Level"
        case .logLevelAll: return "All"
        case .logLevelDebug: return "Debug"
        case .logLevelInfo: return "Info"
        case .logLevelWarning: return "Warning"
        case .logLevelError: return "Error"
        case .logAutoScroll: return "Auto-scroll"
        case .logCopyAll: return "Copy all log lines"
        case .logClear: return "Clear the log"
        case .logRepeatSuffix: return " (x%1$ld, since %2$@)"
        case .logRepeatTooltip:
            return "This line repeated %1$ld times in a row, first at %2$@"

        // MARK: Menu bar panel

        case .downstreamShort: return "Down"
        case .upstreamShort: return "Up"
        case .noIPConfigured: return "No IP address"
        case .menuBarNoDevice:
            return "No device detected. Connect a phone with a data cable and turn on USB "
                    + "tethering."
        case .menuBarReady:
            return "Ready: %@. Open the main window to connect."
        case .openMainWindow: return "Open the main window"
        case .quit: return "Quit"
        case .quitTooltip:
            return "Quitting only closes the interface; an established connection keeps running."

        // MARK: Network card

        case .dnsEffectivenessTooltip:
            return "In manual mode, whether DNS takes effect is up to the system resolver, so "
                    + "trust what \"Currently effective\" reads back."
        case .ipModeLabel: return "Configure IPv4"
        case .connectBeforeConfiguring:
            return "Connect a device before configuring the network"
        case .dhcpHelp:
            return "The system asks the device for an address; DNS and routes are set up "
                    + "automatically"
        case .dhcpTooltip:
            return "Nearly every phone's USB tethering runs its own DHCP server, so this is the "
                    + "recommended choice. Applying it waits up to 10 seconds; a timeout usually "
                    + "means tethering is not enabled on the device."
        case .routerOptional: return "Optional"
        case .deleteThisEntry: return "Remove this entry"
        case .addDNSServer: return "Add a DNS server (up to 4)"
        case .setDefaultRoute: return "Route all traffic through this interface"
        case .setDefaultRouteHelp:
            return "When off, only traffic explicitly bound to this interface uses it."
        case .setDefaultRouteTooltip:
            return "When off, the rest of your traffic keeps using the current primary network. "
                    + "If this Mac has no other network available you usually do not need this -- "
                    + "the system picks this interface as the primary service on its own."
        case .clearConfiguration: return "Clear the configuration"
        case .clearConfigurationTooltip:
            return "Equivalent to ipconfig set <interface> NONE; removes the address and its "
                    + "routes"
        case .currentlyEffective: return "Currently effective"
        case .primaryDefaultRoute: return "Primary default route"
        case .notEffective: return "Not in effect"
        case .noAddressYet:
            return "%@ has no IP address yet. Pick a configuration and press Apply."

        // MARK: Status card

        case .linkUp: return "Link up"
        case .linkDown: return "Link down"
        case .transferPaused: return "Transfer paused"
        case .pendingDevice: return "Ready to connect: %@"
        case .noRNDISDeviceDetected:
            return "No RNDIS device detected. Connect a phone with a data cable and turn on "
                    + "\"USB tethering\" on the phone."
        case .interfaceLabel: return "Interface"
        case .interfaceCreating: return "Creating…"
        case .connectDisabledHint:
            return "Connect a device with USB tethering switched on first"

        // MARK: Throughput card

        case .throughputLive: return "Live"
        case .throughputDownstream: return "Down (device -> Mac)"
        case .throughputUpstream: return "Up (Mac -> device)"
        case .throughputDownstreamFPS: return "Down frame rate"
        case .throughputUpstreamFPS: return "Up frame rate"
        case .chartTime: return "Time"
        case .chartRate: return "Rate"
        case .chartDirection: return "Direction"
        case .throughputCollecting: return "Collecting…"
        case .throughputPlaceholder:
            return "Live throughput appears once you connect"
        case .totalDownstream: return "Total down"
        case .totalUpstream: return "Total up"
        case .downstreamFrames: return "Down frames"
        case .upstreamFrames: return "Up frames"
        case .downstreamDropped: return "Down dropped"
        case .upstreamDropped: return "Up dropped"
        case .kernelDrops: return "Kernel drops"
        case .transmitBackpressure: return "TX backpressure"

        // MARK: Sidebar pages

        case .pageConnectionTitle: return "Connection"
        case .pageNetworkTitle: return "Network"
        case .pageThroughputTitle: return "Throughput"
        case .pageLogTitle: return "Log"
        case .pageManageTitle: return "Manage"
        case .manageHelperSectionTitle: return "Privileged helper"
        }
    }
}
