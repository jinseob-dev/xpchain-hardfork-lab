#!/usr/bin/env bash
set -euo pipefail

DEFAULT_PREFIX="${HOME}/.local/xpchain"
PREFIX="${1:-${XPCHAIN_PREFIX:-$DEFAULT_PREFIX}}"

if [[ -d "${PREFIX}/bin" ]]; then
  rm -f "${PREFIX}/bin/xpchaind" "${PREFIX}/bin/xpchain-cli" \
        "${PREFIX}/bin/xpchain-tx" "${PREFIX}/bin/xpchain-qt" \
        "${PREFIX}/bin/xpchain-testnet"
  rmdir "${PREFIX}/bin" 2>/dev/null || true
  rmdir "${PREFIX}" 2>/dev/null || true
fi

rm -f "${HOME}/.local/share/applications/xpchain-qt.desktop"
rm -f "${HOME}/.local/share/applications/xpchain-testnet.desktop"
echo "Removed XPChain Core from ${PREFIX} (if present)."
