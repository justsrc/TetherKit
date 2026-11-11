import Darwin
import TethernetIPC
import Foundation

/// Execution entry for "one-click install / uninstall from the App": when this
/// binary is launched by AEWP with `--install` or `--uninstall`, it takes over
/// the process and runs the corresponding script from its own payload directory
/// as real root.
///
/// Why this mode is needed instead of having the App run the install script
/// through AEWP directly:
///
///   AEWP's setuid carrier (security_authtrampoline) gives the child process
///   credentials euid=0 / ruid=caller. bash drops euid back to ruid when
///   euid != ruid and -p is not set -- an old rule against setuid scripts, and
///   zsh/dash behave the same. The root check at the top of the script then
///   inevitably reports "root required". Confirmed on a real machine, not a
///   theoretical deduction.
///
///   Binaries are not subject to that rule. Starting here with euid=0, setuid(0)
///   zeroes ruid/suid as well, yielding credentials identical to sudo; the
///   script exec'd afterward, together with all its child processes (cp, chown,
///   launchctl), then has no credential ambiguity.
///
/// Three details:
///   * stderr is merged into stdout: AEWP provides only a pipe connected to
///     stdout, and the script's die paths go to stderr; without merging, the App
///     side cannot see the failure reason. Do this first so that even a setuid
///     failure reaches the pipe.
///   * setgid must come before setuid -- the other way around, euid is no longer
///     0 and setgid will fail.
///   * The script path is taken from this executable's directory: the payload is
///     flat, so it works wherever it is copied. The copy installed in
///     /Library/PrivilegedHelperTools has no scripts beside it and will produce
///     a clear error instead of a mis-install.
func runInstallerModeIfRequested() {
    let scriptName: String
    switch CommandLine.arguments.dropFirst().first {
    case "--install": scriptName = "install-helper.sh"
    case "--uninstall": scriptName = "uninstall-helper.sh"
    default: return
    }

    dup2(STDOUT_FILENO, STDERR_FILENO)

    guard setgid(0) == 0, setuid(0) == 0 else {
        print(L(.installerNeedsRoot, Int(geteuid()), Int(getuid()))
              + L(.installerUseTerminal, scriptName))
        exit(1)
    }

    let executable = URL(fileURLWithPath: Bundle.main.executablePath
                                          ?? CommandLine.arguments[0])
    let script = executable.resolvingSymlinksInPath().deletingLastPathComponent()
        .appendingPathComponent(scriptName).path
    guard FileManager.default.isExecutableFile(atPath: script) else {
        print(L(.installerScriptMissing, script))
        exit(1)
    }

    // execv does not return; returning means failure.
    var argv: [UnsafeMutablePointer<CChar>?] = [strdup("/bin/bash"), strdup(script), nil]
    execv("/bin/bash", &argv)
    perror("execv /bin/bash")
    exit(1)
}
