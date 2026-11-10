import Foundation

/// XPC interface called by the App on the helper.
///
/// Which methods require authorization and which do not:
///
///   Every method with an `authorization` parameter requires it -- the parameter
///   is the external form (32 bytes) of a credential the App obtained through
///   AuthorizationCopyRights, and the helper verifies that it actually contains
///   the required right.
///
///   Probe methods (helperVersion / status / queryNetwork / drainFeed)
///   intentionally do not require authorization. The reason: if even "is the
///   helper installed?" required a fingerprint prompt first, the user could not
///   tell "helper not installed" apart from "authorization failed" -- two
///   completely different failures. These methods are read-only, and the
///   information they expose is limited to this machine's network state.
///
/// Why privileged methods reply with two values, (String?, Bool):
///   The second value indicates "this failure was an authorization problem". The
///   App needs to distinguish the two cases: an expired authorization can be
///   retried after re-prompting, while an operation failure (for example, no
///   device connected) will not change no matter how many times it is retried.
///   Mixing them into one error message would force the App to match strings --
///   the most fragile kind of coupling.
///
/// Why errors use String? instead of NSError:
///   Passing NSError over XPC requires both sides to deserialize its userInfo;
///   if that contains a non-encodable object it becomes a runtime exception. And
///   what we need to show the user is already just a message string, so passing
///   a plain string is simple and cannot fail. nil means success.
@objc public protocol TetherKitHelperProtocol {
    /// Connectivity probe. Does not require authorization -- otherwise "not
    /// installed" and "not authorized" would be indistinguishable.
    func helperVersion(reply: @escaping (String) -> Void)

    /// Runtime environment preflight (root status, feth creation-time sysctls,
    /// MTU limit).
    func environment(reply: @escaping (Data?, String?) -> Void)

    /// Enumerate RNDIS devices.
    ///
    /// Enumeration is done by the helper rather than the App: the App side can
    /// also enumerate (no root required), but once a session is running the
    /// device is held exclusively by the helper and the App's attempts to read
    /// string descriptors will fail. Going through the helper avoids that
    /// inconsistency.
    func listDevices(reply: @escaping (Data?, String?) -> Void)

    /// Start an RNDIS session. Requires authorization.
    func startSession(authorization: Data, configuration: Data,
                      reply: @escaping (String?, Bool) -> Void)

    /// Stop the session and tear down the virtual interfaces. Requires
    /// authorization.
    func stopSession(authorization: Data, reply: @escaping (String?, Bool) -> Void)

    /// Fetch a session status snapshot.
    func sessionStatus(reply: @escaping (Data?, String?) -> Void)

    /// Apply a network access mode (DHCP / manual IP / revoke) to an interface.
    /// Requires authorization.
    ///
    /// In DHCP mode this call blocks until a lease is obtained or it times out
    /// (library-internal limit 10 seconds), so the helper side must not place it
    /// on a queue that would serially block other requests.
    func applyNetwork(authorization: Data, interface: String, configuration: Data,
                      reply: @escaping (String?, Bool) -> Void)

    /// Read back the interface's effective IP state.
    func queryNetwork(interface: String, reply: @escaping (Data?, String?) -> Void)

    /// Drain logs and notices accumulated on the helper side.
    func drainFeed(reply: @escaping (Data?) -> Void)

    /// Tell the helper which language to use when rendering the text it
    /// produces. Does not require authorization -- it affects only wording, not
    /// behavior.
    ///
    /// Why this is necessary: the helper runs as root under launchd and cannot
    /// see the user's language preference, yet the notices it produces and the
    /// libtetherkit logs are shown as-is in the App's log card. Without keeping
    /// them in sync, the UI and the logs would speak different languages.
    ///
    /// The parameter is the rawValue of `Language` (currently `"english"`).
    /// Passing a string rather than an integer means a future language can be
    /// added so that an old helper receiving an unrecognized value ignores it
    /// instead of treating it as some enum case that happens to exist. An
    /// unrecognized value (including the removed `"chinese"` spelling) leaves
    /// the helper rendering in its current language.
    func setLanguage(_ rawValue: String, reply: @escaping () -> Void)
}
