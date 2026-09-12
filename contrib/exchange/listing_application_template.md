# XPChain (XPC) — Exchange Listing Application Template

Fill this form (or attach the output of `contrib/exchange/listing_facts.py`) when
applying to list XPC. Values below match the reference implementation in this
repository.

## 1. General

| Field | Value |
|-------|-------|
| Full name | XPChain |
| Ticker | XPC |
| Website | https://www.xpchain.co.kr/ |
| Whitepaper | https://www.xpchain.co.kr/?loc=lnkwhitepaper |
| Source code | https://github.com/jinseob-dev/xpchain-community-core-upgrade |
| License | MIT |
| Consensus | Proof-of-Stake (Peercoin-style) + legacy PoW heritage |
| Implementation | Bitcoin Core 0.17.0 derivative (`xpchaind`) |

## 2. Tokenomics / units

| Field | Value |
|-------|-------|
| Decimals | **4** (1 XPC = 10,000 base units) |
| Max supply | 210,000,000,000 XPC |
| Smallest unit | 0.0001 XPC |
| Amount validation | Reject >4 decimal places |

## 3. Network

| Field | Mainnet | Testnet | Regtest |
|-------|---------|---------|---------|
| P2P port | 8798 | 18798 | 28798 |
| RPC port | 8762 | 18762 | 28762 |
| Bech32 HRP | `xpc` | `txpc` | `xpcrt` |
| Block time | ~60s | ~60s | instant (mining) |
| Magic bytes | `fc87bac0` | `fabfb5da` | `fc87bcc1` |
| DNS seeds | seed1/2/3.xpchain.co.kr | — | — |

- Genesis (main): `000000009f4a28557aad6be5910c39d40e8a44e596d5ad485a9e4a7d4d72937c`
- Coinbase / stake maturity: **100 blocks**
- Suggested deposit confirmations: **10** (exchange policy may vary)

## 4. Addresses & wallet

| Field | Value |
|-------|-------|
| Recommended deposit type | Native SegWit bech32 (`getnewaddress "" "bech32"`) |
| Address length | Support **at least 80–90 characters** (do not cap at 34–42) |
| Legacy P2PKH version | 76 (mainnet) |
| Legacy P2SH version | 28 (mainnet) |
| Hot wallet flag | **`-minting=0`** (mandatory) |
| Encryption | `encryptwallet` / `walletpassphrase`; SQLCipher builds need DB passphrase |

## 5. Deposit / withdrawal API (JSON-RPC)

| Operation | RPC |
|-----------|-----|
| New deposit address | `getnewaddress` |
| Validate address | `validateaddress` |
| List deposits | `listtransactions`, `listreceivedbyaddress` |
| Balances | `getbalance`, `getwalletinfo` (`immature_balance`) |
| Withdraw | `sendtoaddress`, `sendmany` |
| Fees | `estimatesmartfee` |
| Restore | `rescanblockchain`, `backupwallet` |

Optional: ZeroMQ `hashblock` / `rawtx` (see `doc/zmq.md`).

## 6. Integration risks (must acknowledge)

1. **Coinstake false deposits** if minting is enabled on the hot wallet.
2. **Immature rewards** can appear on explorers before they are spendable (~100 blocks).
3. **4-decimal** precision differs from Bitcoin-based exchange templates.
4. **Long bech32** addresses break DB schemas that assume short Base58 lengths.

## 7. Test evidence checklist

- [ ] Fact sheet generated: `./contrib/exchange/listing_facts.py --format md`
- [ ] Readiness check PASS: `./contrib/exchange/readiness_check.py ...`
- [ ] Testnet/regtest deposit credited only for `category=receive`
- [ ] Withdrawal of `0.0001` XPC succeeds; `1.00001` rejected
- [ ] Address length ≥ bech32 sample accepted in exchange DB
- [ ] Backup / restore / rescan verified

## 8. Contacts

| Role | Name | Email | Telegram/Other |
|------|------|-------|----------------|
| Technical | | | |
| Business | | | |
| Emergency | | | |

---

## 한국어 안내

위 표는 거래소 상장 신청서에 그대로 첨부할 수 있는 기술 스펙입니다.
자동 생성본이 필요하면:

```bash
./contrib/exchange/listing_facts.py --format md > xpc-listing-facts.md
```

상세 연동 가이드: `doc/exchange-integration.md`.
