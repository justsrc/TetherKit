import Foundation

// Data models transferred between the App and the helper.
//
// Why everything goes through Codable + Data, not NSSecureCoding classes:
//   XPC only accepts plist types or classes that implement NSSecureCoding, and the
//   latter requires both sides to register the allowed class set. Getting one wrong
//   becomes a runtime "disallowed class" exception whose message is hard to pin
//   down. Encoding Codable structs into JSON Data and sharing the same type
//   definitions on both sides leaves validation to JSONDecoder -- simple, and
//   adding fields is backward compatible by default.

// MARK: - Session

/// Startup configuration for an RNDIS session. Fields correspond one-to-one with
/// tk_session_config_t in the C ABI.
public struct SessionConfiguration: Codable, Hashable, Sendable {
    /// Device filter. All zeros mean use the first RNDIS device found.
    public var vendorID: UInt16
    public var productID: UInt16
    /// Bus number and device address must be given together to distinguish two
    /// devices of the same model.
    public var busNumber: UInt8
    public var deviceAddress: UInt8

    /// Desired MTU. Negotiated down when the device cannot accommodate it.
    public var mtu: UInt32
    /// Whether to set the system-side interface MAC to the address reported by the
    /// device.
    public var adoptDeviceMAC: Bool

    public init(vendorID: UInt16 = 0,
                productID: UInt16 = 0,
                busNumber: UInt8 = 0,
                deviceAddress: UInt8 = 0,
                mtu: UInt32 = 1500,
                adoptDeviceMAC: Bool = true) {
        self.vendorID = vendorID
        self.productID = productID
        self.busNumber = busNumber
        self.deviceAddress = deviceAddress
        self.mtu = mtu
        self.adoptDeviceMAC = adoptDeviceMAC
    }
}

/// Session lifecycle state. Raw values align with C's tk_run_state_t.
public enum RunState: Int32, Codable, Sendable {
    case idle = 0
    case starting = 1
    case running = 2
    case stopping = 3
    case stopped = 4
    case failed = 5

    /// Whether this is a "busy" transitional state -- the UI uses this to disable
    /// buttons and show progress.
    public var isTransitional: Bool { self == .starting || self == .stopping }
}

/// RNDIS host-side state. Raw values align with C's tk_rndis_state_t.
public enum RndisState: Int32, Codable, Sendable {
    case uninitialized = 0
    case initializing = 1
    case initialized = 2
    /// Data has started flowing.
    case dataInitialized = 3
    case halting = 4
}

/// Session status snapshot.
public struct SessionStatus: Codable, Hashable, Sendable {
    public var runState: RunState
    public var rndisState: RndisState
    public var linkUp: Bool
    /// Whether data movement is paused (while the link is down or during a device
    /// soft reset).
    public var paused: Bool

    /// System-side interface name -- the user configures IP on this one.
    public var systemInterface: String
    /// Driver-side interface name, shown only for troubleshooting.
    public var driverInterface: String

    /// Formatted as "aa:bb:cc:dd:ee:ff"; empty before negotiation.
    public var deviceMAC: String
    public var mtu: UInt32
    public var linkSpeedMbps: UInt32
    public var vendorDescription: String
    public var deviceDescription: String

    public var rxFrames: UInt64
    public var rxBytes: UInt64
    public var rxDropped: UInt64
    public var txFrames: UInt64
    public var txBytes: UInt64
    public var txDropped: UInt64
    public var rxQueueDepth: UInt64
    public var linkKernelDrops: UInt64
    public var txBackpressure: UInt64

    /// Monotonic nanosecond count at sample time. Rate calculations must use this
    /// as the denominator, not the UI timer period -- the real interval between
    /// two fetches can be stretched by scheduling, which would make the rate read
    /// high.
    public var monotonicNanos: Int64

    /// Reason when runState == .failed.
    public var fatalMessage: String

    /// Empty state when not connected.
    public static let idle = SessionStatus(
        runState: .idle, rndisState: .uninitialized, linkUp: false, paused: false,
        systemInterface: "", driverInterface: "", deviceMAC: "", mtu: 0, linkSpeedMbps: 0,
        vendorDescription: "", deviceDescription: "",
        rxFrames: 0, rxBytes: 0, rxDropped: 0, txFrames: 0, txBytes: 0, txDropped: 0,
        rxQueueDepth: 0, linkKernelDrops: 0, txBackpressure: 0,
        monotonicNanos: 0, fatalMessage: "")

    public init(runState: RunState, rndisState: RndisState, linkUp: Bool, paused: Bool,
                systemInterface: String, driverInterface: String, deviceMAC: String,
                mtu: UInt32, linkSpeedMbps: UInt32, vendorDescription: String,
                deviceDescription: String, rxFrames: UInt64, rxBytes: UInt64, rxDropped: UInt64,
                txFrames: UInt64, txBytes: UInt64, txDropped: UInt64, rxQueueDepth: UInt64,
                linkKernelDrops: UInt64, txBackpressure: UInt64, monotonicNanos: Int64,
                fatalMessage: String) {
        self.runState = runState
        self.rndisState = rndisState
        self.linkUp = linkUp
        self.paused = paused
        self.systemInterface = systemInterface
        self.driverInterface = driverInterface
        self.deviceMAC = deviceMAC
        self.mtu = mtu
        self.linkSpeedMbps = linkSpeedMbps
        self.vendorDescription = vendorDescription
        self.deviceDescription = deviceDescription
        self.rxFrames = rxFrames
        self.rxBytes = rxBytes
        self.rxDropped = rxDropped
        self.txFrames = txFrames
        self.txBytes = txBytes
        self.txDropped = txDropped
        self.rxQueueDepth = rxQueueDepth
        self.linkKernelDrops = linkKernelDrops
        self.txBackpressure = txBackpressure
        self.monotonicNanos = monotonicNanos
        self.fatalMessage = fatalMessage
    }
}

// MARK: - Device

/// A USB device identified as RNDIS.
public struct DeviceDescriptor: Codable, Hashable, Identifiable, Sendable {
    public var vendorID: UInt16
    public var productID: UInt16
    public var busNumber: UInt8
    public var deviceAddress: UInt8
    /// Manufacturer / product / serial are best-effort: reading them requires
    /// opening the device, which fails while it is held -- but the helper
    /// backfills the last successfully read values, so names stay stable across a
    /// connection. Only when even the backfilled value is missing (never read
    /// since the helper started) is this an empty string, and the UI falls back
    /// to showing `description`.
    public var manufacturer: String
    public var product: String
    public var serial: String
    /// Formatted as "Bus 020 Device 003: 18d1:4ee4"; always available.
    public var summary: String
    public var usedAndroidQuirk: Bool

    /// Bus + address uniquely identify a connected device; distinguishes two of
    /// the same model.
    public var id: String { "\(busNumber).\(deviceAddress)" }

    /// Primary title shown in the UI.
    public var displayName: String {
        if !product.isEmpty {
            return manufacturer.isEmpty ? product : "\(manufacturer) \(product)"
        }
        return L(.usbDeviceFallbackName, Int(vendorID), Int(productID))
    }

    public init(vendorID: UInt16, productID: UInt16, busNumber: UInt8, deviceAddress: UInt8,
                manufacturer: String, product: String, serial: String, summary: String,
                usedAndroidQuirk: Bool) {
        self.vendorID = vendorID
        self.productID = productID
        self.busNumber = busNumber
        self.deviceAddress = deviceAddress
        self.manufacturer = manufacturer
        self.product = product
        self.serial = serial
        self.summary = summary
        self.usedAndroidQuirk = usedAndroidQuirk
    }
}

// MARK: - Network configuration

/// Network access mode. Raw values align with C's tk_ip_mode_t.
public enum IPMode: Int32, Codable, CaseIterable, Sendable {
    case dhcp = 0
    case manual = 1
    /// Revoke configuration.
    case none = 2

    public var displayName: String {
        switch self {
        case .dhcp: return L(.ipModeDhcp)
        case .manual: return L(.ipModeManual)
        case .none: return L(.ipModeNone)
        }
    }
}

/// Network access configuration for an interface.
public struct NetworkConfiguration: Codable, Hashable, Sendable {
    public var mode: IPMode
    /// The following four fields are used only when mode == .manual.
    public var address: String
    public var netmask: String
    public var router: String
    public var dnsServers: [String]
    /// Whether to also point the global default route at this interface.
    ///
    /// When off, there is only a scoped default route bound to this interface.
    /// This is needed only when a higher-priority connected service (Wi-Fi, VPN)
    /// also exists -- and the typical USB tethering scenario is exactly the case
    /// with no other network available, where this interface is naturally the
    /// primary service.
    public var setDefaultRoute: Bool

    public static let dhcp = NetworkConfiguration(mode: .dhcp)

    public init(mode: IPMode = .dhcp,
                address: String = "",
                netmask: String = "255.255.255.0",
                router: String = "",
                dnsServers: [String] = [],
                setDefaultRoute: Bool = false) {
        self.mode = mode
        self.address = address
        self.netmask = netmask
        self.router = router
        self.dnsServers = dnsServers
        self.setDefaultRoute = setDefaultRoute
    }
}

/// The interface's currently effective state.
///
/// Intentionally reads back from the system rather than restating what we
/// applied: in manual mode, whether DNS takes effect depends on whether
/// IPMonitor recognizes this service, so only a read-back can give the user
/// accurate feedback.
public struct NetworkState: Codable, Hashable, Sendable {
    public var hasAddress: Bool
    public var address: String
    public var netmask: String
    public var router: String
    public var dnsServers: [String]
    /// Configuration method reported by IPConfiguration ("DHCP" / "MANUAL" / "").
    public var method: String
    /// Service state reported by IPConfiguration (for example "BOUND").
    public var serviceState: String
    public var hasDefaultRoute: Bool
    /// Whether the global default route currently points at this interface.
    public var isPrimaryDefaultRoute: Bool

    public static let empty = NetworkState(
        hasAddress: false, address: "", netmask: "", router: "", dnsServers: [],
        method: "", serviceState: "", hasDefaultRoute: false, isPrimaryDefaultRoute: false)

    public init(hasAddress: Bool, address: String, netmask: String, router: String,
                dnsServers: [String], method: String, serviceState: String,
                hasDefaultRoute: Bool, isPrimaryDefaultRoute: Bool) {
        self.hasAddress = hasAddress
        self.address = address
        self.netmask = netmask
        self.router = router
        self.dnsServers = dnsServers
        self.method = method
        self.serviceState = serviceState
        self.hasDefaultRoute = hasDefaultRoute
        self.isPrimaryDefaultRoute = isPrimaryDefaultRoute
    }
}

// MARK: - Logs and events

public enum LogLevel: Int32, Codable, Comparable, Sendable {
    case trace = 0
    case debug = 1
    case info = 2
    case warning = 3
    case error = 4

    public static func < (lhs: LogLevel, rhs: LogLevel) -> Bool {
        lhs.rawValue < rhs.rawValue
    }

    public var label: String {
        switch self {
        case .trace: return "TRACE"
        case .debug: return "DEBUG"
        case .info: return "INFO"
        case .warning: return "WARN"
        case .error: return "ERROR"
        }
    }
}

public struct LogEntry: Codable, Hashable, Identifiable, Sendable {
    public var id: UUID
    public var level: LogLevel
    public var timestamp: Date
    public var thread: String
    public var message: String

    public init(id: UUID = UUID(), level: LogLevel, timestamp: Date, thread: String,
                message: String) {
        self.id = id
        self.level = level
        self.timestamp = timestamp
        self.thread = thread
        self.message = message
    }
}

/// Response for one helper "drain pending content" call.
///
/// Events and logs are merged into a single call to keep XPC round trips to one
/// per refresh cycle -- the UI refreshes every 500 ms, and fetching them
/// separately would double the interprocess round trips.
public struct HelperFeed: Codable, Sendable {
    public var logs: [LogEntry]
    /// Number of log entries dropped (oldest dropped when the buffer was full).
    /// The UI uses this to indicate that the log has gaps.
    public var droppedLogs: UInt64
    /// Key events that have occurred but not yet been consumed by the UI, given
    /// as text.
    public var notices: [String]

    public static let empty = HelperFeed(logs: [], droppedLogs: 0, notices: [])

    public init(logs: [LogEntry], droppedLogs: UInt64, notices: [String]) {
        self.logs = logs
        self.droppedLogs = droppedLogs
        self.notices = notices
    }
}

// MARK: - Environment

/// Runtime environment preflight result.
public struct EnvironmentReport: Codable, Hashable, Sendable {
    public var isRoot: Bool
    public var sysctlsOK: Bool
    /// Specific details when sysctls are not acceptable (including remediation
    /// commands).
    public var sysctlDetail: String
    public var fethMaxMTU: UInt32
    public var version: String
    public var buildDescription: String
    public var libusbVersion: String

    public init(isRoot: Bool, sysctlsOK: Bool, sysctlDetail: String, fethMaxMTU: UInt32,
                version: String, buildDescription: String, libusbVersion: String) {
        self.isRoot = isRoot
        self.sysctlsOK = sysctlsOK
        self.sysctlDetail = sysctlDetail
        self.fethMaxMTU = fethMaxMTU
        self.version = version
        self.buildDescription = buildDescription
        self.libusbVersion = libusbVersion
    }
}
