import Security
import XCTest

@testable import TethernetIPC

/// Regression tests for the authorization credential lifecycle.
///
/// This pins down a real pitfall that was hit:
///   The 32 bytes produced by `AuthorizationMakeExternalForm` are not the
///   credential itself, only a key pointing at that authorization inside
///   securityd. If the App side releases the AuthorizationRef early (especially
///   with `.destroyRights`), the helper's restore will fail -- and the failure
///   message is just `-60005`, with nothing in the code indicating "it was
///   destroyed by us ahead of time".
///
///   These cases never show an authorization dialog: they only call
///   `AuthorizationCreate` (to establish an empty authorization session), never
///   `AuthorizationCopyRights`, so they need no user interaction and can run in
///   CI.
final class AuthorizationTests: XCTestCase {
    /// Externalize an AuthorizationRef.
    private func externalForm(of authorization: AuthorizationRef) throws -> Data {
        var external = AuthorizationExternalForm()
        let status = AuthorizationMakeExternalForm(authorization, &external)
        try XCTSkipUnless(status == errAuthorizationSuccess,
                          "AuthorizationMakeExternalForm failed (\(status)), skipping")
        return withUnsafeBytes(of: &external) { Data($0) }
    }

    /// Simulate the helper-side restore and return the status code.
    private func restore(_ data: Data) -> OSStatus {
        var external = AuthorizationExternalForm()
        _ = withUnsafeMutableBytes(of: &external) { destination in
            data.copyBytes(to: destination.bindMemory(to: UInt8.self))
        }
        var restored: AuthorizationRef?
        let status = AuthorizationCreateFromExternalForm(&external, &restored)
        if let restored {
            AuthorizationFree(restored, [])
        }
        return status
    }

    /// While the token is alive, the other side can restore it. This is the
    /// normal path.
    func testExternalFormRestorableWhileTokenAlive() throws {
        var authorization: AuthorizationRef?
        XCTAssertEqual(AuthorizationCreate(nil, nil, [], &authorization), errAuthorizationSuccess)
        let reference = try XCTUnwrap(authorization)

        let token = AuthorizationToken(authorization: reference,
                                       externalForm: try externalForm(of: reference))

        XCTAssertEqual(restore(token.externalForm), errAuthorizationSuccess,
                       "while the token is alive, the helper side must be able to restore the credential")

        withExtendedLifetime(token) {}
    }

    /// Once the token is released, it can no longer be restored -- this was
    /// exactly the cause of -60005.
    ///
    /// Conversely, if this case ever turns red, it means "early release is fine
    /// now", and the entire AuthorizationToken layer has no reason to exist; the
    /// system behavior change should be understood before touching it.
    func testExternalFormUnusableAfterTokenReleased() throws {
        var authorization: AuthorizationRef?
        XCTAssertEqual(AuthorizationCreate(nil, nil, [], &authorization), errAuthorizationSuccess)
        let reference = try XCTUnwrap(authorization)

        let capturedForm: Data
        do {
            let token = AuthorizationToken(authorization: reference,
                                           externalForm: try externalForm(of: reference))
            capturedForm = token.externalForm
        }  // token deinit runs here, destroying the rights with it

        XCTAssertEqual(restore(capturedForm), errAuthorizationDenied,
                       "the external form must be invalid after the token is released; this was the cause of -60005")
    }
}
