# Cold staking testnet guide

[한국어 안내](cold-staking-testnet.ko.md)

Cold staking separates the owner key, which can withdraw funds, from the staking
key used by an online node. During the testnet phase all rewards return to the
same cold-staking contract. Operator commission is intentionally disabled.

These wallet policies do not change the consensus minimum stake age or the
existing reward formula:

- `-coldstaketargetage=<days>`: preferred delegated-output age (default `32`;
  `0` uses the network consensus minimum).
- `-coldstakefallbackage=<days>`: policy floor if the chain stalls (default `3`;
  never below the consensus minimum).
- `-coldstakefallbackdelay=<minutes>`: stale-tip interval over which the target
  is reduced to the fallback (default `15`).

## Testnet flow

Create/import the same contract in the owner and staking-node wallets:

```text
createcoldstakingaddress "owner_address" "staker_address"
```

Preview an amount-dependent split, then create it:

```text
estimatecoldstakingsplit 1000
delegatecoldstaking "contract_address" 1000
```

The optional object accepted by both commands supports
`minimum_age_days`, `target_age_days`, `maximum_age_days`,
`target_probability`, `maximum_outputs`, and `minimum_output_amount`.
The calculation uses floating point only as non-consensus wallet policy; all
transaction amounts are integer satoshis and the output sum exactly equals the
requested delegation amount.

Inspect contracts and roles:

```text
listcoldstaking
```

Recover funds from an owner wallet. The fee is taken from change returned to
the same contract, so the amount must leave enough contract balance for a fee:

```text
withdrawcoldstaking "contract_address" "destination_address" 10
```

For a short regtest run, pass `-coldstaketargetage=0` to the staking node. This
changes only its local candidate-selection policy; consensus validation still
enforces the network minimum age.

## Automated safety coverage

`feature_pos_staking.py` exercises the complete delegated-staking lifecycle with
separate owner and staker nodes. In addition to Taproot staking, reward routing,
P2P propagation and reindex validation, it asserts that:

- the owner wallet reports `owner=true, staker=false`;
- the staking wallet reports `owner=false, staker=true`;
- `withdrawcoldstaking` is rejected by the staking wallet;
- an owner withdrawal uses the empty (`OP_0`) owner-branch selector and confirms;
- a wallet-file backup restores the cold-staking contract and owner capability.

Run it with:

```text
python3 test/functional/test_runner.py feature_pos_staking.py
```

## Public testnet prerequisites

The hardfork-lab public testnet uses P2P port `18798`, Bech32 HRP `txpc`, a
one-hour consensus minimum stake age, and activates Taproot and cold staking
from genesis. Proof of Stake starts at height `201`, after 200 easy Proof-of-Work
bootstrap blocks. Its genesis block and network magic are intentionally distinct
from both mainnet and the retired legacy XPChain testnet. Do not reuse mainnet
wallet keys or data directories.

The preview client contains the two current hardfork-lab bootstrap IPs as numeric
seeds. They can also be supplied explicitly with one or more of:

```text
-addnode=<bootstrap-ip>:18798
```

Verify readiness on every participant:

```text
getconnectioncount
getblockchaininfo
getpeerinfo
```

The gate is: at least two independent peers, matching best block hash, and
`initialblockdownload=false`. Do not fund or delegate testnet coins before this
gate passes.

The retired `seed1`/`seed2`/`seed3.xpchain.co.kr` DNS entries are not used by
this isolated network. Before starting the new network, every participant must
stop the old preview client and reset only its testnet chain data. Wallet backups
must be retained separately.
