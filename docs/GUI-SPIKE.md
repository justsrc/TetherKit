# GUI and privilege escalation feasibility spike

This document records the conclusions of an exploratory spike: **can a native
Swift GUI be built on top of Tethernet, and can it obtain the root privileges
needed to configure the network in an appropriate way?**

The answer is yes, but three routes that looked more "orthodox" were ruled out
along the way. **The ruled-out routes and the reasons for ruling them out are
worth recording more than the final design itself** -- otherwise someone will
walk the same path again next time.

All the conclusions were measured on macOS 26.6 / Apple Silicon / Xcode 26.6 /
Swift 6.3.3.

---

## 1. Summary of the conclusions

| Capability | Conclusion | Evidence |
|---|---|---|
| C API status (at the time of the spike) | **does not exist** | zero `extern "C"` in the whole repo, only C++23 headers + a CLI |
| Swift calling C++ | not feasible | `std::expected` / `std::span` / abstract base classes, which the Swift interop cannot swallow |
| USB enumeration | no root needed | measured, works |
| Creating feth | root needed | measured, works |
| Configuring the IP (`SIOCAIFADDR`) | feasible, configd does not interfere | measured, works |
| Subnet / specific routes | feasible | measured, works |
| DHCP | **the system client works** | measured a lease, `State: BOUND` |
| DNS | **set up automatically** (scoped) | measured |
| scoped default route | **set up automatically** | measured |
| Taking over the global default route | one manual step needed | only when a service of higher priority is running |

---

## 2. The core fact: feth has no IOKit node

This single fact is the **common root cause** of the whole chain of limitations
that follows.

`if_fake` (feth) is created through `SIOCIFCREATE` -> `ifnet_attach` and
**registers no IOKit provider at all**, therefore there is no
`IONetworkInterface` node in the IORegistry.

Measured comparison:

```
IOKit IONetworkInterface: en0-en7, anpi0/1/3, vmenet0/1         <- no feth
NetworkInterfaces.plist: feth appears 0 times
ipconfig getiflist:      en4 en5 en6 bridge0 en0 en7              <- no feth
SC dynamic store:        State:/Network/Interface/feth0/Link       <- yes!
```

The last line is the key contrast: configd's **dynamic store** can see feth's
link state (that layer is fed by routing socket / KEV events), but the
**SCNetworkInterface service layer cannot** (that layer enumerates IOKit).

In one sentence: **the read-only link state is obtainable, the writable service
configuration is not.**

Note that `vmenet0/1` (Virtualization.framework) **is** in IOKit -- virtual
interfaces themselves are fine; the problem is that `if_fake`, being attached
purely at the ifnet level, has no IOKit presence.

---

## 3. The three routes that were ruled out

### 3.1 SCPreferences for a persistent network service

The idea: use `SCPreferences` + `SCNetworkService` to create a real network
service and let macOS run the DHCP, handle the DNS and compute the routes
itself.

The reason: `SCNetworkInterfaceCopyAll()` enumerates IOKit nodes, feth is not
among them, and no `SCNetworkService` can be created for it.

For SC to see feth, an IOKit driver would have to be written -- which is
exactly what this project wants to avoid.

### 3.2 Injecting a service into the SCDynamicStore out of thin air

The idea: bypass the preferences layer, write `State:/Network/Service/<uuid>/IPv4`
and `/DNS` straight into the dynamic store (the common trick of VPN clients)
and let configd work out that feth is a service.

The measured process:

1. Writing only `/DNS` -> not adopted
2. Completing `/IPv4` (with `Addresses`, `SubnetMasks`, `InterfaceName`), and
   both the interface and the address really exist -> **still not adopted**

The injected resolver never shows up in `scutil --dns`; the `PrimaryService` in
`State:/Network/Global/IPv4` is a UUID, which shows that IPMonitor only
accepts the services instantiated from the `Setup:` layer.

And writing the `Setup:` layer leads back to the dead end of 3.1.

Warning: **the scope of this finding is very narrow, do not generalize it.** It
only proves that the "**hand-fabricated**" dynamic store entries are not
adopted.

Measured counter-evidence: after `ipconfig set feth0 DHCP`, IPConfiguration
published `State:/Network/Service/DHCP-feth0/{DHCP,DNS,IPv4}` -- **exactly the
same keys that failed here when they were written by hand** -- and IPMonitor
adopts them completely (the DNS takes effect, the default route is installed).
See section 6.3.

The difference lies in whether the service was registered through the proper
path, not in the content of the keys. So the configd integration on feth **is
feasible**, it just cannot use the shortcut of hand-writing the dynamic store.

### 3.3 SMAppService for installing the privileged helper

The idea: register the LaunchDaemon with the modern API of macOS 13+.

The reason has nothing to do with the technology and everything to do with the
**distribution method**: `SMPrivilegedExecutables` / `SMAuthorizedClients`
bind in both directions through the code signature's designated requirement.
Under source distribution (a Homebrew formula) the cdhash produced on each
machine differs, so the DR hard-coded in the plist can never match.

As a side note: SMAppService **does not pop a password box** -- the approval
entry point is "System Settings -> Login Items & Extensions". This is a
deliberate design of Apple since macOS 13; `SMJobBless`, which did pop a
password box, is deprecated.

---

## 4. The final design

```
Tethernet.app (uid 501, no root needed)
  | 1. call the C API directly for the root-free work: environment preflight, USB enumeration
  | 2. AuthorizationCopyRights -> pop a fingerprint/password -> AuthorizationRef
  | 3. AuthorizationMakeExternalForm -> 32-byte credential
  |
  +-- XPC (com.tethernet.helper, .privileged) -->  tethernet-helper (uid 0)
  |                                                     | 4. CreateFromExternalForm restores it
  |                                                     | 5. CopyRights re-checks it
  |                                                     |    (without interactionAllowed)
  |                                                     | 6. only after the re-check passes does it do the privileged work
  +--------------------  result  <----------------------+
```

The helper is launched by a traditional LaunchDaemon
(`/Library/LaunchDaemons/` + `MachServices`), it is started on demand and
needs no `KeepAlive`.

### Why the security rests on the AuthorizationRef rather than on the code signature

The helper's root **comes from launchd** and has nothing to do with whether the
user touched their fingerprint -- it is root from the moment it starts, and any
process that can reach the Mach service can send requests. So "who is calling"
has to be answered by the helper itself.

Two ways to answer it:

- **Verify the code signature of the caller** (the SMJobBless approach) --
  depends on a certificate, which does not work under source distribution
- **Require every call to carry the AuthorizationRef that the user has just
  confirmed** -- depends on no certificate at all

The latter is chosen. For an open-source, source-distributed tool this is the
more appropriate model: whoever built it, it is equally safe.

### Three details that are easy to get wrong

1. **The re-check must never carry `interactionAllowed`.** A daemon has no UI
   session; letting it pop a dialog would let any process trigger the system
   authorization dialog at will to harass the user. That step only asks "does
   this credential already contain this right?", it does not acquire new rights.
2. **`AuthorizationFree` must not carry `destroyRights`.** The credential
   belongs to the App, the helper only borrows it to check; carrying it would
   void the App's authorization as well.
3. **The connectivity probe methods do not require authorization.** Otherwise
   the two failures "the helper is not installed" and "the authorization did not
   pass" get mixed together and the user cannot be given an accurate message.

### Addendum: installing the helper itself can go through the same authorization (AEWP)

Later on, "installing the helper" was moved from the terminal into the App as
well (`HelperInstaller.swift`). The mechanism is the long-deprecated
`AuthorizationExecuteWithPrivileges`, measured on macOS 26:

- `dlsym(RTLD_DEFAULT, "AuthorizationExecuteWithPrivileges")` still resolves;
- the setuid trampoline `/usr/libexec/security_authtrampoline` is still there
  (mode 04711).

It happens to take an `AuthorizationRef`, so it has the same origin as the trust
model of section 4 -- the token cached by the App is reused directly, and
installing pops no dialog at all within the 5-minute window.
`osascript with administrator privileges` was ruled out because it creates its
own authorization session and cannot hook into the token cache. Three measured
constraints:

- **The execution target of AEWP must be a binary, not a shell script.** The
  credential that the trampoline gives the child process is euid=0 /
  **ruid=the caller**, and bash (zsh and dash likewise) drops the euid back to
  the ruid when euid != ruid and `-p` is not given -- the old rule against
  setuid scripts. AEWP-ing the install script directly yields, on a real
  machine, the script's own "root privileges required" error. The solution: the
  `--install` mode of the AEWP helper binary, which first normalizes the
  credential with `setgid(0)` + `setuid(0)` into exactly the same credential as
  sudo, and then execs the script (`InstallerMode.swift`).
- **Neither a pid nor an exit code is given**, only a pipe connected to the
  child's stdout -- stderr has to be dup2-ed into the stream inside the child,
  and the success has to be judged by a real result such as "after the install,
  can we connect to a helper of the matching version";
- use dlsym instead of linking directly: if the symbol is removed in the
  future it can degrade into a terminal guide instead of a dyld binding failure.

---

## 5. The key clarification: authorization != root

A successful `AuthorizationCopyRights` **changes nothing about the process**. The
`AuthorizationRef` it produces is a **credential**, whose semantics are "the
user confirmed at some moment with a password / fingerprint that they agree to
grant a certain right". Not one of the uid and euid of the process moves.

Measured: on the line right after the authorization succeeds, the App calls
`tn_feth_create()` directly and still gets `EPERM`; the same function called
through the helper succeeds.

The Authorization Services have always been an **advisory** authorization
system, serving the components that already have root. The privilege itself
must come from somewhere else (a daemon started by launchd, a setuid binary).

**The order cannot be reversed**: you cannot "pop the fingerprint to get root
and then create the interface"; it can only be "have a resident root helper
first, pop the fingerprint for each operation and hand the credential to it for
re-checking".

---

## 6. The boundaries of the network configuration capability

### 6.1 Feasible (pure kernel ioctl, configd not involved)

- `SIOCAIFADDR` to configure an address -- measured: it is still there 5
  seconds later, configd does not come to clean it up
- once the address is configured, the subnet route is generated automatically
- scoped default route (`RTF_IFSCOPE`) -- macOS supports a separate default
  route per interface; measured: 4 default routes coexisted on one machine
  (ipsec0 / en0 / bridge100 / bridge101)

configd not seeing feth only means **it cannot manage feth**, it does not
affect **managing it yourself**.

### 6.2 DHCP: the system client works

**IPConfiguration accepts feth**, there is no need to implement a DHCP client
ourselves.

```bash
sudo ipconfig set feth9 DHCP     # exit=0
ipconfig getiflist               # -> ... feth9    <- it is there
ipconfig getsummary feth9        # -> ConfigMethod: DHCP, ServiceID: DHCP-feth9
```

Warning: **a trap that once led the conclusion astray**: `ipconfig getiflist`
lists the "**interfaces that currently have a service**", not the "interfaces
that can be configured". An interface that has never run a service is never in
it -- not even a real wired NIC (measured: `ipconfig getsummary en1` likewise
reports "interface doesn't exist", and en1 is a perfectly ordinary Thunderbolt
NIC). **Its absence must not be taken as evidence that "IPConfiguration does
not recognize this interface".**

The link must be up: while feth has no peer, `status: inactive` and the
service stops at `Active: FALSE` / `State: INACTIVE`. In a real scenario
feth0<->feth1 are already paired and the link is active.

**The cost**: `ipconfig set` creates a **temporary service**, it lives only
until the next network configuration change and does not appear in "System
Settings -> Network". For the GUI this is actually easier -- the App is the
configuration entry point itself, and when the service is gone it can simply
set it again.

### 6.3 What one command buys you

Executed on a real link (feth0<->feth1 paired, the device side running a DHCP
server):

```bash
sudo ipconfig set feth0 DHCP
```

**IPConfiguration automatically does all four things**:

| Result | Measured evidence |
|---|---|
| A lease is obtained | `ipconfig getifaddr feth0` -> `192.168.35.128`, `State: BOUND` |
| The DNS is configured (scoped) | `scutil --dns` shows `nameserver 192.168.35.7 / if_index 43 (feth0) / Scoped` |
| The scoped default route is installed | `netstat -rn` shows `default 192.168.35.7 UGScIg feth0` |
| It is published to the dynamic store | `State:/Network/Service/DHCP-feth0/{DHCP,DNS,IPv4}`, `IsPublished: TRUE` |

The last row is worth noting: **these are exactly the three keys that failed
when they were written by hand in section 3.2**. A service registered by
IPConfiguration through the proper path is adopted completely by IPMonitor; a
hand-fabricated one is not.

### 6.4 The only manual step: taking over the global default route

feth0 **does not** become the primary service automatically. In the measurement
the `PrimaryInterface` in `State:/Network/Global/IPv4` was still `ipsec0` (at
the time both a VPN and Wi-Fi were running, ranked above feth0).

The result: feth0 has a **scoped** default route (the traffic bound to that
interface goes through it), but the **unbound traffic** still goes through the
primary service.

To make the global traffic go through the device, it is the original command:

```bash
sudo route -n change default $(ipconfig getoption feth0 router)
```

**Mind the conditions of applicability**: this step is only needed when a
service of higher priority is running and also has connectivity. And the
typical scenario of USB network sharing is precisely the one where no other
network is available (the user plugged in the phone precisely because there is
no Wi-Fi); there feth0 most likely becomes the primary service on its own and
the command does not even have to run.

### 6.5 The impact on the implementation

**The `tn_net_*` group of C APIs is basically not needed.** The DHCP client
originally planned to be implemented by hand (opening BPF on feth0 to run
DISCOVER/OFFER/REQUEST/ACK, and coupling its timing to the RNDIS state machine)
is saved as a whole. The helper only needs:

1. `ipconfig set <feth0> DHCP` -- or, for a static IP, the `SIOCAIFADDR` of
   section 6.1
2. optionally `route change default`

**The cost**: `ipconfig set` creates a **temporary service**, it lives only
until the next network configuration change and does not appear in "System
Settings -> Network". For the GUI this is actually easier -- the App is the
configuration entry point itself, and when the service is gone, set it again.

**Cleanup**: `sudo ipconfig set feth0 NONE`.

---

## 7. The pitfalls hit along the way

### Hardened Runtime and Homebrew's libusb

The signed .app crashes on startup:

```
Library not loaded: /opt/homebrew/opt/libusb/lib/libusb-1.0.0.dylib
Reason: ... have different Team IDs
```

Under the Hardened Runtime macOS refuses to load a library whose Team ID
differs from the main program's. Homebrew's libusb is ad-hoc signed (no Team
ID), while the App is signed with a developer certificate (it has a Team ID).

**Any signed and distributed Tethernet.app must embed libusb** and re-sign it
with the same identity. The other road is to add the
`com.apple.security.cs.disable-library-validation` exemption, but that amounts
to turning off the library validation of the whole process, which is not worth
it.

Note: this pitfall is **brought about by the signing**. An ad-hoc artifact
built from the source does not have this problem.

### After `install_name_tool` you must re-sign

arm64 requires the executable to have a valid signature; changing the bytes
invalidates the signature, and then the startup is rejected outright by the
kernel (`Killed: 9`), with no reason visible in the launchd log.

### `install` breaks the symlinks into separate files

`libtethernet.dylib` and `libtethernet.0.dylib` are symlinks to
`.0.1.1.dylib`. `install` does not preserve the symlinks and copies a
**separate real file** for each. The later `install_name_tool` only changes one
of them, and the one that gets loaded via `@rpath` is exactly the one that was
not changed -- **a copy that is never used got fixed, the problem is still
there, and with no warning whatsoever**. Use `cp -a`.

### Look up the dependencies against the right object

`libusb` is a dependency of `libtethernet.dylib`, not of the helper
executable. Running `otool -L` against the helper yields an empty result, the
whole step is silently skipped -- after the install everything looks perfectly
normal, yet it still depends on Homebrew.

### Under a UTF-8 locale, `$VAR` followed by full-width punctuation blows up

```bash
echo "cleaned（items + $IFACE）"   # -> IFACE?: unbound variable
```

bash decides the characters of an identifier byte by byte; under a UTF-8
locale the lead byte of the full-width punctuation (0xEF) is judged true by
`isalnum()` and is absorbed into the variable name. It does not reproduce under
the C locale.

**Rule: when a variable is immediately followed by Chinese punctuation, it must
be written as `${VAR}`.**

### The deployment target follows the host by default

The project accepts `CMAKE_OSX_DEPLOYMENT_TARGET` but sets no default, so the
artifacts have always been built against the host version (26.0 in this case),
while the README claims support for 13.3+.

Incidentally: Homebrew's libusb itself is also built against the host version,
so **the prebuilt Release binaries actually do not run on 13.3**. This has
nothing to do with the GUI, but it does affect the existing Release artifacts.

---

## 8. Homebrew distribution

### The two measured constraints

- **The service DSL of `brew services` has no `MachServices`** (confirmed by
  looking at `Library/Homebrew/service.rb`), so an XPC service cannot be
  registered
- **`brew install` never runs as root**, a formula cannot write
  `/Library/LaunchDaemons`

So the helper can only be installed into the prefix, and the caveats guide the
user to run one `sudo`. This is the standard practice.

### Where the .app goes

A formula **can** install an .app into the Cellar (`python@3.14` installs two),
but it **does not** show up in `/Applications`, in the Launchpad or in the
Spotlight search -- the caveats have to guide the user to create a symlink, or
the distribution has to switch to a Cask.

### The signing requirements

| Distribution method | Does it need a certificate? |
|---|---|
| Formula (built from the source) | **no** -- a local build carries no quarantine, Gatekeeper is not involved |
| Cask (prebuilt .app) | Developer ID + notarization needed -- a Cask puts the quarantine on the download by default |

The source-build route is feasible precisely because the security model chose
the AuthorizationRef re-check over the signature binding.

---

## 9. The C API gap list

To go further, the existing C++ layer needs the following (as of the spike):

**The accessors that must be added** -- `Runtime` currently exposes only
`SystemInterfaceName()` and `DeviceInfo()`, and the following are all
unobtainable: the bridge statistics, the current RNDIS state, the link up/down,
the negotiated parameters, the driver side interface name, the reason for a
fatal error.

**The behaviours that must change**:

- `RunUntilStopped()` blocks and is unusable from a GUI -- `Start()` must start
  the control thread itself and return (the control channel has to be on a
  thread that is not the libusb event thread, this constraint cannot be
  dropped)
- the five callbacks of the observer only log today and have to be forwarded to
  the host

**An event queue is recommended rather than callbacks**: the callbacks arrive
from the libusb event thread and from the control thread, so the Swift side has
to marshal across the threads; worse is re-entrancy -- calling stop from inside
a callback self-waits into a deadlock (`Stop()` joins the control thread).

**The other gaps**:

| Gap | Explanation |
|---|---|
| Log sink | The logs only go to stderr, the GUI cannot get them |
| Device string descriptors | `DeviceCandidate` has no vendor name / product name / serial number, two devices of the same model cannot be told apart |
| Hot plug | `Context::SupportsHotplug()` exists but has never been used |
| Orphan feth cleanup | When the process is SIGKILLed the destructor does not run and the interface stays in the kernel |
| helper signal handling | `launchctl bootout` sends SIGTERM, the Swift `deinit` does not run, and the interface is leaked just the same |

The orphan cleanup is the backstop and matters more than the signal handling --
SIGKILL is something that no signal handler can catch.

---

## 10. Verification command memo

```bash
# whether feth is in IOKit (expected: it is not)
ioreg -c IONetworkInterface -r -d1 | grep '"BSD Name"' | sort -u

# whether IPConfiguration accepts feth (expected: yes, exit=0 and it then shows up in getiflist)
sudo ifconfig feth9 create
sudo ipconfig set feth9 DHCP && ipconfig getiflist
sudo ipconfig set feth9 NONE && sudo ifconfig feth9 destroy

# Warning: getiflist lists the "interfaces that currently have a service", not
#    the "interfaces that can be configured". An interface that has never run a
#    service is never in it, not even a real wired NIC.
#    Do not take its absence as evidence that "IPConfiguration does not
#    recognize this interface" -- I stepped on that one.

# the complete verification on a real link (needs the device plugged in and tethernet running)
sudo ipconfig set feth0 DHCP
ipconfig getifaddr feth0                     # the lease
scutil --dns | grep -A3 feth0                # scoped DNS (it is in the later part, do not cut it with head)
netstat -rn -f inet | grep -E "^default"     # scoped default route
ipconfig getsummary feth0 | grep -E "State|IsPublished|Router"
sudo ipconfig set feth0 NONE                 # cleanup

# whether the kernel agrees to configure an address on feth (expected: it does)
sudo ifconfig feth9 create
sudo ifconfig feth9 inet 10.99.99.1 netmask 255.255.255.0 up
netstat -rn -f inet | grep 10.99.99
sudo ifconfig feth9 destroy

# whether the Homebrew service DSL supports MachServices (expected: it does not)
grep -i machservice /opt/homebrew/Library/Homebrew/service.rb
```

The `!` at the end of the interface route lines in the `netstat -rn` output is
the scoped route marker (`RTF_IFSCOPE`), **not an error** -- the ordinary
en0/bridge routes carry it too.
