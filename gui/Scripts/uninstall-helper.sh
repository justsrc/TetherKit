#!/usr/bin/env bash
#
# Uninstall tethernet-helper. **Must run with sudo.**
#
#   sudo ./gui/Scripts/uninstall-helper.sh
set -euo pipefail

LABEL="com.tethernet.helper"
TOOLS_DIR="/Library/PrivilegedHelperTools"
INSTALL_PATH="${TOOLS_DIR}/${LABEL}"
PLIST_PATH="/Library/LaunchDaemons/${LABEL}.plist"
STATE_FILE="/var/run/tethernet-interfaces"

log() { printf '\033[32m==>\033[0m %s\n' "$1"; }

[[ "${EUID}" -eq 0 ]] || { printf 'root privileges required; run with sudo\n' >&2; exit 1; }

# bootout sends SIGTERM; the helper catches it, stops the sessions, and
# destroys the virtual interfaces. It must happen BEFORE the files are
# deleted -- with the binary gone, it can no longer shut down gracefully.
if launchctl print "system/${LABEL}" >/dev/null 2>&1; then
  log "Stopping and unregistering the service"
  launchctl bootout "system/${LABEL}" 2>/dev/null || true
  # Give it a moment to finish its shutdown teardown.
  sleep 1
fi

log "Deleting files"
rm -f "${PLIST_PATH}" "${INSTALL_PATH}"
rm -f "${TOOLS_DIR}"/libtethernet*.dylib "${TOOLS_DIR}"/libusb-*.dylib

# Backstop: if the helper was ever force-killed, the state file may still
# list interfaces it never destroyed.
if [[ -f "${STATE_FILE}" ]]; then
  while read -r interface; do
    [[ "${interface}" =~ ^feth[0-9]+$ ]] || continue
    log "Destroying leftover virtual interface ${interface}"
    ifconfig "${interface}" destroy 2>/dev/null || true
  done < "${STATE_FILE}"
  rm -f "${STATE_FILE}"
fi

log "Uninstalled"
