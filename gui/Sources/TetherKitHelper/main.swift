// tetherkit-helper -- the privileged helper running as root.
//
// Where its root comes from:
//   From launchd: /Library/LaunchDaemons/com.tetherkit.helper.plist declares
//   MachServices, so when the App connects to this Mach service, launchd launches
//   it on demand, and it starts as root. This has nothing to do with whether the
//   user pressed a fingerprint -- so "who is calling" must be verified by the
//   helper itself on every request; see TetherKitIPC/Authorization.swift.
//
// Why not SMAppService:
//   SMPrivilegedExecutables / SMAuthorizedClients rely on a code-signature
//   designated requirement binding in both directions. Under source distribution
//   (Homebrew formula), the cdhash produced by each machine's build differs, so a
//   DR hardcoded in the plist can never match. See docs/GUI-SPIKE.md section 3.3.
import Foundation
import TetherKitCore
import TetherKitIPC

// ---- Installer mode ----
//
// When launched by the App with `--install`, it takes over the process (see
// InstallerMode.swift), unrelated to the daemon mode below -- this must come
// first; log capture and orphan cleanup must not run in installer mode.
runInstallerModeIfRequested()

/// Write a line to stderr. The helper is launched by launchd, so stderr goes to
/// the LaunchDaemon log file -- when troubleshooting "the helper won't start",
/// that is the only place anything can be seen.
func writeToStandardError(_ message: String) {
    FileHandle.standardError.write(Data((message + "\n").utf8))
}

// ---- Logging ----
//
// Enable capture so the App can see the library's internal logs in the UI.
// Output to stderr is unaffected.
TetherKitLibrary.startLogCapture(level: .info)

// ---- Backstop cleanup ----
//
// If the previous run was killed with SIGKILL, destructors never ran and feth
// interfaces were left in the kernel. This is the only place that can recover
// from that -- signal handlers cannot catch SIGKILL.
do {
    let removed = try TetherKitLibrary.cleanupOrphanInterfaces()
    if removed > 0 {
        writeToStandardError(L(.helperOrphansCleaned, Int(removed)))
    }
} catch {
    writeToStandardError(L(.helperOrphanCleanupFailed, error.localizedDescription))
}

let service = HelperService()
let delegate = HelperListenerDelegate(service: service)

// ---- Graceful shutdown ----
//
// `launchctl bootout` sends SIGTERM, but Swift's deinit does not run when a
// process is terminated. Not catching it would leak interfaces in the kernel
// (the on-disk registration is still a backstop, but cleaning up on the spot is
// better).
//
// Use DispatchSource instead of a signal(2) handler: the latter runs in signal
// context, where very little is allowed (no locking, no memory allocation), yet
// the shutdown teardown we need to do requires both. DispatchSource turns it
// into a normal queue callback, removing those restrictions.
//
// SIG_IGN must come first: before DispatchSource takes over, the default action
// (terminate the process) is still in effect.
signal(SIGTERM, SIG_IGN)
let terminationSource = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .main)
terminationSource.setEventHandler {
    writeToStandardError(L(.helperSigtermReceived))
    service.shutdown()
    exit(0)
}
terminationSource.resume()

let listener = NSXPCListener(machServiceName: HelperConstants.machServiceName)
listener.delegate = delegate
listener.resume()

writeToStandardError(L(.helperReady, TetherKitLibrary.versionInfo.version))

// Block on the main runloop.
//
// Deliberately does not "exit after being idle for a while": once a session is
// running, the helper owns feth interfaces and BPF descriptors, and exiting is
// equivalent to cutting the user's network. The LaunchDaemon is launched on
// demand; staying alive does not consume resources when unused -- because in
// that case it would not have been launched at all.
dispatchMain()
