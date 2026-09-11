// Copyright (c) 2026 The XPChain Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef XPCHAIN_POS_DELEGATION_H
#define XPCHAIN_POS_DELEGATION_H

#include <amount.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pos {

struct ColdStakeSplitOptions {
    int minimumAgeDays{3};
    int targetAgeDays{32};
    int maximumAgeDays{60};
    double targetProbability{0.50};
    size_t maximumOutputs{20};
    CAmount minimumOutputAmount{COIN};
};

struct ColdStakeSplitPlan {
    CAmount totalAmount{0};
    CAmount targetOutputAmount{0};
    double estimatedProbability{0};
    std::vector<CAmount> outputs;
};

/** Wallet-policy recommendation only; never used for consensus validation. */
bool RecommendColdStakeSplit(CAmount totalAmount, double difficulty,
                             const ColdStakeSplitOptions& options,
                             ColdStakeSplitPlan& plan);

/**
 * Return the wallet-policy age threshold for delegated staking.
 *
 * The preferred target is used while the chain is moving. If the tip becomes
 * stale, the threshold is reduced linearly to the fallback age. Consensus still
 * applies its own minimum-age check independently of this policy.
 */
int64_t GetColdStakeEffectiveTargetAge(int64_t consensusMinimumAge,
                                       int64_t configuredTargetAge,
                                       int64_t configuredFallbackAge,
                                       int64_t fallbackDelay,
                                       int64_t secondsSinceTip);

} // namespace pos

#endif // XPCHAIN_POS_DELEGATION_H
