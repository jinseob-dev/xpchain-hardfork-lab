#!/usr/bin/env bash
# Package XPChain Core release artifacts with install helpers.
# Usage: package-release.sh <linux|linux-arm64|win64|win32> <tag> <output-dir-with-binaries>
set -euo pipefail

PLATFORM="${1:?platform}"
TAG="${2:?tag}"
SRC="${3:?source directory with built binaries}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
STAGING="$(mktemp -d)"
trap 'rm -rf "${STAGING}"' EXIT

case "${PLATFORM}" in
  linux|linux-arm64)
    if [[ "${PLATFORM}" == "linux-arm64" ]]; then
      ARCH="arm64"
    else
      ARCH="x86_64"
    fi
    PKG="xpchain-${TAG}-linux-${ARCH}"
    mkdir -p "${STAGING}/${PKG}/bin"
    install -m 755 "${SRC}/xpchaind" "${SRC}/xpchain-cli" "${SRC}/xpchain-tx" "${STAGING}/${PKG}/bin/"
    if [[ -f "${SRC}/xpchain-qt" ]]; then
      install -m 755 "${SRC}/xpchain-qt" "${STAGING}/${PKG}/bin/"
    fi
    install -m 755 "${ROOT}/contrib/install/linux/install.sh" "${STAGING}/${PKG}/"
    install -m 755 "${ROOT}/contrib/install/linux/uninstall.sh" "${STAGING}/${PKG}/"
    cp "${ROOT}/contrib/install/README-INSTALL.txt" "${STAGING}/${PKG}/"
    (cd "${STAGING}" && tar -czf "${ROOT}/${PKG}.tar.gz" "${PKG}")
    echo "Created ${ROOT}/${PKG}.tar.gz"
    ;;
  win64|win32)
    BITS="${PLATFORM#win}"
    PKG="xpchain-${TAG}-win${BITS}"
    mkdir -p "${STAGING}/${PKG}"
    for exe in xpchaind.exe xpchain-cli.exe xpchain-tx.exe xpchain-qt.exe; do
      [[ -f "${SRC}/${exe}" ]] && cp "${SRC}/${exe}" "${STAGING}/${PKG}/"
    done
    cp "${ROOT}/contrib/install/windows/install.bat" "${STAGING}/${PKG}/"
    cp "${ROOT}/contrib/install/windows/install.ps1" "${STAGING}/${PKG}/"
    cp "${ROOT}/contrib/install/README-INSTALL.txt" "${STAGING}/${PKG}/"
    (cd "${STAGING}/${PKG}" && zip -9 -r "${ROOT}/${PKG}.zip" .)
    echo "Created ${ROOT}/${PKG}.zip"
    ;;
  *)
    echo "unknown platform: ${PLATFORM}" >&2
    exit 1
    ;;
esac
