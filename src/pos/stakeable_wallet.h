// Copyright (c) 2018-2026 The XPChain Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef XPCHAIN_POS_STAKEABLE_WALLET_H
#define XPCHAIN_POS_STAKEABLE_WALLET_H

#include <amount.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <script/standard.h>
#include <uint256.h>

#include <string>
#include <utility>
#include <vector>

namespace pos {

/** UTXO candidate available for staking */
struct StakeCandidate {
    COutPoint outpoint;
    CTxOut txout;
    uint256 hashBlock;
    bool isColdStake{false};
};

/**
 * Interface representing wallet capabilities required for PoS minting and block assembly.
 *
 * This lives apart from staker.h so that the block assembler can depend on the
 * contract alone, without depending on the staking thread that drives it.
 */
class IStakeableWallet {
public:
    virtual ~IStakeableWallet() = default;

    virtual std::string GetWalletName() const = 0;
    virtual bool IsLocked() const = 0;
    virtual void GetStakeCandidates(std::vector<StakeCandidate>& vCandidates) = 0;
    virtual bool CreateCoinStake(const StakeCandidate& candidate, CAmount nCompoundReward,
                                 CTransactionRef& txNew, CAmount& nFees) = 0;
    virtual bool SignReward(uint32_t nTime, CTransactionRef txCoinStake,
                            const std::vector<std::pair<CScript, CAmount>>& vValues,
                            CScript& script) const = 0;
    virtual bool SignBlock(CBlock* pblock) const = 0;
    virtual std::vector<std::pair<CTxDestination, int>> GetRewardPct(const CTxDestination& defaultDestination) const = 0;
    virtual bool GetPrevTx(const uint256& hash, CTransactionRef& txOut, uint256& hashBlock) const = 0;
};

} // namespace pos

#endif // XPCHAIN_POS_STAKEABLE_WALLET_H
