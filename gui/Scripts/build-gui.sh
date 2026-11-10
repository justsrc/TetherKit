#!/usr/bin/env bash
#
# Build the TetherKit GUI: TetherKit.app and tetherkit-helper.
#
# Usage:
#   ./gui/Scripts/build-gui.sh              # uses the default build/ directory
#   TETHERKIT_BUILD_DIR=/path ./gui/Scripts/build-gui.sh
#   ./gui/Scripts/build-gui.sh --debug      # debug build -- faster compile, breakpoints
#
# There is exactly one artifact:
#   dist/TetherKit.app        -- the double-clickable application
#
# The privileged-helper install payload (helper binary + dylibs + plist +
# install script) is embedded under the .app's Contents/Library/HelperTools/;
# the in-app "Install privileged components" button and the terminal's
# install-helper.sh both consume exactly this bundle.
#
# bash identifier note: a variable immediately followed by full-width or any
# non-ASCII text must be written ${VAR}. bash decides identifier characters
# per byte; under a UTF-8 locale the leading byte (0xEF) of full-width
# punctuation passes isalnum() and merges into the name, yielding
# "VAR?: unbound variable". The scripts carry English-only text now, but the
# rule stays so non-ASCII output can never re-introduce the trap.
set -euo pipefail

# ------------------------------------------------------------------------------
# Paths and arguments
# ------------------------------------------------------------------------------
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
GUI_DIR="${REPO_ROOT}/gui"
BUILD_DIR="${TETHERKIT_BUILD_DIR:-${REPO_ROOT}/build}"
DIST_DIR="${REPO_ROOT}/dist"

SWIFT_CONFIGURATION="release"
# Extra flags appended to swift build. Homebrew formulae must pass
# --disable-sandbox: brew's build sandbox cannot nest SwiftPM's own
# sandbox-exec (homebrew-core does this for every Swift formula). Local
# builds pass nothing.
SWIFT_BUILD_FLAGS=()
for argument in "$@"; do
  case "${argument}" in
    --debug) SWIFT_CONFIGURATION="debug" ;;
    --swift-build-flags=*) IFS=' ' read -r -a SWIFT_BUILD_FLAGS <<< "${argument#*=}" ;;
    -h|--help) sed -n '2,16p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown option: ${argument}" >&2; exit 2 ;;
  esac
done

log() { printf '\033[32m==>\033[0m %s\n' "$1"; }
die() { printf '\033[31merror: \033[0m%s\n' "$1" >&2; exit 1; }

# ------------------------------------------------------------------------------
# Preconditions
# ------------------------------------------------------------------------------
command -v swift >/dev/null || die "swift not found; install Xcode or the command line tools first"

LIB_DIR="${BUILD_DIR}/lib"
if [[ ! -f "${LIB_DIR}/libtetherkit.dylib" ]]; then
  die "cannot find ${LIB_DIR}/libtetherkit.dylib
  build the C++ part first:
    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build build -j"
fi

VERSION="$(sed -n 's/^  VERSION \([0-9.]*\)$/\1/p' "${REPO_ROOT}/CMakeLists.txt" | head -1)"
[[ -n "${VERSION}" ]] || die "could not parse the version from CMakeLists.txt"

log "TetherKit ${VERSION} (Swift ${SWIFT_CONFIGURATION} build)"

# ------------------------------------------------------------------------------
# Compile Swift
# ------------------------------------------------------------------------------
log "Compiling the Swift targets"
# An empty array conflicts with set -u under macOS's bundled bash 3.2
# ("${arr[@]}" reports unbound variable), so the expansion must use the
# ${arr[@]+...} guard form.
TETHERKIT_LIB_DIR="${LIB_DIR}" swift build \
  --package-path "${GUI_DIR}" \
  --configuration "${SWIFT_CONFIGURATION}" \
  ${SWIFT_BUILD_FLAGS[@]+"${SWIFT_BUILD_FLAGS[@]}"}

SWIFT_BIN_DIR="$(TETHERKIT_LIB_DIR="${LIB_DIR}" swift build \
  --package-path "${GUI_DIR}" \
  --configuration "${SWIFT_CONFIGURATION}" \
  ${SWIFT_BUILD_FLAGS[@]+"${SWIFT_BUILD_FLAGS[@]}"} \
  --show-bin-path)"

# ------------------------------------------------------------------------------
# Assemble the .app
# ------------------------------------------------------------------------------
APP_DIR="${DIST_DIR}/TetherKit.app"
log "Assembling ${APP_DIR}"

rm -rf "${APP_DIR}"
mkdir -p "${APP_DIR}/Contents/MacOS" "${APP_DIR}/Contents/Frameworks" \
         "${APP_DIR}/Contents/Resources"

cp "${SWIFT_BIN_DIR}/TetherKitApp" "${APP_DIR}/Contents/MacOS/TetherKit"
sed "s/__TETHERKIT_VERSION__/${VERSION}/g" \
  "${GUI_DIR}/Resources/App-Info.plist" > "${APP_DIR}/Contents/Info.plist"
cp "${GUI_DIR}/Resources/AppIcon.icns" "${APP_DIR}/Contents/Resources/"

# cp -a here is mandatory, never install:
#   libtetherkit.dylib and libtetherkit.0.dylib are both symlinks to
#   .0.1.1.dylib. install does not preserve symlinks -- it copies each as an
#   independent real file, and install_name_tool then gets applied to only
#   one of them while @rpath happens to resolve to the untouched one: a copy
#   that is never used gets fixed, the problem persists, and nothing warns.
#   Recorded in docs/GUI-SPIKE.md section 7.
cp -a "${LIB_DIR}"/libtetherkit*.dylib "${APP_DIR}/Contents/Frameworks/"

# ------------------------------------------------------------------------------
# Assemble the privileged-helper payload (embedded into the .app)
#
# Why Contents/Library/HelperTools/ and not a sibling dist/helper/:
#   The in-app one-click install needs to fetch its payload from one fixed
#   location, and the .app is the only unit guaranteed to be distributed as a
#   whole -- the payload rides with it, on any machine it lands on.
#
# Why one flat directory:
#   Binary, dylibs, plist, and install scripts all sit at a single level, so
#   install-helper.sh can copy the directory wholesale without knowing
#   anything about bundle internals. A second dylib copy for the installer,
#   next to the App-facing one in Frameworks/, costs about 0.8 MB and buys a
#   self-contained install source; worth it.
# ------------------------------------------------------------------------------
PAYLOAD_DIR="${APP_DIR}/Contents/Library/HelperTools"
log "Assembling ${PAYLOAD_DIR}"

mkdir -p "${PAYLOAD_DIR}"
cp "${SWIFT_BIN_DIR}/tetherkit-helper" "${PAYLOAD_DIR}/com.tetherkit.helper"
cp -a "${LIB_DIR}"/libtetherkit*.dylib "${PAYLOAD_DIR}/"
cp "${GUI_DIR}/Resources/com.tetherkit.helper.plist" "${PAYLOAD_DIR}/"
# The install/uninstall scripts also ride in the payload, so a machine that
# has only the .app (no repo) can install and uninstall.
cp "${SCRIPT_DIR}/install-helper.sh" "${SCRIPT_DIR}/uninstall-helper.sh" "${PAYLOAD_DIR}/"
chmod 755 "${PAYLOAD_DIR}/install-helper.sh" "${PAYLOAD_DIR}/uninstall-helper.sh"

# The legacy layout kept the payload at dist/helper/; leftovers only mislead
# -- that copy is no longer updated.
rm -rf "${DIST_DIR}/helper"

# ------------------------------------------------------------------------------
# Embed libusb
#
# Why embed:
#   Homebrew's libusb lives under /opt/homebrew; an artifact copied to
#   another machine will not find it there. And once the build is signed
#   with a developer certificate, Hardened Runtime rejects Homebrew's
#   ad-hoc-signed library outright over the Team ID mismatch
#   (docs/GUI-SPIKE.md section 7).
#
# Which file gets patched:
#   libusb is a dependency of **libtetherkit.dylib**, not of the executable.
#   Running otool -L on the executable returns nothing and the whole section
#   is silently skipped: the install looks perfect while the binary still
#   depends on Homebrew. Recorded in that same section 7.
# ------------------------------------------------------------------------------
embed_libusb() {
  local destination_dir="$1"
  # The real file (not a symlink) is the patch target.
  local real_dylib
  real_dylib="$(ls "${destination_dir}"/libtetherkit.*.*.*.dylib 2>/dev/null | head -1)"
  [[ -n "${real_dylib}" ]] || die "no real libtetherkit file found in ${destination_dir}"

  local libusb_path
  libusb_path="$(otool -L "${real_dylib}" | awk '/libusb-1\.0/ {print $1; exit}')"
  if [[ -z "${libusb_path}" || "${libusb_path}" == @* ]]; then
    return 0  # no absolute-path dependency, nothing to do
  fi
  [[ -f "${libusb_path}" ]] || die "libtetherkit depends on ${libusb_path}, but the file does not exist"

  local libusb_name
  libusb_name="$(basename "${libusb_path}")"
  cp -a "${libusb_path}" "${destination_dir}/${libusb_name}"
  chmod u+w "${destination_dir}/${libusb_name}"

  install_name_tool -change "${libusb_path}" "@loader_path/${libusb_name}" "${real_dylib}"
  install_name_tool -id "@loader_path/${libusb_name}" "${destination_dir}/${libusb_name}"

  # Re-signing after byte edits is mandatory.
  #   arm64 requires valid signatures on executables and dylibs;
  #   install_name_tool invalidates the original one, after which load is
  #   rejected outright by the kernel (Killed: 9) with no reason in any log.
  codesign --force --sign - --timestamp=none "${destination_dir}/${libusb_name}" >/dev/null
  codesign --force --sign - --timestamp=none "${real_dylib}" >/dev/null
}

log "Embedding libusb"
embed_libusb "${APP_DIR}/Contents/Frameworks"
embed_libusb "${PAYLOAD_DIR}"

# ------------------------------------------------------------------------------
# Strip the rpath that points at the build directory
#
# So that `swift run` works during development, Package.swift adds an
# **absolute-path** rpath into build/lib. A shipped artifact must not carry
# it: on the dev machine that path always exists, dyld picks that copy, and
# the copy embedded in the .app never gets exercised -- the problem only
# surfaces on some other machine.
# ------------------------------------------------------------------------------
log "Removing the build-directory rpath"
strip_build_rpath() {
  local binary="$1"
  # otool's output looks like "         path /some/dir (offset 12)"; anchoring
  # at end-of-line matches nothing, so the trailing " (offset" must come along.
  if otool -l "${binary}" | grep -q "path ${LIB_DIR} (offset"; then
    install_name_tool -delete_rpath "${LIB_DIR}" "${binary}"
    codesign --force --sign - --timestamp=none "${binary}" >/dev/null
  fi
}
strip_build_rpath "${APP_DIR}/Contents/MacOS/TetherKit"
strip_build_rpath "${PAYLOAD_DIR}/com.tetherkit.helper"

# ------------------------------------------------------------------------------
# Signing
#
# Ad-hoc signing (-) is enough: source-built artifacts carry no quarantine
# attribute, so Gatekeeper never engages. Developer ID + notarization are
# only needed to distribute prebuilt binaries.
# The order must be "sign the inner libraries first, then the outer .app" --
# the other way around, any inner change invalidates the outer signature.
# ------------------------------------------------------------------------------
log "Ad-hoc signing"
codesign --force --sign - --timestamp=none "${PAYLOAD_DIR}/com.tetherkit.helper" >/dev/null
codesign --force --sign - --timestamp=none --deep "${APP_DIR}" >/dev/null

# ------------------------------------------------------------------------------
# Self-check
# ------------------------------------------------------------------------------
log "Self-check"
codesign --verify --deep --strict "${APP_DIR}" || die ".app signature verification failed"

# LC_BUILD_VERSION's sdk stamp must be >= 26, otherwise the whole app
# renders legacy traffic lights on Tahoe (AGENTS.md pitfall 19). Enforce
# the "recheck on every release" rule in the script, not in memory.
sdk_stamp="$(otool -l "${APP_DIR}/Contents/MacOS/TetherKit" | awk '/LC_BUILD_VERSION/{s=1} s && / sdk /{print $NF; exit}')"
if [[ "$(printf '%s' "${sdk_stamp}" | cut -d. -f1)" -lt 26 ]]; then
  die "sdk stamp is ${sdk_stamp:-missing}; Tahoe would render legacy chrome (need >= 26, see -platform_version in Package.swift)"
fi
echo "  OK LC_BUILD_VERSION sdk = ${sdk_stamp}"

if otool -L "${APP_DIR}/Contents/Frameworks"/libtetherkit.*.*.*.dylib | grep -q '/opt/homebrew'; then
  echo "  WARNING the artifact still depends on /opt/homebrew; it will not run once copied to another machine"
else
  echo "  OK no absolute-path Homebrew dependency"
fi

cat <<EOF

Build complete:
  ${APP_DIR}
  (privileged-helper payload embedded at Contents/Library/HelperTools/)

Next steps:
  open ${APP_DIR}
  First launch guides you through installing the privileged components --
  click "Install privileged components" and enter the admin password once.
  If you prefer the terminal: sudo ./gui/Scripts/install-helper.sh
EOF
