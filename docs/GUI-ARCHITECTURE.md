# GUI implementation notes

This document records what the TetherKit graphical interface **has
implemented, why it is implemented that way, and which parts must not be
touched**.

Companion reading: [GUI-SPIKE.md](GUI-SPIKE.md) records the feasibility spike
from before the work started (in particular the three routes that were ruled
out); this document records the structure and the constraints after it landed.
**Read both of them before changing any GUI code** -- it saves re-learning the
pitfalls the hard way.

---

## 1. Process and trust model

```
+---------------------------------------------------+         +-------------------------------------------+
|  TetherKit.app  (uid 501)                         |         |  tetherkit-helper  (uid 0)                |
|                                                   |   XPC   |                                           |
|  - SwiftUI interface                              | ------> |  - holds the RNDIS session                |
|  - pops the system auth dialog for a credential   |         |  - creates/destroys feth, opens BPF       |
|  - polls the state on a timer to refresh the UI   | <------ |  - configures the IP (ipconfig / route)   |
|  - never touches USB, never touches the interface |         |  - re-checks the credential on every call |
+---------------------------------------------------+         +-------------------------------------------+
                                                                    ^
                                                                    | started on demand (MachServices)
                                                              +-----+------+
                                                              |  launchd   |
                                                              +------------+
```

**The helper's root comes from launchd and has nothing to do with whether the
user touched their fingerprint.** It is root from the moment it starts, and any
process that can reach the Mach service can send requests. So "who is calling"
has to be answered by the helper itself -- every privileged method requires the
external form of the `AuthorizationRef` the App just obtained, and the helper
re-checks it on the spot.

Security rests on **credential re-checking**, not on **code-signature
verification**. The latter (the SMJobBless approach) depends on the
certificate's designated requirement, and under source distribution the cdhash
produced on each machine differs, so a hard-coded DR can never match. Credential
re-checking is the better model for an open-source tool: whoever built it, it
is equally safe.

> **One sentence to remember: authorization != root.**
> A successful `AuthorizationCopyRights` changes nothing about the process; not
> one of uid / euid moves. It only produces a credential that says "the user
> confirmed at some moment". The order cannot be reversed: it is not "pop the
> dialog and get root", but "have a resident root helper first, pop the dialog,
> then hand the credential to it for re-checking".

---

## 2. Directory layout

```
include/tetherkit/capi/tetherkit_c.h   the only extern "C" boundary in the project
src/capi/                              C ABI implementation
|-- capi_support.{h,cc}                fixed-size buffer copies, error translation, interface name checks
|-- core_foundation_support.{h,cc}     minimal RAII wrappers for CF / SCDynamicStore
|-- environment.cc                     version, environment preflight, device enumeration (no root needed)
|-- log_ring.cc                        log ring buffer
|-- net_config.cc                      DHCP / static IP / status read-back
|-- orphan_cleanup.cc                  on-disk feth registry and orphan cleanup
|-- process_runner.{h,cc}              runs external tools through posix_spawn (never through a shell)
`-- session.cc                         session lifecycle, status snapshots, event polling

gui/
|-- Package.swift                      SwiftPM project
|-- Sources/
|   |-- CTetherKit/                    module map for the C ABI (the headers are symlinks)
|   |-- TetherKitIPC/                  shared by the App and the helper: protocol, models, authorization, string table
|   |-- TetherKitCore/                 Swift wrapper of the C ABI
|   |-- TetherKitHelper/               privileged helper
|   |-- TetherKitUIModel/              main window page model (SidebarPage: fixed colors and
|   |                                  session state colors, persisted selection parsing) -- a
|   |                                  separate library so that it can be unit tested (an executable
|   |                                  target cannot be @testable imported)
|   `-- TetherKitApp/                  SwiftUI interface (main window management and the page views)
|-- Tests/TetherKitIPCTests/           authorization credential lifecycle + string table placeholder consistency
|-- Tests/TetherKitUIModelTests/       page tint semantics + persisted selection parsing
|-- Resources/                         Info.plist, LaunchDaemon plist
`-- Scripts/                           build / install / uninstall scripts
```

---

## 3. Data flow

| Direction | Mechanism | Period |
|---|---|---|
| library -> helper: status | `tk_session_status_get` copies a snapshot | on demand |
| library -> helper: events | `tk_session_poll_events` takes from the ring queue | rides along with the status query |
| library -> helper: logs | `tk_drain_logs` takes from the ring queue | every `drainFeed` |
| helper -> App | XPC, JSON-encoded Codable | 500 ms |

**Not a single callback crosses the language boundary anywhere in the chain.**
This is deliberate: the low-level callbacks arrive from the libusb event
thread and from the control thread, so crossing languages means marshalling;
worse is re-entrancy -- calling stop from inside a callback self-waits into a
deadlock (`Stop()` joins the control thread). Everything is changed to "queue
inside the library, the host polls".

---

## 4. Key implementation constraints (do not break these when changing the code)

### 4.1 C ABI

- **No ownership crosses the boundary.** Output always goes into the
  fixed-length struct the caller provides. The only exception is `tk_session_t*`,
  whose create / destroy pairing is explicit.
- **Truncation of a fixed-length buffer must land on a UTF-8 character
  boundary.** The error messages can contain non-ASCII characters, and cutting
  them hard at a byte boundary turns the whole `String(cString:)` on the Swift
  side into replacement characters. Both directions need care: on the C side it
  is `CopyText` in `capi_support.h`, on the Swift side it is `CInterop.swift`.
- **The C enums and the C++ enums are cast directly on the strength of a
  matching order**, and a set of `static_assert`s in `session.cc` welds that
  down. Inserting an enum value in the middle shifts them silently, with no
  error at all.
- **The member declaration order of `tk_session` is constrained**: `events` must
  come before `runtime`, so that on destruction `runtime` joins the control
  thread first. The other way round is a use-after-free.

### 4.2 Runtime threading model

`Runtime` owns its own control thread; **the start sequence, the keep-alive
loop and the shutdown teardown all run on it**. This is not just about making
`Start()` non-blocking; it turns the `StateMachine`'s "every method is called from
the same thread" from a verbal convention into a structural guarantee -- a
graceful shutdown has to send a synchronous control message, and libusb's
synchronous API returns `LIBUSB_ERROR_BUSY` when it is called from the event
thread.

The host thread reads the state **only** through `Snapshot()` and never touches
those component pointers -- otherwise the `reset()` inside `Stop()` and the
host's reads form a data race, which shows up as random crashes.

`RequestStop()` stays a single atomic write (async-signal-safe), at the cost of
being seen up to one loop period late (<=250 ms); `Stop()` additionally wakes
the control thread with a condition variable, so pressing "Disconnect" in the
interface responds immediately.

### 4.3 Interface configuration

- **Always push the addresses through `ipconfig`; do not go back to a bare
  `SIOCAIFADDR`.** An address configured with the bare ioctl is accepted by
  the kernel but not by configd -- no service, no scoped DNS, no default route.
  This was measured and confirmed in sections 3.2 / 6.3 of GUI-SPIKE.
- **Run the external tools through `posix_spawn` with an argv array, never
  through a shell.** The arguments are all validated, but as soon as they pass
  through `/bin/sh` the defense rests on a single question: "was the validation
  complete?"
- **Drain the pipe before `waitpid`.** The other way round is the classic pipe
  deadlock: the child fills the 64 KiB buffer and blocks in `write` while we
  block in `waitpid`. `process_runner.cc` has a matching test.
- **Interface names are only accepted in the form `feth<number>`.** This is not
  fastidiousness -- it blocks accidents of the "someone passes en0 and wipes
  out the user's Wi-Fi configuration" kind.
- **The `SCDynamicStore` handle must be process-lifetime.** The values in the
  dynamic store are held by the session that set them; once the session is
  released the values are gone. A temporary store produces the extremely
  hard-to-diagnose phenomenon "the write returns success, the read comes back
  empty".

### 4.4 Authorization re-check (`TetherKitIPC/Authorization.swift`)

Three details that turn into a vulnerability the moment they are written wrong;
all of them are documented in the comments of that file:

1. On the helper side, the re-check must **never** carry
   `.interactionAllowed` -- a daemon has no UI session, and letting it pop a
   dialog would let any process trigger the system authorization dialog at
   will to harass the user.
2. **`AuthorizationFree` must not carry `.destroyRights`** -- the credential
   belongs to the App, the helper only borrows it to check, and carrying it
   would void the App's authorization as well.
3. **The probe methods do not require authorization** -- otherwise the two
   failures "the helper is not installed" and "the authorization did not pass"
   get mixed together and the user cannot be given an accurate message.

Obtaining the credential (App side, **with** `.interactionAllowed`) and
re-checking it (helper side, **without**) are deliberately kept in the same
file: they differ by a single flag, so when one side is changed the other is
right in front of you.

A fourth one, **actually hit in practice**:

4. **The 32 bytes produced by `AuthorizationMakeExternalForm` are not the
   credential itself; they are only a key pointing at the authorization inside
   securityd.** If the App side releases the `AuthorizationRef` **before** the
   XPC round trip **completes** (especially with `.destroyRights`), the helper
   fails to restore it and reports `errAuthorizationDenied (-60005)`.

   And from the code alone this error code gives no hint of "it was destroyed
   early by ourselves" -- it looks like the user denied the authorization. So
   the credential is held by `AuthorizationToken`, and
   `AuthorizationBroker.withAuthorization` uses `withExtendedLifetime` to
   guarantee that it lives until the call ends. **Do not bare-catch the return
   value of `requestAuthorization()` and use it afterwards** -- ARC is free to
   release the token right after the last read of `externalForm`.

   `gui/Tests/TetherKitIPCTests/AuthorizationTests.swift` nails this behaviour
   down; neither of the two cases pops an authorization dialog, so it can go
   into CI.

**The token must be cached and reused, otherwise every operation makes the user
authenticate again.** The measured parameters of `system.privilege.admin`
(`security authorizationdb read`):

| Field | Value | Meaning |
|---|---|---|
| `shared` | `false` | The credential is **not shared across AuthorizationRefs**. Every new ref created by `AuthorizationCreate` necessarily re-authenticates. |
| `timeout` | `300` | But a credential on the same ref stays valid for 5 minutes. |

So the App caches the `AuthorizationToken` and reuses it: the first operation
pops one dialog, and for the next 5 minutes neither "configure the network" nor
"disconnect" bothers the user any more. Once it expires, the helper's re-check
fails and sets the second parameter of the reply to `true`; the App then drops
the cache, pops the dialog once more and retries the operation.

### 4.5 About Touch ID: this road does not work (verified)

**Conclusion: the system authorization dialog popped by
`AuthorizationCopyRights` supports no fingerprint, only a password.** This is a
limitation of the API, not an implementation problem. Do not spend more time
trying.

The verification process (this machine has Touch ID hardware, `bioutil -r` shows
it enrolled and enabled):

1. Wrote a minimal probe app that requests `system.privilege.admin`, read the
   text of the SecurityAgent window, and got **"Enter password to allow this
   operation."** -- there is no biometric entry point anywhere in the interface.
2. Compared it with the system's own `system.preferences` rule: `class` /
   `group` / `authenticate-user` are exactly the same, only `shared` and
   `timeout` differ -- and those two fields only affect the credential cache,
   not the authentication method. So a different right name does not help.
3. Looked in the SDK for a bridge between LocalAuthentication and the
   Authorization Services: the environment items in `AuthorizationTags.h` are
   only username / password / shared / prompt / icon, **no LAContext**. The
   only place `GetLAContext` appears is `AuthorizationPlugin.h` -- that is a
   callback for **writing a SecurityAgent plugin**, and its comment says it
   returns a smart card PIN (`LACredentialCTKPIN`), not a Touch ID.

`LAContext.evaluatePolicy` really can pop a fingerprint prompt, but the result
it produces **cannot be converted into a credential the helper can
re-check** -- switching to it would mean giving up the entire trust model. If
you really want to do it, the only road left is "write your own SecurityAgent
plugin and install it into /Library/Security/SecurityAgentPlugins", and both
the cost and the risk are wildly out of proportion for this project.

The workable alternative is to **reduce how often the dialog pops**, which is the
token caching described above. If that still feels too frequent, you can use
`security authorizationdb write` in `install-helper.sh` to install a right of
your own, lengthen `timeout` and set `shared` to true -- but that is still a
password box, it just asks less often.

### 4.6 XPC interface revision number

`HelperConstants.protocolRevision` must be incremented every time
`TetherKitHelperProtocol` changes. It is currently **3** (1: the first
version; 2: the reply of a privileged method carries "did the authorization
fail"; 3: adds `setLanguage`). The helper encodes it in the `helperVersion`
reply, and the App compares it as soon as it connects.

**Why not add a dedicated method just to report the version**: adding a method
is itself a protocol change, and the old helper simply does not have it -- that
puts us back to "mismatched and undetectable". Only a method that even an old
helper is guaranteed to answer can reliably identify an old helper. The string
returned by an old helper contains no separator, the parsed revision number
comes out as 0, and the interface uses that to show "the privileged helper
needs updating".

### 4.7 The Swift side

- **The XPC error block and the reply block may both be invoked** (the
  connection dropped after the request went out), and fulfilling a
  `CheckedContinuation` twice is a straight crash. Every call in
  `HelperClient` is wrapped in a `ContinuationGuard`.
- **The cached `NSXPCConnection` must be discarded after the connection
  drops**, otherwise every later call keeps hitting the dead connection, which
  shows up as "the helper is clearly installed but cannot be reached".
- **Every operation in the helper that might take time replies asynchronously**
  (starting a session needs a USB handshake, DHCP waits for a lease for up to
  10 seconds). Blocking on the XPC queue stalls the later status polls on the
  same connection, and the interface looks completely frozen.
- **The denominator of the rate is the monotonic clock delta on the library
  side**, not the interface timer period -- the real interval between two
  fetches is stretched by the scheduling, and using a fixed period as the
  denominator computes the rate too high.
- **A fixed-length C char array is a tuple in Swift**; it cannot be
  subscripted and cannot be turned into a String directly, so everything goes
  through `CInterop.swift`. Read it by "scan to the NUL or to the end", not
  with `String(cString:)` -- the latter reads out of bounds when the buffer is
  full and has no NUL.
- **A non-negative C enum is imported as `UInt32`** while the struct field is
  `int32_t`, so an explicit conversion is required before comparing.
- **The main window is managed directly by AppKit (`MainWindowManager`), no
  longer through a SwiftUI Window scene.** Two reasons: first, SwiftUI will
  "recreate the Window scene on its own" in menu bar only mode and the main
  window mysteriously comes back (`applicationShouldHandleReopen` is not taken;
  instrumentation confirmed that it is never called, and there is no clean
  delegate hook to block it) -- managing the window yourself removes that road
  entirely; second, on macOS 26 the window chrome is decided by the SDK in
  `LC_BUILD_VERSION`: a SwiftPM-linked `sdk 14.0` gives the whole machine the
  legacy traffic lights (14x16pt button frames, 20pt spacing, only a roughly
  12pt circle drawn inside), and adding `-platform_version macos 14.0.0 26.5.0`
  while keeping minos 14.0 gets the stock traffic lights back (both aspects are
  in items 19-21 of section 7 of AGENTS.md). The main window is therefore an
  ordinary NSWindow with `fullSizeContentView` plus a transparent, hidden title
  bar, its content hosted by an NSHostingView; both the App startup and the
  menu bar's "Open the main window" go through
  `MainWindowManager.shared.present(model)`, closing = orderOut, back to menu
  bar only mode
  (`NSWindowDelegate.windowWillClose` falls back to the accessory policy).
- **A bare dev run now pops the window as usual.** The window is created by
  the delegate in `applicationDidFinishLaunching` and `makeKeyAndOrderFront`-ed,
  no longer depending on the instantiation timing of SwiftUI and no longer
  needing a manual rescue from the menu bar. The `.prohibited -> .regular`
  self-check in `TetherKitApplication.init()` is kept: it governs the normal
  activation semantics of a process that is not active yet.
- **The polling is adaptive**: a running session / starting or stopping / the
  window open -> 500 ms; purely idle in the background -> 2 seconds. When the
  window is reopened, a refresh is issued immediately so that the first glance
  is not stale data.

### 4.7b Interface language

The string table is compiled into the binary, it does not go through `.lproj`.
The reason is written at the top of `TetherKitIPC/Localization.swift`, and its
core is that **the helper is a bare executable installed in
`/Library/PrivilegedHelperTools`**, with no resource bundle next to it, and
there should not be one, yet it still has to produce user-facing text of its
own.

The table currently holds a single column (English) and the GUI language menu
offers "Follow system" and "English"; the multi-language plumbing is
deliberately kept so that adding a language later stays a bounded five-touch
change (the checklist lives at the top of
`include/tetherkit/common/messages.def`).

When the language changes, **all three** places must be updated together, and
missing one of them produces "the interface in one language, the log in
another":

| Place | How to change it | Consequence of not changing it |
|---|---|---|
| Swift string table | `L10n.apply(_:)` | the interface does not change |
| libtetherkit | `TetherKitLibrary.setLanguage(_:)` -> `tk_set_language` | the library logs in the log card stay in the old language |
| helper | the XPC `setLanguage(_:)` | the helper's messages and the library logs on its side stay in the old language |

All three are done together in `AppModel.applyLanguage`; do not call one of
them on its own somewhere else. The helper has to be told separately because it
runs as root under launchd and cannot see the user's language preference.

Two SwiftUI specific pitfalls (both hit in practice):

- **Switching the language does not make any `@Observable` property look
  changed**, so the view bodies do not recompute. The countermeasure is to
  make the language change itself an observable value
  (`AppModel.languageRevision` increments every time), and then use it as an
  identity: `.id(model.languageRevision)`, which forces the whole tree to be
  rebuilt.
- **Each `Button` inside `.commands { }` is its own expression**, so only the
  one that reads an observable value recomputes. In practice, after switching
  the language only the language menu itself changed, while "Check for
  Updates…" next to it was still in the old language. The fix is to wrap that
  group in a single View (`AppMenuItems`) and read the revision once in its
  body. The same goes for the **`static let` members that hold text**: they
  are evaluated only once, so turn them all into computed properties.

### 4.8 Packaging

- **Copying the dylib must use `cp -a`, never `install`.** `libtetherkit.dylib`
  and `libtetherkit.0.dylib` are symlinks; `install` copies a separate real
  file for each, and afterwards `install_name_tool` only changes one of them
  -- while the one that is loaded via `@rpath` is exactly the one that was not
  changed.
- **After `install_name_tool` has changed the bytes, the signature must be
  redone.** arm64 requires a valid signature; changing the bytes invalidates
  the original signature and the load is then rejected by the kernel outright
  (`Killed: 9`), with no reason visible in the log.
- **libusb is a dependency of `libtetherkit.dylib`, not of the executable.**
  Running `otool -L` against the executable yields an empty result, the whole
  step is silently skipped -- after the install everything looks normal, yet
  it still depends on Homebrew.
- **The rpath that points at the build directory must be removed from the
  release artifacts.** It is an absolute path, it certainly exists on the
  development machine, dyld prefers it, and the embedded copy is therefore
  never validated -- it only shows up on a different machine. `build-gui.sh`
  removes it; `Package.swift` also puts it last as a second line of defence.
- **In bash, a variable immediately followed by Chinese punctuation must be
  written as `${VAR}`.** Under a UTF-8 locale the lead byte of the full-width
  punctuation (0xEF) is judged true by `isalnum()` and absorbed into the
  variable name. It does not reproduce under the C locale.

### 4.9 One-click installation of the privileged helper inside the App (AEWP)

When the helper is missing or its version does not match, the interface offers
an "Install the helper" or "Update the helper" button: click it, type the
administrator password once, and it is installed -- no need to open a terminal.
The mechanism (`HelperInstaller.swift`):

- **The payload is embedded in the .app**: `Contents/Library/HelperTools/`
  holds the helper binary, the dylib, the plist and the install / uninstall
  scripts flat, assembled by `build-gui.sh`. At install time the whole
  directory is copied away, so the install source naturally travels with the
  .app. The old `dist/helper/` layout is abolished.
- **The privilege escalation goes through `AuthorizationExecuteWithPrivileges`
  (AEWP)**: it takes exactly the AuthorizationRef, so the authorization token
  cached by the App (section 4.4) is reused directly -- within the 5-minute
  window not a single dialog pops; clicking "Connect" right after the install
  does not pop one either. The osascript route was ruled out precisely because
  it creates its own authorization session and cannot hook into this cache.
  SMJobBless / SMAppService were ruled out long ago (SPIKE 3.3).
- **AEWP is deprecated but usable (measured on macOS 26)**: the symbol still
  resolves, and the setuid trampoline `/usr/libexec/security_authtrampoline`
  is still there. The code resolves it with dlsym at runtime instead of linking
  it directly: if the system ever removes it, the app degrades into "please
  install from a terminal" instead of failing to bind at dyld time.
- **The execution target of AEWP must be a binary, not a script**: the
  credential AEWP gives the child process is euid=0 / ruid=the normal user,
  and bash (zsh and dash likewise) drops the euid back to the ruid when
  euid != ruid and `-p` is not given -- the old rule against setuid scripts.
  AEWP-ing the install script directly yields, on a real machine, the script's
  own "root privileges required" error (**hit in practice**). So what the App
  launches is the `--install` mode of the helper binary in the payload
  (`InstallerMode.swift`): setuid(0) normalizes the credential into exactly
  the same real root as sudo, dup2 merges stderr into stdout (AEWP's pipe only
  picks up stdout), and then it execs the `install-helper.sh` in the same
  directory. The script locates the payload by "the directory the script is in
  -> the repository artifacts", so the terminal and the App share one
  implementation.
- **Success is not judged by the exit code**: AEWP gives neither a pid nor an
  exit code, only a pipe connected to the child's stdout. The criterion is
  "after reading EOF, can we really connect to a helper of the matching
  version" -- `AppModel.installHelper()` confirms it with an XPC round trip as
  a backstop, and on failure it puts the tail of the script output (with the
  ANSI colour codes stripped) into the dialog.
- **Self-check before the dialog**: if the payload is missing (the bare
  executable of `swift run`) or AEWP is gone, that has to be reported **before**
  the password is requested, and the user guided back to the terminal script.
- **The uninstall goes through the same chain**: the "Uninstall the privileged
  helper?" (a low-key caption line on the "Manage" page -- a very rare
  action must not steal the attention, but it must exist in the main
  interface) -> confirmation dialog -> `--uninstall` -> `uninstall-helper.sh`.
  The success criterion runs the other way: what is waited for is "cannot
  connect". After the uninstall the interface naturally returns to the install
  guide page, and it can be reinstalled at any time.

### 4.10 Update check (check only, never replace)

`UpdateChecker.swift` requests GitHub's `releases/latest` and does a semantic
comparison against the version number in Info.plist (CMakeLists is the single
source of truth). **The automatic download and replace is deliberately not
done**: under certificate-free distribution the download carries quarantine,
and the replaced .app gets blocked by Gatekeeper -- the same wall that rules
out a Cask. The real update channel is `brew upgrade` or rebuilding from the
source.

- Two entry points: the App menu's "Check for Updates…" (a result dialog
  with a copyable brew command and a link to the release page); the automatic
  check runs at most once a day, and when a new version is found the
  privileged helper row on the "Manage" page lights up "x.y.z is available" --
  it never pops a dialog to interrupt. The hint is remembered across the
  launches (defaults) and goes out by itself after the upgrade because of the
  version comparison.
- Privacy: only the public GitHub API is accessed, and the failures are
  silent; `defaults write com.tetherkit.app updateCheckDisabled -bool YES`
  turns it off completely.
- The bare executable of `swift run` has no Info.plist version number, so the
  check skips itself automatically.
- Release discipline: the check reads the GitHub Releases, so **every version
  must be tagged `vX.Y.Z` and published as a Release**, otherwise it is not
  found.
- When a new version is found, the existing mechanisms take over:
  `brew upgrade` replaces the App, and after the restart the
  `protocolRevision` mismatch triggers the one-click "update the privileged
  helper" reinstall (sections 4.6 / 4.9).

### 4.11 Why the page switch has zero animation (the interpolation chain of macOS 26)

Switching pages in the sidebar has to be pixel-exact. Previously, on macOS 26,
clicking to switch pages briefly scaled the new page up by about 8-10% and then
back down, and **only a click triggered it** (the same state driven by a timer
was completely clean). Two layers of root cause (items 22 and 23 in section 7 of
AGENTS.md) stack up; blocking either one alone only makes the jump faster and
does not eliminate it -- three clamps, none of them optional:

1. **`SidebarRow`: `InstantButtonStyle` (`PrimitiveButtonStyle`)** -- the
   default system style creates a **button activation context** (a transaction
   with an animation) for the pressed / label state; it uses
   `.transaction { animation = nil }` + `.contentShape(Rectangle)` +
   `.simultaneousGesture(TapGesture().onEnded { trigger() })` to create none at
   all (it calls `trigger()` on the configuration, not `action()`).
2. **`SidebarShell.select`: the state write jumps out of the activation
   beat** -- `DispatchQueue.main.async` writes on the next beat, and the write
   site carries an instant transaction (`animation = nil` +
   `disablesAnimations = true`) as a backstop; content/pageBody also carry
   `.transition(.identity)`, so the entry and the exit never interpolate.
3. **`MainWindowManager`: cut off the resize event source** -- macOS 26
   synchronizes the "content size change" with the "window frame animation"
   (WWDC25-256, release notes 61158194); the default `sizingOptions` of
   `NSHostingView` pushes each page's intrinsic size into the window (different
   page heights -> one resize per page switch). `hosting.sizingOptions = []` is
   set **before the contentView is attached**; min/max are pinned to a fixed
   size.

**Why blocking one layer alone is not enough**: a transition only interpolates
when the transaction carries an animation, and an activation context always
carries an animation; `disablesAnimations` only blocks new implicit `.animation`
writes (the two objc.io articles on Transaction / transitions) -- the only
environment that "does not interpolate at all" is the next beat transaction
with a nil animation, which is item 2 above.

**The invariant**: every new code path on the view side that changes the
sidebar selection **must go through `select()`** (keeping the jump out of the
activation beat); writing `storedPage` directly inside an activation
transaction is forbidden -- every additional page switch entry point has to
keep this. The only exception is the window claim path (the arrow keys and the
Return claim of `SidebarMainWindow`): the window layer has no access to the
shell's model, so it writes the storage key directly through
`ButtonTurn.write`, and deliberately does not re-anchor the AX focus (it does
not call `syncAXFocus`) -- a jump out write as well, semantically consistent
with the invariant. On the window side: the delegate's `windowDidBecomeMain` /
`windowDidResize` idempotently re-pins min/max -- measured, macOS 26 resets
min/max to "not set" shortly after the window is shown, accompanied by a +32pt
chrome settle (call stack `_setFrameCommon:display:fromServer:`); the
`styleMask` never contains `.resizable` (min/max only guard against
programmatic changes), and the idempotent form also prevents `windowDidResize`
from recursing into itself.

**The accessibility press (VoiceOver's AXPress)**: within the press round
`NSApp.currentEvent == nil`, and macOS 26.5 **discards everything** that was
queued into `DispatchQueue.main.async` / `Task { @MainActor }` within that
round (the body never runs), while a synchronous write survives. So `select()`
splits on the event type of `NSApp.currentEvent`: inside a mouse/keyboard round
it keeps the iron rule of this section, "jump out of the beat and then write",
and in an accessibility press round it writes synchronously. A direct write
inside a keyboard round has a second pitfall: the re-render after the page
switch re-dispatches the same key event, so one keypress advances two pages.
The complete investigation record is in pitfall 24 in section 7 of AGENTS.md.

**The keyboard (the activation pipeline, round 6)**: with a real FKA the Space
and the arrow keys are intercepted by universalaccessd at the HID layer (a
synthesized NSEvent never reaches that layer), and the Space becomes an
out-of-process AXPress on the focused row -- it lands on the row's
`.accessibilityAction(.default)` and is written directly by
`ButtonTurn.writeDirect` (the `NSApp.currentEvent` left over from an
out-of-process AX round fools the beat detection and is discarded by 26.5);
the Return passes through unchanged and is intercepted in the
`performKeyEquivalent` of `SidebarMainWindow` (condition: the accessibility
panel's FKA is on (`SelfAccessibility.isFullKeyboardAccessOn`, the ground
truth correction is in pitfall 24 round 7), the system-wide AX focus of the
own process resolves to the row title, and no field editor is active), and
when nothing matches it falls through to the focused row's own
`.onKeyPress(.return)` backstop. `NSApp.isFullKeyboardAccessEnabled` reads the
old preference `AppleKeyboardUIMode` (keyboard navigation), not the
accessibility panel's FKA switch; with the panel switch off, the AX focus left
on a row makes a bare Space/Return silently switch pages (a real machine
regression in round 7). The arrow keys: the sidebar claims the bare arrow keys
in `performKeyEquivalent` first and switches the page directly (conditions:
FKA off, the first responder is not a text input; the `.function` bit is not a
chord, so the ban has to be an explicit modifier key set), after the claim the
container does not step a second time; the container keeps the arrow key
`.onKeyPress` as a bubbling backstop; with FKA on the arrow keys belong to the
HID layer. For the rest of the details: `ButtonTurn.write`; the level filter
and the clear button of the LogCard go through the same path, and a
click-driven filter change / clear no longer carries the interpolation of an
activation transaction.

**The focus response synchronization and the self-healing exploration (round 8,
the self-healing logic was superseded in round 11)**: the keyboard
participation of the rows is gated on the ground truth of the accessibility
panel (the shape settled in round 11: every row declares
`.focusable(SelfAccessibility.isFullKeyboardAccessOn || isSelected)`, and the
container no longer carries focusable); the `.focused` / `.accessibilityFocused`
dual engines are bound row by row, and the landing point of the keyboard engine
is synchronized into the AX response via `onChange(of: focusedRow)` -- this is
the only supported channel for moving the AX focus inside the process, writing
`kAXFocusedUIElement` from the outside is a no-op (pitfall 24 round 8 (1)).
(Historical exploration / superseded): Round 8 once tried to self-heal the
system response after the polling re-render via
`.onChange(of: model.status.monotonicNanos)` and an involvement gate. That
mechanism was completely switched off in the Round 11 native architecture (the
related self-healing code was removed, the focus is now managed by the SwiftUI
native engine). The FKA-off contract was narrowed to **the arrow keys only**
(r8 used `.focusable(false)`, later restructured in r9/r11). The level filter
changed from segmented to `.menu`: segmented does not respond to the keyboard
under keyboard navigation and under a real FKA, and the `.menu` popup is the
orthodox keyboard-operable alternative; the popup runs in
`NSEventTrackingRunLoopMode`, and a headless environment cannot synthesize
opening and committing it (a hardware checklist item), so the existence of the
menu is proven by a CGWindowList diff (pitfall 24 round 8 (4)); the filter
binding now goes through `ButtonTurn.routed` (`ButtonTurn` was moved into
TetherKitUIModel as well, its commit semantics have unit tests).

**Tab re-entering the sidebar and the row level fallback for the Return (round
9, real machine re-check)**: the user's real machine re-check overturned the
r8 FKA-off "arrow keys only" contract -- under keyboard navigation Tab could
never reach the sidebar (the rows were `.focusable(false)`, which excludes
them from the Tab ring), and as long as any control held the focus, the
window's arrow key claim preconditions failed and the arrow keys stalled. The
contract changed to: **Tab reaches the sidebar in both modes; Space/Return
stay inert on the rows with FKA off; the arrow keys keep switching pages from
the row focus**. Three landings: the rows' `.focusable(true)` became a
constant (one condition less, a steadier AX identity; the r8 probe matrix's
expectation "Tab never lands on a row" is void from now on, so change the
expectation before running the matrix again); the actions of the row Buttons
all go through `rowSelect`, and with FKA off they split on the type of
`NSApp.currentEvent` -- the keyboard driven native activation (the live
keyDown) is discarded, mouse events are let through (`currentEvent` is read in
two places -- here and in the beat detection of `ButtonTurn.write` -- and both
are used only as a negative / routing signal: an AXPress round leaves the old
event behind, see pitfall 24 round 6); the window's arrow key claim steps the
page when the first responder is not a text input. The Return gets the row
level fallback: under a real FKA the AX self-read that the window's claim
depends on cannot resolve the row (the synthesized probe passes everything --
a headless environment cannot get around the HID layer, and only a real machine
can verify that link), so the focused row itself carries
`.onKeyPress(.return)` (an FKA-on gate, calling `rowSelect` directly, with
zero AX-read dependency); the keypad Enter (keycode 76) is still only covered
by the window claim (SwiftUI's `KeyPress.Key` has no keypad variant). The real
machine re-check list is updated accordingly: FKA off -- Tab reaches the
sidebar rows, Space/Return inert, the arrow keys switch the pages from the row
focus; FKA on -- the Return after a Tab also switches the page.

**The self-driven keyboard ring exploration (round 10, removed and superseded
in round 11)**: round 9's "Tab reachable in both modes" had been misjudged in
the early exploration as "the sidebar rows are unreachable with FKA off", and
round 10 tried a scheme in which AppKit intercepts the bare Tab and a
self-driven keyboard ring is maintained on `AppModel.sidebarRingRow`. But that
scheme had the problems that the AppKit interception easily encroaches on the
focus order of the content buttons and that it introduces a complex state
machine. **That self-driven keyboard ring scheme was completely removed and
abandoned in round 11** (`sidebarRingRow` and `SidebarFocusBridge` are both
deleted), and the final shape is the purely native architecture of round 11.

**The native focus ring and the arrow keys in every state (round 11, the final
architecture)**:

1. **The Tab cycle is connected in both directions**: the AppKit level
   self-driven interception of Tab tried in r10 encroaches on the natural focus
   order of the content buttons, while the root cause of the sidebar being
   skipped before, under pure SwiftUI, was the
   `.focusable(SelfAccessibility.isFullKeyboardAccessOn)` bound to the `rows`
   container, which blocks the whole sidebar group when FKA is off. Remove the
   focusable of the container, the selected row uses `.focusable(isSelected)`
   when FKA is off (with FKA on every row is focusable), Tab then forms a
   bidirectional reachable cycle of "the sidebar selection <-> the content
   buttons", and after Tab has walked through all the content buttons it
   returns smoothly to the sidebar.
2. **The arrow keys switch the pages in every state**: the old
   `handleArrowKey` guard demanded
   `firstResponder == nil || firstResponder === contentView`, and as soon as
   the user pressed Tab and a content button (such as `SwiftUIAppKitButton` /
   `KeyViewProxy`) took the focus, the arrow keys were wrongly blocked. The
   pass condition is now `!isFirstResponderEditableText`: as long as the first
   responder is not a text input (where the caret has to keep moving), the up
   and down arrow keys step the selection and switch the pages at any time,
   whether the focus is in the sidebar or on a content button, and after the
   switch the focus is reset and self-heals, so the user is never stuck on the
   current page. The container keeps the up and down `.onKeyPress` as a
   bubbling backstop.
3. **The Return claim and the row level fallback**: with FKA on, when the
   first responder is not a field editor, the window's `performKeyEquivalent`
   tries to read the system-wide FKA focus track
   (`SelfAccessibility.systemWideFocusedDescription()`). Warning: **the TCC
   permission note**: inspecting a UIElement of the own process needs no
   permission, but reading the cross-process system-wide focus element through
   `AXUIElementCreateSystemWide()` is gated by the macOS TCC (Accessibility);
   the app does not prompt for it by default, and when the Accessibility
   permission has not been granted `AXIsProcessTrusted()` is false, the
   system-wide read silently returns nil, and the event naturally falls through
   to the focused row's own `.onKeyPress(.return)` backstop which switches the
   page (no system permission required).
4. **The visual feedback and the window losing the focus**: when a row has the
   focus and the window is active (`controlActiveState == .key`), a 1pt accent
   stroke and a light gray background are shown; when the window goes to the
   background the stroke hides itself; the selection moves along with the arrow
   keys.

**The verification (headless, not by eye)**: a synthesized click of `NSEvent`
mouseDown/mouseUp + `NSWindow.sendEvent` goes through the real response chain
(no Accessibility / screen recording permission needed); `AXUIElement` can be
used against the own process to read the button geometry and self-check the
hit (no TCC prompt for one's own process), 12/12 hits; for each click, 20
frames of the upper content band are captured at 35ms intervals = 240 frames,
and a YAVG pairing analysis shows that every page switch is exactly **a single
frame jump**, with no decaying sequence (if a 0.2-0.3s settle were still
there, 5-8 consecutive medium amplitude pairs would appear), and the click
path has zero `didResize`. Warning: the CA animation key probe stays at
`animLayers=0` throughout: **the SwiftUI interpolation is invisible to
CALayer, only a bitmap capture can see it** -- do not use "is there a CA
animation" as the criterion. AX notes: the constants are imported as `String`
(cast them to `as CFString` when passing them to the AX API); the `CFArray`
elements are taken with `CFArrayGetValueAtIndex` + `unsafeBitCast`, checked
against `AXValueGetTypeID()`.

Not affected: the auto-scroll of the `LogCard`
(`withAnimation(.easeOut 0.15)`) and the repeatForever breathing animation of
the `StatusHeroCard` own their transactions, so the clamps of `select()` do not
affect them (verified in practice).

---

## 5. Implemented / not implemented

### Implemented

| Capability | Where |
|---|---|
| Environment preflight (root, feth sysctls, MTU ceiling) | `tk_check_environment` |
| Device enumeration + vendor name / product name / serial number | `tk_list_devices` |
| Session lifecycle (non-blocking start, status snapshot, events) | `tk_session_*` |
| Log capture and polling | `tk_drain_logs` |
| DHCP / static IP / clearing | `tk_net_apply`, `tk_net_clear` |
| Read-back of the really effective state (address, gateway, DNS, main default route) | `tk_net_query` |
| Orphan feth cleanup | `tk_cleanup_orphan_interfaces` |
| Privileged helper + credential re-check | `gui/Sources/TetherKitHelper` |
| One-click install / update / uninstall of the privileged helper inside the App | `gui/Sources/TetherKitApp/HelperInstaller.swift` |
| Update check (check only, never replace; daily automatic + manual menu) | `gui/Sources/TetherKitApp/UpdateChecker.swift` |
| Automatic Finder alias maintenance (created on the first launch, searchable and launchable in Spotlight; the brew postinstall cannot create it because of the sandbox) | `gui/Sources/TetherKitApp/FinderAlias.swift` |
| SwiftUI interface (five sidebar pages: Connect / Network / Throughput / Log / Manage, paged by `SidebarShell`; the list of the log page fills the page height) | `gui/Sources/TetherKitApp/Views/SidebarShell.swift` |
| Live rate in the menu bar + background running (menu bar only mode) | `gui/Sources/TetherKitApp/Views/MenuBarPanel.swift` |
| Dev self capture tool (the App captures its own window into a PNG, so the iteration loop needs no screen recording permission) | `gui/Sources/TetherKitApp/Views/UiSelfCapture.swift` |
| English user-facing strings (the runtime language switch is kept, and the interface / the library log / the helper messages are switched together) | `gui/Sources/TetherKitIPC/Localization.swift`, `tk_set_language` |

### Not implemented / known limitations

| Item | Explanation |
|---|---|
| **The DNS of the static IP is not verified on a real device** | IPConfiguration publishes DNS only in DHCP mode. In static mode we add a DNS key to the service it created, and whether IPMonitor adopts it has not been verified. Therefore `tk_net_query` always **reads back** the resolver that is really in effect, and the interface shows the read-back result -- if we guessed wrong, at worst the DNS does not take effect, nothing is broken. |
| **End to end already works on a real device (the DHCP path)** | Verified on 2026-07-28 with an Android phone: plug it in -> the App connects -> feth0 gets an address in the phone's subnet via DHCP -> the host and the phone can ping each other in both directions -> iperf3 measures RX 325 / TX 235 Mbps. **The step "can it reach the public internet" could not be verified** -- a VPN on this machine holds the default route and hijacks the tethered traffic, which is a problem of the test environment and not of the GUI. After the App quits the session is still held by the helper (background running mode, as designed). |
| **The authorization can only take a password, no fingerprint** | A limitation of the system API, not a defect. Verified and recorded in section 4.5. The mitigation is the token cache: it asks only once every 5 minutes. |
| **Hot plug** | `Context::SupportsHotplug()` exists but is not wired up; for now the interface re-enumerates every 2 seconds. |
| **IPv6** | The interface configuration only covers IPv4. |
| **Multiple sessions** | The helper allows only one session at a time. |
| **App icon** | There is no `.icns` yet, the default system icon is used. |
| **English only, for now** | The string table is compiled into the binary as a single column (not `.lproj`, the reason is at the top of `Localization.swift`). The multi-language plumbing is deliberately kept, so adding another language means changing the `Language` enum and two switch statements, it is not just a matter of adding a directory. |

---

## 6. Build and install

```bash
# 1. The C++ part (produces libtetherkit.dylib)
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j

# 2. The GUI (equivalent to running ./gui/Scripts/build-gui.sh directly)
cmake --build build --target gui

# 3. Run. On the first launch it walks you through installing the privileged
#    helper: click the button, type the administrator password once (see
#    section 4.9).
#    Terminal alternative: sudo ./gui/Scripts/install-helper.sh
open dist/TetherKit.app
```

During development you can also use SwiftPM directly, without going through
the packaging script:

```bash
export TETHERKIT_LIB_DIR="$PWD/build/lib"
swift build --package-path gui
swift test  --package-path gui     # the unit tests of TetherKitIPC + TetherKitUIModel
```

Uninstall: the "Uninstall the privileged helper?" button on the "Manage" page
(see section 4.9), or:

```bash
sudo ./gui/Scripts/uninstall-helper.sh
```

Troubleshooting entry points:

| Symptom | Where to look first |
|---|---|
| The interface keeps showing "The privileged helper must be installed" | `launchctl print system/com.tetherkit.helper` |
| The helper does not come up | `/var/log/tetherkit-helper.log` |
| The connection fails | the "Log" panel in the interface (one click to copy) |
| After the install it still depends on Homebrew | `otool -L dist/TetherKit.app/Contents/Frameworks/libtetherkit.0.*.dylib` |
| Clicking "Connect" reports "Cannot restore the authorization credential (-60005)" | The App side released the AuthorizationRef too early, see item 4 in section 4.4 |
| The interface shows "The privileged helper needs updating" | The XPC protocol was changed but the helper was not reinstalled; click "Update the helper" (or run the install script) |
| Clicking "Install the helper" reports "This copy of TetherKit carries no installer payload" | The bare executable of `swift run` is running; the payload exists only in the .app assembled by build-gui.sh; install it with the terminal script |
| The authorization box only takes a password, no fingerprint | This is a limitation of the system API, not a defect, see section 4.5 |
| After unplugging and replugging the device, clicking "Connect" reports "A session is already running" | The helper is an old version (a failed state old session was not cleaned up); reinstall the helper |
| Double-clicking the Finder icon does nothing | Already fixed: the main window is presented again in `applicationShouldHandleReopen` (this callback only became available once AppKit managed the window itself) |
