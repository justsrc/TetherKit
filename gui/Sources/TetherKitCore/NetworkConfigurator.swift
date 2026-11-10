import CTetherKit
import Foundation
import TetherKitIPC

/// Network access configuration for a virtual interface.
///
/// `apply` / `clear` require root (called only from the helper); `query` does not.
public enum NetworkConfigurator {
    /// Capacity of each address field in tk_ip_config_t, used to locate rows in
    /// the 2D array `dns[4][46]`.
    private static let addressStride = Int(TK_ADDRESS_CAPACITY)
    private static let maxDNSServers = Int(TK_DNS_MAX)

    /// Apply configuration.
    ///
    /// In DHCP mode this blocks until a lease is obtained or it times out
    /// (library-internal limit 10 seconds). This is deliberate: "applied, poll it
    /// yourself" cannot give the UI meaningful success/failure feedback. The
    /// caller must ensure this is not called on the main thread.
    public static func apply(_ configuration: NetworkConfiguration, to interface: String) throws {
        var raw = tk_ip_config_t()
        tk_ip_config_init(&raw)
        raw.mode = configuration.mode.rawValue
        raw.set_default_route = configuration.setDefaultRoute

        if configuration.mode == .manual {
            setFixedCArray(&raw.address, to: configuration.address)
            setFixedCArray(&raw.netmask, to: configuration.netmask)
            setFixedCArray(&raw.router, to: configuration.router)

            let servers = configuration.dnsServers
                .map { $0.trimmingCharacters(in: .whitespaces) }
                .filter { !$0.isEmpty }
                .prefix(maxDNSServers)
            for (index, server) in servers.enumerated() {
                setFixedCArrayRow(&raw.dns, row: index, stride: addressStride, to: server)
            }
            raw.dns_count = Int32(servers.count)
        }

        var error = tk_error_t()
        let result = interface.withCString { tk_net_apply($0, &raw, &error) }
        try check(result, error)
    }

    /// Revoke the IP configuration on an interface.
    public static func clear(interface: String) throws {
        var error = tk_error_t()
        let result = interface.withCString { tk_net_clear($0, &error) }
        try check(result, error)
    }

    /// Read back the interface's effective state.
    ///
    /// Returns an all-empty state instead of throwing when the interface does not
    /// yet exist -- the UI also refreshes before a session is up, and surfacing a
    /// spurious error there would only mislead.
    public static func query(interface: String) throws -> NetworkState {
        var raw = tk_net_state_t()
        var error = tk_error_t()
        let result = interface.withCString { tk_net_query($0, &raw, &error) }
        try check(result, error)

        var servers: [String] = []
        for index in 0..<Int(max(0, min(raw.dns_count, Int32(maxDNSServers)))) {
            let server = fixedCArrayRow(raw.dns, row: index, stride: addressStride)
            if !server.isEmpty { servers.append(server) }
        }

        return NetworkState(
            hasAddress: raw.has_address,
            address: String(fixedCArray: raw.address),
            netmask: String(fixedCArray: raw.netmask),
            router: String(fixedCArray: raw.router),
            dnsServers: servers,
            method: String(fixedCArray: raw.method),
            serviceState: String(fixedCArray: raw.service_state),
            hasDefaultRoute: raw.has_default_route,
            isPrimaryDefaultRoute: raw.is_primary_default_route)
    }
}

// MARK: - Validation

/// Input validation for the manual IP form.
///
/// Kept in the shared layer rather than the UI: the helper side must also reject
/// bad input (XPC can be reached by any local process), and both sides must use
/// the same rules to avoid "UI accepts but helper rejects" splits.
public enum NetworkValidator {
    /// Whether the text is a valid dotted-decimal IPv4 address.
    public static func isValidIPv4(_ text: String) -> Bool {
        var address = in_addr()
        return text.withCString { inet_pton(AF_INET, $0, &address) } == 1
    }

    /// Whether the text is a valid subnet mask.
    ///
    /// Stricter than "is a valid IPv4 address": a mask's binary form must be
    /// contiguous 1s followed by contiguous 0s. Values like 255.255.0.255 pass
    /// inet_pton but are wrong as masks, and the kernel reacts to them with a
    /// bizarre route that is very hard to diagnose -- so reject them at entry.
    public static func isValidNetmask(_ text: String) -> Bool {
        var address = in_addr()
        guard text.withCString({ inet_pton(AF_INET, $0, &address) }) == 1 else { return false }
        let host = UInt32(bigEndian: address.s_addr)
        // A contiguous mask is necessary and sufficient: bitwise-not plus one is
        // a power of two (or zero, corresponding to 255.255.255.255).
        let inverted = ~host
        return inverted & (inverted &+ 1) == 0
    }

    /// Validate a manual IP configuration item by item and return the first
    /// failure message; returns nil when everything passes.
    public static func validationMessage(for configuration: NetworkConfiguration) -> String? {
        guard configuration.mode == .manual else { return nil }

        if !isValidIPv4(configuration.address) {
            return L(.invalidIPAddress)
        }
        if !isValidNetmask(configuration.netmask) {
            return L(.invalidNetmask)
        }
        let router = configuration.router.trimmingCharacters(in: .whitespaces)
        if !router.isEmpty, !isValidIPv4(router) {
            return L(.invalidRouter)
        }
        if configuration.setDefaultRoute, router.isEmpty {
            return L(.routerRequiredForDefaultRoute)
        }
        for server in configuration.dnsServers where !server.trimmingCharacters(in: .whitespaces).isEmpty {
            if !isValidIPv4(server.trimmingCharacters(in: .whitespaces)) {
                return L(.invalidDNSServer, server)
            }
        }
        return nil
    }
}
