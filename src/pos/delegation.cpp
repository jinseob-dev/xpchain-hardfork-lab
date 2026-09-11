// Copyright (c) 2026 The XPChain Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/delegation.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace pos {

bool RecommendColdStakeSplit(CAmount totalAmount, double difficulty,
                             const ColdStakeSplitOptions& options,
                             ColdStakeSplitPlan& plan)
{
    plan = {};
    if (totalAmount <= 0 || difficulty <= 0 || options.minimumAgeDays < 0 ||
        options.targetAgeDays <= options.minimumAgeDays ||
        options.maximumAgeDays <= options.targetAgeDays ||
        options.targetProbability <= 0 || options.targetProbability >= 1 ||
        options.maximumOutputs == 0 || options.minimumOutputAmount <= 0 ||
        totalAmount < options.minimumOutputAmount) {
        return false;
    }

    static constexpr long double SECONDS_PER_DAY = 24.0L * 60.0L * 60.0L;
    static constexpr long double TWO_POW_32 = 4294967296.0L;
    const long double startWeight = (options.targetAgeDays - options.minimumAgeDays) * SECONDS_PER_DAY;
    const long double endWeight = (options.maximumAgeDays - options.minimumAgeDays) * SECONDS_PER_DAY;
    const long double attempts = endWeight - startWeight;
    // Approximate one independent kernel attempt per second. This calculation
    // only recommends a wallet layout and never participates in consensus.
    const long double coinDaySeconds = attempts * (startWeight + endWeight) / (2.0L * SECONDS_PER_DAY);
    const long double lambdaPerCoin = coinDaySeconds / (TWO_POW_32 * difficulty);
    if (!(lambdaPerCoin > 0)) return false;

    const long double targetCoins = -std::log1p(-options.targetProbability) / lambdaPerCoin;
    const long double targetSatoshis = std::ceil(targetCoins * COIN);
    if (!std::isfinite(static_cast<double>(targetSatoshis)) ||
        targetSatoshis > static_cast<long double>(std::numeric_limits<CAmount>::max())) {
        return false;
    }

    plan.totalAmount = totalAmount;
    plan.targetOutputAmount = std::max(options.minimumOutputAmount,
                                       static_cast<CAmount>(targetSatoshis));
    // Round up so no output exceeds the calculated target merely because the
    // total is not an exact multiple (subject to the explicit output cap).
    size_t outputCount = static_cast<size_t>(1 + (totalAmount - 1) / plan.targetOutputAmount);
    outputCount = std::max<size_t>(1, std::min(outputCount, options.maximumOutputs));
    while (outputCount > 1 && totalAmount / static_cast<CAmount>(outputCount) < options.minimumOutputAmount) {
        --outputCount;
    }

    const CAmount baseAmount = totalAmount / outputCount;
    CAmount remainder = totalAmount % outputCount;
    plan.outputs.assign(outputCount, baseAmount);
    for (CAmount& output : plan.outputs) {
        if (remainder == 0) break;
        ++output;
        --remainder;
    }

    const long double outputCoins = static_cast<long double>(baseAmount) / COIN;
    const long double lambda = outputCoins * lambdaPerCoin;
    plan.estimatedProbability = static_cast<double>(1.0L - std::exp(-lambda));
    return true;
}

int64_t GetColdStakeEffectiveTargetAge(int64_t consensusMinimumAge,
                                       int64_t configuredTargetAge,
                                       int64_t configuredFallbackAge,
                                       int64_t fallbackDelay,
                                       int64_t secondsSinceTip)
{
    if (consensusMinimumAge < 0) consensusMinimumAge = 0;

    const int64_t targetAge = configuredTargetAge > 0
        ? std::max(consensusMinimumAge, configuredTargetAge)
        : consensusMinimumAge;
    const int64_t fallbackAge = configuredFallbackAge > 0
        ? std::max(consensusMinimumAge, std::min(configuredFallbackAge, targetAge))
        : consensusMinimumAge;

    if (fallbackDelay <= 0 || secondsSinceTip >= fallbackDelay) return fallbackAge;
    if (secondsSinceTip <= 0 || targetAge == fallbackAge) return targetAge;

    const int64_t ageRange = targetAge - fallbackAge;
    return targetAge - ageRange * secondsSinceTip / fallbackDelay;
}

} // namespace pos
