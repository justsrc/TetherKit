import Foundation
import Observation
import SwiftUI
import TethernetCore
import TethernetIPC

/// Instantaneous rate measured by one sample.
struct ThroughputSample: Identifiable {
    let id = UUID()
    let timestamp: Date
    let receiveBitsPerSecond: Double
    let transmitBitsPerSecond: Double
    let receivePacketsPerSecond: Double
    let transmitPacketsPerSecond: Double

    static let zero = ThroughputSample(timestamp: .distantPast, receiveBitsPerSecond: 0,
                                       transmitBitsPerSecond: 0, receivePacketsPerSecond: 0,
                                       transmitPacketsPerSecond: 0)
}

/// Consecutive repeated log lines collapse into a single row.
///
/// A periodic path (the device enumeration every 2 seconds, for example)
/// repeats the same sentence down the whole column and buries the lines
/// that actually carry information; collapsing them into "one line x N"
/// carries the same information. The id comes from the first entry -- a
/// repeat only updates the count and the timestamp, the row's identity does
/// not change, so the list never needs a full rebuild.
struct CollapsedLogEntry: Identifiable {
    let first: LogEntry
    private(set) var latest: LogEntry
    private(set) var count: Int = 1

    init(_ entry: LogEntry) {
        first = entry
        latest = entry
    }

    var id: UUID { first.id }

    /// Only entries identical apart from the timestamp count as repeats --
    /// a message that carries a changing value (an address, a count)
    /// deserves a line of its own.
    func matches(_ entry: LogEntry) -> Bool {
        entry.level == first.level && entry.thread == first.thread
            && entry.message == first.message
    }

    mutating func absorb(_ entry: LogEntry) {
        latest = entry
        count += 1
    }
}

/// Helper reachability. The UI uses it to decide between "show the install
/// guide" and "work normally".
enum HelperAvailability: Equatable {
    case unknown
    case available(version: String)
    case missing(reason: String)
    /// The installed helper is older (or newer) than the current app, so
    /// the XPC interface does not match.
    ///
    /// A state of its own rather than folded into missing: the two cases
    /// need different remedies -- one is "go install it", the other is "go
    /// reinstall it" -- and the wording has to tell them apart.
    case outdated(installed: Int, expected: Int)

    var isAvailable: Bool {
        if case .available = self { return true }
        return false
    }
}

/// The installed privileged helper is not the same version as the one the
/// app ships.
///
/// Division of labor with `HelperAvailability.outdated`:
///   That one means **the protocol does not match**: the method signatures
///   already differ, so any further call either hangs or crashes, which is
///   why it has to be blocked on the install guide page. This one means
///   **the protocol is still compatible but the versions differ** -- the
///   helper works as usual, it just still carries the pre-upgrade
///   libtethernet, so none of the problems the new version fixes are fixed
///   on the side that actually does the work (`brew upgrade` only swaps the
///   .app and cannot touch /Library/PrivilegedHelperTools).
///
///   So it does not block anything; it only lights up an "update the
///   privileged helper" action in the management row. Versions that keep
///   the protocol number (0.1.4 -> 0.1.5 and the like) used to give no
///   hint at all, and the only trace a user could notice was "the problem
///   the release notes say is fixed is still there".
struct HelperVersionMismatch: Equatable {
    /// The installed one, version number only (for example `"0.1.3"`).
    let installed: String
    /// The one the app ships -- the version that gets installed once the
    /// button is pressed.
    let expected: String
}

/// The result of a manual "Check for Updates...", which drives a popup.
enum UpdateCheckResult: Equatable {
    case upToDate(current: String)
    case updateAvailable(UpdateChecker.Release)
    case failed(String)
    /// A development build (the bare executable from `swift run`) has no
    /// version number, so there is nothing to compare.
    case unavailable
}

/// All state and actions of the UI.
///
/// Why everything goes through the helper instead of the app calling the
/// library itself:
///   The app runs as an ordinary user, and creating feth and opening BPF
///   both need root, so the session can only run inside the helper. Device
///   enumeration and the environment preflight do not need root either, but
///   they go through the helper as well -- once a session runs, the device
///   is already exclusively held by the helper, and the app reading it
///   would only produce inconsistent results.
@Observable
@MainActor
final class AppModel {
    // MARK: - State exposed to the UI

    private(set) var helperAvailability: HelperAvailability = .unknown
    /// The two version numbers when the installed helper and the app
    /// disagree; nil when they match (or have not been probed yet).
    private(set) var helperVersionMismatch: HelperVersionMismatch?
    private(set) var environment: EnvironmentReport?
    private(set) var devices: [DeviceDescriptor] = []
    private(set) var status: SessionStatus = .idle
    private(set) var networkState: NetworkState = .empty
    private(set) var throughput: ThroughputSample = .zero
    private(set) var throughputHistory: [ThroughputSample] = []
    private(set) var logs: [LogEntry] = []
    private(set) var droppedLogCount: UInt64 = 0

    /// The device the user picked in the device list. nil means "use the
    /// first one found".
    var selectedDeviceID: String?
    /// The parts of the session configuration the user can adjust.
    var requestedMTU: UInt32 = 1500
    var adoptDeviceMAC: Bool = true
    /// The network configuration form.
    var networkConfiguration: NetworkConfiguration = .dhcp

    /// Interface language preference. Changing it does **three** things at
    /// once: switch the Swift-side string table, push the language to
    /// libtethernet (otherwise another language leaks into the log card),
    /// and push it to the helper (it runs as root under launchd and cannot
    /// see the user's language preference).
    ///
    /// `didSet` also bumps `languageRevision` -- the strings come from a
    /// global table lookup, so SwiftUI has no way of knowing they changed;
    /// this value is what forces the whole view tree to rebuild once.
    var languagePreference: LanguagePreference = .system {
        didSet {
            guard languagePreference != oldValue else { return }
            applyLanguage(languagePreference)
        }
    }

    /// How many times the language changed. The view root carries
    /// `.id(model.languageRevision)`, which is what triggers the rebuild.
    private(set) var languageRevision = 0

    /// A privileged operation is in flight -- the UI uses this to disable
    /// buttons and show progress.
    private(set) var isBusy: Bool = false
    /// An error that has to be shown to the user.
    var alertMessage: String?

    /// The known new version. Drives the "a new version is available" hint
    /// in the management row; nil = none, or not checked yet.
    private(set) var availableUpdate: UpdateChecker.Release?
    /// The result popup of the manual "Check for Updates...". The view
    /// resets it to nil when the popup closes.
    var updateCheckResult: UpdateCheckResult?

    var logLevelFilter: LogLevel = .info

    // MARK: - Internals

    private let client = HelperClient()
    private var pollingTask: Task<Void, Never>?
    /// The previous status snapshot, used to compute rates by differencing.
    private var previousStatus: SessionStatus?
    private var sessionStartedAt: Date?
    /// When the devices were last enumerated. Uses the monotonic clock, so
    /// an adjusted system clock cannot produce a negative interval.
    private var lastDeviceRefresh: ContinuousClock.Instant?

    /// Main window visible? Affects poll cadence only; the dock icon
    /// policy lives in MainWindowManager.
    private var isWindowVisible = true

    /// The cached authorization token. nil means the next privileged
    /// operation has to pop the dialog.
    ///
    /// We do not track the expiry ourselves: the system's timeout is
    /// governed by the authorization database (an administrator may change
    /// it), and a copy we compute ourselves would only disagree with the
    /// system. The helper's re-check is the more reliable authority.
    private var cachedAuthorization: AuthorizationToken?

    /// The explanation shown in the system authorization dialog.
    ///
    /// Without this sentence the box only says "Tethernet wants to make
    /// changes" and the user has no way to judge whether to approve. A
    /// computed property rather than `static let`: `static let` is
    /// evaluated only once, when the type is first used, so a later language
    /// switch would never update it.
    private static var authorizationPrompt: String { L(.authPromptSession) }

    /// The explanation in the authorization dialog when installing the
    /// privileged helper. A separate sentence -- what is being approved
    /// here is "putting something into a system directory", which is not the
    /// same kind of action as the daily operation above, so the wording has
    /// to tell the truth.
    private static var helperInstallPrompt: String { L(.authPromptInstall) }

    /// The explanation in the authorization dialog when uninstalling the
    /// privileged helper.
    private static var helperUninstallPrompt: String { L(.authPromptUninstall) }

    /// The polling interval.
    ///
    /// 500 ms is the balance point between "looks live" and "do not turn the
    /// XPC round trip into a burden": any faster is indistinguishable to the
    /// eye, any slower makes the rate curve look jerky.
    private static let pollInterval: Duration = .milliseconds(500)

    /// The polling interval while purely idling in the background (window
    /// closed, no session running).
    private static let backgroundPollInterval: Duration = .seconds(2)

    /// The minimum interval between device enumerations.
    ///
    /// Much slower than the status polling, because enumeration has to
    /// libusb_open the device to read its string descriptors. A 2 second
    /// plug/unplug response delay is barely noticeable to the user, while
    /// the cost drops to a quarter of what it was.
    private static let deviceRefreshInterval: Duration = .seconds(2)

    /// How many samples the throughput curve keeps. 120 points x 500 ms =
    /// the last 60 seconds.
    private static let historyCapacity = 120

    /// How many lines the log panel keeps.
    ///
    /// 2000 lines are enough to look back over a whole startup sequence plus
    /// several minutes of running; beyond that the SwiftUI list starts to
    /// stutter.
    private static let logCapacity = 2000

    // MARK: - Lifecycle

    /// Apply a language preference: this process -> libtethernet -> helper,
    /// none of them may be missed.
    ///
    /// UserDefaults is written only here and read in
    /// `restoreLanguagePreference()` -- both sides use the same key name
    /// constant, so renaming it cannot miss one of them.
    func applyLanguage(_ preference: LanguagePreference) {
        let resolved = L10n.apply(preference)
        TethernetLibrary.setLanguage(resolved)
        UserDefaults.standard.set(preference.rawValue, forKey: Self.languageDefaultsKey)
        languageRevision += 1
        Task { await client.setLanguage(resolved) }
    }

    /// Restore the language picked last time at startup. If nothing was
    /// stored, it is `.system`.
    private func restoreLanguagePreference() {
        let stored = UserDefaults.standard.string(forKey: Self.languageDefaultsKey)
        let preference = stored.flatMap(LanguagePreference.init(rawValue:)) ?? .system
        // Writing the stored property directly would fire didSet and save
        // to disk again; bypass it: this is a "restore", not a "the user
        // changed it".
        if preference != languagePreference {
            languagePreference = preference
        } else {
            applyLanguage(preference)
        }
    }

    private static let languageDefaultsKey = "TethernetLanguagePreference"

    func start() {
        guard pollingTask == nil else { return }
        restoreLanguagePreference()
        pollingTask = Task { [weak self] in
            while !Task.isCancelled {
                guard let self else { return }
                await self.refresh()
                // The interval is dynamic: it stays fluid while a session
                // runs or the window is open, and slows down while purely
                // idling in the background.
                try? await Task.sleep(for: self.pollDelay)
            }
        }
        restoreKnownUpdate()
        Task { await checkForUpdatesQuietly() }
    }

    func stopPolling() {
        pollingTask?.cancel()
        pollingTask = nil
    }

    /// The main window appeared (including when it is reopened from the
    /// menu bar).
    ///
    /// Refresh right away instead of waiting for the next cycle: the
    /// background polling may be sleeping through the 2 second long
    /// interval, and the first thing the user sees after opening the
    /// window should not be stale data.
    func windowDidAppear() {
        isWindowVisible = true
        Task { await refresh() }
    }

    /// The main window closed. The app switches to background mode; polling
    /// continues (the menu bar feeds on it) but slows down.
    func windowDidDisappear() {
        isWindowVisible = false
    }

    /// The polling interval to use right now.
    ///
    /// Three situations keep the fast pace: a session is running (the menu
    /// bar has to show live rates), starting or stopping is in progress (the
    /// user is waiting for a result), the window is open (the user is
    /// looking at it). Only "purely idling in the background" slows down --
    /// there the only products of polling are the helper liveness probe and
    /// the device scan, and nobody needs them twice a second.
    private var pollDelay: Duration {
        let sessionActive = status.runState == .running || status.runState.isTransitional
        return sessionActive || isWindowVisible ? Self.pollInterval : Self.backgroundPollInterval
    }

    // MARK: - Polling

    private func refresh() async {
        do {
            let (revision, version) = HelperConstants.decodeVersion(try await client.helperVersion())
            guard revision == HelperConstants.protocolRevision else {
                helperAvailability = .outdated(installed: revision,
                                               expected: HelperConstants.protocolRevision)
                // When the protocol does not match, do not also report a
                // version mismatch: that card exists to send the user off
                // to update the helper, and saying the same thing twice
                // only makes them suspect two separate problems.
                helperVersionMismatch = nil
                // Do not keep sending requests when the interface does not
                // match -- the shapes of the arguments and the replies may
                // all differ.
                return
            }
            helperAvailability = .available(version: version)
            helperVersionMismatch = Self.versionMismatch(installed: version)
        } catch {
            helperAvailability = .missing(reason: error.localizedDescription)
            helperVersionMismatch = nil
            // Do not send further requests when the connection fails --
            // each one would repeat the same failure and only flood the
            // log.
            return
        }

        if environment == nil {
            environment = try? await client.environment()
        }

        if let fresh = try? await client.sessionStatus() {
            apply(status: fresh)
        }
        if let feed = try? await client.drainFeed() {
            apply(feed: feed)
        }

        // The device list is refreshed only while nothing is running: once
        // running, the device is exclusively held and the list should not
        // change.
        //
        // And it refreshes **more slowly than the status**. One enumeration
        // has to read the USB string descriptors, which means libusb_open
        // really has to open the device -- doing that on a 500 ms status
        // polling cadence means switching the user's device on and off
        // twice a second, wasteful and possibly disruptive to it. A 2
        // second plug/unplug response delay does not affect the feel of
        // the product at all.
        if status.runState != .running, shouldRefreshDevices() {
            if let fresh = try? await client.listDevices() {
                devices = fresh
                // When the selected device is unplugged, the selection
                // has to become invalid as well, otherwise "Start" looks
                // for a device at a bus address that does not exist.
                if let selected = selectedDeviceID,
                   !devices.contains(where: { $0.id == selected }) {
                    selectedDeviceID = nil
                }
            }
        }

        if !status.systemInterface.isEmpty {
            networkState = (try? await client.queryNetwork(interface: status.systemInterface))
                ?? .empty
        } else {
            networkState = .empty
        }
    }


    private func apply(status fresh: SessionStatus) {
        defer {
            previousStatus = fresh
            status = fresh
        }

        // Record/clear the moment of connection, used to show how long the
        // session has been connected.
        if fresh.runState == .running, sessionStartedAt == nil {
            sessionStartedAt = Date()
        } else if fresh.runState != .running, fresh.runState != .stopping {
            sessionStartedAt = nil
        }

        guard let previous = previousStatus,
              fresh.runState == .running,
              fresh.monotonicNanos > previous.monotonicNanos else {
            if fresh.runState != .running {
                throughput = .zero
            }
            return
        }

        // The denominator is the monotonic clock difference on the library
        // side, not our polling period -- scheduling stretches the real
        // interval between two fetches, and using a fixed period as the
        // denominator would overestimate the rate.
        let seconds = Double(fresh.monotonicNanos - previous.monotonicNanos) / 1_000_000_000
        guard seconds > 0 else { return }

        let sample = ThroughputSample(
            timestamp: Date(),
            receiveBitsPerSecond: Double(fresh.rxBytes &- previous.rxBytes) * 8 / seconds,
            transmitBitsPerSecond: Double(fresh.txBytes &- previous.txBytes) * 8 / seconds,
            receivePacketsPerSecond: Double(fresh.rxFrames &- previous.rxFrames) / seconds,
            transmitPacketsPerSecond: Double(fresh.txFrames &- previous.txFrames) / seconds)

        throughput = sample
        throughputHistory.append(sample)
        if throughputHistory.count > Self.historyCapacity {
            throughputHistory.removeFirst(throughputHistory.count - Self.historyCapacity)
        }
    }

    private func apply(feed: HelperFeed) {
        if !feed.logs.isEmpty {
            logs.append(contentsOf: feed.logs)
            if logs.count > Self.logCapacity {
                logs.removeFirst(logs.count - Self.logCapacity)
            }
        }
        droppedLogCount += feed.droppedLogs
    }

    // MARK: - Actions

    /// How long the session has been connected; nil when not connected.
    var connectedDuration: TimeInterval? {
        sessionStartedAt.map { Date().timeIntervalSince($0) }
    }

    var selectedDevice: DeviceDescriptor? {
        if let selectedDeviceID {
            return devices.first { $0.id == selectedDeviceID }
        }
        return devices.first
    }

    /// The filtered and consecutively deduplicated logs.
    ///
    /// Collapsing happens **after** filtering: in the raw stream two
    /// identical INFO lines may have trace lines between them, so
    /// collapsing in raw order would fail, while the adjacent repeats the
    /// user sees at a given level are exactly the ones that should be
    /// merged. Recomputed in O(n), n <= 2000, which is imperceptible at a
    /// 500 ms refresh cadence.
    var filteredLogs: [CollapsedLogEntry] {
        var collapsed: [CollapsedLogEntry] = []
        for entry in logs where entry.level >= logLevelFilter {
            if let last = collapsed.last, last.matches(entry) {
                collapsed[collapsed.count - 1].absorb(entry)
            } else {
                collapsed.append(CollapsedLogEntry(entry))
            }
        }
        return collapsed
    }

    /// Start the session. Pops one system authorization dialog.
    func startSession() async {
        guard !isBusy else { return }
        isBusy = true
        defer { isBusy = false }

        var configuration = SessionConfiguration(mtu: requestedMTU,
                                                 adoptDeviceMAC: adoptDeviceMAC)
        // Specify the bus and address rather than VID/PID: two devices of
        // the same model have exactly the same VID/PID, and only the bus
        // address tells them apart.
        if let device = selectedDevice {
            configuration.busNumber = device.busNumber
            configuration.deviceAddress = device.deviceAddress
        }

        await authorized { [self] authorization in
            throughputHistory.removeAll()
            try await client.startSession(authorization: authorization,
                                          configuration: configuration)
        }
    }

    /// Stop the session.
    func stopSession() async {
        guard !isBusy else { return }
        isBusy = true
        defer { isBusy = false }

        await authorized { [self] authorization in
            try await client.stopSession(authorization: authorization)
        }
    }

    /// Push the network configuration.
    func applyNetworkConfiguration() async {
        guard !isBusy else { return }
        let interface = status.systemInterface
        guard !interface.isEmpty else {
            alertMessage = L(.interfaceNotReadyYet)
            return
        }
        if let message = NetworkValidator.validationMessage(for: networkConfiguration) {
            alertMessage = message
            return
        }

        isBusy = true
        defer { isBusy = false }

        await authorized { [self] authorization in
            try await client.applyNetwork(authorization: authorization, interface: interface,
                                          configuration: networkConfiguration)
            // Read it back right away so the UI reflects the address that
            // actually took effect, without waiting for the next polling
            // cycle.
            networkState = (try? await client.queryNetwork(interface: interface)) ?? .empty
        }
    }

    /// Revoke the IP configuration on the interface.
    ///
    /// A separate action rather than "pick 'Not configured' in the access
    /// mode selector and press Apply": revoking is a one-shot operation, and
    /// burying it in the mode selector makes people think that selecting it
    /// has already taken effect.
    func clearNetworkConfiguration() async {
        guard !isBusy else { return }
        let interface = status.systemInterface
        guard !interface.isEmpty else { return }

        isBusy = true
        defer { isBusy = false }

        await authorized { [self] authorization in
            try await client.applyNetwork(authorization: authorization, interface: interface,
                                          configuration: NetworkConfiguration(mode: .none))
            networkState = (try? await client.queryNetwork(interface: interface)) ?? .empty
        }
    }

    /// Refresh the device list manually (the refresh button in the UI).
    ///
    /// Manual triggers are not throttled -- when the user presses the
    /// button they want to see the result immediately.
    func refreshDevices() async {
        lastDeviceRefresh = .now
        devices = (try? await client.listDevices()) ?? []
    }

    /// Whether enough time has passed since the last enumeration. Records
    /// this round's time on the way.
    private func shouldRefreshDevices() -> Bool {
        let now = ContinuousClock.now
        if let last = lastDeviceRefresh, now - last < Self.deviceRefreshInterval {
            return false
        }
        lastDeviceRefresh = now
        return true
    }

    func clearLogs() {
        logs.removeAll()
        droppedLogCount = 0
    }

    /// The library version the app ships -- the standard answer to "which
    /// version should the privileged helper be".
    ///
    /// The **library** version rather than the app version in Info.plist:
    /// what gets installed into /Library/PrivilegedHelperTools is a copy of
    /// the libtethernet inside the .app (see the payload assembly in
    /// build-gui.sh), and only same-origin values compare meaningfully.
    /// Moreover the bare executable from `swift run` has no bundle at all,
    /// so on that path the Info.plist check would fail outright.
    ///
    /// Caching it in a `static let` is safe because it is a constant
    /// compiled into the dylib that cannot change while the process lives
    /// -- unlike the prompts above, which have to follow the language. The
    /// version extraction comes along for the ride: this check hangs off a
    /// 500 ms polling loop, and there is no reason to re-run the regex
    /// every time.
    private static let bundledVersion =
        HelperConstants.semanticVersion(of: TethernetLibrary.versionInfo.version)

    /// Whether the installed helper is the same version as the one the app
    /// ships. Returns nil when they match.
    private static func versionMismatch(installed: String) -> HelperVersionMismatch? {
        let installedVersion = HelperConstants.semanticVersion(of: installed)
        guard installedVersion != bundledVersion else { return nil }
        return HelperVersionMismatch(installed: installedVersion, expected: bundledVersion)
    }

    /// Install / update the privileged helper (the "one-click install"
    /// button on the guide card, and the "update the privileged helper"
    /// action in the management row when the versions differ -- an update
    /// is just reinstalling the payload from the .app).
    func installHelper() async {
        await maintainHelper(.install, prompt: Self.helperInstallPrompt,
                             expectAvailable: true,
                             failureNotice: L(.installScriptRanButUnreachable))
    }

    /// Uninstall the privileged helper (the "Uninstall..." action on the
    /// Manage page; the confirmation alert lives in the UI layer).
    ///
    /// bootout sends the helper a SIGTERM -- a running session is shut down
    /// gracefully and the virtual interface destroyed, and that has to be
    /// spelled out in the confirmation dialog. Once the uninstall finishes
    /// the UI naturally returns to the install guide page.
    func uninstallHelper() async {
        await maintainHelper(.uninstall, prompt: Self.helperUninstallPrompt,
                             expectAvailable: false,
                             failureNotice: L(.uninstallScriptRanButStillAlive))
    }

    /// The execution shell shared by install and uninstall: authorize
    /// (reuse the cached token) -> AEWP execution -> XPC probe as a
    /// backstop.
    ///
    /// The authorization and the daily privileged operations share the same
    /// right (system.privilege.admin), so the token cache is naturally
    /// universal: install the helper and press "Connect" within 5 minutes
    /// and no dialog appears again. The rare path where the cached token
    /// has expired falls back to the system-default wording dialog that
    /// AEWP raises itself; no dedicated "re-authorize with custom wording"
    /// is built for it.
    ///
    /// How success and failure are decided:
    ///   AEWP gives no exit code (see the HelperInstaller notes), so after
    ///   the script runs we confirm with a real XPC round trip, waiting a
    ///   few seconds for launchd to finish the registration and the
    ///   on-demand launch (or the logout to take effect). The direction is
    ///   decided by expectAvailable: install waits for "can connect",
    ///   uninstall waits for "cannot connect". On failure the tail of the
    ///   script output goes into the dialog -- the real reason is almost
    ///   always written at the end.
    private func maintainHelper(_ action: HelperInstaller.Action,
                                prompt: String,
                                expectAvailable: Bool,
                                failureNotice: String) async {
        guard !isBusy else { return }
        isBusy = true
        defer { isBusy = false }

        // Preflight the payload and AEWP first -- whichever is missing
        // has to be said clearly **before** the authorization dialog pops
        // up.
        if let failure = HelperInstaller.preflightError() {
            alertMessage = failure.localizedDescription
            return
        }

        let output: String
        do {
            let token: AuthorizationToken
            if let cached = cachedAuthorization {
                token = cached
            } else {
                token = try AuthorizationBroker.requestAuthorization(prompt: prompt)
                cachedAuthorization = token
            }
            defer { withExtendedLifetime(token) {} }
            output = try await HelperInstaller.run(action, with: token)
        } catch AuthorizationBroker.Failure.userCancelled {
            return
        } catch HelperInstaller.Failure.userCancelled {
            return
        } catch {
            alertMessage = error.localizedDescription
            return
        }

        for _ in 0..<6 {
            await refresh()
            if helperAvailability.isAvailable == expectAvailable { return }
            try? await Task.sleep(for: .milliseconds(500))
        }
        alertMessage = L(.scriptOutput, failureNotice, Self.tail(of: output))
    }

    // MARK: - Update check

    /// Manual check (the "Check for Updates..." item in the app menu). The
    /// result pops up a dialog whether it is good news or bad.
    func checkForUpdates() async {
        guard let current = UpdateChecker.currentVersion else {
            updateCheckResult = .unavailable
            return
        }
        do {
            let latest = try await UpdateChecker.fetchLatestRelease()
            remember(latest)
            if UpdateChecker.isNewer(latest.version, than: current) {
                availableUpdate = latest
                updateCheckResult = .updateAvailable(latest)
            } else {
                availableUpdate = nil
                updateCheckResult = .upToDate(current: current)
            }
        } catch {
            updateCheckResult = .failed(error.localizedDescription)
        }
    }

    /// Automatic check: at most once a day, silent on failure, and a new
    /// version only lights up the hint in the management row -- it never
    /// interrupts with a dialog, since knowing about an update is a "by the
    /// way" matter that does not deserve a modal box.
    /// `defaults write com.tethernet.app updateCheckDisabled -bool YES`
    /// turns it off.
    private func checkForUpdatesQuietly() async {
        guard let current = UpdateChecker.currentVersion else { return }
        let defaults = UserDefaults.standard
        guard !defaults.bool(forKey: Self.updateCheckDisabledKey) else { return }
        if let last = defaults.object(forKey: Self.updateLastCheckedKey) as? Date,
           Date().timeIntervalSince(last) < 24 * 60 * 60 {
            return
        }

        guard let latest = try? await UpdateChecker.fetchLatestRelease() else { return }
        defaults.set(Date(), forKey: Self.updateLastCheckedKey)
        remember(latest)
        availableUpdate = UpdateChecker.isNewer(latest.version, than: current) ? latest : nil
    }

    /// Record the newest version found in defaults -- when tomorrow's
    /// startup is blocked by the throttle, the hint should not disappear.
    private func remember(_ release: UpdateChecker.Release) {
        let defaults = UserDefaults.standard
        defaults.set(release.version, forKey: Self.updateKnownVersionKey)
        defaults.set(release.pageURL.absoluteString, forKey: Self.updateKnownPageKey)
    }

    /// Restore the new-version hint at startup. It expires naturally once
    /// the upgrade is done (current version >= the remembered version), so
    /// no cleanup logic is needed.
    private func restoreKnownUpdate() {
        guard let current = UpdateChecker.currentVersion,
              let version = UserDefaults.standard.string(forKey: Self.updateKnownVersionKey),
              let page = UserDefaults.standard.string(forKey: Self.updateKnownPageKey),
              let pageURL = URL(string: page),
              UpdateChecker.isNewer(version, than: current) else { return }
        availableUpdate = UpdateChecker.Release(version: version, pageURL: pageURL)
    }

    private static let updateCheckDisabledKey = "updateCheckDisabled"
    private static let updateLastCheckedKey = "updateLastCheckedAt"
    private static let updateKnownVersionKey = "updateKnownVersion"
    private static let updateKnownPageKey = "updateKnownPageURL"

    /// Take the last few lines of the output for the dialog -- the full
    /// output can be long, and the real reason is almost always at the end.
    /// The ANSI color codes from the script are stripped along the way,
    /// since in the dialog they are just noise.
    private static func tail(of output: String,
                             maxLines: Int = 10, maxCharacters: Int = 700) -> String {
        let plain = output.replacingOccurrences(of: "\u{1B}\\[[0-9;]*m", with: "",
                                                options: .regularExpression)
        let lines = plain.split(separator: "\n", omittingEmptySubsequences: true)
        var kept = lines.suffix(maxLines).joined(separator: "\n")
        if kept.count > maxCharacters {
            kept = String(kept.suffix(maxCharacters))
        }
        return kept.isEmpty ? L(.scriptNoOutput) : kept
    }

    /// Run one privileged operation with an authorization credential,
    /// popping the system authorization dialog only when needed.
    ///
    /// Why the token is cached:
    ///   The measured parameters of `system.privilege.admin` are
    ///   `shared = false` and `timeout = 300`. `shared = false` means the
    ///   credential is **not shared across AuthorizationRefs** -- every
    ///   operation creates a new ref and therefore requires the user to
    ///   authenticate again, so "connect, configure the network, disconnect"
    ///   would pop three dialogs in a row. `timeout = 300` means a
    ///   credential on the same ref stays valid for 5 minutes.
    ///
    ///   So the token is cached: the first operation pops one dialog, and
    ///   none are needed again for 5 minutes. After it expires the helper's
    ///   re-check fails and says clearly that "this is an authorization
    ///   problem", and we discard the cache, pop one dialog again and retry
    ///   the operation -- what the user sees is still "one dialog before
    ///   the operation", not a baffling failure.
    ///
    /// Why we cannot catch the credential ourselves and reuse it:
    ///   The external form is just a key pointing at the authorization held
    ///   in securityd, not the credential itself. As soon as the
    ///   AuthorizationRef is released, the helper's re-check reports
    ///   -60005. So the token is held by `cachedAuthorization`, and
    ///   `withExtendedLifetime` keeps it alive until the XPC round trip has
    ///   finished.
    ///
    /// A user cancellation does **not** show an error -- cancelling is a
    /// normal outcome, and an extra "cancelled" dialog is just annoying.
    private func authorized(_ body: @escaping (Data) async throws -> Void) async {
        // First pass: use the cache if there is one, do not bother the
        // user.
        if let cached = cachedAuthorization {
            // withExtendedLifetime cannot take an async closure, so a defer
            // pins the token to the end of the scope -- a plain local let
            // is not enough, ARC may release it right after the last read
            // of externalForm, and the XPC round trip has not come back
            // yet.
            defer { withExtendedLifetime(cached) {} }
            do {
                try await body(cached.externalForm)
                return
            } catch let failure as HelperClient.Failure where failure.isAuthorizationProblem {
                // The credential expired. Drop the cache and fall through
                // to "re-authorize and retry".
                cachedAuthorization = nil
            } catch {
                alertMessage = error.localizedDescription
                return
            }
        }

        // Second pass: pop the dialog for a fresh credential, then run (or
        // retry).
        do {
            let token = try AuthorizationBroker.requestAuthorization(
                prompt: Self.authorizationPrompt)
            defer { withExtendedLifetime(token) {} }
            cachedAuthorization = token
            try await body(token.externalForm)
        } catch AuthorizationBroker.Failure.userCancelled {
            return
        } catch {
            alertMessage = error.localizedDescription
        }
    }
}
