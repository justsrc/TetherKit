// Registry and cleanup of orphan feth interfaces.
//
//  The problem to solve
//   FethInterface is RAII, so on a normal exit the interface is destroyed.
//   But when the **process is SIGKILLed the destructor does not run at all**,
//   and the interface stays in the kernel forever -- and SIGKILL cannot be
//   caught by any signal handler, so "install a signal handler" does not
//   solve this.
//
//   The only reliable backstop is to persist the names of the interfaces we
//   created and clean them up on the next startup. This file is that on-disk
//   registry and the cleanup.
//
//  Why not "destroy every feth at startup"
//   feth is a shared facility; other programs (or the user's own ifconfig
//   invocations) may be using it too. Only destroying the ones we registered
//   ourselves avoids collateral damage.
#include "tethernet/capi/tethernet_c.h"
#include "tethernet/common/i18n.h"
#include "tethernet/common/logging.h"
#include "tethernet/net/feth_device.h"

#include <unistd.h>

#include <cstddef>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "capi_support.h"

namespace {

using tethernet::Msg;
using tethernet::Tr;
using tethernet::capi::ClearError;
using tethernet::capi::FillGenericError;
using tethernet::capi::IsValidFethName;

/// Path of the registry file.
///
/// In /var/run (= /private/var/run) rather than /tmp: only root can write
/// there, and being able to write this file is equivalent to being able to
/// make us destroy an interface of any name -- the `feth<digits>` validation
/// is a backstop, but tightening the permissions to root-only is the correct
/// first line of defense.
///
/// A reboot empties /var/run, and a reboot also takes every feth with it --
/// the two lifetimes coincide, so stale entries need no extra handling.
constexpr const char* kRegistryPath = "/var/run/tethernet-interfaces";

std::mutex& RegistryMutex() {
  static std::mutex mutex;
  return mutex;
}

[[nodiscard]] std::vector<std::string> ReadRegistry() {
  std::vector<std::string> names;
  std::ifstream input{kRegistryPath};
  if (!input) {
    return names;
  }
  std::string line;
  while (std::getline(input, line)) {
    // Strip possible trailing whitespace, then validate the name once more --
    // the file contents are always treated as untrusted input.
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
      line.pop_back();
    }
    if (IsValidFethName(line)) {
      names.push_back(line);
    }
  }
  return names;
}

void WriteRegistry(const std::vector<std::string>& names) noexcept {
  // An empty list means deleting the file, so nobody is left looking at an
  // empty file and wondering whether something is still dangling.
  if (names.empty()) {
    ::unlink(kRegistryPath);
    return;
  }
  std::ofstream output{kRegistryPath, std::ios::trunc};
  if (!output) {
    return;
  }
  for (const std::string& name : names) {
    output << name << '\n';
  }
}

/// Installs the callback for net::SetInterfaceRegistry.
///
/// Every create/destroy rewrites the whole file. That sounds wasteful, but a
/// session creates only two interfaces, and switching to "append only" would
/// mean handling line removal on destroy plus unbounded file growth -- not
/// worth the complexity.
void OnInterfaceChanged(std::string_view name, bool created) noexcept {
  // The whole callback must be noexcept -- the destroy path may run inside a
  // destructor. fstream does not throw by default (no exceptions() set), so
  // as long as we do not throw ourselves it is safe.
  const std::lock_guard<std::mutex> guard(RegistryMutex());
  std::vector<std::string> names = ReadRegistry();

  if (created) {
    for (const std::string& existing : names) {
      if (existing == name) {
        return;
      }
    }
    names.emplace_back(name);
  } else {
    std::erase(names, std::string{name});
  }
  WriteRegistry(names);
}

}  // namespace

namespace tethernet::capi {

void InstallInterfaceRegistry() {
  net::SetInterfaceRegistry(&OnInterfaceChanged);
}

}  // namespace tethernet::capi

tn_result_t tn_cleanup_orphan_interfaces(size_t* out_removed, tn_error_t* out_error) {
  ClearError(out_error);
  if (out_removed != nullptr) {
    *out_removed = 0;
  }
  if (::geteuid() != 0) {
    FillGenericError(out_error, Tr(Msg::kCapiCleanupNeedsRoot));
    return TN_ERR_PERMISSION;
  }

  //  The destroy loop must **never** hold RegistryMutex: a successful
  // DestroyInterfaceByName triggers the registry callback, and the callback
  // takes the very same lock -- std::mutex is not reentrant, so entering it
  // that way is an instant self-deadlock. Therefore "read", "destroy" and
  // "finalize" are split into three parts, and the lock is held only in the
  // first and the last.
  std::vector<std::string> names;
  {
    const std::lock_guard<std::mutex> guard(RegistryMutex());
    names = ReadRegistry();
  }
  if (names.empty()) {
    return TN_OK;
  }

  std::size_t removed = 0;
  for (const std::string& name : names) {
    // The most common reason for a failed destroy is that the interface no
    // longer exists (the system rebooted, say), which is exactly what we
    // wanted. A genuine failure is only logged and does not stop the
    // remaining entries.
    if (const auto status = tethernet::net::DestroyInterfaceByName(name); status) {
      ++removed;
      continue;
    }
    TETHERNET_DEBUG_TR(Msg::kCapiOrphanAlreadyGone, name);
  }

  // Finalize: the ones that could be destroyed were already removed line by
  // line by the callback; what is left are the entries that "were not in the
  // kernel to begin with", and keeping them would only make the next startup
  // retry forever, so they are cleared as well.
  {
    const std::lock_guard<std::mutex> guard(RegistryMutex());
    WriteRegistry({});
  }

  if (out_removed != nullptr) {
    *out_removed = removed;
  }
  return TN_OK;
}
