# Cold staking testnet guide

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
