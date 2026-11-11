import Foundation
import TethernetCore
import TethernetIPC

/// XPC service implementation for the helper.
///
/// Threading model:
///
///   XPC methods are called on the connection's own queue. Any operation that may
///   take time (starting a session does a USB handshake, DHCP waits for a lease)
///   is dispatched immediately to its own serial queue and replied to
///   asynchronously, never blocking on the XPC queue -- otherwise subsequent
///   status polling on the same connection would be stuck with it, and the UI
///   would appear completely frozen.
///
///   Session lifecycle and network configuration each use their own serial queue:
///   the two do not block each other, but each is strictly serialized internally
///   (concurrent start/stop or concurrent ipconfig calls would be disastrous).
///   Status queries are not queued and go straight to the C layer -- snapshots
///   there are already thread-safe.
final class HelperService: NSObject, TethernetHelperProtocol {
    private let lifecycleQueue = DispatchQueue(label: "com.tethernet.helper.lifecycle")
    private let networkQueue = DispatchQueue(label: "com.tethernet.helper.network")

    /// Protects session and notices. Hold times are very short, so a plain lock
    /// is sufficient.
    private let stateLock = NSLock()
    private var session: TethernetSession?
    /// startSession is constructing / handshaking. In this window the session is
    /// not yet registered, but the device has already been opened by this process
    /// -- enumeration's "in use" check must count it, otherwise string reads would
    /// insert another transfer onto the control endpoint mid-handshake.
    private var sessionStarting = false
    /// Notices not yet taken by the App.
    private var pendingNotices: [String] = []

    // MARK: - Probe interfaces that do not require authorization

    func helperVersion(reply: @escaping (String) -> Void) {
        // Include the XPC interface revision so the App can detect "the helper is
        // an old pre-upgrade version".
        reply(HelperConstants.encodeVersion(TethernetLibrary.versionInfo.version))
    }

    func environment(reply: @escaping (Data?, String?) -> Void) {
        respond(with: TethernetLibrary.checkEnvironment(), reply: reply)
    }

    func listDevices(reply: @escaping (Data?, String?) -> Void) {
        // Do not read string descriptors while the device is held by this process:
        // reading requires libusb_open, which would fail for nothing and would
        // insert another transfer onto the control endpoint mid-handshake. The
        // check uses "actually held" rather than "session != nil" -- dead sessions
        // in failed/stopped state have already torn down USB, and counting them as
        // "held" would prevent names from being read after the device is unplugged
        // and plugged back in.
        // Skipping does not lose names: the C layer backfills the last
        // successfully read values (see environment.cc).
        let deviceHeld = stateLock.withLock { () -> Bool in
            if sessionStarting { return true }
            guard let session else { return false }
            switch session.status().runState {
            case .starting, .running, .stopping: return true
            case .idle, .stopped, .failed: return false
            }
        }
        do {
            let devices = try TethernetLibrary.listDevices(readStrings: !deviceHeld)
            respond(with: devices, reply: reply)
        } catch {
            reply(nil, error.localizedDescription)
        }
    }

    func sessionStatus(reply: @escaping (Data?, String?) -> Void) {
        let status = withState { $0?.status() } ?? .idle
        // Piggyback session events into the notice queue -- status polling already
        // happens once per refresh cycle, so this avoids an extra XPC round trip.
        if let notices = withState({ $0?.drainNotices() }), !notices.isEmpty {
            appendNotices(notices)
        }
        respond(with: status, reply: reply)
    }

    func queryNetwork(interface: String, reply: @escaping (Data?, String?) -> Void) {
        do {
            respond(with: try NetworkConfigurator.query(interface: interface), reply: reply)
        } catch {
            reply(nil, error.localizedDescription)
        }
    }

    func drainFeed(reply: @escaping (Data?) -> Void) {
        let drained = TethernetLibrary.drainLogs()
        let notices = takeNotices()
        let feed = HelperFeed(logs: drained.entries, droppedLogs: drained.dropped, notices: notices)
        reply(try? JSONEncoder().encode(feed))
    }

    func setLanguage(_ rawValue: String, reply: @escaping () -> Void) {
        // An unrecognized value leaves everything untouched on purpose: keep
        // rendering in the current language instead of falling back to the
        // default because the App sent something new. An App that still sends
        // the removed Chinese spelling ("chinese") decodes to nil here, and
        // the helper simply stays in English.
        if let language = Language(rawValue: rawValue) {
            L10n.apply(.english)
            TethernetLibrary.setLanguage(language)
        }
        reply()
    }

    // MARK: - Privileged interfaces that require authorization

    func startSession(authorization: Data, configuration: Data,
                      reply: @escaping (String?, Bool) -> Void) {
        guard let configuration = decode(SessionConfiguration.self, from: configuration, reply: reply),
              authorize(authorization, reply: reply) else {
            return
        }

        lifecycleQueue.async { [weak self] in
            guard let self else { return }

            // An old session that is completely dead (failed / stopped) must not
            // block a new connection.
            //
            // Typical scenario: the device is unplugged -> keep-alive fails
            // repeatedly -> the session enters failed. At that point the C++ side
            // has already torn down USB and the interfaces completely; the Swift
            // layer only has an empty shell left -- but it is non-nil. If the
            // rule were simply "reject when session != nil", the user could never
            // connect again after replugging the device without reinstalling the
            // helper. This was a real pitfall.
            if let existing = self.withState({ $0 }) {
                let state = existing.status().runState
                guard state == .failed || state == .stopped else {
                    reply(L(.helperSessionAlreadyRunning), false)
                    return
                }
                existing.stop()  // idempotent, just in case
                self.stateLock.withLock { self.session = nil }
            }

            // Mark "device is held" for the entire handshake window from
            // construction through start() returning, so concurrent listDevices
            // calls skip string reads (see the note there).
            self.stateLock.withLock { self.sessionStarting = true }
            defer { self.stateLock.withLock { self.sessionStarting = false } }
            do {
                let session = try TethernetSession(configuration: configuration)
                try session.start()
                self.stateLock.withLock { self.session = session }
                reply(nil, false)
            } catch {
                reply(error.localizedDescription, false)
            }
        }
    }

    func stopSession(authorization: Data, reply: @escaping (String?, Bool) -> Void) {
        guard authorize(authorization, reply: reply) else { return }

        lifecycleQueue.async { [weak self] in
            guard let self else { return }
            // Remove the session from state before stopping it: stopping joins
            // the control thread and may take hundreds of milliseconds, during
            // which we must not continue to report "session is running".
            let session = self.stateLock.withLock { () -> TethernetSession? in
                defer { self.session = nil }
                return self.session
            }
            session?.stop()
            self.appendNotices([L(.helperSessionStopped)])
            reply(nil, false)
        }
    }

    func applyNetwork(authorization: Data, interface: String, configuration: Data,
                      reply: @escaping (String?, Bool) -> Void) {
        guard let configuration = decode(NetworkConfiguration.self, from: configuration, reply: reply),
              authorize(authorization, reply: reply) else {
            return
        }
        // The UI has already validated once, but XPC can be reached by any local
        // process, so the helper must reject bad input itself -- and using the
        // same rules, so the two sides cannot disagree.
        if let message = NetworkValidator.validationMessage(for: configuration) {
            reply(message, false)
            return
        }

        // DHCP blocks until a lease is obtained (library-internal limit 10
        // seconds), so the reply must be sent asynchronously.
        networkQueue.async { [weak self] in
            do {
                try NetworkConfigurator.apply(configuration, to: interface)
                self?.appendNotices([L(.helperNetworkApplied, configuration.mode.displayName)])
                reply(nil, false)
            } catch {
                reply(error.localizedDescription, false)
            }
        }
    }

    // MARK: - Shutdown cleanup

    /// Called when SIGTERM is received (`launchctl bootout`).
    ///
    /// Swift's deinit does not run when a process is terminated; not stopping
    /// actively would leak feth interfaces in the kernel. The on-disk registration
    /// catches SIGKILL, but when a graceful exit is possible it should be taken
    /// -- that way even the "clean up on next launch" step is unnecessary.
    func shutdown() {
        let session = stateLock.withLock { () -> TethernetSession? in
            defer { self.session = nil }
            return self.session
        }
        session?.stop()
    }

    // MARK: - Internal helpers

    /// Verify authorization; on failure, reply with an error and set the second
    /// parameter to true to tell the App "this is an authorization problem --
    /// re-prompting and trying again might succeed".
    private func authorize(_ data: Data, reply: @escaping (String?, Bool) -> Void) -> Bool {
        do {
            try AuthorizationVerifier.verify(externalForm: data)
            return true
        } catch {
            reply(error.localizedDescription, true)
            return false
        }
    }

    private func decode<T: Decodable>(_ type: T.Type, from data: Data,
                                      reply: @escaping (String?, Bool) -> Void) -> T? {
        do {
            return try JSONDecoder().decode(type, from: data)
        } catch {
            reply(L(.helperRequestDecodeFailed, error.localizedDescription), false)
            return nil
        }
    }

    private func respond<T: Encodable>(with value: T, reply: (Data?, String?) -> Void) {
        do {
            reply(try JSONEncoder().encode(value), nil)
        } catch {
            reply(nil, L(.helperReplyEncodeFailed, error.localizedDescription))
        }
    }

    private func withState<T>(_ body: (TethernetSession?) -> T) -> T {
        stateLock.withLock { body(session) }
    }

    private func appendNotices(_ notices: [String]) {
        guard !notices.isEmpty else { return }
        stateLock.withLock {
            pendingNotices.append(contentsOf: notices)
            // Cap at 200: if the App does not come to fetch for a long time (for
            // example, suspended), this must not grow without bound.
            if pendingNotices.count > 200 {
                pendingNotices.removeFirst(pendingNotices.count - 200)
            }
        }
    }

    private func takeNotices() -> [String] {
        stateLock.withLock {
            defer { pendingNotices.removeAll() }
            return pendingNotices
        }
    }
}

/// XPC listener delegate.
final class HelperListenerDelegate: NSObject, NSXPCListenerDelegate {
    private let service: HelperService

    init(service: HelperService) {
        self.service = service
    }

    func listener(_ listener: NSXPCListener,
                  shouldAcceptNewConnection connection: NSXPCConnection) -> Bool {
        // Why connections are accepted unconditionally:
        //
        //   Security is not built on "who can connect", but on "every privileged
        //   call must carry a credential the user just confirmed". This is a
        //   deliberate choice: the other route is to validate the caller's code
        //   signature (the SMJobBless approach), but that depends on a
        //   certificate's designated requirement, and under source distribution
        //   the cdhash produced by each machine's build differs, so a hardcoded
        //   DR can never match.
        //
        //   For an open-source, source-distributed tool, credential verification
        //   is the more appropriate model: it is equally safe regardless of who
        //   compiled it.
        connection.exportedInterface = NSXPCInterface(with: TethernetHelperProtocol.self)
        connection.exportedObject = service
        connection.resume()
        return true
    }
}
