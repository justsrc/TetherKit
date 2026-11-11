import Foundation
import Security

/// Obtaining (App side) and verifying (helper side) authorization credentials.
///
/// The two sides are deliberately kept in the same file:
///   They differ by only one flag (`.interactionAllowed`), and setting that flag
///   in the wrong place is a security vulnerability. Kept together, the other
///   side is in view when one is changed.
///
/// One sentence that must be remembered: authorization != root.
///   A successful `AuthorizationCopyRights` does not change anything about the
///   process -- uid and euid are untouched. All it produces is a credential
///   proving "the user confirmed with a password/fingerprint at some point".
///   The privilege itself must come from elsewhere (here, the helper launched as
///   root by launchd).
///
///   So the order cannot be reversed: it is not "show the fingerprint prompt to
///   get root, then create the interface", but "first have a resident root
///   helper, then on each operation show the fingerprint prompt to obtain a
///   credential and hand it to the helper for verification".

/// Holder of one authorization.
///
/// The only reason it exists: the external form is not the credential itself,
/// only a reference.
///
///   The 32 bytes produced by `AuthorizationMakeExternalForm` contain no rights
///   information; they are just a key pointing at that authorization inside
///   securityd. As soon as the App side releases the AuthorizationRef (especially
///   with `.destroyRights`), the thing in securityd is gone -- the helper's
///   subsequent `AuthorizationCreateFromExternalForm` will fail with
///   `errAuthorizationDenied (-60005)`.
///
///   So the AuthorizationRef must stay alive until the XPC round trip has
///   finished. Holding it in a class and letting ARC manage that is far more
///   reliable than writing "remember to free at the end" at every call site.
///
/// Why it is safe to mark @unchecked Sendable:
///   HelperInstaller needs to pass the token to a background queue (AEWP blocks
///   on the authorization dialog and cannot occupy the main thread). There is no
///   mutable state here -- both stored properties are let; AuthorizationRef
///   itself is thread-safe per Authorization Services documentation (the real
///   state lives in the securityd process, and cross-process calls are
///   serialized by nature); ARC guarantees deinit runs exactly once.
public final class AuthorizationToken: @unchecked Sendable {
    /// 32-byte external form that can be passed across processes to the helper.
    public let externalForm: Data

    private let authorization: AuthorizationRef

    init(authorization: AuthorizationRef, externalForm: Data) {
        self.authorization = authorization
        self.externalForm = externalForm
    }

    /// Briefly lend the underlying AuthorizationRef to an API that needs the ref
    /// itself (currently only `AuthorizationExecuteWithPrivileges`, which takes a
    /// ref, not the external form).
    ///
    /// Exposed as a scoped borrow rather than a property: the ref's lifetime is
    /// owned by this class; storing it outside the closure would become a dangling
    /// reference as soon as the token is released -- a mistake the compiler cannot
    /// catch.
    public func withReference<T>(_ body: (AuthorizationRef) throws -> T) rethrows -> T {
        try body(authorization)
    }

    deinit {
        // With .destroyRights: the credential belongs to the App and is destroyed
        // when done, leaving no long-lived pass in the process. (The helper must
        // not pass this flag when verifying, otherwise it would also invalidate the
        // App-side authorization -- see AuthorizationVerifier.)
        AuthorizationFree(authorization, [.destroyRights])
    }
}

/// App side: request authorization from the user and obtain a credential that
/// can be passed across processes.
public enum AuthorizationBroker {
    public enum Failure: LocalizedError {
        case userCancelled
        case denied(OSStatus)
        case internalFailure(OSStatus)

        public var errorDescription: String? {
            switch self {
            case .userCancelled:
                return L(.authorizationCancelled)
            case .denied(let status):
                return L(.authorizationDenied, Int(status))
            case .internalFailure(let status):
                return L(.authorizationSessionFailed, Int(status))
            }
        }
    }

    /// Show the system authorization dialog and, on success, return a token
    /// holding this authorization.
    ///
    /// The token should be cached and reused, not requested again for every
    /// operation:
    ///
    ///   The measured parameters for the `system.privilege.admin` rule are
    ///   `shared = false`, `timeout = 300`:
    ///     * `shared = false` -- credentials are not shared across
    ///       AuthorizationRefs. Creating a new ref with `AuthorizationCreate`
    ///       for each operation inevitably requires the user to authenticate
    ///       again.
    ///     * `timeout = 300` -- but on the same ref, the credential remains
    ///       valid for 5 minutes.
    ///
    ///   So showing a prompt for "connect", "configure network", and
    ///   "disconnect" separately is purely because we create a new ref each
    ///   time. Reusing the same token means only one authentication within 5
    ///   minutes. This is also what Apple does in EvenBetterAuthorizationSample.
    ///
    ///   Once expired, helper-side verification will fail, and the caller can
    ///   discard the cache and request a new one.
    ///
    /// The caller must keep the token alive until the XPC round trip has
    /// finished. Once the token is released, the authorization in securityd is
    /// gone and the helper will fail to restore the external form with
    /// `errAuthorizationDenied (-60005)`.
    ///
    /// - Parameter prompt: A sentence shown in the system authorization dialog
    ///   explaining what this authorization is for. Without it, the dialog shows
    ///   only a bare "wants to make changes".
    ///
    /// Must be called on the main thread -- it presents UI.
    public static func requestAuthorization(
        right: String = HelperConstants.privilegedRightName,
        prompt: String? = nil
    ) throws -> AuthorizationToken {
        var authorization: AuthorizationRef?
        let createStatus = AuthorizationCreate(nil, nil, [], &authorization)
        guard createStatus == errAuthorizationSuccess, let authorization else {
            throw Failure.internalFailure(createStatus)
        }
        // Release immediately on the failure path; hand ownership to
        // AuthorizationToken on the success path.
        var handedOff = false
        defer {
            if !handedOff {
                AuthorizationFree(authorization, [.destroyRights])
            }
        }

        let copyStatus = copyRights(authorization, right: right, prompt: prompt)
        guard copyStatus == errAuthorizationSuccess else {
            throw copyStatus == errAuthorizationCanceled
                ? Failure.userCancelled
                : Failure.denied(copyStatus)
        }

        var external = AuthorizationExternalForm()
        let externalStatus = AuthorizationMakeExternalForm(authorization, &external)
        guard externalStatus == errAuthorizationSuccess else {
            throw Failure.internalFailure(externalStatus)
        }

        handedOff = true
        return AuthorizationToken(authorization: authorization,
                                  externalForm: withUnsafeBytes(of: &external) { Data($0) })
    }

    /// Request the right, showing a prompt when necessary.
    ///
    /// Extracted because constructing the environment items nests several
    /// `withUnsafe*` layers -- those buffers must stay alive until
    /// `AuthorizationCopyRights` returns, and inlining this in the main flow
    /// makes it easy for a later "tidy up" to turn them into dangling pointers.
    private static func copyRights(_ authorization: AuthorizationRef,
                                   right: String,
                                   prompt: String?) -> OSStatus {
        var name = Array(right.utf8CString)
        var promptKey = Array(kAuthorizationEnvironmentPrompt.utf8CString)
        var promptValue = Array(prompt?.utf8 ?? "".utf8)

        return name.withUnsafeMutableBufferPointer { nameBuffer in
            var rightItem = AuthorizationItem(name: nameBuffer.baseAddress!, valueLength: 0,
                                              value: nil, flags: 0)
            return withUnsafeMutablePointer(to: &rightItem) { rightPointer in
                var rights = AuthorizationRights(count: 1, items: rightPointer)

                // The App side must include .interactionAllowed -- the prompt is
                // exactly what we want. .preAuthorize grants the right on the
                // spot rather than at actual use, so the credential enters this
                // ref's cache and can be reused for subsequent operations.
                let flags: AuthorizationFlags = [.extendRights, .interactionAllowed, .preAuthorize]

                guard prompt != nil else {
                    return AuthorizationCopyRights(authorization, &rights, nil, flags, nil)
                }
                return promptKey.withUnsafeMutableBufferPointer { keyBuffer in
                    promptValue.withUnsafeMutableBufferPointer { valueBuffer in
                        var promptItem = AuthorizationItem(
                            name: keyBuffer.baseAddress!,
                            valueLength: valueBuffer.count,
                            value: valueBuffer.baseAddress,
                            flags: 0)
                        return withUnsafeMutablePointer(to: &promptItem) { promptPointer in
                            var environment = AuthorizationEnvironment(count: 1,
                                                                       items: promptPointer)
                            return AuthorizationCopyRights(authorization, &rights, &environment,
                                                           flags, nil)
                        }
                    }
                }
            }
        }
    }
}

/// Helper side: verify a credential handed over by the caller.
public enum AuthorizationVerifier {
    public enum Failure: LocalizedError {
        case malformedCredential
        case restoreFailed(OSStatus)
        case rightNotHeld(OSStatus)

        public var errorDescription: String? {
            switch self {
            case .malformedCredential:
                return L(.authorizationBlobMalformed)
            case .restoreFailed(let status):
                return L(.authorizationRestoreFailed, Int(status))
            case .rightNotHeld(let status):
                return L(.authorizationRightMissing, Int(status))
            }
        }
    }

    /// Verify that the credential already contains the specified right. Throws
    /// if it does not.
    ///
    /// Three details that must be followed:
    ///
    ///   1. Never include `.interactionAllowed`. A daemon has no UI session; if
    ///      it could show a prompt, any process able to connect to the Mach
    ///      service could trigger system authorization dialogs at will to harass
    ///      the user. This step only checks "does this credential already hold
    ///      this right?" -- it does not acquire new rights.
    ///
    ///   2. `AuthorizationFree` must not include `.destroyRights`. The
    ///      credential belongs to the App; the helper only borrows it for
    ///      checking. Including the flag would also invalidate the App-side
    ///      authorization.
    ///
    ///   3. Verification must happen on every privileged call. The helper's root
    ///      comes from launchd and has nothing to do with whether the user
    ///      pressed a fingerprint -- it is root from the moment it starts, and
    ///      any process able to connect to the Mach service can send requests.
    ///      So "who is calling" can only be answered by the helper itself.
    public static func verify(externalForm data: Data,
                              right: String = HelperConstants.privilegedRightName) throws {
        guard data.count == MemoryLayout<AuthorizationExternalForm>.size else {
            throw Failure.malformedCredential
        }

        var external = AuthorizationExternalForm()
        _ = withUnsafeMutableBytes(of: &external) { destination in
            data.copyBytes(to: destination.bindMemory(to: UInt8.self))
        }

        var authorization: AuthorizationRef?
        let restoreStatus = AuthorizationCreateFromExternalForm(&external, &authorization)
        guard restoreStatus == errAuthorizationSuccess, let authorization else {
            throw Failure.restoreFailed(restoreStatus)
        }
        // Note: no .destroyRights; see detail 2 above.
        defer { AuthorizationFree(authorization, []) }

        var name = Array(right.utf8CString)
        let checkStatus: OSStatus = name.withUnsafeMutableBufferPointer { buffer in
            var item = AuthorizationItem(name: buffer.baseAddress!, valueLength: 0,
                                         value: nil, flags: 0)
            return withUnsafeMutablePointer(to: &item) { itemPointer in
                var rights = AuthorizationRights(count: 1, items: itemPointer)
                // Only .extendRights, no .interactionAllowed; see detail 1 above.
                return AuthorizationCopyRights(authorization, &rights, nil, [.extendRights], nil)
            }
        }
        guard checkStatus == errAuthorizationSuccess else {
            throw Failure.rightNotHeld(checkStatus)
        }
    }
}
