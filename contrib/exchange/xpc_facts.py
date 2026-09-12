#!/usr/bin/env python3
# Copyright (c) 2026 The XPChain Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Static XPChain network facts for exchange listing / integration tooling.

Values are taken from the reference implementation sources:
  - src/amount.h
  - src/consensus/consensus.h
  - src/chainparams.cpp
  - src/chainparamsbase.cpp
"""

from __future__ import annotations

from typing import Any, Dict, Optional

# src/amount.h
COIN = 10_000  # 1 XPC = 10_000 base units
DECIMALS = 4
MAX_MONEY_XPC = 210_000_000_000  # MAX_MONEY / COIN

# src/consensus/consensus.h
COINBASE_MATURITY = 100

# Block target spacing (main/test): src/chainparams.cpp consensus.nPowTargetSpacing
BLOCK_TIME_SECONDS = {
    "main": 60,
    "test": 60,
    "regtest": 60,  # regtest spacing is also 60; blocks are solved instantly when mining
}

NETWORKS: Dict[str, Dict[str, Any]] = {
    "main": {
        "name": "mainnet",
        "ticker": "XPC",
        "p2p_port": 8798,
        "rpc_port": 8762,
        "bech32_hrp": "xpc",
        "legacy_pubkey_version": 76,   # base58 'X'
        "legacy_script_version": 28,   # base58 'C'
        "dns_seeds": [
            "seed1.xpchain.co.kr",
            "seed2.xpchain.co.kr",
            "seed3.xpchain.co.kr",
        ],
        "genesis_hash": "000000009f4a28557aad6be5910c39d40e8a44e596d5ad485a9e4a7d4d72937c",
        "message_start_hex": "fc87bac0",
        "website": "https://www.xpchain.co.kr/",
    },
    "test": {
        "name": "testnet",
        "ticker": "tXPC",
        "p2p_port": 18798,
        "rpc_port": 18762,
        "bech32_hrp": "txpc",
        "legacy_pubkey_version": 138,
        "legacy_script_version": 88,
        "dns_seeds": ["158.179.20.33", "207.211.156.219"],
        "genesis_hash": "17e8ac05fcd037f9b1fe25da878bc6b8e1ebca64083a7fe3168bf509a74a53be",
        "message_start_hex": "fabfb5da",
        "website": "https://www.xpchain.co.kr/",
    },
    "regtest": {
        "name": "regtest",
        "ticker": "XPC",
        "p2p_port": 28798,
        "rpc_port": 28762,
        "bech32_hrp": "xpcrt",
        "legacy_pubkey_version": 138,
        "legacy_script_version": 88,
        "dns_seeds": [],
        "genesis_hash": None,
        "message_start_hex": "fc87bcc1",
        "website": "https://www.xpchain.co.kr/",
    },
}

# Common exchange integration pitfalls (also documented in doc/exchange-integration.md)
INTEGRATION_WARNINGS = [
    "Amounts have at most 4 decimal places (Bitcoin has 8). Store integer base units.",
    "Bech32 addresses use HRP 'xpc' (main), 'txpc' (test), 'xpcrt' (regtest) and are ~62–75+ chars.",
    "Run hot wallets with -minting=0 so coinstake cannot create false deposit credits.",
    "Immature coinbase/stake outputs (~100 blocks) must not be treated as withdrawable.",
    "Credit only payments to assigned deposit addresses; ignore coinstake / immature categories.",
]

REQUIRED_RPCS = [
    "getblockchaininfo",
    "getnetworkinfo",
    "getwalletinfo",
    "getnewaddress",
    "validateaddress",
    "listtransactions",
    "listreceivedbyaddress",
    "getbalance",
    "sendtoaddress",
    "sendmany",
    "estimatesmartfee",
    "backupwallet",
    "rescanblockchain",
]


def amount_to_base_units(amount_str: str) -> int:
    """Parse an XPC decimal string into base units. Raises ValueError on bad input."""
    s = amount_str.strip()
    if not s or s.startswith("-") or s.startswith("+"):
        raise ValueError("amount must be a non-negative decimal")
    if "." in s:
        whole, frac = s.split(".", 1)
        if not whole:
            whole = "0"
        if not whole.isdigit() or not frac.isdigit():
            raise ValueError("amount must be decimal digits")
        if len(frac) > DECIMALS:
            raise ValueError("amount has more than %d decimal places" % DECIMALS)
        frac = frac.ljust(DECIMALS, "0")
        return int(whole) * COIN + int(frac)
    if not s.isdigit():
        raise ValueError("amount must be decimal digits")
    return int(s) * COIN


def base_units_to_amount(units: int) -> str:
    """Format base units as an XPC decimal string (trim trailing zeros after the point)."""
    if units < 0:
        raise ValueError("negative amount")
    whole, frac = divmod(units, COIN)
    if frac == 0:
        return "%d" % whole
    frac_s = ("%0*d" % (DECIMALS, frac)).rstrip("0")
    return "%d.%s" % (whole, frac_s)


def looks_like_bech32(address: str, network: str = "main") -> bool:
    """Lightweight HRP/charset check (not a full bech32 checksum verifier)."""
    hrp = NETWORKS[network]["bech32_hrp"]
    a = address.strip().lower()
    if not a.startswith(hrp + "1"):
        return False
    # bech32 charset excluding '1', 'b', 'i', 'o'
    charset = set("qpzry9x8gf2tvdw0s3jn54khce6mua7l")
    data = a[len(hrp) + 1 :]
    if len(data) < 6:
        return False
    return all(c in charset for c in data)


def listing_fact_sheet(network: str = "main", live: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    """Build a structured fact sheet suitable for exchange listing forms."""
    if network not in NETWORKS:
        raise KeyError("unknown network %r" % network)
    net = NETWORKS[network]
    sheet: Dict[str, Any] = {
        "asset": {
            "name": "XPChain",
            "ticker": "XPC",
            "website": net["website"],
            "source_repository": "https://github.com/jinseob-dev/xpchain-community-core-upgrade",
            "consensus": "Proof-of-Stake (Peercoin-style) with legacy PoW heritage",
            "implementation_base": "Bitcoin Core 0.17.0 derivative",
        },
        "units": {
            "decimals": DECIMALS,
            "base_units_per_coin": COIN,
            "example_valid": "1.2345",
            "example_invalid": "1.23456",
            "max_supply_coins": MAX_MONEY_XPC,
        },
        "network": {
            "id": network,
            "display_name": net["name"],
            "block_time_seconds": BLOCK_TIME_SECONDS[network],
            "p2p_port": net["p2p_port"],
            "rpc_port": net["rpc_port"],
            "dns_seeds": list(net["dns_seeds"]),
            "magic_bytes_hex": net["message_start_hex"],
            "genesis_hash": net.get("genesis_hash"),
            "coinbase_maturity_blocks": COINBASE_MATURITY,
            "recommended_confirmations": 10,
        },
        "addresses": {
            "recommended_type": "bech32 (native SegWit P2WPKH)",
            "bech32_hrp": net["bech32_hrp"],
            "example_prefix": net["bech32_hrp"] + "1q...",
            "min_supported_length": 14,
            "recommended_max_length": 90,
            "legacy_p2pkh_version": net["legacy_pubkey_version"],
            "legacy_p2sh_version": net["legacy_script_version"],
            "notes": [
                "Do not hard-code Bitcoin address length limits (34–42).",
                "Prefer generating deposit addresses with getnewaddress \"\" \"bech32\".",
            ],
        },
        "node_requirements": {
            "daemon": "xpchaind",
            "cli": "xpchain-cli",
            "hot_wallet_flags": ["-minting=0", "-server=1", "-txindex=1 (recommended for explorers/ops)"],
            "wallet_encryption": "encryptwallet + walletpassphrase; SQLCipher builds need -walletdbpassphrase / loadwallet dbpassphrase",
            "required_rpcs": list(REQUIRED_RPCS),
        },
        "deposit_withdrawal": {
            "deposit_detection": ["listtransactions", "listreceivedbyaddress", "optional ZMQ hashblock/rawtx"],
            "withdrawal": ["sendtoaddress", "sendmany"],
            "fee_estimation": "estimatesmartfee",
            "ignore_categories": ["immature", "generate", "orphan"],
            "credit_categories": ["receive"],
        },
        "warnings": list(INTEGRATION_WARNINGS),
        "documentation": {
            "exchange_guide": "doc/exchange-integration.md",
            "wallet_upgrade": "doc/wallet-upgrade.md",
            "rest_interface": "doc/REST-interface.md",
            "zmq": "doc/zmq.md",
        },
    }
    if live:
        sheet["live_node"] = live
    return sheet
