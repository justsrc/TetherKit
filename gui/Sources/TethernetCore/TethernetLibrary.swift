import CTethernet
import Foundation
import TethernetIPC

/// Process-wide interface to libtethernet: version, environment preflight,
/// device enumeration, and logging.
///
/// This group does not require root; both the App and the helper can call it.
public enum TethernetLibrary {
    /// Maximum number of devices enumerated in one call.
    ///
    /// 16 is a limit that will never be hit: 16 RNDIS devices plugged into one
    /// machine at the same time is already absurd, and a fixed array buys "no
    /// memory management". If it really is exceeded, tn_list_devices reports the
    /// true total; we simply do not show the extra entries.
    private static let deviceCapacity = 16

    /// Maximum number of log entries drained in one call.
    ///
    /// Matches the C-side ring buffer capacity (256): smaller would never fully
    /// drain a backlog; larger is meaningless.
    private static let logCapacity = 256

    /// Sync the UI language to libtethernet.
    ///
    /// Must be called -- otherwise the UI would be in one language while library
    /// logs in the log card would be in another. Logs already queued are not
    /// re-rendered (they became strings at the moment they were produced), so the
    /// sooner this is called the better; after a language switch, newly produced
    /// logs are immediately in the new language.
    public static func setLanguage(_ language: Language) {
        tn_set_language(language.cValue)
    }

    public static var versionInfo: (version: String, build: String, libusb: String) {
        var info = tn_version_info_t()
        tn_version(&info)
        return (String(fixedCArray: info.text),
                String(fixedCArray: info.build),
                String(fixedCArray: info.libusb))
    }

    /// Runtime environment preflight. Always succeeds -- "environment not
    /// acceptable" is a result, not a failure.
    public static func checkEnvironment() -> EnvironmentReport {
        var raw = tn_environment_t()
        _ = tn_check_environment(&raw)
        let version = versionInfo
        return EnvironmentReport(
            isRoot: raw.is_root,
            sysctlsOK: raw.sysctls_ok,
            sysctlDetail: String(fixedCArray: raw.sysctl_detail),
            fethMaxMTU: raw.feth_max_mtu,
            version: version.version,
            buildDescription: version.build,
            libusbVersion: version.libusb)
    }

    /// Enumerate RNDIS devices.
    ///
    /// - Parameter readStrings: Whether to read manufacturer / product / serial.
    ///   This requires opening the device; while a session is running the device
    ///   is held exclusively, so pass false then to avoid a pointless attempt.
    public static func listDevices(readStrings: Bool = true) throws -> [DeviceDescriptor] {
        var buffer = [tn_device_info_t](repeating: tn_device_info_t(), count: deviceCapacity)
        var count = 0
        var error = tn_error_t()

        let result = tn_list_devices(&buffer, deviceCapacity, &count, readStrings, &error)
        try check(result, error)

        return buffer.prefix(min(count, deviceCapacity)).map { raw in
            DeviceDescriptor(
                vendorID: raw.vendor_id,
                productID: raw.product_id,
                busNumber: raw.bus_number,
                deviceAddress: raw.device_address,
                manufacturer: String(fixedCArray: raw.manufacturer),
                product: String(fixedCArray: raw.product),
                serial: String(fixedCArray: raw.serial),
                summary: String(fixedCArray: raw.description),
                usedAndroidQuirk: raw.used_android_quirk)
        }
    }

    // MARK: - Logs

    /// Enable log capture and set the level.
    public static func startLogCapture(level: LogLevel = .info) {
        tn_set_log_level(level.rawValue)
        tn_enable_log_capture(true)
    }

    public static func stopLogCapture() {
        tn_enable_log_capture(false)
    }

    /// Drain buffered logs. Returns the entries and the number dropped because
    /// the buffer was full.
    public static func drainLogs() -> (entries: [LogEntry], dropped: UInt64) {
        var buffer = [tn_log_record_t](repeating: tn_log_record_t(), count: logCapacity)
        var dropped: UInt64 = 0
        let taken = tn_drain_logs(&buffer, logCapacity, &dropped)

        let entries = buffer.prefix(taken).map { raw in
            LogEntry(
                level: LogLevel(rawValue: raw.level) ?? .info,
                timestamp: Date(timeIntervalSince1970: Double(raw.wall_nanos) / 1_000_000_000),
                thread: String(fixedCArray: raw.thread),
                message: String(fixedCArray: raw.message))
        }
        return (Array(entries), dropped)
    }

    // MARK: - Orphan interfaces

    /// Clean up feth interfaces left in the kernel after the process was killed.
    /// Requires root.
    ///
    /// The helper should call this unconditionally on startup: if the previous
    /// run was killed with SIGKILL, interfaces remain in the kernel, and neither
    /// RAII nor signal handlers can recover from that case.
    @discardableResult
    public static func cleanupOrphanInterfaces() throws -> Int {
        var removed = 0
        var error = tn_error_t()
        let result = tn_cleanup_orphan_interfaces(&removed, &error)
        try check(result, error)
        return removed
    }
}
