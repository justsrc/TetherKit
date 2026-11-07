#!/usr/bin/env bash
#
# 构建 TetherKit 的图形界面：TetherKit.app 与 tetherkit-helper。
#
# 用法：
#   ./gui/Scripts/build-gui.sh              # 用默认的 build/ 目录
#   TETHERKIT_BUILD_DIR=/path ./gui/Scripts/build-gui.sh
#   ./gui/Scripts/build-gui.sh --debug      # 调试构建，编译快、便于断点
#
# 产物只有一个：
#   dist/TetherKit.app        —— 双击运行的图形界面
#
# 特权组件的安装载荷（helper 二进制 + dylib + plist + 安装脚本）内嵌在
# .app 的 Contents/Library/HelperTools/ 里，App 内「一键安装」与终端里的
# install-helper.sh 用的都是这一份。
#
# ⚠️ 本脚本里所有「变量后面紧跟中文标点」的地方都必须写成 ${VAR}。
#    bash 按字节判断标识符字符，UTF-8 locale 下全角标点的首字节（0xEF）会被
#    isalnum() 判为真、吸进变量名，得到 "VAR?: unbound variable"。
#    这个坑在 C locale 下不复现，只在中文环境炸。
set -euo pipefail

# ------------------------------------------------------------------------------
# 路径与参数
# ------------------------------------------------------------------------------
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
GUI_DIR="${REPO_ROOT}/gui"
BUILD_DIR="${TETHERKIT_BUILD_DIR:-${REPO_ROOT}/build}"
DIST_DIR="${REPO_ROOT}/dist"

SWIFT_CONFIGURATION="release"
# 追加给 swift build 的额外参数。Homebrew formula 需要传 --disable-sandbox：
# brew 的构建沙箱里嵌套不了 SwiftPM 自己的 sandbox-exec（homebrew-core 里
# 所有 Swift formula 都这么干）。本地构建不传即可。
SWIFT_BUILD_FLAGS=()
for argument in "$@"; do
  case "${argument}" in
    --debug) SWIFT_CONFIGURATION="debug" ;;
    --swift-build-flags=*) IFS=' ' read -r -a SWIFT_BUILD_FLAGS <<< "${argument#*=}" ;;
    -h|--help) sed -n '2,20p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "未知参数：${argument}" >&2; exit 2 ;;
  esac
done

log() { printf '\033[32m==>\033[0m %s\n' "$1"; }
die() { printf '\033[31m错误：\033[0m%s\n' "$1" >&2; exit 1; }

# ------------------------------------------------------------------------------
# 前置检查
# ------------------------------------------------------------------------------
command -v swift >/dev/null || die "找不到 swift，请先安装 Xcode 或命令行工具"

LIB_DIR="${BUILD_DIR}/lib"
if [[ ! -f "${LIB_DIR}/libtetherkit.dylib" ]]; then
  die "找不到 ${LIB_DIR}/libtetherkit.dylib
  请先构建 C++ 部分：
    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build build -j"
fi

VERSION="$(sed -n 's/^  VERSION \([0-9.]*\)$/\1/p' "${REPO_ROOT}/CMakeLists.txt" | head -1)"
[[ -n "${VERSION}" ]] || die "无法从 CMakeLists.txt 解析版本号"

log "TetherKit ${VERSION}（Swift ${SWIFT_CONFIGURATION} 构建）"

# ------------------------------------------------------------------------------
# 编译 Swift
# ------------------------------------------------------------------------------
log "编译 Swift 目标"
# ⚠️ 空数组在 macOS 自带的 bash 3.2 里与 set -u 相冲（"${arr[@]}" 报
#    unbound variable），必须用 ${arr[@]+...} 这种守护写法展开。
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
# 组装 .app
# ------------------------------------------------------------------------------
APP_DIR="${DIST_DIR}/TetherKit.app"
log "组装 ${APP_DIR}"

rm -rf "${APP_DIR}"
mkdir -p "${APP_DIR}/Contents/MacOS" "${APP_DIR}/Contents/Frameworks" \
         "${APP_DIR}/Contents/Resources"

cp "${SWIFT_BIN_DIR}/TetherKitApp" "${APP_DIR}/Contents/MacOS/TetherKit"
sed "s/__TETHERKIT_VERSION__/${VERSION}/g" \
  "${GUI_DIR}/Resources/App-Info.plist" > "${APP_DIR}/Contents/Info.plist"
cp "${GUI_DIR}/Resources/AppIcon.icns" "${APP_DIR}/Contents/Resources/"

# ★ 必须用 cp -a，不能用 install ★
#   libtetherkit.dylib 与 libtetherkit.0.dylib 都是指向 .0.1.1.dylib 的软链。
#   install 不保留软链，会各拷一份**独立的真实文件**；后续 install_name_tool
#   只改到其中一份，而按 @rpath 加载的恰好是没改到的那份 —— 修了个不被使用的
#   副本，问题却还在，且毫无征兆。这个坑记在 docs/GUI-SPIKE.md 第 7 节。
cp -a "${LIB_DIR}"/libtetherkit*.dylib "${APP_DIR}/Contents/Frameworks/"

# ------------------------------------------------------------------------------
# 组装特权组件载荷（内嵌进 .app）
#
# ★ 为什么放在 Contents/Library/HelperTools/ 而不是旁边的 dist/helper/ ★
#   App 内「一键安装」要从一个固定位置拿到载荷，而 .app 是唯一保证被整体
#   分发的单位 —— 载荷跟着它走，装到哪台机器都找得到。
#
# ★ 为什么平铺一个目录 ★
#   二进制、dylib、plist、安装脚本全在一层，install-helper.sh 整目录拷走
#   即可，不用关心 bundle 的内部结构。dylib 在 Frameworks/ 已有一份给 App
#   运行时用，这里再放一份给安装器拷 —— 约 0.8 MB 的重复，换来安装源目录
#   自包含，值得。
# ------------------------------------------------------------------------------
PAYLOAD_DIR="${APP_DIR}/Contents/Library/HelperTools"
log "组装 ${PAYLOAD_DIR}"

mkdir -p "${PAYLOAD_DIR}"
cp "${SWIFT_BIN_DIR}/tetherkit-helper" "${PAYLOAD_DIR}/com.tetherkit.helper"
cp -a "${LIB_DIR}"/libtetherkit*.dylib "${PAYLOAD_DIR}/"
cp "${GUI_DIR}/Resources/com.tetherkit.helper.plist" "${PAYLOAD_DIR}/"
# 安装 / 卸载脚本也进载荷：只有 .app（没有仓库）的机器也能装能卸。
cp "${SCRIPT_DIR}/install-helper.sh" "${SCRIPT_DIR}/uninstall-helper.sh" "${PAYLOAD_DIR}/"
chmod 755 "${PAYLOAD_DIR}/install-helper.sh" "${PAYLOAD_DIR}/uninstall-helper.sh"

# 旧版布局把载荷放在 dist/helper/，留着只会误导 —— 那份不再被更新。
rm -rf "${DIST_DIR}/helper"

# ------------------------------------------------------------------------------
# 内嵌 libusb
#
# ★ 为什么要嵌 ★
#   Homebrew 的 libusb 装在 /opt/homebrew 下，产物拷到别的机器上就找不到了。
#   而且一旦用开发者证书签名，Hardened Runtime 会因为 Team ID 不一致直接拒绝
#   加载 Homebrew 的 ad-hoc 签名库（docs/GUI-SPIKE.md 第 7 节）。
#
# ★ 改哪个文件 ★
#   libusb 是 **libtetherkit.dylib** 的依赖，不是可执行文件的。对着可执行文件
#   查 otool -L 会得到空结果，整段处理被静默跳过 —— 装完看起来一切正常，
#   实际仍依赖 Homebrew。这个坑同样记在第 7 节。
# ------------------------------------------------------------------------------
embed_libusb() {
  local destination_dir="$1"
  # 真实文件（不是软链）才是要改的对象。
  local real_dylib
  real_dylib="$(ls "${destination_dir}"/libtetherkit.*.*.*.dylib 2>/dev/null | head -1)"
  [[ -n "${real_dylib}" ]] || die "在 ${destination_dir} 里找不到 libtetherkit 的实体文件"

  local libusb_path
  libusb_path="$(otool -L "${real_dylib}" | awk '/libusb-1\.0/ {print $1; exit}')"
  if [[ -z "${libusb_path}" || "${libusb_path}" == @* ]]; then
    return 0  # 没有绝对路径依赖，不用处理
  fi
  [[ -f "${libusb_path}" ]] || die "libtetherkit 依赖 ${libusb_path}，但该文件不存在"

  local libusb_name
  libusb_name="$(basename "${libusb_path}")"
  cp -a "${libusb_path}" "${destination_dir}/${libusb_name}"
  chmod u+w "${destination_dir}/${libusb_name}"

  install_name_tool -change "${libusb_path}" "@loader_path/${libusb_name}" "${real_dylib}"
  install_name_tool -id "@loader_path/${libusb_name}" "${destination_dir}/${libusb_name}"

  # ★ 改完字节必须重签 ★
  #   arm64 要求可执行文件与 dylib 有有效签名，install_name_tool 改字节会让原
  #   签名失效，之后加载会被内核直接拒绝（Killed: 9），而且日志里看不出原因。
  codesign --force --sign - --timestamp=none "${destination_dir}/${libusb_name}" >/dev/null
  codesign --force --sign - --timestamp=none "${real_dylib}" >/dev/null
}

log "内嵌 libusb"
embed_libusb "${APP_DIR}/Contents/Frameworks"
embed_libusb "${PAYLOAD_DIR}"

# ------------------------------------------------------------------------------
# 摘掉指向构建目录的 rpath
#
# Package.swift 为了让开发时 `swift run` 能直接跑，加了一条指向 build/lib 的
# **绝对路径** rpath。发布产物里必须删掉它，否则：在开发机上它一定存在，dyld
# 会用那一份，内嵌到 .app 里的副本永远得不到验证 —— 等换台机器才暴露问题。
# ------------------------------------------------------------------------------
log "移除构建目录 rpath"
strip_build_rpath() {
  local binary="$1"
  # otool 的输出形如 "         path /some/dir (offset 12)"，锚在行尾匹配不到，
  # 必须带上后面的 " (offset"。
  if otool -l "${binary}" | grep -q "path ${LIB_DIR} (offset"; then
    install_name_tool -delete_rpath "${LIB_DIR}" "${binary}"
    codesign --force --sign - --timestamp=none "${binary}" >/dev/null
  fi
}
strip_build_rpath "${APP_DIR}/Contents/MacOS/TetherKit"
strip_build_rpath "${PAYLOAD_DIR}/com.tetherkit.helper"

# ------------------------------------------------------------------------------
# 签名
#
# ad-hoc 签名（-）就够：源码构建的产物不带 quarantine，Gatekeeper 不参与。
# 需要对外分发预编译产物时才需要 Developer ID + 公证。
# 顺序必须是「先签内部的库，再签外层的 .app」—— 反过来内层的改动会让外层签名失效。
# ------------------------------------------------------------------------------
log "ad-hoc 签名"
codesign --force --sign - --timestamp=none "${PAYLOAD_DIR}/com.tetherkit.helper" >/dev/null
codesign --force --sign - --timestamp=none --deep "${APP_DIR}" >/dev/null

# ------------------------------------------------------------------------------
# 自检
# ------------------------------------------------------------------------------
log "自检"
codesign --verify --deep --strict "${APP_DIR}" || die ".app 签名校验失败"


# LC_BUILD_VERSION 的 sdk 戳必须 ≥ 26，否则 Tahoe 上整台机器都是 legacy
# 三灯（AGENTS.md 第 7 节第 19 条）。这里把那条「每次发版都核对」的要求
# 变成脚本强制执行，而不是靠人记。
sdk_stamp="$(otool -l "${APP_DIR}/Contents/MacOS/TetherKit" | awk '/LC_BUILD_VERSION/{s=1} s && / sdk /{v=$NF} END{print v}')"
if [[ "$(printf '%s' "${sdk_stamp}" | cut -d. -f1)" -lt 26 ]]; then
  die "产物 sdk 戳是 ${sdk_stamp:-未检测到}，Tahoe 上会是 legacy chrome（需要 ≥ 26，见 Package.swift 的 -platform_version）"
fi
echo "  ✓ LC_BUILD_VERSION sdk = ${sdk_stamp}"

if otool -L "${APP_DIR}/Contents/Frameworks"/libtetherkit.*.*.*.dylib | grep -q '/opt/homebrew'; then
  echo "  ⚠️  产物仍依赖 /opt/homebrew，拷到别的机器上会跑不起来"
else
  echo "  ✓ 无 Homebrew 绝对路径依赖"
fi

cat <<EOF

构建完成：
  ${APP_DIR}
  （特权组件载荷内嵌在 Contents/Library/HelperTools/）

接下来：
  open ${APP_DIR}
  首次运行会引导安装特权组件 —— 点「安装特权组件」、输一次管理员密码即可。
  偏好终端的话：sudo ./gui/Scripts/install-helper.sh
EOF
