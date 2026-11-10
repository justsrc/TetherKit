// The C ABI of TetherKit -- the **only** extern "C" boundary in the whole
// project.
//
// Why it exists: Swift's C++ interop cannot swallow this project's C++23
// interfaces (std::expected, std::span, abstract base classes, move
// semantics), so the GUI cannot call the C++ layer directly. This layer
// translates those types into "plain POD plus opaque handles" so that Swift,
// Objective-C, and even other languages can use them.
//
//  Design conventions (follow them and you cannot go wrong)
//
//   1. **No ownership crosses the boundary.** Every output is written into a
//      fixed-length struct supplied by the caller, and the library never
//      allocates memory that the caller would have to free. The only
//      exception is the opaque handle tk_session_t, which is created by
//      `tk_session_create` and destroyed by `tk_session_destroy` -- a clear
//      pairing.
//
//   2. **No callbacks, an event queue instead.** The observer callbacks
//      underneath arrive on the libusb event thread and on the control
//      thread, and the Swift side would have to marshal across threads;
//      worse is reentrancy -- calling stop from inside a callback deadlocks
//      on itself (Stop() joins the control thread). So everything became
//      "the library queues, the host polls". The same goes for the logs, see
//      `tk_drain_logs`.
//
//   3. **Every function returns `tk_result_t` (0 is success, negative is
//      failure)**, and the reason for the failure goes into the optional
//      `tk_error_t*`. Passing NULL means the reason does not matter. No
//      errno, no thread-local last-error -- both are easy to misread in a
//      multithreaded host.
//
//   4. **Strings are always fixed-length NUL-terminated UTF-8 buffers**, and
//      they are truncated rather than failing when the capacity is too
//      small. Truncation only affects the display, not the function, which
//      is far simpler than making the caller negotiate lengths across two
//      calls.
//
//  Permissions (which functions need root)
//
//   No root required: `tk_version`, `tk_check_environment`, `tk_list_devices`,
//                     `tk_drain_logs`, `tk_net_query`
//   Root required  : `tk_session_*` (it creates the feth and opens
//                   /dev/bpf*), `tk_net_apply`, `tk_net_clear`,
//                   `tk_cleanup_orphan_interfaces`
//
//   What the GUI does: the app itself only calls the functions that need no
//   root, and hands the ones that do to `tetherkit-helper` (launched by
//   launchd as root). See docs/GUI-ARCHITECTURE.md.
#ifndef TETHERKIT_CAPI_TETHERKIT_C_H_
#define TETHERKIT_CAPI_TETHERKIT_C_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

#if defined(_WIN32)
#define TK_API
#else
#define TK_API __attribute__((visibility("default")))
#endif

// =============================================================================
// Capacity constants
//
// The capacities are hardcoded in the header instead of having the caller
// negotiate lengths: these structs get mapped directly to a Swift struct and
// copied by value, and a fixed length saves all of the memory management. The
// numbers all leave generous headroom.
// =============================================================================

/// Error message buffer capacity. The underlying error strings carry a
/// multi-line "what to do next" hint, so 1 KiB is enough.
#define TK_ERROR_MESSAGE_CAPACITY 1024
/// Interface name capacity. Matches the kernel's IFNAMSIZ (16, terminator
/// included).
#define TK_INTERFACE_NAME_CAPACITY 16
/// USB string descriptor capacity (manufacturer name / product name / serial
/// number).
#define TK_USB_STRING_CAPACITY 128
/// IP address text capacity. Matches INET6_ADDRSTRLEN (46).
#define TK_ADDRESS_CAPACITY 46
/// How many DNS servers one configuration accepts at most.
#define TK_DNS_MAX 4
/// Text capacity of a single log line / event.
#define TK_TEXT_CAPACITY 512
/// Thread name capacity, matching the real limit of pthread_setname_np.
#define TK_THREAD_NAME_CAPACITY 16

// =============================================================================
// Result codes and errors
// =============================================================================

typedef enum tk_result {
  TK_OK = 0,                     ///< Success.
  TK_ERR_INVALID_ARGUMENT = -1,  ///< Invalid argument (null pointer, out-of-range
                                 ///< enum, malformed address).
  TK_ERR_INVALID_STATE = -2,     ///< The operation is not allowed in the current
                                 ///< state (e.g. a repeated start).
  TK_ERR_FAILED = -3,            ///< The underlying operation failed; the
                                 ///< details are in tk_error_t.
  TK_ERR_PERMISSION = -4,        ///< Root is required but the process is not root.
  TK_ERR_NOT_FOUND = -5,         ///< The target does not exist (no device, no
                                 ///< such interface).
} tk_result_t;

/// The domain an error code comes from; it decides how the `code` field is to
/// be interpreted. It corresponds one to one with `tetherkit::ErrorDomain` on
/// the C++ side.
typedef enum tk_error_domain {
  TK_ERROR_DOMAIN_GENERIC = 0,  ///< A purely logical error; `code` is
                                ///< meaningless.
  TK_ERROR_DOMAIN_ERRNO = 1,    ///< A POSIX errno.
  TK_ERROR_DOMAIN_LIBUSB = 2,   ///< libusb's enum libusb_error (negative).
  TK_ERROR_DOMAIN_RNDIS = 3,    ///< RNDIS_STATUS_*.
} tk_error_domain_t;

/// The full description of one failure. `message` is already a text that can
/// be shown to the user as is.
typedef struct tk_error {
  int32_t domain; ///< tk_error_domain_t
  int64_t code;
  char message[TK_ERROR_MESSAGE_CAPACITY];
} tk_error_t;

// =============================================================================
// Interface language
//
// Every string the library produces on its own -- error messages, log lines,
// event text -- renders in the language set here. The host (the GUI) pushes
// the user's selection in; the command line decides from environment
// variables and --lang on its own.
//
// One process-wide state, not one per session: logs and errors come from
// multiple threads, and per-thread state would split a single session's
// output across languages.
// =============================================================================

// Language tags the shared library understands. English is the only language
// shipped today. When a language is added (see the checklist at the top of
// messages.def), this enum gains a value; old callers compiled against the
// narrower enum keep working because unknown values are ignored at set
// and degrade to English output by design.
typedef enum tk_language {
  TK_LANGUAGE_ENGLISH = 0,
} tk_language_t;

/// Sets the interface language. Any thread, any time.
///
/// Takes effect immediately for new strings, but log lines and events that
/// are already queued do not re-render -- they were materialized as strings
/// when they were produced. The GUI should call this once early at startup
/// and then only when the user switches the language.
///
/// A `language` value outside tk_language_t is ignored by design, so a
/// caller built against an older (or future) enumeration keeps working and
/// simply stays in English.
TK_API void tk_set_language(int32_t language);

/// Reads the current interface language as a tk_language_t value.
TK_API int32_t tk_get_language(void);

/// Infers the language from environment variables, checking TETHERKIT_LANG,
/// LC_ALL, LC_MESSAGES, and LANG in that order.
///
/// Infers only; does not change the current setting. The GUI normally does
/// not need it (macOS language preference belongs to NSLocale); it exists
/// for command-line hosts and third-party bindings.
TK_API int32_t tk_detect_system_language(void);

// =============================================================================
// Version and environment preflight (neither needs root)
// =============================================================================

typedef struct tk_version_info {
  uint16_t major;
  uint16_t minor;
  uint16_t patch;
  /// Of the form "TetherKit 0.1.1 (C++23, macOS 13.3+)".
  char text[64];
  /// The build configuration description, of the form "RelWithDebInfo,
  /// AppleClang 21.0.0, ...".
  char build[192];
  /// The libusb version string.
  char libusb[64];
} tk_version_info_t;

TK_API void tk_version(tk_version_info_t* out_version);

/// The result of the environment preflight. The GUI shows it before startup
/// to make "why it will not run" clear up front.
typedef struct tk_environment {
  /// Whether the current process runs as root.
  bool is_root;
  /// Whether the feth creation-time sysctls all hold the values we require.
  ///
  /// These switches (hwcsum / fcs / tso_support / lro / trailer_length /
  /// separate_frame_header) are a **creation-time snapshot**, changing them
  /// after creation has no effect, so they have to be checked up front.
  bool sysctls_ok;
  /// The specifics when the sysctls do not qualify; an empty string when
  /// they do.
  char sysctl_detail[TK_ERROR_MESSAGE_CAPACITY];
  /// The MTU ceiling feth supports (sysctl net.link.fake.max_mtu). 0 when
  /// the query failed.
  uint32_t feth_max_mtu;
} tk_environment_t;

/// Collects the environment information. **This function always returns
/// TK_OK** -- "the environment does not qualify" is not a call failure but a
/// result to show to the user.
TK_API tk_result_t tk_check_environment(tk_environment_t* out_environment);

// =============================================================================
// Device enumeration (no root required)
// =============================================================================

/// A candidate device recognized as RNDIS.
typedef struct tk_device_info {
  uint16_t vendor_id;
  uint16_t product_id;
  uint8_t bus_number;
  uint8_t device_address;
  /// The communication-class interface (carries the control channel and the
  /// interrupt notifications).
  uint8_t control_interface;
  /// The data-class interface (carries bulk IN/OUT).
  uint8_t data_interface;
  /// The matched interface signature, used to say "which RNDIS shape this
  /// was recognized as".
  uint8_t interface_class;
  uint8_t interface_subclass;
  uint8_t interface_protocol;
  /// Whether the Android quirk fallback path was taken (when the CDC Union
  /// descriptor cannot be trusted).
  bool used_android_quirk;
  /// The three string descriptors below are **best effort**: reading them
  /// requires libusb_open, and the device may already be held exclusively by
  /// another process on this machine. When this read fails, the value
  /// **last read successfully in this process** is filled back in (the device
  /// has not changed, so the name should not vanish just because it is busy);
  /// only when there is no previous value either is it an empty string, and
  /// then the GUI should fall back to showing `description` (the VID:PID
  /// form).
  char manufacturer[TK_USB_STRING_CAPACITY];
  char product[TK_USB_STRING_CAPACITY];
  char serial[TK_USB_STRING_CAPACITY];
  /// Of the form "Bus 020 Device 003: 18d1:4ee4", available in every case.
  char description[64];
} tk_device_info_t;

/// Enumerates the RNDIS devices currently connected.
///
/// @param out_devices    The array supplied by the caller; may be NULL (in
///                       which case only the count is computed).
/// @param capacity       The capacity of the array. When more devices are
///                       found than fit, only the first `capacity` ones are
///                       filled in.
/// @param out_count      Receives the total number of devices found (it may
///                       be greater than capacity).
/// @param read_strings   Whether to try to read the manufacturer name /
///                       product name / serial number. This requires
///                       libusb_open and fails when the device is already
///                       held (not fatal).
///                       When refreshing the list while a session is running,
///                       false is recommended -- whether they are skipped or
///                       the read fails, the values last read successfully in
///                       this process are filled back in, so the names will
///                       not turn into empty strings because of it.
TK_API tk_result_t tk_list_devices(tk_device_info_t* out_devices, size_t capacity,
                                   size_t* out_count, bool read_strings, tk_error_t* out_error);

// =============================================================================
// Logs
// =============================================================================

typedef enum tk_log_level {
  TK_LOG_TRACE = 0,
  TK_LOG_DEBUG = 1,
  TK_LOG_INFO = 2,
  TK_LOG_WARN = 3,
  TK_LOG_ERROR = 4,
  TK_LOG_OFF = 5,
} tk_log_level_t;

typedef struct tk_log_record {
  int32_t level; ///< tk_log_level_t
  /// Wall clock time (nanoseconds since the Unix epoch). Wall clock rather
  /// than monotonic time: the logs have to line up with the system logs.
  int64_t wall_nanos;
  char thread[TK_THREAD_NAME_CAPACITY];
  char message[TK_TEXT_CAPACITY];
} tk_log_record_t;

TK_API void tk_set_log_level(int32_t level);

/// Turns log capture on / off.
///
/// **Off** by default -- a command line program does not need it, and having
/// it on only costs an extra copy. A GUI host calls
/// `tk_enable_log_capture(true)` at startup and then periodically takes the
/// records away with `tk_drain_logs`.
TK_API void tk_enable_log_capture(bool enabled);

/// Takes the buffered log lines away (first in, first out) and returns how
/// many were actually taken.
///
/// The buffer is a fixed-length ring queue; when it fills up the **oldest**
/// records are dropped -- when the host stalls, keeping the newest scene is
/// more useful than keeping the beginning. The number of dropped records is
/// reported through `out_dropped`, so the GUI can hint "there is a gap in the
/// logs".
TK_API size_t tk_drain_logs(tk_log_record_t* out_records, size_t capacity,
                            uint64_t* out_dropped);

// =============================================================================
// Sessions (root required)
// =============================================================================

/// The host-side RNDIS state, corresponding one to one with
/// `tetherkit::rndis::State` on the C++ side.
typedef enum tk_rndis_state {
  TK_RNDIS_UNINITIALIZED = 0,
  TK_RNDIS_INITIALIZING = 1,
  TK_RNDIS_INITIALIZED = 2,
  TK_RNDIS_DATA_INITIALIZED = 3,  ///< The data starts flowing.
  TK_RNDIS_HALTING = 4,
} tk_rndis_state_t;

/// The lifecycle state of a session. This is the state the GUI main window
/// binds to directly.
typedef enum tk_run_state {
  TK_RUN_IDLE = 0,      ///< Created, not yet started.
  TK_RUN_STARTING = 1,  ///< Start sequence: enumerate, handshake, create the
                        ///< interface, open the bridge.
  TK_RUN_RUNNING = 2,   ///< The data paths are up.
  TK_RUN_STOPPING = 3,
  TK_RUN_STOPPED = 4,
  TK_RUN_FAILED = 5,  ///< The start failed or an unrecoverable error
                      ///< occurred; the reason is in status.fatal.
} tk_run_state_t;

/// Session configuration. Fill in the defaults with `tk_session_config_init`
/// and then change the fields you care about -- a plain `{0}` initialization
/// leaves a pile of invalid zeros.
typedef struct tk_session_config {
  /// The device filter. All-zero means use the first RNDIS device found.
  uint16_t vendor_id;
  uint16_t product_id;
  /// Match only the given address on the given bus (both must be given).
  /// Used to tell two devices of the same model apart.
  uint8_t bus_number;
  uint8_t device_address;

  /// The desired MTU. It is negotiated down when the device cannot take it;
  /// the ceiling is the feth sysctl net.link.fake.max_mtu.
  uint32_t mtu;
  /// Whether to set the MAC of the system-side interface to the
  /// device-reported address. On by default.
  bool adopt_device_mac;

  /// How many bulk IN / OUT transfers are in flight concurrently.
  uint32_t rx_transfer_count;
  uint32_t tx_transfer_count;
  /// The KiB size of each bulk IN buffer.
  uint32_t rx_transfer_kib;
  /// The MaxTransferSize (KiB) advertised in INITIALIZE_MSG. This is the
  /// only way to make the device aggregate multiple packets, and it is the
  /// main lever on throughput.
  uint32_t max_transfer_kib;
  /// The KiB size of the kernel BPF capture buffer.
  uint32_t bpf_buffer_kib;
} tk_session_config_t;

/// Fills the configuration with the recommended defaults.
TK_API void tk_session_config_init(tk_session_config_t* out_config);

/// Session events.
typedef enum tk_event_kind {
  /// An RNDIS state transition. `a` = the state before the transition,
  /// `b` = the state after it (both tk_rndis_state_t).
  TK_EVENT_RNDIS_STATE = 0,
  /// Negotiation complete. `a` = the final MTU, `b` = the link speed (Mbps).
  TK_EVENT_NEGOTIATED = 1,
  /// Link up / down. `a` = 1 means connected.
  TK_EVENT_LINK = 2,
  /// Device soft reset. `a` = 1 means the addressing information was lost
  /// and replayed.
  TK_EVENT_DEVICE_RESET = 3,
  /// An unrecoverable error; `text` is the reason. The session then enters
  /// TK_RUN_FAILED.
  TK_EVENT_FATAL = 4,
  /// A lifecycle state change of the session. `a` = before the transition,
  /// `b` = after it (both tk_run_state_t).
  TK_EVENT_RUN_STATE = 5,
} tk_event_kind_t;

typedef struct tk_event {
  int32_t kind; ///< tk_event_kind_t
  int64_t wall_nanos;
  int64_t a;
  int64_t b;
  char text[TK_TEXT_CAPACITY];
} tk_event_t;

/// A complete snapshot of the data paths and the link. The GUI pulls it
/// periodically (500 ms to 1 s is recommended) to refresh the UI.
typedef struct tk_session_status {
  int32_t run_state;   ///< tk_run_state_t
  int32_t rndis_state; ///< tk_rndis_state_t
  bool link_up;
  bool paused;  ///< Whether the data movement is paused (link down or during a
                ///< device reset).

  /// The system-side interface name (the host configures the IP on this
  /// one). An empty string until it is created.
  char system_interface[TK_INTERFACE_NAME_CAPACITY];
  /// The driver-side interface name (TetherKit's BPF attaches to this one).
  /// For diagnostics display only.
  char driver_interface[TK_INTERFACE_NAME_CAPACITY];

  /// The permanent MAC the device reports. All zero when not negotiated.
  uint8_t device_mac[6];
  /// The final MTU after negotiation.
  uint32_t mtu;
  /// The link speed (Mbps).
  uint32_t link_speed_mbps;
  /// The device vendor description string (from OID_GEN_VENDOR_DESCRIPTION).
  char vendor_description[64];
  /// The device in use, of the form "Bus 020 Device 003: 18d1:4ee4".
  char device_description[64];

  /// The cumulative counters (RX = device -> host, TX = host -> device).
  /// The GUI computes the rates itself by differencing two snapshots; the
  /// library does no window smoothing.
  uint64_t rx_frames;
  uint64_t rx_bytes;
  uint64_t rx_dropped;
  uint64_t tx_frames;
  uint64_t tx_bytes;
  uint64_t tx_dropped;
  /// The current RX queue depth, used to tell which side is the bottleneck.
  uint64_t rx_queue_depth;
  /// The cumulative frame drops on the kernel BPF side (bs_drop).
  uint64_t link_kernel_drops;
  /// How many times backpressure occurred because the TX transfer pool had
  /// no free slot.
  uint64_t tx_backpressure;

  /// The monotonic nanosecond count at the sampling moment. The GUI uses it
  /// to compute the rates -- do not use its own clock, the real interval
  /// between two pulls can be stretched by scheduling.
  int64_t monotonic_nanos;

  /// The reason when run_state == TK_RUN_FAILED; an empty string otherwise.
  char fatal[TK_ERROR_MESSAGE_CAPACITY];
} tk_session_status_t;

/// An opaque session handle.
typedef struct tk_session tk_session_t;

/// Creates a session. **Does no I/O at all**, it only records the
/// configuration.
TK_API tk_session_t* tk_session_create(const tk_session_config_t* config, tk_error_t* out_error);

/// Starts the session. **Non-blocking**: internally it starts one control
/// thread that runs the start sequence and the keep-alive loop, and this
/// function returns immediately.
///
/// Returning TK_OK only means "the start request has been accepted"; the real
/// outcome has to be observed through the run_state of `tk_session_status` and
/// the event queue. The reason for this design is that the start sequence
/// contains a USB handshake, which on a slow device can take hundreds of
/// milliseconds to seconds, and blocking the GUI main thread is
/// unacceptable.
///
/// The control channel must run on a **non-libusb event thread** (the
/// synchronous API returns LIBUSB_ERROR_BUSY on the event thread); the
/// library guarantees this internally, so the caller does not have to care
/// about threads.
TK_API tk_result_t tk_session_start(tk_session_t* session, tk_error_t* out_error);

/// Requests the shutdown and waits for the control thread to exit.
/// Idempotent.
///
///  **Must not be called from inside an event polling callback** -- this
/// function joins the control thread. The C ABI layer has no callbacks, so
/// as long as the caller does not hold a lock in its own event handling that
/// would cause reentrancy, it is fine.
TK_API tk_result_t tk_session_stop(tk_session_t* session);

/// Stops and destroys. Passing NULL is a no-op.
TK_API void tk_session_destroy(tk_session_t* session);

/// Takes a status snapshot. Callable from any thread.
TK_API tk_result_t tk_session_status_get(tk_session_t* session, tk_session_status_t* out_status);

/// Takes the queued events away (first in, first out) and returns how many
/// were actually taken.
///
/// Like the logs it is a fixed-length ring queue; when it fills up the oldest
/// are dropped. Missing a few state transition events does not affect
/// correctness -- the authoritative state is always the snapshot from
/// `tk_session_status_get`; the events are only for animations and hints.
TK_API size_t tk_session_poll_events(tk_session_t* session, tk_event_t* out_events,
                                     size_t capacity);

// =============================================================================
// Interface IP configuration (root required)
// =============================================================================

/// The network access mode.
typedef enum tk_ip_mode {
  /// Hand it to the system's IPConfiguration to run DHCP. It takes care of
  /// four things: getting the lease, setting up the scoped DNS, installing
  /// the scoped default route, and publishing the service into the dynamic
  /// store.
  TK_IP_MODE_DHCP = 0,
  /// A static IP. The address is still delivered through IPConfiguration
  /// (rather than a bare SIOCAIFADDR), because only that way does one get a
  /// service "registered through the proper path" -- writing entries into
  /// the dynamic store by hand is **not adopted by IPMonitor**, which has
  /// been measured and confirmed.
  TK_IP_MODE_MANUAL = 1,
  /// Revokes the configuration (equivalent to `ipconfig set <if> NONE`).
  TK_IP_MODE_NONE = 2,
} tk_ip_mode_t;

typedef struct tk_ip_config {
  int32_t mode; ///< tk_ip_mode_t

  /// The four items below are only used when mode == TK_IP_MODE_MANUAL.
  char address[TK_ADDRESS_CAPACITY];
  char netmask[TK_ADDRESS_CAPACITY];
  /// The router (gateway). An empty string means no default route is
  /// configured at all.
  char router[TK_ADDRESS_CAPACITY];
  char dns[TK_DNS_MAX][TK_ADDRESS_CAPACITY];
  int32_t dns_count;

  /// Whether to point the **global** default route at this interface as well.
  ///
  /// When this is off there is only a scoped default route (`RTF_IFSCOPE`);
  /// the traffic bound to this interface takes it, while unbound traffic
  /// still takes the system's primary service. It only needs to be on when a
  /// higher-priority connected service (Wi-Fi, VPN) exists at the same time
  /// -- and the typical scenario of USB network sharing is precisely that no
  /// other network is available, so this interface naturally is the primary
  /// service and the switch does not have to be touched.
  bool set_default_route;
} tk_ip_config_t;

/// Fills the configuration with the defaults (DHCP).
TK_API void tk_ip_config_init(tk_ip_config_t* out_config);

/// The IP state the interface **really has in effect** right now.
///
/// It deliberately does not repeat "what we pushed down" but reads the system
/// state back: in static mode whether the DNS takes effect depends on
/// IPMonitor accepting the service, and only a readback can give the user
/// accurate feedback.
typedef struct tk_net_state {
  bool has_address;
  char address[TK_ADDRESS_CAPACITY];
  char netmask[TK_ADDRESS_CAPACITY];
  char router[TK_ADDRESS_CAPACITY];
  char dns[TK_DNS_MAX][TK_ADDRESS_CAPACITY];
  int32_t dns_count;
  /// The configuration method IPConfiguration reports ("DHCP" / "MANUAL" /
  /// "NONE" / "").
  char method[16];
  /// The service state IPConfiguration reports (e.g. "BOUND").
  char service_state[24];
  /// Whether a default route (scoped or global) exists on this interface.
  bool has_default_route;
  /// Whether the global default route currently points at this interface.
  bool is_primary_default_route;
} tk_net_state_t;

/// Configures the IP of an interface according to the configuration. Needs
/// root.
TK_API tk_result_t tk_net_apply(const char* interface_name, const tk_ip_config_t* config,
                                tk_error_t* out_error);

/// Revokes the IP configuration on an interface (`ipconfig set <if> NONE`).
/// Needs root.
TK_API tk_result_t tk_net_clear(const char* interface_name, tk_error_t* out_error);

/// Reads back the IP state really in effect on an interface. **Does not need
/// root**.
TK_API tk_result_t tk_net_query(const char* interface_name, tk_net_state_t* out_state,
                                tk_error_t* out_error);

// =============================================================================
// Orphan interface cleanup (root required)
// =============================================================================

/// Destroys the feth interfaces left behind in the kernel after the process
/// was forcibly killed.
///
/// Why it is needed: when the process is SIGKILLed the C++ destructors do
/// not run and the feth stays in the kernel; a signal handler cannot catch
/// SIGKILL, so the only option is a **cleanup backstop at the next start**.
/// The session records the interface names in `/var/run/tetherkit-interfaces`
/// when it creates the feth; this function reads it and destroys them one by
/// one.
///
/// @param out_removed  Receives the number of interfaces actually destroyed;
///                     may be NULL.
TK_API tk_result_t tk_cleanup_orphan_interfaces(size_t* out_removed, tk_error_t* out_error);

#if defined(__cplusplus)
}  // extern "C"
#endif

#endif  // TETHERKIT_CAPI_TETHERKIT_C_H_
