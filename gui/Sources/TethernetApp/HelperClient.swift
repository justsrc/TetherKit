import Foundation
import TethernetIPC

/// The client that talks to tethernet-helper.
///
/// It wraps "reply block based XPC" into async/await. It looks fiddly, but
/// one thing has to be handled with care: the NSXPCConnection error block
/// and the method's reply block **may both be called** (the request went
/// out and the connection dropped right after, for instance), and resuming
/// a CheckedContinuation twice is an immediate crash. So every call wraps
/// the continuation in a ContinuationGuard, which guarantees the result is
/// delivered exactly once.
final class HelperClient {
    enum Failure: LocalizedError {
        /// Cannot connect -- the most common cause is that the helper is
        /// not installed yet.
        case unreachable(String)
        /// The helper explicitly returned an error.
        case helper(String)
        /// The helper's authorization re-check did not pass -- most likely
        /// the credential expired, so popping the dialog again and retrying
        /// is enough.
        case authorizationRejected(String)
        /// The reply cannot be decoded (the two sides run different
        /// versions).
        case malformedResponse

        var errorDescription: String? {
            switch self {
            case .unreachable(let detail):
                return L(.helperConnectFailed, detail)
            case .helper(let message), .authorizationRejected(let message):
                return message
            case .malformedResponse:
                return L(.helperReplyUnparsable)
            }
        }

        var isUnreachable: Bool {
            if case .unreachable = self { return true }
            return false
        }

        /// Whether it is worth fetching a fresh authorization and trying
        /// again.
        var isAuthorizationProblem: Bool {
            if case .authorizationRejected = self { return true }
            return false
        }
    }

    /// Guarantees that a continuation is delivered exactly once.
    ///
    /// XPC's error block and reply block can both be called, and resuming
    /// twice crashes -- this trap only shows up with the timing "the
    /// connection drops after the request went out", which is hard to hit
    /// with a test.
    private final class ContinuationGuard<T>: @unchecked Sendable {
        private let lock = NSLock()
        private var continuation: CheckedContinuation<T, Error>?

        init(_ continuation: CheckedContinuation<T, Error>) {
            self.continuation = continuation
        }

        func resume(returning value: T) {
            take()?.resume(returning: value)
        }

        func resume(throwing error: Error) {
            take()?.resume(throwing: error)
        }

        private func take() -> CheckedContinuation<T, Error>? {
            lock.withLock {
                defer { continuation = nil }
                return continuation
            }
        }
    }

    private let connectionLock = NSLock()
    private var cachedConnection: NSXPCConnection?

    deinit {
        cachedConnection?.invalidate()
    }

    /// Get (creating it if needed) the connection to the helper.
    private func connection() -> NSXPCConnection {
        connectionLock.withLock {
            if let existing = cachedConnection {
                return existing
            }
            let created = NSXPCConnection(machServiceName: HelperConstants.machServiceName,
                                          options: .privileged)
            created.remoteObjectInterface = NSXPCInterface(with: TethernetHelperProtocol.self)

            // The cached connection has to be discarded once it breaks,
            // otherwise every later call hits a dead connection and it
            // looks like "the helper is installed but never connects".
            let invalidate: @Sendable () -> Void = { [weak self] in
                self?.connectionLock.withLock { self?.cachedConnection = nil }
            }
            created.invalidationHandler = invalidate
            created.interruptionHandler = invalidate

            created.resume()
            cachedConnection = created
            return created
        }
    }

    /// Issue one call. `body` receives the proxy and the guard, and is
    /// responsible for starting the request and delivering the result.
    private func invoke<T>(
        _ body: @escaping (TethernetHelperProtocol, ContinuationGuard<T>) -> Void
    ) async throws -> T {
        try await withCheckedThrowingContinuation { continuation in
            let guarded = ContinuationGuard(continuation)
            let proxy = connection().remoteObjectProxyWithErrorHandler { error in
                guarded.resume(throwing: Failure.unreachable(error.localizedDescription))
            }
            guard let typed = proxy as? TethernetHelperProtocol else {
                guarded.resume(throwing: Failure.malformedResponse)
                return
            }
            body(typed, guarded)
        }
    }

    /// The shared handling of a reply that is "one Codable value or one
    /// error".
    private func decode<T: Decodable>(_ type: T.Type, data: Data?, message: String?,
                                      into guarded: ContinuationGuard<T>) {
        if let message {
            guarded.resume(throwing: Failure.helper(message))
            return
        }
        guard let data, let value = try? JSONDecoder().decode(type, from: data) else {
            guarded.resume(throwing: Failure.malformedResponse)
            return
        }
        guarded.resume(returning: value)
    }

    // MARK: - Probing (no authorization required)

    func helperVersion() async throws -> String {
        try await invoke { proxy, guarded in
            proxy.helperVersion { guarded.resume(returning: $0) }
        }
    }

    func environment() async throws -> EnvironmentReport {
        try await invoke { proxy, guarded in
            proxy.environment { data, message in
                self.decode(EnvironmentReport.self, data: data, message: message, into: guarded)
            }
        }
    }

    func listDevices() async throws -> [DeviceDescriptor] {
        try await invoke { proxy, guarded in
            proxy.listDevices { data, message in
                self.decode([DeviceDescriptor].self, data: data, message: message, into: guarded)
            }
        }
    }

    func sessionStatus() async throws -> SessionStatus {
        try await invoke { proxy, guarded in
            proxy.sessionStatus { data, message in
                self.decode(SessionStatus.self, data: data, message: message, into: guarded)
            }
        }
    }

    func queryNetwork(interface: String) async throws -> NetworkState {
        try await invoke { proxy, guarded in
            proxy.queryNetwork(interface: interface) { data, message in
                self.decode(NetworkState.self, data: data, message: message, into: guarded)
            }
        }
    }

    func drainFeed() async throws -> HelperFeed {
        try await invoke { proxy, guarded in
            proxy.drainFeed { data in
                guard let data, let feed = try? JSONDecoder().decode(HelperFeed.self, from: data)
                else {
                    guarded.resume(returning: .empty)
                    return
                }
                guarded.resume(returning: feed)
            }
        }
    }

    /// Tell the helper the interface language. **Failures are swallowed
    /// silently** -- a language that did not synchronize only means the log
    /// card mixes in another language, and that must not surface to the
    /// user as a "cannot connect to helper" error (in the normal startup
    /// order the helper may not be installed yet).
    func setLanguage(_ language: Language) async {
        _ = try? await invoke { proxy, guarded in
            proxy.setLanguage(language.rawValue) { guarded.resume(returning: ()) }
        }
    }

    // MARK: - Privileged operations (an authorization credential is
    // required)

    func startSession(authorization: Data, configuration: SessionConfiguration) async throws {
        let payload = try JSONEncoder().encode(configuration)
        try await invokeVoid { proxy, guarded in
            proxy.startSession(authorization: authorization,
                               configuration: payload) { message, authorizationFailed in
                Self.finish(message, authorizationFailed, guarded)
            }
        }
    }

    func stopSession(authorization: Data) async throws {
        try await invokeVoid { proxy, guarded in
            proxy.stopSession(authorization: authorization) { message, authorizationFailed in
                Self.finish(message, authorizationFailed, guarded)
            }
        }
    }

    func applyNetwork(authorization: Data, interface: String,
                      configuration: NetworkConfiguration) async throws {
        let payload = try JSONEncoder().encode(configuration)
        try await invokeVoid { proxy, guarded in
            proxy.applyNetwork(authorization: authorization, interface: interface,
                               configuration: payload) { message, authorizationFailed in
                Self.finish(message, authorizationFailed, guarded)
            }
        }
    }

    private func invokeVoid(
        _ body: @escaping (TethernetHelperProtocol, ContinuationGuard<Void>) -> Void
    ) async throws {
        _ = try await invoke(body)
    }

    private static func finish(_ message: String?, _ authorizationFailed: Bool,
                               _ guarded: ContinuationGuard<Void>) {
        guard let message else {
            guarded.resume(returning: ())
            return
        }
        guarded.resume(throwing: authorizationFailed
            ? Failure.authorizationRejected(message)
            : Failure.helper(message))
    }
}
