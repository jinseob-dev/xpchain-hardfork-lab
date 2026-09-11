#!/usr/bin/env bash
# Install XPChain Core CLI + GUI binaries from this archive.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_PREFIX="${HOME}/.local/xpchain"
PREFIX="${1:-${XPCHAIN_PREFIX:-$DEFAULT_PREFIX}}"

if [[ ! -d "${ROOT}/bin" ]]; then
  echo "error: ${ROOT}/bin not found. Extract the release archive first." >&2
  exit 1
fi

mkdir -p "${PREFIX}/bin"
install -m 755 "${ROOT}/bin/"* "${PREFIX}/bin/"

cat > "${PREFIX}/bin/xpchain-testnet" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
BIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TESTNET_DATADIR="${XPCHAIN_TESTNET_DATADIR:-${HOME}/.xpchain-testnet}"
exec "${BIN_DIR}/xpchain-qt" -testnet -datadir="${TESTNET_DATADIR}" "$@"
EOF
chmod 755 "${PREFIX}/bin/xpchain-testnet"

DESKTOP_DIR="${HOME}/.local/share/applications"
if [[ -f "${ROOT}/bin/xpchain-qt" ]]; then
  mkdir -p "${DESKTOP_DIR}"
  cat > "${DESKTOP_DIR}/xpchain-qt.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=XPChain Core
Comment=XPChain wallet and node
Exec=${PREFIX}/bin/xpchain-qt
Icon=wallet
Terminal=false
Categories=Finance;Network;
EOF
  echo "Wrote ${DESKTOP_DIR}/xpchain-qt.desktop"

  cat > "${DESKTOP_DIR}/xpchain-testnet.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=XPChain Testnet
Comment=XPChain testnet wallet and node
Exec=${PREFIX}/bin/xpchain-testnet
Icon=wallet
Terminal=false
Categories=Finance;Network;
EOF
  echo "Wrote ${DESKTOP_DIR}/xpchain-testnet.desktop"
fi

echo ""
echo "Installed to ${PREFIX}/bin"
echo "Add to your shell profile:"
echo "  export PATH=\"${PREFIX}/bin:\$PATH\""
echo ""
echo "Start GUI:  xpchain-qt"
echo "Testnet:    xpchain-testnet"
echo "Start node: xpchaind -daemon"
echo "CLI:        xpchain-cli getwalletinfo"
