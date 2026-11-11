import CTethernet
import Foundation
import TethernetIPC

/// An RNDIS session. Requires root, so it is used only inside the helper
/// process.
///
/// Thread safety: the C-side session is itself thread-safe (status snapshots and
/// the event queue are both locked), but the caller must ensure `start` / `stop`
/// are not called concurrently. The helper satisfies this by serializing all
/// lifecycle calls on a single serial queue.
public final class TethernetSession {
    private let handle: OpaquePointer

    /// Maximum number of events drained in one call, matching the C-side ring
    /// buffer capacity (128).
    private static let eventCapacity = 128

    public init(configuration: SessionConfiguration) throws {
        var raw = tn_session_config_t()
        // Must init before mutating: the struct carries many tuning knobs, and
        // zero-initialization would produce an invalid configuration.
        tn_session_config_init(&raw)
        raw.vendor_id = configuration.vendorID
        raw.product_id = configuration.productID
        raw.bus_number = configuration.busNumber
        raw.device_address = configuration.deviceAddress
        raw.mtu = configuration.mtu
        raw.adopt_device_mac = configuration.adoptDeviceMAC

        var error = tn_error_t()
        guard let created = tn_session_create(&raw, &error) else {
            throw TethernetError(result: TN_ERR_FAILED.rawValue, error: error)
        }
        handle = created
    }

    deinit {
        // tn_session_destroy stops internally first (idempotent), so there is no
        // need to call stop here.
        tn_session_destroy(handle)
    }

    /// Start. Non-blocking -- returning only means the request was accepted;
    /// success or failure is reflected in subsequent status.
    public func start() throws {
        var error = tn_error_t()
        let result = tn_session_start(handle, &error)
        try check(result, error)
    }

    /// Stop and wait for all teardown to complete. Idempotent.
    public func stop() {
        _ = tn_session_stop(handle)
    }

    /// Take a status snapshot. Callable from any thread.
    public func status() -> SessionStatus {
        var raw = tn_session_status_t()
        guard tn_session_status_get(handle, &raw) == TN_OK else {
            return .idle
        }
        return SessionStatus(
            runState: RunState(rawValue: raw.run_state) ?? .idle,
            rndisState: RndisState(rawValue: raw.rndis_state) ?? .uninitialized,
            linkUp: raw.link_up,
            paused: raw.paused,
            systemInterface: String(fixedCArray: raw.system_interface),
            driverInterface: String(fixedCArray: raw.driver_interface),
            deviceMAC: formatMAC(raw.device_mac),
            mtu: raw.mtu,
            linkSpeedMbps: raw.link_speed_mbps,
            vendorDescription: String(fixedCArray: raw.vendor_description),
            deviceDescription: String(fixedCArray: raw.device_description),
            rxFrames: raw.rx_frames,
            rxBytes: raw.rx_bytes,
            rxDropped: raw.rx_dropped,
            txFrames: raw.tx_frames,
            txBytes: raw.tx_bytes,
            txDropped: raw.tx_dropped,
            rxQueueDepth: raw.rx_queue_depth,
            linkKernelDrops: raw.link_kernel_drops,
            txBackpressure: raw.tx_backpressure,
            monotonicNanos: raw.monotonic_nanos,
            fatalMessage: String(fixedCArray: raw.fatal))
    }

    /// Drain queued events and render them as one-line notices for the user.
    ///
    /// Keeps only the categories the user cares about: link changes, device
    /// resets, and fatal errors. RNDIS-internal state transitions and lifecycle
    /// transitions are already shown through status in the UI; surfacing them as
    /// notices again would just be noise.
    public func drainNotices() -> [String] {
        var buffer = [tn_event_t](repeating: tn_event_t(), count: Self.eventCapacity)
        let taken = tn_session_poll_events(handle, &buffer, Self.eventCapacity)

        return buffer.prefix(taken).compactMap { event -> String? in
            // On the C side tn_event_kind values are all non-negative and are
            // imported into Swift as UInt32, while the struct field is int32_t
            // -- they cannot be compared directly, so an explicit cast is needed.
            switch event.kind {
            case Int32(TN_EVENT_LINK.rawValue):
                return L(event.a == 1 ? .eventLinkUp : .eventLinkDown)
            case Int32(TN_EVENT_DEVICE_RESET.rawValue):
                return L(event.a == 1 ? .eventDeviceResetReplayed : .eventDeviceReset)
            case Int32(TN_EVENT_FATAL.rawValue):
                return String(fixedCArray: event.text)
            case Int32(TN_EVENT_NEGOTIATED.rawValue):
                return L(.eventNegotiated, Int(event.a), Int(event.b))
            default:
                return nil
            }
        }
    }
}
