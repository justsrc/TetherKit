#!/usr/bin/env bash
#
# Install tetherkit-helper and register the LaunchDaemon. **Must run as root.**
#
#   sudo ./gui/Scripts/install-helper.sh
#
# Why this step exists: creating the feth virtual interface and opening
# /dev/bpf* both require root, while the App itself runs as the normal user.
# The helper's root comes from launchd -- see docs/GUI-ARCHITECTURE.md.
#
# The in-app "Install privileged components" button runs this same script:
# build-gui.sh packs it into the payload under the .app's
# Contents/Library/HelperTools/, and the App executes that copy as root via
# AuthorizationExecuteWithPrivileges (the SMAppService route is unavailable
# for source-distributed builds -- every machine produces a different cdhash,
# so a code-signature bound to it can never match; see docs/GUI-SPIKE.md
# section 3.3). The terminal entry point remains for GUI-less scenarios and
# for people who would rather not enter their password in the App.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# Locate the payload directory by priority:
#   1. TETHERKIT_HELPER_DIR -- packaging / CI can point it wherever it wants;
#   2. the script's own directory -- the copy packed into the .app payload
#      takes this branch: the payload sits right next to it;
#   3. the repo's default artifact location -- running
#      sudo ./gui/Scripts/install-helper.sh inside the repo takes this one.
if [[ -n "${TETHERKIT_HELPER_DIR:-}" ]]; then
  SOURCE_DIR="${TETHERKIT_HELPER_DIR}"
elif [[ -f "${SCRIPT_DIR}/com.tetherkit.helper" ]]; then
  SOURCE_DIR="${SCRIPT_DIR}"
else
  SOURCE_DIR="$(cd -- "${SCRIPT_DIR}/../.." && pwd)/dist/TetherKit.app/Contents/Library/HelperTools"
fi

LABEL="com.tetherkit.helper"
TOOLS_DIR="/Library/PrivilegedHelperTools"
INSTALL_PATH="${TOOLS_DIR}/${LABEL}"
PLIST_PATH="/Library/LaunchDaemons/${LABEL}.plist"

log() { printf '\033[32m==>\033[0m %s\n' "$1"; }
die() { printf '\033[31merror: \033[0m%s\n' "$1" >&2; exit 1; }

[[ "${EUID}" -eq 0 ]] || die "root privileges required; run this script with sudo"
[[ -f "${SOURCE_DIR}/${LABEL}" ]] || die "cannot find ${SOURCE_DIR}/${LABEL}
  build it first: ./gui/Scripts/build-gui.sh"

# ------------------------------------------------------------------------------
# Uninstall the old copy first
#
# The order matters: bootout must happen before the files are overwritten.
# The other way around, launchd still holds a reference to the old binary,
# and after the overwrite the next spawn may silently be the old one.
# ------------------------------------------------------------------------------
if launchctl print "system/${LABEL}" >/dev/null 2>&1; then
  log "Uninstalling the registered old version"
  launchctl bootout "system/${LABEL}" 2>/dev/null || true
fi

# ------------------------------------------------------------------------------
# Install the files
# ------------------------------------------------------------------------------
log "Installing to ${INSTALL_PATH}"
mkdir -p "${TOOLS_DIR}"

# cp -a rather than install: libtetherkit's two symlinks must survive as
# symlinks. install copies each of them into an independent real file, and
# after that there is no telling which copy @rpath ends up loading.
cp -a "${SOURCE_DIR}/${LABEL}" "${INSTALL_PATH}"
cp -a "${SOURCE_DIR}"/libtetherkit*.dylib "${TOOLS_DIR}/"
if compgen -G "${SOURCE_DIR}/libusb-*.dylib" >/dev/null; then
  cp -a "${SOURCE_DIR}"/libusb-*.dylib "${TOOLS_DIR}/"
fi

# The LaunchDaemon has hard permission requirements: owner must be root:wheel,
# and it must NOT be group- or other-writable. When that is violated, launchd
# refuses to load it with the vague "Service cannot load in requested
# session".
chown -R root:wheel "${TOOLS_DIR}"
chmod 755 "${TOOLS_DIR}"
chmod 544 "${INSTALL_PATH}"
find "${TOOLS_DIR}" -maxdepth 1 -name '*.dylib' -type f -exec chmod 444 {} +

log "Installing ${PLIST_PATH}"
cp "${SOURCE_DIR}/${LABEL}.plist" "${PLIST_PATH}"
chown root:wheel "${PLIST_PATH}"
chmod 644 "${PLIST_PATH}"

# ------------------------------------------------------------------------------
# Register
# ------------------------------------------------------------------------------
log "Registering the LaunchDaemon"
launchctl bootstrap system "${PLIST_PATH}"

# A service that declares MachServices is spawned on demand: right after
# registration there is no process yet -- that is normal, not a failure.
# Validating means checking that the service is registered.
if launchctl print "system/${LABEL}" >/dev/null 2>&1; then
  log "Done. TetherKit.app picks it up automatically; restarting the App is unnecessary."
else
  die "Service still not found after registration; check /var/log/tetherkit-helper.log"
fi

cat <<EOF

Installed:
  ${INSTALL_PATH}
  ${PLIST_PATH}

Log:
  /var/log/tetherkit-helper.log

Uninstall:
  sudo "${SOURCE_DIR}/uninstall-helper.sh"
EOF
