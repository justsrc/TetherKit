// Network access mode configuration for the virtual interface: DHCP / static
// IP / clearing, plus a read-back of the state that is actually in effect.
//
//  Why `ipconfig` instead of issuing SIOCAIFADDR ourselves
//
//   Addresses configured with a bare ioctl are honored by the kernel (measured,
//   docs/GUI-SPIKE.md section 6.1), but **configd does not recognize them at
//   all** -- no service, no scoped DNS, no default route.
//   And `ipconfig set` goes through IPConfiguration's proper registration
//   path; the service it creates is adopted wholesale by IPMonitor. The very
//   same dynamic-store keys are ignored when written by hand and honored when
//   written through IPConfiguration -- the difference is only whether the
//   service was registered through the proper path (sections 3.2 / 6.3).
//
//   The cost: `ipconfig set` creates a **temporary service** that lives only
//   until the next network configuration change, and it does not appear in
//   "System Settings -> Network". For the GUI that is convenient anyway --
//   the App is the configuration entry point itself, and when the service
//   drops it simply sets it again.
//
//  Static-IP DNS is "best effort + verified by read-back"
//
//   IPConfiguration publishes DNS only in DHCP mode (it comes from the DHCP
//   options). MANUAL mode has no DNS source, so all we can do is add a DNS
//   key to the service it created.
//   That move is **not verified on a real device**, so we never promise
//   success upward: tk_net_query always reads back the resolver that is
//   actually in effect in the system, and the GUI shows the read-back result,
//   not the value we pushed down.
#include <SystemConfiguration/SystemConfiguration.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "capi_support.h"
#include "core_foundation_support.h"
#include "process_runner.h"
#include "tetherkit/capi/tetherkit_c.h"
#include "tetherkit/common/i18n.h"
#include "tetherkit/common/logging.h"

namespace {

using tetherkit::Error;
using tetherkit::Msg;
using tetherkit::Result;
using tetherkit::Status;
using tetherkit::Text;
using tetherkit::Tr;
using tetherkit::capi::ClearError;
using tetherkit::capi::CopyText;
using tetherkit::capi::CopyToStdString;
using tetherkit::capi::FillError;
using tetherkit::capi::FillGenericError;
using tetherkit::capi::IsValidFethName;
using tetherkit::capi::MakeCFString;
using tetherkit::capi::ProcessResult;
using tetherkit::capi::RunTool;
using tetherkit::capi::ScopedCFRef;
using tetherkit::capi::SharedDynamicStore;

constexpr std::string_view kIpconfigPath = "/usr/sbin/ipconfig";
constexpr std::string_view kRoutePath = "/sbin/route";

/// Upper bound on the DHCP lease wait.
///
/// Why 10 seconds: the DHCP DISCOVER retransmission uses exponential backoff
/// (1/2/4/8 seconds), so 10 seconds covers the first three retransmissions.
/// Beyond that we should tell the user "there is probably no DHCP server on
/// the other end" instead of spinning forever.
constexpr std::chrono::seconds kDhcpLeaseTimeout{10};
constexpr std::chrono::milliseconds kDhcpPollInterval{250};

// ---------------------------------------------------------------------------
// Argument validation
// ---------------------------------------------------------------------------

[[nodiscard]] bool IsValidIpv4(std::string_view text) noexcept {
  if (text.empty() || text.size() >= TK_ADDRESS_CAPACITY) {
    return false;
  }
  const std::string owned{text};
  ::in_addr parsed{};
  return ::inet_pton(AF_INET, owned.c_str(), &parsed) == 1;
}

/// Validates the interface name and turns a rejection into a plain error.
[[nodiscard]] Status ValidateInterface(const char* interface_name) {
  if (interface_name == nullptr) {
    return std::unexpected(Error::Generic(Tr(Msg::kCapiInterfaceNameNull)));
  }
  if (!IsValidFethName(interface_name)) {
    return std::unexpected(Error::Generic(Tr(Msg::kCapiInterfaceNotOurs, interface_name)));
  }
  return tetherkit::Ok();
}

// ---------------------------------------------------------------------------
// External tool invocation
// ---------------------------------------------------------------------------

/// Runs one tool command; a non-zero exit is treated as failure and its
/// output is carried along verbatim.
///
/// Verbatim matters: the error messages of ipconfig / route are themselves the
/// most accurate diagnostic information, and paraphrasing them only loses
/// detail.
[[nodiscard]] Status RunOrFail(std::string_view executable,
                               const std::vector<std::string>& arguments,
                               std::string_view what) {
  TETHERKIT_ASSIGN_OR_RETURN(const ProcessResult result, RunTool(executable, arguments));
  if (!result.Succeeded()) {
    std::string detail{result.output};
    // Tool output usually carries a trailing newline; that reads badly when
    // spliced into a one-line error.
    while (!detail.empty() && (detail.back() == '\n' || detail.back() == '\r')) {
      detail.pop_back();
    }
    return std::unexpected(Error::Generic(
        Tr(Msg::kCapiCommandFailed, what, result.exit_code,
           detail.empty() ? std::string{Text(Msg::kCapiCommandNoOutput)} : detail)));
  }
  return tetherkit::Ok();
}

/// Reads the interface's current IPv4 address; returns std::nullopt when there
/// is none.
///
/// An ioctl rather than `ipconfig getifaddr`: this is the real kernel state,
/// it goes through no intermediate layer, and it does not fork a process.
[[nodiscard]] std::optional<std::string> QueryAddress(std::string_view interface_name,
                                                      unsigned long request) noexcept {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return std::nullopt;
  }

  ::ifreq ifr{};
  const std::size_t copy_length = std::min(interface_name.size(), sizeof(ifr.ifr_name) - 1);
  std::memcpy(ifr.ifr_name, interface_name.data(), copy_length);
  ifr.ifr_addr.sa_family = AF_INET;

  std::optional<std::string> result;
  if (::ioctl(fd, request, &ifr) == 0) {
    std::array<char, INET_ADDRSTRLEN> text{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* address = reinterpret_cast<const ::sockaddr_in*>(&ifr.ifr_addr);
    if (::inet_ntop(AF_INET, &address->sin_addr, text.data(), text.size()) != nullptr) {
      result = std::string{text.data()};
    }
  }
  ::close(fd);
  return result;
}

// ---------------------------------------------------------------------------
// SCDynamicStore queries
// ---------------------------------------------------------------------------

/// Finds the service ID that IPConfiguration created for this interface.
///
/// The approach is to enumerate `State:/Network/Service/<id>/IPv4` and match
/// on `InterfaceName`, **not** to guess how a service ID is spelled. The
/// measured ID for `ipconfig set feth0 DHCP` is "DHCP-feth0", which looks
/// concatenable, but that is an implementation detail; matching on the field
/// is what holds up.
[[nodiscard]] std::optional<std::string> FindServiceId(SCDynamicStoreRef store,
                                                       std::string_view interface_name) {
  const ScopedCFRef<CFStringRef> pattern = MakeCFString("State:/Network/Service/[^/]+/IPv4");
  const ScopedCFRef<CFArrayRef> keys{
      ::SCDynamicStoreCopyKeyList(store, pattern.Get())};
  if (!keys) {
    return std::nullopt;
  }

  const CFIndex count = ::CFArrayGetCount(keys.Get());
  for (CFIndex i = 0; i < count; ++i) {
    const auto* const key = static_cast<CFStringRef>(::CFArrayGetValueAtIndex(keys.Get(), i));
    const ScopedCFRef<CFDictionaryRef> entry{
        static_cast<CFDictionaryRef>(::SCDynamicStoreCopyValue(store, key))};
    if (!entry) {
      continue;
    }
    const auto* const name = static_cast<CFStringRef>(
        ::CFDictionaryGetValue(entry.Get(), CFSTR("InterfaceName")));
    if (name == nullptr || CopyToStdString(name) != interface_name) {
      continue;
    }

    // Extracts <id> from "State:/Network/Service/<id>/IPv4".
    const std::string full = CopyToStdString(key);
    constexpr std::string_view kPrefix = "State:/Network/Service/";
    constexpr std::string_view kSuffix = "/IPv4";
    if (full.size() <= kPrefix.size() + kSuffix.size()) {
      continue;
    }
    return full.substr(kPrefix.size(), full.size() - kPrefix.size() - kSuffix.size());
  }
  return std::nullopt;
}

[[nodiscard]] ScopedCFRef<CFDictionaryRef> CopyServiceEntry(SCDynamicStoreRef store,
                                                            std::string_view service_id,
                                                            std::string_view leaf) {
  const ScopedCFRef<CFStringRef> key =
      MakeCFString(std::format("State:/Network/Service/{}/{}", service_id, leaf));
  return ScopedCFRef<CFDictionaryRef>{
      static_cast<CFDictionaryRef>(::SCDynamicStoreCopyValue(store, key.Get()))};
}

/// Reads a string field from the dictionary; returns an empty string when the
/// field is missing.
[[nodiscard]] std::string StringField(CFDictionaryRef dictionary, CFStringRef field) {
  if (dictionary == nullptr) {
    return {};
  }
  const auto* const value = static_cast<CFStringRef>(::CFDictionaryGetValue(dictionary, field));
  if (value == nullptr || ::CFGetTypeID(value) != ::CFStringGetTypeID()) {
    return {};
  }
  return CopyToStdString(value);
}

/// Reads a string-array field from the dictionary.
[[nodiscard]] std::vector<std::string> StringArrayField(CFDictionaryRef dictionary,
                                                        CFStringRef field) {
  std::vector<std::string> values;
  if (dictionary == nullptr) {
    return values;
  }
  const auto* const array = static_cast<CFArrayRef>(::CFDictionaryGetValue(dictionary, field));
  if (array == nullptr || ::CFGetTypeID(array) != ::CFArrayGetTypeID()) {
    return values;
  }
  const CFIndex count = ::CFArrayGetCount(array);
  values.reserve(static_cast<std::size_t>(count));
  for (CFIndex i = 0; i < count; ++i) {
    const auto* const item = static_cast<CFStringRef>(::CFArrayGetValueAtIndex(array, i));
    if (item != nullptr && ::CFGetTypeID(item) == ::CFStringGetTypeID()) {
      values.push_back(CopyToStdString(item));
    }
  }
  return values;
}

/// Which interface the global default route currently points at.
[[nodiscard]] std::string PrimaryInterface(SCDynamicStoreRef store) {
  const ScopedCFRef<CFStringRef> key = MakeCFString("State:/Network/Global/IPv4");
  const ScopedCFRef<CFDictionaryRef> global{
      static_cast<CFDictionaryRef>(::SCDynamicStoreCopyValue(store, key.Get()))};
  return StringField(global.Get(), CFSTR("PrimaryInterface"));
}

// ---------------------------------------------------------------------------
// Applying
// ---------------------------------------------------------------------------

/// Writes the DNS servers onto the service IPConfiguration created.
///
///  **A move that is not verified on a real device.** What is confirmed:
/// hand-fabricating an entire service is not adopted by IPMonitor (GUI-SPIKE
/// section 3.2). The bet here is that "adding a DNS key to an **already
/// properly registered** service gets adopted". A wrong bet causes no damage
/// -- the DNS simply does not take effect -- so a failure is only logged and
/// never fails the whole static-IP configuration. The real result is reported
/// by the tk_net_query read-back.
///
/// One more thing to note: a value in SCDynamicStore is **owned by the session
/// that set it**, and the value disappears when that session is released.
/// That is why this uses the process-lifetime store (see the SharedDynamicStore
/// documentation); it must never be changed to a locally created one.
void TryPublishDns(SCDynamicStoreRef store, std::string_view service_id,
                   const std::vector<std::string>& servers) {
  if (servers.empty()) {
    return;
  }

  std::vector<ScopedCFRef<CFStringRef>> owned;
  std::vector<const void*> raw;
  owned.reserve(servers.size());
  raw.reserve(servers.size());
  for (const std::string& server : servers) {
    owned.push_back(MakeCFString(server));
    raw.push_back(owned.back().Get());
  }

  const ScopedCFRef<CFArrayRef> addresses{::CFArrayCreate(
      kCFAllocatorDefault, raw.data(), static_cast<CFIndex>(raw.size()), &kCFTypeArrayCallBacks)};
  const void* keys[] = {CFSTR("ServerAddresses")};
  const void* values[] = {addresses.Get()};
  const ScopedCFRef<CFDictionaryRef> payload{
      ::CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                           &kCFTypeDictionaryValueCallBacks)};

  const ScopedCFRef<CFStringRef> key =
      MakeCFString(std::format("State:/Network/Service/{}/DNS", service_id));
  if (::SCDynamicStoreSetValue(store, key.Get(), payload.Get()) == 0) {
    TETHERKIT_WARN_TR(Msg::kCapiDnsPublishFailed, service_id);
  }
}

/// Waits for the DHCP lease to arrive. Returns false on timeout.
[[nodiscard]] bool WaitForLease(std::string_view interface_name) {
  const auto deadline = std::chrono::steady_clock::now() + kDhcpLeaseTimeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (QueryAddress(interface_name, SIOCGIFADDR).has_value()) {
      return true;
    }
    std::this_thread::sleep_for(kDhcpPollInterval);
  }
  return QueryAddress(interface_name, SIOCGIFADDR).has_value();
}

/// Installs a default route **bound to this interface**.
///
/// A scoped route (RTF_IFSCOPE) is macOS's per-interface default route:
/// traffic bound to that interface takes it, and the system's primary
/// service is left alone. In DHCP mode IPConfiguration installs this one by
/// itself; only the static mode needs us to add it.
[[nodiscard]] Status InstallScopedDefaultRoute(std::string_view interface_name,
                                               std::string_view router) {
  const std::vector<std::string> add_arguments{
      "-n", "add", "-inet", "-ifscope", std::string{interface_name}, "default", std::string{router}};
  if (const auto status =
          RunOrFail(kRoutePath, add_arguments, Text(Msg::kCapiWhatAddScopedRoute));
      status) {
    return tetherkit::Ok();
  }
  // When an identical route already exists, add fails with EEXIST -- retry
  // once with change.
  const std::vector<std::string> change_arguments{
      "-n",      "change",  "-inet",   "-ifscope",
      std::string{interface_name}, "default", std::string{router}};
  return RunOrFail(kRoutePath, change_arguments, Text(Msg::kCapiWhatUpdateScopedRoute));
}

/// Switches the **global** default route to the given gateway.
[[nodiscard]] Status PromoteToGlobalDefaultRoute(std::string_view router) {
  const std::vector<std::string> arguments{"-n", "change", "-inet", "default", std::string{router}};
  if (const auto status = RunOrFail(kRoutePath, arguments, Text(Msg::kCapiWhatSwitchGlobalRoute));
      status) {
    return tetherkit::Ok();
  }
  // The system may have no global default route at all (nothing is connected),
  // in which case change fails and add is the right call. That is exactly the
  // typical USB-tethering scenario, so it has to be handled.
  const std::vector<std::string> add_arguments{"-n", "add", "-inet", "default",
                                               std::string{router}};
  return RunOrFail(kRoutePath, add_arguments, Text(Msg::kCapiWhatAddGlobalRoute));
}

/// In DHCP mode, pulls the gateway address out of the lease.
[[nodiscard]] std::optional<std::string> QueryDhcpRouter(std::string_view interface_name) {
  const auto result = RunTool(kIpconfigPath, {"getoption", interface_name, "router"});
  if (!result || !result->Succeeded()) {
    return std::nullopt;
  }
  std::string router{result->output};
  while (!router.empty() && (router.back() == '\n' || router.back() == '\r')) {
    router.pop_back();
  }
  return IsValidIpv4(router) ? std::optional{router} : std::nullopt;
}

[[nodiscard]] Status ApplyDhcp(std::string_view interface_name, bool set_default_route) {
  TETHERKIT_RETURN_IF_ERROR(
      RunOrFail(kIpconfigPath, {"set", std::string{interface_name}, "DHCP"},
                Text(Msg::kCapiWhatStartDhcp)));

  // Wait for the lease. IPConfiguration automatically does four things: get
  // the lease, set up the scoped DNS, install the scoped default route, and
  // publish the service to the dynamic store -- we do none of it ourselves.
  if (!WaitForLease(interface_name)) {
    return std::unexpected(Error::Generic(Tr(Msg::kCapiDhcpTimeout, kDhcpLeaseTimeout.count())));
  }

  if (!set_default_route) {
    return tetherkit::Ok();
  }
  const std::optional<std::string> router = QueryDhcpRouter(interface_name);
  if (!router.has_value()) {
    return std::unexpected(Error::Generic(Tr(Msg::kCapiDhcpNoRouter)));
  }
  return PromoteToGlobalDefaultRoute(*router);
}

[[nodiscard]] Status ApplyManual(std::string_view interface_name, const tk_ip_config_t& config) {
  if (!IsValidIpv4(config.address)) {
    return std::unexpected(
        Error::Generic(Tr(Msg::kCapiInvalidAddress, config.address)));
  }
  if (!IsValidIpv4(config.netmask)) {
    return std::unexpected(
        Error::Generic(Tr(Msg::kCapiInvalidNetmask, config.netmask)));
  }
  const std::string_view router{config.router};
  if (!router.empty() && !IsValidIpv4(router)) {
    return std::unexpected(
        Error::Generic(Tr(Msg::kCapiInvalidRouter, router)));
  }
  if (config.set_default_route && router.empty()) {
    return std::unexpected(Error::Generic(Tr(Msg::kCapiDefaultRouteNeedsRouter)));
  }

  std::vector<std::string> dns_servers;
  for (std::int32_t i = 0; i < config.dns_count && i < TK_DNS_MAX; ++i) {
    const std::string_view server{config.dns[i]};
    if (server.empty()) {
      continue;
    }
    if (!IsValidIpv4(server)) {
      return std::unexpected(
          Error::Generic(Tr(Msg::kCapiInvalidDns, server)));
    }
    dns_servers.emplace_back(server);
  }

  // The address still goes out through IPConfiguration; see the file header
  // for the reason.
  TETHERKIT_RETURN_IF_ERROR(RunOrFail(
      kIpconfigPath,
      {"set", std::string{interface_name}, "MANUAL", config.address, config.netmask},
      Text(Msg::kCapiWhatApplyManual)));

  if (!router.empty()) {
    TETHERKIT_RETURN_IF_ERROR(InstallScopedDefaultRoute(interface_name, router));
    if (config.set_default_route) {
      TETHERKIT_RETURN_IF_ERROR(PromoteToGlobalDefaultRoute(router));
    }
  }

  if (!dns_servers.empty()) {
    SCDynamicStoreRef store = SharedDynamicStore();
    if (store != nullptr) {
      if (const std::optional<std::string> service_id = FindServiceId(store, interface_name);
          service_id.has_value()) {
        TryPublishDns(store, *service_id, dns_servers);
      } else {
        TETHERKIT_WARN_TR(Msg::kCapiNoServiceForDns, interface_name);
      }
    }
  }
  return tetherkit::Ok();
}

[[nodiscard]] Status ApplyNone(std::string_view interface_name) {
  return RunOrFail(kIpconfigPath, {"set", std::string{interface_name}, "NONE"},
                   Text(Msg::kCapiWhatClearConfig));
}

}  // namespace

void tk_ip_config_init(tk_ip_config_t* out_config) {
  if (out_config == nullptr) {
    return;
  }
  *out_config = tk_ip_config_t{};
  // Default is DHCP: an RNDIS device almost always ships with its own DHCP
  // server, and for the vast majority of users that is the only option they
  // need.
  out_config->mode = TK_IP_MODE_DHCP;
  // Default: do **not** grab the global default route. It is only needed when
  // a higher-priority connected service also exists, and the typical
  // USB-tethering scenario is precisely that no other network is available --
  // there this interface naturally becomes the primary service.
  out_config->set_default_route = false;
}

tk_result_t tk_net_apply(const char* interface_name, const tk_ip_config_t* config,
                         tk_error_t* out_error) {
  ClearError(out_error);
  if (config == nullptr) {
    FillGenericError(out_error, Tr(Msg::kCapiApplyConfigNull));
    return TK_ERR_INVALID_ARGUMENT;
  }
  if (const auto status = ValidateInterface(interface_name); !status) {
    FillError(out_error, status.error());
    return TK_ERR_INVALID_ARGUMENT;
  }
  if (::geteuid() != 0) {
    FillGenericError(out_error, Tr(Msg::kCapiApplyNeedsRoot));
    return TK_ERR_PERMISSION;
  }

  Status status = tetherkit::Ok();
  switch (config->mode) {
    case TK_IP_MODE_DHCP:
      status = ApplyDhcp(interface_name, config->set_default_route);
      break;
    case TK_IP_MODE_MANUAL:
      status = ApplyManual(interface_name, *config);
      break;
    case TK_IP_MODE_NONE:
      status = ApplyNone(interface_name);
      break;
    default:
      FillGenericError(out_error, Tr(Msg::kCapiUnknownIpMode, config->mode));
      return TK_ERR_INVALID_ARGUMENT;
  }

  if (!status) {
    FillError(out_error, status.error());
    return TK_ERR_FAILED;
  }
  return TK_OK;
}

tk_result_t tk_net_clear(const char* interface_name, tk_error_t* out_error) {
  ClearError(out_error);
  if (const auto status = ValidateInterface(interface_name); !status) {
    FillError(out_error, status.error());
    return TK_ERR_INVALID_ARGUMENT;
  }
  if (::geteuid() != 0) {
    FillGenericError(out_error, Tr(Msg::kCapiClearNeedsRoot));
    return TK_ERR_PERMISSION;
  }

  // Remove the DNS key we wrote ourselves (if any) first, then let
  // IPConfiguration tear the service down. The other way round, the service is
  // already gone and the key becomes an unclaimed orphan.
  if (SCDynamicStoreRef store = SharedDynamicStore(); store != nullptr) {
    if (const std::optional<std::string> service_id = FindServiceId(store, interface_name);
        service_id.has_value()) {
      const ScopedCFRef<CFStringRef> key =
          MakeCFString(std::format("State:/Network/Service/{}/DNS", *service_id));
      ::SCDynamicStoreRemoveValue(store, key.Get());
    }
  }

  if (const auto status = ApplyNone(interface_name); !status) {
    FillError(out_error, status.error());
    return TK_ERR_FAILED;
  }
  return TK_OK;
}

tk_result_t tk_net_query(const char* interface_name, tk_net_state_t* out_state,
                         tk_error_t* out_error) {
  ClearError(out_error);
  if (out_state == nullptr) {
    return TK_ERR_INVALID_ARGUMENT;
  }
  if (const auto status = ValidateInterface(interface_name); !status) {
    FillError(out_error, status.error());
    return TK_ERR_INVALID_ARGUMENT;
  }
  *out_state = tk_net_state_t{};

  // ---- Address and netmask: the real state in the kernel ----
  if (const std::optional<std::string> address = QueryAddress(interface_name, SIOCGIFADDR);
      address.has_value()) {
    out_state->has_address = true;
    CopyText(out_state->address, *address);
  }
  if (const std::optional<std::string> netmask = QueryAddress(interface_name, SIOCGIFNETMASK);
      netmask.has_value()) {
    CopyText(out_state->netmask, *netmask);
  }

  // ---- Service-level information: gateway, DNS, configuration method ----
  SCDynamicStoreRef store = SharedDynamicStore();
  if (store == nullptr) {
    // Not being able to reach the dynamic store is not a query failure -- the
    // address half is already there, so return as usual.
    return TK_OK;
  }

  const std::optional<std::string> service_id = FindServiceId(store, interface_name);
  if (service_id.has_value()) {
    const ScopedCFRef<CFDictionaryRef> ipv4 = CopyServiceEntry(store, *service_id, "IPv4");
    const std::string router = StringField(ipv4.Get(), CFSTR("Router"));
    if (!router.empty()) {
      CopyText(out_state->router, router);
      out_state->has_default_route = true;
    }
    CopyText(out_state->method, StringField(ipv4.Get(), CFSTR("ConfigMethod")));

    // The DHCP dictionary only exists in DHCP mode; State is BOUND / INIT and
    // the like.
    const ScopedCFRef<CFDictionaryRef> dhcp = CopyServiceEntry(store, *service_id, "DHCP");
    CopyText(out_state->service_state, StringField(dhcp.Get(), CFSTR("State")));

    // The DNS is always read back from **what is actually in effect in the
    // system**, never restated from the value we pushed down -- whether DNS
    // works in static mode depends on IPMonitor accepting it, and only a
    // read-back is accurate.
    const ScopedCFRef<CFDictionaryRef> dns = CopyServiceEntry(store, *service_id, "DNS");
    const std::vector<std::string> servers = StringArrayField(dns.Get(), CFSTR("ServerAddresses"));
    for (const std::string& server : servers) {
      if (out_state->dns_count >= TK_DNS_MAX) {
        break;
      }
      CopyText(out_state->dns[out_state->dns_count], TK_ADDRESS_CAPACITY, server);
      ++out_state->dns_count;
    }
  }

  out_state->is_primary_default_route = PrimaryInterface(store) == interface_name;
  return TK_OK;
}
