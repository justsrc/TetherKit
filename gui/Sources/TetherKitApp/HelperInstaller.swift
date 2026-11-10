import Foundation
import Security
import TetherKitIPC

/// Install the privileged helper payload embedded in the .app as a
/// LaunchDaemon, or uninstall it cleanly again -- the whole mechanism
/// behind the "one-click install / uninstall" buttons. The real steps
/// live in the payload's install-helper.sh / uninstall-helper.sh; this
/// file does only two things: find them, and run them as root.
///
/// Why AuthorizationExecuteWithPrivileges (AEWP):
///
///   There is exactly one goal: run the install script once as root. The
///   system offers these ways of doing that:
///
///   * SMJobBless / SMAppService -- they bind the App and the helper
///     bidirectionally through the code signing DR, and under source
///     distribution every machine has a different cdhash, so they cannot
///     work (docs/GUI-SPIKE.md section 3.3);
///   * osascript "with administrator privileges" -- it works, but it creates
///     its own authorization session, which cannot connect to the
///     AuthorizationRef the App caches (a 5 minute window) at all, so it
///     necessarily pops extra dialogs;
///   * AEWP -- its parameter is the AuthorizationRef: while the cached
///     token is still valid it pops no dialog at all, and when the right
///     is missing it raises the standard authorization dialog under
///     system.privilege.admin. It fits the existing trust model
///     (AuthorizationRef re-check, see Authorization.swift) seamlessly.
///
///   AEWP has been marked deprecated since 10.7, but on macOS 26 the
///   symbol still resolves in practice and its setuid carrier
///   /usr/libexec/security_authtrampoline is still there. It is resolved
///   here with dlsym at runtime rather than linked directly: if a future
///   system really removes it, the user sees this page degrade into
///   "please install from a terminal" instead of the app failing to
///   start at the dyld binding stage.
///
/// Why the target of AEWP is the helper binary and not the install script:
///
///   The credential AEWP gives the child process is euid=0 / ruid=the
///   ordinary user, and bash drops euid back to ruid when euid != ruid
///   and -p is not given -- running a script through AEWP directly makes
///   the root check at the top of that script fail for sure (hit on a
///   real device). So what gets launched here is the --install mode of
///   the helper binary in the payload: it first setuid(0)s, normalizing
///   the credential into exactly the same true root as sudo, merges
///   stderr into stdout (AEWP's pipe only takes stdout), and then execs
///   the install-helper.sh in the same directory.
///   See TetherKitHelper/InstallerMode.swift.
///
/// Why success and failure are not read from the exit code:
///
///   AEWP gives neither the child's pid nor its exit code, only a pipe
///   connected to the child's stdout. So this file hands out only "the
///   script finished" and "what it said"; the real verdict is reached
///   by the caller (AppModel) with one real XPC round trip -- "can
///   connect to a helper of the matching version" is inherently closer
///   to the truth than any exit code. The output is shown to the user
///   only on failure, and then only to explain why.
enum HelperInstaller {
    /// The maintenance action to perform, matching the helper binary's
    /// command line argument and the script in the payload.
    enum Action {
        case install
        case uninstall

        var flag: String {
            switch self {
            case .install: return "--install"
            case .uninstall: return "--uninstall"
            }
        }
    }

    enum Failure: LocalizedError {
        /// The embedded payload cannot be found. Typical case: the bare
        /// executable run directly by `swift run` -- only the .app
        /// assembled by build-gui.sh carries
        /// Contents/Library/HelperTools.
        case payloadMissing
        /// AEWP cannot be resolved at runtime (some future macOS removed
        /// it).
        case executorUnavailable
        /// AEWP returned an error (a user cancellation is a separate
        /// case).
        case launchFailed(OSStatus)
        case userCancelled

        var errorDescription: String? {
            switch self {
            case .payloadMissing:
                return L(.installerNoPayload)
            case .executorUnavailable:
                return L(.installerAPIUnavailable)
            case .launchFailed(let status):
                return L(.installerLaunchFailed, Int(status))
            case .userCancelled:
                return L(.authorizationCancelled)
            }
        }
    }

    /// The embedded payload directory (the helper binary, the dylib, the
    /// plist and the install scripts sit side by side in it).
    /// nil when not running from an assembled .app.
    static var payloadDirectory: URL? {
        let directory = Bundle.main.bundleURL
            .appendingPathComponent("Contents/Library/HelperTools", isDirectory: true)
        // The binary (AEWP's execution target) and the two scripts (the
        // actual install/uninstall logic) are all required.
        let required = [helperBinaryName, installScriptName, uninstallScriptName]
            .map { directory.appendingPathComponent($0).path }
        let complete = required.allSatisfy { FileManager.default.isExecutableFile(atPath: $0) }
        return complete ? directory : nil
    }

    /// A quick preflight before entering the install flow: is the payload
    /// there, is AEWP still there.
    ///
    /// It is exposed separately so the caller can find out that the
    /// install is impossible **before the authorization dialog pops up** --
    /// asking for the user's password and only then saying "this cannot
    /// be installed" is the worst possible order.
    static func preflightError() -> Failure? {
        if payloadDirectory == nil { return .payloadMissing }
        if loadExecutor() == nil { return .executorUnavailable }
        return nil
    }

    /// Run the install / uninstall script from the payload as root and
    /// return its whole output (stdout and stderr merged).
    ///
    /// It blocks on "the authorization dialog that may pop up" and on "the
    /// script execution", so the whole thing is moved to a background
    /// queue; returning means the script has run to completion (the pipe
    /// reached EOF), and it does **not** mean the action succeeded -- see
    /// "Why success and failure are not read from the exit code" in the
    /// type notes.
    static func run(_ action: Action, with token: AuthorizationToken) async throws -> String {
        guard let payload = payloadDirectory else { throw Failure.payloadMissing }
        guard let execute = loadExecutor() else { throw Failure.executorUnavailable }
        let installer = payload.appendingPathComponent(helperBinaryName).path

        return try await withCheckedThrowingContinuation { continuation in
            DispatchQueue.global(qos: .userInitiated).async {
                continuation.resume(with: Result {
                    // The token has to stay alive until AEWP returns and
                    // the script has run -- the authorization lives in
                    // it.
                    try withExtendedLifetime(token) {
                        try run(execute, token: token, installer: installer,
                                flag: action.flag)
                    }
                })
            }
        }
    }

    // MARK: - Implementation

    private static let installScriptName = "install-helper.sh"
    private static let uninstallScriptName = "uninstall-helper.sh"
    /// The file name of the helper binary in the payload, identical to the
    /// LaunchDaemon's Label.
    private static let helperBinaryName = HelperConstants.machServiceName

    /// The C prototype of AEWP. The second to last parameter is
    /// `char *const *` (a NULL-terminated argv that does not contain
    /// argv[0] -- the trampoline fills the tool path into argv[0]).
    private typealias ExecuteFunction = @convention(c) (
        AuthorizationRef,
        UnsafePointer<CChar>,
        UInt32,
        UnsafePointer<UnsafeMutablePointer<CChar>?>,
        UnsafeMutablePointer<UnsafeMutablePointer<FILE>?>?
    ) -> OSStatus

    private static func loadExecutor() -> ExecuteFunction? {
        // RTLD_DEFAULT: Security.framework is already linked by this
        // process, so the symbol is definitely in the global namespace (if
        // it still exists at all). The Darwin module does not export this
        // constant to Swift, so it has to be hardcoded to -2 as defined
        // in dlfcn.h.
        guard let symbol = dlsym(UnsafeMutableRawPointer(bitPattern: -2),
                                 "AuthorizationExecuteWithPrivileges") else { return nil }
        return unsafeBitCast(symbol, to: ExecuteFunction.self)
    }

    private static func run(_ execute: ExecuteFunction,
                            token: AuthorizationToken,
                            installer: String,
                            flag: String) throws -> String {
        // The execution target is the helper binary's install/uninstall
        // mode, not the script -- the reason is in the type notes. The
        // stderr merge is done there as well (dup2); here we only launch
        // it and collect the output.
        var pipe: UnsafeMutablePointer<FILE>?
        let status = token.withReference { reference in
            withArgv([flag]) { argv in
                execute(reference, installer, 0 /* kAuthorizationFlagDefaults */,
                        argv, &pipe)
            }
        }

        switch status {
        case errAuthorizationSuccess:
            break
        case errAuthorizationCanceled:
            throw Failure.userCancelled
        default:
            throw Failure.launchFailed(status)
        }

        guard let pipe else { return "" }
        defer { fclose(pipe) }

        // Read the underlying fd straight to EOF -- EOF means the child
        // has finished (the LaunchDaemon's stdio is given separately by
        // launchd through the StandardOutPath in the plist and does not
        // inherit this pipe, so there is no hang of the "grandchild keeps
        // the pipe open" kind).
        var output = Data()
        var buffer = [UInt8](repeating: 0, count: 4096)
        let descriptor = fileno(pipe)
        while true {
            let count = read(descriptor, &buffer, buffer.count)
            if count > 0 {
                output.append(buffer, count: count)
            } else if count == 0 {
                break
            } else if errno != EINTR {
                break
            }
        }
        return String(decoding: output, as: UTF8.self)
    }

    /// Turn a Swift string array into a NULL-terminated C argv and feed it
    /// to body.
    private static func withArgv<T>(
        _ arguments: [String],
        _ body: (UnsafePointer<UnsafeMutablePointer<CChar>?>) -> T
    ) -> T {
        var argv: [UnsafeMutablePointer<CChar>?] = arguments.map { strdup($0) }
        argv.append(nil)
        defer { argv.forEach { free($0) } }
        return argv.withUnsafeBufferPointer { body($0.baseAddress!) }
    }
}
