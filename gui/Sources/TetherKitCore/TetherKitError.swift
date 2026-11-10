import CTetherKit
import TetherKitIPC
import Foundation

/// Error from the C ABI.
///
/// `message` is already text that can be shown directly to the user (the library
/// writes it to that standard), so the UI does not need to translate it again.
public struct TetherKitError: LocalizedError, Sendable {
    public enum Domain: Int32, Sendable {
        case generic = 0
        case errno = 1
        case libusb = 2
        case rndis = 3
    }

    public let result: Int32
    public let domain: Domain
    public let code: Int64
    public let message: String

    public var errorDescription: String? { message }

    /// Whether this is "permission denied" -- the UI uses this to decide between
    /// requesting authorization and reporting an error.
    public var isPermissionDenied: Bool { result == TK_ERR_PERMISSION.rawValue }

    init(result: Int32, error: tk_error_t) {
        self.result = result
        self.domain = Domain(rawValue: error.domain) ?? .generic
        self.code = error.code
        let text = String(fixedCArray: error.message)
        // The C side does not always fill in a message on argument-validation
        // failure paths (for example, a plain null-pointer check), so provide a
        // backstop message here -- better than showing an empty error box in the
        // UI.
        self.message = text.isEmpty ? L(.libraryGenericFailure, Int(result)) : text
    }

    public init(message: String) {
        self.result = TK_ERR_FAILED.rawValue
        self.domain = .generic
        self.code = 0
        self.message = message
    }
}

/// Translate a C result code + error struct into a Swift throw.
func check(_ result: tk_result_t, _ error: tk_error_t) throws {
    guard result != TK_OK else { return }
    throw TetherKitError(result: result.rawValue, error: error)
}
