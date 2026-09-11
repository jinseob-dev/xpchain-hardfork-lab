// Copyright (c) 2026 The XPChain Community developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Regression tests for the XPChain proof-of-stake consensus rules.
//
// These tests exist to freeze the rules described in doc/xpchain-pos-consensus.md
// so that the staged modernization (doc/architecture-separation-roadmap.md) cannot
// silently change consensus while moving code around. Everything covered here is a
// pure function: the parts of proof-of-stake that need chain state (CheckProofOfStake,
// IsCoinStakeTx) are covered by test/functional/feature_pos_staking.py instead.
//
// Where a serialization format is pinned, the expected bytes are assembled by hand
// rather than by reusing the production CDataStream expression, so that a change to
// the production field order or field types actually fails a test.

#include <amount.h>
#include <arith_uint256.h>
#include <chainparams.h>
#include <consensus/merkle.h>
#include <hash.h>
#include <key.h>
#include <keystore.h>
#include <policy/policy.h>
#include <pos/delegation.h>
#include <pos/height.h>
#include <pos/kernel.h>
#include <pos/reward.h>
#include <pos/stake.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/ismine.h>
#include <script/sign.h>
#include <script/standard.h>
#include <uint256.h>
#include <validation.h>

#include <test/test_xpchain.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include <boost/test/unit_test.hpp>

// BasicTestingSetup selects mainnet, which is what CheckStakeKernelHash reads its
// stake age limits from via the global Params().
BOOST_FIXTURE_TEST_SUITE(pos_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(cold_staking_split_plan_scales_with_amount)
{
    pos::ColdStakeSplitOptions options;
    options.minimumOutputAmount = 100 * COIN;
    options.maximumOutputs = 20;

    pos::ColdStakeSplitPlan small;
    BOOST_REQUIRE(pos::RecommendColdStakeSplit(1000 * COIN, 1.0, options, small));
    BOOST_REQUIRE(!small.outputs.empty());

    pos::ColdStakeSplitPlan large;
    BOOST_REQUIRE(pos::RecommendColdStakeSplit(1000000 * COIN, 1.0, options, large));
    BOOST_CHECK(large.outputs.size() >= small.outputs.size());
    BOOST_CHECK(large.outputs.size() <= options.maximumOutputs);

    CAmount sum = 0;
    for (const CAmount amount : large.outputs) {
        BOOST_CHECK(amount >= options.minimumOutputAmount);
        sum += amount;
    }
    BOOST_CHECK_EQUAL(sum, large.totalAmount);
    BOOST_CHECK(large.estimatedProbability > 0);
    BOOST_CHECK(large.estimatedProbability <= 1);
}

BOOST_AUTO_TEST_CASE(cold_staking_split_plan_validates_limits)
{
    pos::ColdStakeSplitOptions options;
    options.minimumOutputAmount = 100 * COIN;
    pos::ColdStakeSplitPlan plan;
    BOOST_CHECK(!pos::RecommendColdStakeSplit(99 * COIN, 1.0, options, plan));
    BOOST_CHECK(!pos::RecommendColdStakeSplit(1000 * COIN, 0.0, options, plan));

    options.targetAgeDays = options.minimumAgeDays;
    BOOST_CHECK(!pos::RecommendColdStakeSplit(1000 * COIN, 1.0, options, plan));
}

BOOST_AUTO_TEST_CASE(cold_staking_target_age_falls_back_when_tip_is_stale)
{
    static constexpr int64_t DAY = 24 * 60 * 60;
    const int64_t minimumAge = 3 * DAY;
    const int64_t targetAge = 32 * DAY;
    const int64_t fallbackDelay = 15 * 60;

    BOOST_CHECK_EQUAL(pos::GetColdStakeEffectiveTargetAge(
        minimumAge, targetAge, minimumAge, fallbackDelay, 0), targetAge);
    BOOST_CHECK_EQUAL(pos::GetColdStakeEffectiveTargetAge(
        minimumAge, targetAge, minimumAge, fallbackDelay, fallbackDelay / 2),
        targetAge - (targetAge - minimumAge) / 2);
    BOOST_CHECK_EQUAL(pos::GetColdStakeEffectiveTargetAge(
        minimumAge, targetAge, minimumAge, fallbackDelay, fallbackDelay), minimumAge);
    BOOST_CHECK_EQUAL(pos::GetColdStakeEffectiveTargetAge(
        minimumAge, 0, 0, fallbackDelay, 0), minimumAge);
}

namespace {

void AppendLE32(std::vector<unsigned char>& out, uint32_t value)
{
    for (int i = 0; i < 4; ++i) out.push_back((value >> (8 * i)) & 0xff);
}

void AppendLE64(std::vector<unsigned char>& out, uint64_t value)
{
    for (int i = 0; i < 8; ++i) out.push_back((value >> (8 * i)) & 0xff);
}

void AppendCompactSize(std::vector<unsigned char>& out, uint64_t size)
{
    // Only the sizes actually used by these tests are needed.
    BOOST_REQUIRE(size < 253);
    out.push_back(static_cast<unsigned char>(size));
}

//! Hand-rolled reimplementation of the kernel preimage, see doc/xpchain-pos-consensus.md §3.2.
//! Note that nTimeBlockFrom is serialized twice; that duplication is consensus.
uint256 ExpectedKernelHash(unsigned int nBits, uint32_t nTimeBlockFrom, unsigned int nTxPrevOffset,
                           uint64_t n, uint32_t nTimeTx)
{
    std::vector<unsigned char> preimage;
    AppendLE32(preimage, nBits);
    AppendLE32(preimage, nTimeBlockFrom);
    AppendLE32(preimage, nTxPrevOffset);
    AppendLE32(preimage, nTimeBlockFrom);
    AppendLE64(preimage, n);
    AppendLE32(preimage, nTimeTx);
    BOOST_REQUIRE_EQUAL(preimage.size(), 28U);

    uint256 out;
    CHash256().Write(preimage.data(), preimage.size()).Finalize(out.begin());
    return out;
}

//! Independent restatement of the kernel target comparison, see §3.3.
bool ExpectedKernelPasses(unsigned int nBits, uint32_t nTimeBlockFrom, CAmount nAmount, uint32_t nTimeTx,
                          const Consensus::Params& params, const uint256& hashProofOfStake)
{
    if (nTimeBlockFrom + params.nStakeMinAge > nTimeTx) return false;

    arith_uint256 target;
    target.SetCompact(nBits);

    const int64_t weight_seconds =
        std::min(static_cast<int64_t>(nTimeTx) - static_cast<int64_t>(nTimeBlockFrom), params.nStakeMaxAge) -
        params.nStakeMinAge;
    const arith_uint256 coin_day_weight = arith_uint256(nAmount) * weight_seconds / COIN / (24 * 60 * 60);

    return !(arith_uint512(UintToArith256(hashProofOfStake)) >
             arith_uint512(coin_day_weight) * arith_uint512(target));
}

CScript P2PKH(const CKey& key)
{
    return GetScriptForDestination(key.GetPubKey().GetID());
}

CKey MakeKey(bool compressed = true)
{
    CKey key;
    key.MakeNewKey(compressed);
    return key;
}

} // namespace

// ---------------------------------------------------------------------------
// §1 PoW -> PoS switch
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(pos_height_switch_is_strictly_greater_than)
{
    for (const std::string& chain : {CBaseChainParams::MAIN, CBaseChainParams::TESTNET, CBaseChainParams::REGTEST}) {
        const auto chainParams = CreateChainParams(chain);
        const Consensus::Params& params = chainParams->GetConsensus();
        const int nSwitch = params.nSwitchHeight;

        BOOST_CHECK(!pos::IsPoSHeight(0, params));
        BOOST_CHECK(!pos::IsPoSHeight(nSwitch - 1, params));
        // The switch height itself is still proof-of-work.
        BOOST_CHECK(!pos::IsPoSHeight(nSwitch, params));
        BOOST_CHECK(pos::IsPoSHeight(nSwitch + 1, params));
        BOOST_CHECK(pos::IsPoSHeight(nSwitch + 1000000, params));
    }
}

BOOST_AUTO_TEST_CASE(pos_switch_heights_are_unchanged)
{
    BOOST_CHECK_EQUAL(CreateChainParams(CBaseChainParams::MAIN)->GetConsensus().nSwitchHeight, 10275);
    BOOST_CHECK_EQUAL(CreateChainParams(CBaseChainParams::TESTNET)->GetConsensus().nSwitchHeight, 10275);
    BOOST_CHECK_EQUAL(CreateChainParams(CBaseChainParams::REGTEST)->GetConsensus().nSwitchHeight, 1680);
}

BOOST_AUTO_TEST_CASE(stake_age_limits_are_unchanged)
{
    const auto main_params = CreateChainParams(CBaseChainParams::MAIN);
    const Consensus::Params& main = main_params->GetConsensus();
    BOOST_CHECK_EQUAL(main.nStakeMinAge, 60 * 60 * 24 * 3);
    BOOST_CHECK_EQUAL(main.nStakeMaxAge, 60 * 60 * 24 * 60);

    const auto test_params = CreateChainParams(CBaseChainParams::TESTNET);
    const Consensus::Params& test = test_params->GetConsensus();
    BOOST_CHECK_EQUAL(test.nStakeMinAge, 60 * 60 * 24 * 3);
    BOOST_CHECK_EQUAL(test.nStakeMaxAge, 60 * 60 * 24 * 60);

    const auto regtest_params = CreateChainParams(CBaseChainParams::REGTEST);
    const Consensus::Params& regtest = regtest_params->GetConsensus();
    BOOST_CHECK_EQUAL(regtest.nStakeMinAge, 10);
    BOOST_CHECK_EQUAL(regtest.nStakeMaxAge, 60 * 60 * 24 * 100);
}

// ---------------------------------------------------------------------------
// §3 Kernel hash
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(stake_kernel_hash_preimage_layout)
{
    const Consensus::Params& params = Params().GetConsensus();

    // Deliberately asymmetric field values so that swapping any two fields, or dropping
    // the duplicated nTimeBlockFrom, produces a different digest.
    const unsigned int nBits = 0x1b0404cb;
    const uint32_t nTimeBlockFrom = 1554076800;
    const unsigned int nTxPrevOffset = 81;
    const uint64_t n = 3;
    const uint32_t nTimeTx = nTimeBlockFrom + static_cast<uint32_t>(params.nStakeMinAge) + 1234;

    uint256 hashProofOfStake;
    // The return value depends on the target; the digest is filled in either way as long
    // as the minimum age requirement is met.
    pos::CheckStakeKernelHash(nBits, nTimeBlockFrom, nTxPrevOffset, 1000 * COIN, n, nTimeTx, hashProofOfStake);

    BOOST_CHECK_EQUAL(hashProofOfStake,
                      ExpectedKernelHash(nBits, nTimeBlockFrom, nTxPrevOffset, n, nTimeTx));
    BOOST_CHECK(hashProofOfStake != uint256());
}

BOOST_AUTO_TEST_CASE(stake_kernel_hash_ignores_amount)
{
    const Consensus::Params& params = Params().GetConsensus();
    const unsigned int nBits = 0x1b0404cb;
    const uint32_t nTimeBlockFrom = 1554076800;
    const uint32_t nTimeTx = nTimeBlockFrom + static_cast<uint32_t>(params.nStakeMinAge) + 4321;

    uint256 small_amount, large_amount;
    pos::CheckStakeKernelHash(nBits, nTimeBlockFrom, 81, 1 * COIN, 0, nTimeTx, small_amount);
    pos::CheckStakeKernelHash(nBits, nTimeBlockFrom, 81, 500000 * COIN, 0, nTimeTx, large_amount);

    // The staked amount is not part of the preimage: it only moves the target.
    BOOST_CHECK_EQUAL(small_amount, large_amount);
}

BOOST_AUTO_TEST_CASE(stake_kernel_hash_is_sensitive_to_every_field)
{
    const Consensus::Params& params = Params().GetConsensus();
    const unsigned int nBits = 0x1b0404cb;
    const uint32_t nTimeBlockFrom = 1554076800;
    const unsigned int nTxPrevOffset = 81;
    const uint64_t n = 3;
    const uint32_t nTimeTx = nTimeBlockFrom + static_cast<uint32_t>(params.nStakeMinAge) + 1234;

    const uint256 base = ExpectedKernelHash(nBits, nTimeBlockFrom, nTxPrevOffset, n, nTimeTx);

    BOOST_CHECK(ExpectedKernelHash(nBits + 1, nTimeBlockFrom, nTxPrevOffset, n, nTimeTx) != base);
    BOOST_CHECK(ExpectedKernelHash(nBits, nTimeBlockFrom + 1, nTxPrevOffset, n, nTimeTx) != base);
    BOOST_CHECK(ExpectedKernelHash(nBits, nTimeBlockFrom, nTxPrevOffset + 1, n, nTimeTx) != base);
    BOOST_CHECK(ExpectedKernelHash(nBits, nTimeBlockFrom, nTxPrevOffset, n + 1, nTimeTx) != base);
    BOOST_CHECK(ExpectedKernelHash(nBits, nTimeBlockFrom, nTxPrevOffset, n, nTimeTx + 1) != base);

    // nTxPrevOffset and the second copy of nTimeBlockFrom occupy distinct slots, so a
    // preimage that omitted the duplicate would be 24 bytes and hash differently.
    std::vector<unsigned char> without_duplicate;
    AppendLE32(without_duplicate, nBits);
    AppendLE32(without_duplicate, nTimeBlockFrom);
    AppendLE32(without_duplicate, nTxPrevOffset);
    AppendLE64(without_duplicate, n);
    AppendLE32(without_duplicate, nTimeTx);
    BOOST_CHECK_EQUAL(without_duplicate.size(), 24U);
    uint256 other;
    CHash256().Write(without_duplicate.data(), without_duplicate.size()).Finalize(other.begin());
    BOOST_CHECK(other != base);
}

BOOST_AUTO_TEST_CASE(stake_kernel_minimum_age)
{
    const Consensus::Params& params = Params().GetConsensus();
    const unsigned int nBits = 0x207fffff; // very easy target
    const uint32_t nTimeBlockFrom = 1554076800;
    const CAmount nAmount = 1000000 * COIN;

    // nTimeBlockFrom + nStakeMinAge > nTimeTx is rejected, and the digest is left untouched.
    uint256 too_young = uint256();
    const uint32_t just_short = nTimeBlockFrom + static_cast<uint32_t>(params.nStakeMinAge) - 1;
    BOOST_CHECK(!pos::CheckStakeKernelHash(nBits, nTimeBlockFrom, 81, nAmount, 0, just_short, too_young));
    BOOST_CHECK_EQUAL(too_young, uint256());

    // Exactly nStakeMinAge is old enough to be considered, so the digest gets computed.
    uint256 old_enough;
    const uint32_t exactly_min = nTimeBlockFrom + static_cast<uint32_t>(params.nStakeMinAge);
    pos::CheckStakeKernelHash(nBits, nTimeBlockFrom, 81, nAmount, 0, exactly_min, old_enough);
    BOOST_CHECK_EQUAL(old_enough, ExpectedKernelHash(nBits, nTimeBlockFrom, 81, 0, exactly_min));
}

BOOST_AUTO_TEST_CASE(stake_kernel_zero_coin_day_weight_never_passes)
{
    const Consensus::Params& params = Params().GetConsensus();
    const uint32_t nTimeBlockFrom = 1554076800;

    // At exactly nStakeMinAge the time weight is zero, so bnCoinDayWeight truncates to
    // zero and no amount or target can make the kernel pass.
    const uint32_t nTimeTx = nTimeBlockFrom + static_cast<uint32_t>(params.nStakeMinAge);
    for (const CAmount nAmount : {CAmount{1}, 1 * COIN, 21000000 * COIN}) {
        for (const unsigned int nBits : {0x207fffffu, 0x1d00ffffu, 0x1b0404cbu}) {
            uint256 hash;
            BOOST_CHECK(!pos::CheckStakeKernelHash(nBits, nTimeBlockFrom, 81, nAmount, 0, nTimeTx, hash));
        }
    }

    // Likewise for a dust amount whose coin-day weight rounds down to zero even with age.
    uint256 hash;
    const uint32_t aged = nTimeBlockFrom + static_cast<uint32_t>(params.nStakeMinAge) + 3600;
    BOOST_CHECK(!pos::CheckStakeKernelHash(0x207fffff, nTimeBlockFrom, 81, 1 * COIN, 0, aged, hash));
}

BOOST_AUTO_TEST_CASE(stake_kernel_matches_independent_target_check)
{
    const Consensus::Params& params = Params().GetConsensus();
    const uint32_t nTimeBlockFrom = 1554076800;
    const unsigned int nTxPrevOffset = 81;

    for (const unsigned int nBits : {0x207fffffu, 0x1d00ffffu, 0x1c00ffffu}) {
        for (const int64_t age_offset : {int64_t{0}, int64_t{86400}, int64_t{86400 * 7},
                                         params.nStakeMaxAge - params.nStakeMinAge,
                                         params.nStakeMaxAge}) {
            for (const CAmount nAmount : {1 * COIN, 1000 * COIN, 5000000 * COIN}) {
                for (const uint64_t n : {uint64_t{0}, uint64_t{1}, uint64_t{7}}) {
                    const uint32_t nTimeTx =
                        nTimeBlockFrom + static_cast<uint32_t>(params.nStakeMinAge + age_offset);

                    uint256 hash;
                    const bool actual =
                        pos::CheckStakeKernelHash(nBits, nTimeBlockFrom, nTxPrevOffset, nAmount, n, nTimeTx, hash);
                    const uint256 expected_hash =
                        ExpectedKernelHash(nBits, nTimeBlockFrom, nTxPrevOffset, n, nTimeTx);
                    const bool expected =
                        ExpectedKernelPasses(nBits, nTimeBlockFrom, nAmount, nTimeTx, params, expected_hash);

                    BOOST_CHECK_EQUAL(actual, expected);
                }
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(stake_kernel_is_monotonic_in_amount)
{
    const Consensus::Params& params = Params().GetConsensus();
    // An easy target, so that the sampled range definitely straddles the threshold.
    const unsigned int nBits = 0x207fffff;
    const uint32_t nTimeBlockFrom = 1554076800;
    const uint32_t nTimeTx = nTimeBlockFrom + static_cast<uint32_t>(params.nStakeMinAge) + 86400;

    // The staked amount only scales the target, so success must be monotonic in it.
    bool seen_success = false;
    CAmount previous = 0;
    for (CAmount nAmount = 1 * COIN; nAmount <= 1000000LL * COIN; nAmount *= 4) {
        uint256 hash;
        const bool ok = pos::CheckStakeKernelHash(nBits, nTimeBlockFrom, 81, nAmount, 0, nTimeTx, hash);
        if (seen_success) {
            BOOST_CHECK_MESSAGE(ok, "kernel regressed from pass to fail between "
                                        << previous << " and " << nAmount);
        }
        seen_success = seen_success || ok;
        previous = nAmount;
    }
    BOOST_CHECK_MESSAGE(seen_success, "no amount in the sampled range satisfied the kernel");
}

// ---------------------------------------------------------------------------
// §5 Reward
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(annual_rate_tiers)
{
    const Consensus::Params& params = Params().GetConsensus();
    const int interval = 60 * 24 * 365; // 525600, nSubsidyReducingInterval

    BOOST_CHECK_EQUAL(pos::GetAnnualRate(0, params), 0.0);
    BOOST_CHECK_EQUAL(pos::GetAnnualRate(params.nSwitchHeight, params), 0.0);

    BOOST_CHECK_EQUAL(pos::GetAnnualRate(params.nSwitchHeight + 1, params), 0.10);
    BOOST_CHECK_EQUAL(pos::GetAnnualRate(interval, params), 0.10);
    BOOST_CHECK_EQUAL(pos::GetAnnualRate(interval + 1, params), 0.09);
    BOOST_CHECK_EQUAL(pos::GetAnnualRate(interval * 2, params), 0.09);
    BOOST_CHECK_EQUAL(pos::GetAnnualRate(interval * 2 + 1, params), 0.08);
    BOOST_CHECK_EQUAL(pos::GetAnnualRate(interval * 3, params), 0.08);
    BOOST_CHECK_EQUAL(pos::GetAnnualRate(interval * 3 + 1, params), 0.07);
    BOOST_CHECK_EQUAL(pos::GetAnnualRate(interval * 4, params), 0.07);
    BOOST_CHECK_EQUAL(pos::GetAnnualRate(interval * 4 + 1, params), 0.06);
    BOOST_CHECK_EQUAL(pos::GetAnnualRate(interval * 5, params), 0.06);
    BOOST_CHECK_EQUAL(pos::GetAnnualRate(interval * 5 + 1, params), 0.05);
    BOOST_CHECK_EQUAL(pos::GetAnnualRate(interval * 50, params), 0.05);
}

BOOST_AUTO_TEST_CASE(pos_reward_is_zero_outside_the_staking_window)
{
    const Consensus::Params& params = Params().GetConsensus();
    const CAmount nAmount = 100000 * COIN;
    const int pos_height = params.nSwitchHeight + 1;

    // Proof-of-work heights never earn a staking reward.
    BOOST_CHECK_EQUAL(pos::GetProofOfStakeReward(0, nAmount, params.nStakeMaxAge, params), 0);
    BOOST_CHECK_EQUAL(pos::GetProofOfStakeReward(params.nSwitchHeight, nAmount, params.nStakeMaxAge, params), 0);

    // Below the minimum stake age nothing is earned, including at the boundary.
    BOOST_CHECK_EQUAL(pos::GetProofOfStakeReward(pos_height, nAmount, 0, params), 0);
    BOOST_CHECK_EQUAL(pos::GetProofOfStakeReward(pos_height, nAmount, params.nStakeMinAge - 1, params), 0);

    // A zero stake earns nothing regardless of age.
    BOOST_CHECK_EQUAL(pos::GetProofOfStakeReward(pos_height, 0, params.nStakeMaxAge, params), 0);
}

BOOST_AUTO_TEST_CASE(pos_reward_is_monotonic_in_age_and_amount)
{
    const Consensus::Params& params = Params().GetConsensus();
    const int pos_height = params.nSwitchHeight + 1;
    const CAmount nAmount = 100000 * COIN;

    CAmount previous = -1;
    for (uint32_t age = static_cast<uint32_t>(params.nStakeMinAge);
         age <= static_cast<uint32_t>(params.nStakeMaxAge); age += 3600) {
        const CAmount reward = pos::GetProofOfStakeReward(pos_height, nAmount, age, params);
        BOOST_CHECK_MESSAGE(reward >= previous, "reward decreased at age " << age);
        previous = reward;
    }

    const uint32_t age = static_cast<uint32_t>(params.nStakeMinAge) + 86400 * 10;
    CAmount previous_by_amount = -1;
    for (CAmount amount = 1 * COIN; amount <= 10000000LL * COIN; amount *= 10) {
        const CAmount reward = pos::GetProofOfStakeReward(pos_height, amount, age, params);
        BOOST_CHECK_MESSAGE(reward >= previous_by_amount, "reward decreased at amount " << amount);
        previous_by_amount = reward;
    }
}

BOOST_AUTO_TEST_CASE(pos_reward_saturates_at_max_stake_age)
{
    const Consensus::Params& params = Params().GetConsensus();
    const int pos_height = params.nSwitchHeight + 1;
    const CAmount nAmount = 100000 * COIN;

    const CAmount at_max = pos::GetProofOfStakeReward(pos_height, nAmount, params.nStakeMaxAge, params);
    BOOST_CHECK(at_max > 0);

    // Ages beyond nStakeMaxAge are clamped, so the reward stops growing. This clamp is
    // also what keeps the uint32 age underflow described in §5.4 bounded.
    for (const int64_t extra : {int64_t{1}, int64_t{86400}, int64_t{86400 * 365}}) {
        BOOST_CHECK_EQUAL(pos::GetProofOfStakeReward(pos_height, nAmount,
                                                static_cast<uint32_t>(params.nStakeMaxAge + extra), params),
                          at_max);
    }

    // A wrapped-around age (block.nTime < prevBlock.nTime) also clamps to nStakeMaxAge,
    // which is exactly why ConnectBlock must run CheckProofOfStake -- whose minimum-age
    // check forbids that ordering -- before computing the reward.
    BOOST_CHECK_EQUAL(pos::GetProofOfStakeReward(pos_height, nAmount, 0xffffffffu, params), at_max);
}

BOOST_AUTO_TEST_CASE(pos_reward_curve_is_clamped_to_one)
{
    const Consensus::Params& params = Params().GetConsensus();
    const int pos_height = params.nSwitchHeight + 1;
    const CAmount nAmount = 100000 * COIN;

    // Once the logistic curve reaches dRewardCurveLimit the coefficient is exactly 1.0,
    // so the reward is a plain pro-rated annual rate. This pins the constants without
    // depending on the exact floating-point coefficient below saturation.
    const uint32_t saturated_age = static_cast<uint32_t>(params.nStakeMaxAge);
    const double expected = static_cast<double>(nAmount) * pos::GetAnnualRate(pos_height, params) *
                            static_cast<double>(saturated_age) / (365.0 * 24.0 * 60.0 * 60.0);
    const CAmount actual = pos::GetProofOfStakeReward(pos_height, nAmount, saturated_age, params);

    BOOST_CHECK_MESSAGE(std::llabs(actual - static_cast<CAmount>(expected)) <= 1,
                        "reward " << actual << " differs from the saturated annual rate "
                                  << static_cast<CAmount>(expected));

    // 60 days at 10% per year on 100000 XPC.
    BOOST_CHECK_CLOSE(static_cast<double>(actual) / COIN, 100000.0 * 0.10 * 60.0 / 365.0, 0.001);
}

BOOST_AUTO_TEST_CASE(pos_reward_excludes_fees)
{
    const Consensus::Params& params = Params().GetConsensus();
    const int pos_height = params.nSwitchHeight + 1;

    // GetProofOfStakeReward has no fee input at all: unlike the proof-of-work branch of
    // ConnectBlock, PoS block reward is not nFees + subsidy. Guard the signature so that
    // a future refactor cannot quietly start adding fees.
    const CAmount reward = pos::GetProofOfStakeReward(pos_height, 100000 * COIN, params.nStakeMaxAge, params);
    BOOST_CHECK(reward > 0);
    BOOST_CHECK(reward < GetBlockSubsidy(pos_height, params));
}

BOOST_AUTO_TEST_CASE(pos_reward_respects_annual_rate_tiers)
{
    const Consensus::Params& params = Params().GetConsensus();
    const CAmount nAmount = 100000 * COIN;
    const uint32_t age = static_cast<uint32_t>(params.nStakeMaxAge);
    const int interval = 60 * 24 * 365;

    // At saturation the reward is proportional to the annual rate, so the tier steps are
    // visible in the reward itself.
    const CAmount tier_10 = pos::GetProofOfStakeReward(interval, nAmount, age, params);
    const CAmount tier_09 = pos::GetProofOfStakeReward(interval + 1, nAmount, age, params);
    const CAmount tier_05 = pos::GetProofOfStakeReward(interval * 5 + 1, nAmount, age, params);

    BOOST_CHECK(tier_10 > tier_09);
    BOOST_CHECK(tier_09 > tier_05);
    BOOST_CHECK_CLOSE(static_cast<double>(tier_09) / tier_10, 0.9, 0.001);
    BOOST_CHECK_CLOSE(static_cast<double>(tier_05) / tier_10, 0.5, 0.001);
}

// ---------------------------------------------------------------------------
// §2 Coinstake / coinbase structure helpers
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(destination_same_accepts_identical_destinations)
{
    const CKey key = MakeKey();

    BOOST_CHECK(pos::IsDestinationSame(P2PKH(key), P2PKH(key)));

    const CScript p2wpkh = GetScriptForDestination(WitnessV0KeyHash(key.GetPubKey().GetID()));
    BOOST_CHECK(pos::IsDestinationSame(p2wpkh, p2wpkh));

    const CScript p2sh = GetScriptForDestination(CScriptID(p2wpkh));
    BOOST_CHECK(pos::IsDestinationSame(p2sh, p2sh));
}

BOOST_AUTO_TEST_CASE(destination_same_rejects_different_destinations)
{
    const CKey a = MakeKey();
    const CKey b = MakeKey();

    BOOST_CHECK(!pos::IsDestinationSame(P2PKH(a), P2PKH(b)));

    // Same key, different script type: a coinstake may not migrate an output between
    // address types.
    const CScript p2wpkh = GetScriptForDestination(WitnessV0KeyHash(a.GetPubKey().GetID()));
    BOOST_CHECK(!pos::IsDestinationSame(P2PKH(a), p2wpkh));

    // Multisig solves to more than one solution and is therefore never accepted.
    const CScript multisig = GetScriptForMultisig(1, {a.GetPubKey(), b.GetPubKey()});
    BOOST_CHECK(!pos::IsDestinationSame(multisig, multisig));

    // Unspendable / unsolvable scripts are rejected too.
    const CScript op_return = CScript() << OP_RETURN << std::vector<unsigned char>{0x01, 0x02};
    BOOST_CHECK(!pos::IsDestinationSame(op_return, op_return));
    BOOST_CHECK(!pos::IsDestinationSame(CScript(), CScript()));
}

BOOST_AUTO_TEST_CASE(coinstake_pubkey_extraction_supported_types)
{
    const CKey key = MakeKey();
    const CPubKey pubkey = key.GetPubKey();

    // Bare pubkey: the key is in the output script itself.
    {
        CMutableTransaction coinstake;
        coinstake.vin.resize(1);
        coinstake.vout.resize(1);
        coinstake.vout[0].scriptPubKey = GetScriptForRawPubKey(pubkey);

        std::vector<CPubKey> keys;
        BOOST_CHECK(pos::GetPubKeysFromCoinStakeTx(MakeTransactionRef(coinstake), keys));
        BOOST_REQUIRE_EQUAL(keys.size(), 1U);
        BOOST_CHECK(keys[0] == pubkey);
    }

    // P2WPKH: the key is the last witness stack element of the coinstake input.
    {
        CMutableTransaction coinstake;
        coinstake.vin.resize(1);
        coinstake.vin[0].scriptWitness.stack.push_back({0x30, 0x00});
        coinstake.vin[0].scriptWitness.stack.push_back(
            std::vector<unsigned char>(pubkey.begin(), pubkey.end()));
        coinstake.vout.resize(1);
        coinstake.vout[0].scriptPubKey = GetScriptForDestination(WitnessV0KeyHash(pubkey.GetID()));

        std::vector<CPubKey> keys;
        BOOST_CHECK(pos::GetPubKeysFromCoinStakeTx(MakeTransactionRef(coinstake), keys));
        BOOST_REQUIRE_EQUAL(keys.size(), 1U);
        BOOST_CHECK(keys[0] == pubkey);
    }
}

BOOST_AUTO_TEST_CASE(coinstake_pubkey_extraction_supports_taproot)
{
    // Taproot staking is supported: a coinstake paying to a v1 witness program yields
    // an x-only public key for block signature validation.
    CMutableTransaction coinstake;
    coinstake.vin.resize(1);
    coinstake.vout.resize(1);
    coinstake.vout[0].scriptPubKey = CScript() << OP_1 << std::vector<unsigned char>(32, 0x02);

    txnouttype type;
    std::vector<std::vector<unsigned char>> solutions;
    BOOST_REQUIRE(Solver(coinstake.vout[0].scriptPubKey, type, solutions));
    BOOST_REQUIRE(type == TX_WITNESS_V1_TAPROOT);

    std::vector<CPubKey> keys;
    BOOST_CHECK(pos::GetPubKeysFromCoinStakeTx(MakeTransactionRef(coinstake), keys));
    BOOST_REQUIRE_EQUAL(keys.size(), 1U);

    BOOST_CHECK(pos::IsDestinationSame(coinstake.vout[0].scriptPubKey, coinstake.vout[0].scriptPubKey));

    Consensus::Params consensus = CreateChainParams(CBaseChainParams::REGTEST)->GetConsensus();
    consensus.TaprootHeight = 1000;
    keys.clear();
    BOOST_CHECK(!pos::GetPubKeysFromCoinStakeTx(MakeTransactionRef(coinstake), keys, 999, consensus));
    BOOST_CHECK(pos::GetPubKeysFromCoinStakeTx(MakeTransactionRef(coinstake), keys, 1000, consensus));
}

BOOST_AUTO_TEST_CASE(taproot_block_signature_is_schnorr_and_height_gated)
{
    const CKey key = MakeKey();
    const XOnlyPubKey outputKey(key.GetPubKey());

    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].scriptSig = CScript() << 1000;
    coinbase.vout.resize(1);

    CMutableTransaction coinstake;
    coinstake.vin.resize(1);
    coinstake.vout.emplace_back(1000 * COIN, CScript() << OP_1 << ToByteVector(outputKey));

    CBlock block;
    block.nVersion = 1;
    block.nTime = 1700000000;
    block.nBits = 0x207fffff;
    block.vtx = {MakeTransactionRef(coinbase), MakeTransactionRef(coinstake)};
    block.hashMerkleRoot = BlockMerkleRoot(block);

    std::vector<unsigned char> signature;
    BOOST_REQUIRE(key.SignSchnorr(block.GetBlockHeader().GetHash(), signature));
    coinbase.vin[0].scriptSig << signature;
    block.vtx[0] = MakeTransactionRef(coinbase);
    block.hashMerkleRoot = BlockMerkleRoot(block);

    Consensus::Params consensus = CreateChainParams(CBaseChainParams::REGTEST)->GetConsensus();
    consensus.TaprootHeight = 1000;
    BOOST_CHECK(!pos::CheckBlockSignature(block, 999, consensus));
    BOOST_CHECK(pos::CheckBlockSignature(block, 1000, consensus));

    signature[0] ^= 1;
    CMutableTransaction badCoinbase(*block.vtx[0]);
    badCoinbase.vin[0].scriptSig = CScript() << 1000 << signature;
    block.vtx[0] = MakeTransactionRef(badCoinbase);
    block.hashMerkleRoot = BlockMerkleRoot(block);
    BOOST_CHECK(!pos::CheckBlockSignature(block, 1000, consensus));
}

BOOST_AUTO_TEST_CASE(reward_hash_preimage_layout)
{
    const CKey key = MakeKey();
    const CScript first = P2PKH(key);
    const CScript second = GetScriptForDestination(WitnessV0KeyHash(MakeKey().GetPubKey().GetID()));

    const std::vector<std::pair<CScript, CAmount>> rewards{{first, 12345}, {second, 67890}};

    CMutableTransaction coinstake;
    coinstake.vin.resize(1);
    coinstake.vin[0].prevout = COutPoint(uint256S("0x1122334455667788990011223344556677889900112233445566778899001122"), 4);
    coinstake.vin[0].scriptSig = CScript() << OP_1;
    coinstake.vin[0].nSequence = 0xfffffffe;
    coinstake.vout.resize(1);
    coinstake.vout[0].nValue = 1 * COIN;
    coinstake.vout[0].scriptPubKey = first;
    const CTransactionRef coinstake_ref = MakeTransactionRef(coinstake);

    const uint32_t nTime = 1554076800;

    // Hand-rolled restatement of doc/xpchain-pos-consensus.md §2.1:
    //   Hash( for each reward: scriptPubKey || nValue, then nTime, then vin[0] )
    std::vector<unsigned char> preimage;
    for (const auto& reward : rewards) {
        AppendCompactSize(preimage, reward.first.size());
        preimage.insert(preimage.end(), reward.first.begin(), reward.first.end());
        AppendLE64(preimage, static_cast<uint64_t>(reward.second));
    }
    AppendLE32(preimage, nTime);
    preimage.insert(preimage.end(), coinstake.vin[0].prevout.hash.begin(),
                    coinstake.vin[0].prevout.hash.end());
    AppendLE32(preimage, coinstake.vin[0].prevout.n);
    AppendCompactSize(preimage, coinstake.vin[0].scriptSig.size());
    preimage.insert(preimage.end(), coinstake.vin[0].scriptSig.begin(),
                    coinstake.vin[0].scriptSig.end());
    AppendLE32(preimage, coinstake.vin[0].nSequence);

    uint256 expected;
    CHash256().Write(preimage.data(), preimage.size()).Finalize(expected.begin());

    BOOST_CHECK_EQUAL(pos::GetRewardHash(rewards, coinstake_ref, nTime), expected);
}

BOOST_AUTO_TEST_CASE(reward_hash_is_sensitive_to_reward_details)
{
    const CKey key = MakeKey();
    const CScript script = P2PKH(key);

    CMutableTransaction coinstake;
    coinstake.vin.resize(1);
    coinstake.vin[0].prevout = COutPoint(uint256S("0x01"), 0);
    coinstake.vout.resize(1);
    coinstake.vout[0].nValue = 1 * COIN;
    coinstake.vout[0].scriptPubKey = script;
    const CTransactionRef coinstake_ref = MakeTransactionRef(coinstake);

    const uint32_t nTime = 1554076800;
    const std::vector<std::pair<CScript, CAmount>> base{{script, 1000}};
    const uint256 base_hash = pos::GetRewardHash(base, coinstake_ref, nTime);

    // Amount, ordering, count, time and the staked outpoint all commit into the hash.
    const std::vector<std::pair<CScript, CAmount>> other_amount{{script, 1001}};
    BOOST_CHECK(pos::GetRewardHash(other_amount, coinstake_ref, nTime) != base_hash);

    const CScript other_script = P2PKH(MakeKey());
    const std::vector<std::pair<CScript, CAmount>> two{{script, 1000}, {other_script, 1000}};
    const std::vector<std::pair<CScript, CAmount>> two_swapped{{other_script, 1000}, {script, 1000}};
    BOOST_CHECK(pos::GetRewardHash(two, coinstake_ref, nTime) != base_hash);
    BOOST_CHECK(pos::GetRewardHash(two, coinstake_ref, nTime) != pos::GetRewardHash(two_swapped, coinstake_ref, nTime));

    BOOST_CHECK(pos::GetRewardHash(base, coinstake_ref, nTime + 1) != base_hash);

    CMutableTransaction other_input = coinstake;
    other_input.vin[0].prevout = COutPoint(uint256S("0x02"), 0);
    BOOST_CHECK(pos::GetRewardHash(base, MakeTransactionRef(other_input), nTime) != base_hash);
}

BOOST_AUTO_TEST_CASE(check_proof_of_stake_pure_basic)
{
    const auto params = CreateChainParams(CBaseChainParams::REGTEST);
    const Consensus::Params& consensus = params->GetConsensus();

    CKey key = MakeKey();
    CScript script = P2PKH(key);

    // Create a previous transaction with output
    CMutableTransaction txPrev;
    txPrev.vout.resize(1);
    txPrev.vout[0].nValue = 1000 * COIN;
    txPrev.vout[0].scriptPubKey = script;

    // Create a coinstake transaction spending txPrev
    CMutableTransaction coinstake;
    coinstake.vin.resize(1);
    coinstake.vin[0].prevout = COutPoint(txPrev.GetHash(), 0);
    coinstake.vout.resize(1);
    coinstake.vout[0].nValue = 1000 * COIN;
    coinstake.vout[0].scriptPubKey = script;

    // Sign the coinstake input
    uint256 hash = SignatureHash(script, coinstake, 0, SIGHASH_ALL, 0, SigVersion::BASE);
    std::vector<unsigned char> vchSig;
    BOOST_REQUIRE(key.Sign(hash, vchSig));
    vchSig.push_back((unsigned char)SIGHASH_ALL);
    coinstake.vin[0].scriptSig = CScript() << vchSig << ToByteVector(key.GetPubKey());

    uint32_t nTimeBlockFrom = 1000000;
    uint32_t nTimeTx = nTimeBlockFrom + consensus.nStakeMinAge + 100;
    unsigned int nTxPrevOffset = 100;
    unsigned int nBits = 0x207fffff; // regtest max target

    const int nHeight = 200;

    uint256 hashProofOfStake;
    bool valid = pos::CheckProofOfStakePure(coinstake, txPrev, nBits, nTimeTx, nTimeBlockFrom, nTxPrevOffset, nHeight, hashProofOfStake, consensus);
    BOOST_CHECK(valid);

    // Too young stake should fail
    uint32_t nTimeTxTooYoung = nTimeBlockFrom + consensus.nStakeMinAge - 1;
    uint256 hashProofOfStakeFail;
    BOOST_CHECK(!pos::CheckProofOfStakePure(coinstake, txPrev, nBits, nTimeTxTooYoung, nTimeBlockFrom, nTxPrevOffset, nHeight, hashProofOfStakeFail, consensus));

    // Invalid signature should fail
    CMutableTransaction badCoinstake = coinstake;
    badCoinstake.vin[0].scriptSig = CScript() << OP_0;
    BOOST_CHECK(!pos::CheckProofOfStakePure(badCoinstake, txPrev, nBits, nTimeTx, nTimeBlockFrom, nTxPrevOffset, nHeight, hashProofOfStakeFail, consensus));

    // The verdict depends on the height passed in, not on the active chain tip:
    // a block is judged the same however far the tip has moved.
    BOOST_CHECK(pos::CheckProofOfStakePure(coinstake, txPrev, nBits, nTimeTx, nTimeBlockFrom, nTxPrevOffset, 0, hashProofOfStake, consensus));
    BOOST_CHECK(pos::CheckProofOfStakePure(coinstake, txPrev, nBits, nTimeTx, nTimeBlockFrom, nTxPrevOffset, 4000000, hashProofOfStake, consensus));
}

BOOST_AUTO_TEST_CASE(coinstake_script_flags_follow_block_height)
{
    Consensus::Params consensus = CreateChainParams(CBaseChainParams::REGTEST)->GetConsensus();
    consensus.TaprootHeight = 1000;
    consensus.ColdStakingHeight = 2000;

    BOOST_CHECK((pos::GetCoinStakeScriptFlags(999, consensus) & SCRIPT_VERIFY_TAPROOT) == 0);
    BOOST_CHECK((pos::GetCoinStakeScriptFlags(1000, consensus) & SCRIPT_VERIFY_TAPROOT) != 0);
    BOOST_CHECK((pos::GetCoinStakeScriptFlags(1001, consensus) & SCRIPT_VERIFY_TAPROOT) != 0);
    BOOST_CHECK((pos::GetCoinStakeScriptFlags(1999, consensus) & SCRIPT_VERIFY_COLDSTAKE) == 0);
    BOOST_CHECK((pos::GetCoinStakeScriptFlags(2000, consensus) & SCRIPT_VERIFY_COLDSTAKE) != 0);
}

BOOST_AUTO_TEST_CASE(cold_staking_staker_path_preserves_contract_and_principal)
{
    const CKey stakerKey = MakeKey();
    const CKey ownerKey = MakeKey();
    const CScript coldScript = pos::CreateColdStakingScript(stakerKey.GetPubKey().GetID(), ownerKey.GetPubKey().GetID());
    const CScript contract = GetScriptForDestination(WitnessV0ScriptHash(coldScript));
    const CAmount principal = 1000 * COIN;

    CMutableTransaction spend;
    spend.vin.resize(1);
    spend.vout.resize(1);
    spend.vout[0] = CTxOut(principal, contract);

    const uint256 sighash = SignatureHash(coldScript, spend, 0, SIGHASH_ALL, principal, SigVersion::WITNESS_V0);
    std::vector<unsigned char> sig;
    BOOST_REQUIRE(stakerKey.Sign(sighash, sig));
    sig.push_back(SIGHASH_ALL);
    spend.vin[0].scriptWitness.stack = {
        sig, ToByteVector(stakerKey.GetPubKey()), {0x01}, ToByteVector(coldScript)
    };

    const auto verifies = [&](const CMutableTransaction& tx, unsigned int flags) {
        ScriptError error = SCRIPT_ERR_UNKNOWN_ERROR;
        return VerifyScript(CScript(), contract, &tx.vin[0].scriptWitness,
                            SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS | flags,
                            MutableTransactionSignatureChecker(&tx, 0, principal), &error);
    };

    BOOST_CHECK(verifies(spend, SCRIPT_VERIFY_COLDSTAKE));

    Consensus::Params consensus = CreateChainParams(CBaseChainParams::REGTEST)->GetConsensus();
    consensus.ColdStakingHeight = 1000;
    const CTransactionRef coldStake = MakeTransactionRef(spend);
    const std::vector<std::pair<CScript, CAmount>> protectedReward{{contract, 1 * COIN}};
    const std::vector<std::pair<CScript, CAmount>> redirectedReward{{P2PKH(stakerKey), 1 * COIN}};
    BOOST_CHECK(pos::CheckColdStakingRewardOutputs(coldStake, protectedReward, 1000, consensus));
    BOOST_CHECK(!pos::CheckColdStakingRewardOutputs(coldStake, redirectedReward, 1000, consensus));
    BOOST_CHECK(pos::CheckColdStakingRewardOutputs(coldStake, redirectedReward, 999, consensus));

    CMutableTransaction stolen = spend;
    stolen.vout[0].scriptPubKey = P2PKH(stakerKey);
    BOOST_CHECK(!MutableTransactionSignatureChecker(&stolen, 0, principal).CheckColdStake(coldScript));
    std::vector<unsigned char> stolenSig;
    BOOST_REQUIRE(stakerKey.Sign(SignatureHash(coldScript, stolen, 0, SIGHASH_ALL, principal, SigVersion::WITNESS_V0), stolenSig));
    stolenSig.push_back(SIGHASH_ALL);
    stolen.vin[0].scriptWitness.stack[0] = stolenSig;

    CMutableTransaction reduced = spend;
    reduced.vout[0].nValue -= 1;
    BOOST_CHECK(!MutableTransactionSignatureChecker(&reduced, 0, principal).CheckColdStake(coldScript));

    CMutableTransaction split = spend;
    split.vout.push_back(CTxOut(1, P2PKH(stakerKey)));
    BOOST_CHECK(!MutableTransactionSignatureChecker(&split, 0, principal).CheckColdStake(coldScript));

    // Before activation OP_CHECKCOLDSTAKEVERIFY retains its historical NOP meaning.
    BOOST_CHECK(verifies(stolen, SCRIPT_VERIFY_NONE));
}

BOOST_AUTO_TEST_CASE(cold_staking_wallet_recognition_and_role_signing)
{
    const CKey stakerKey = MakeKey();
    const CKey ownerKey = MakeKey();
    const CScript coldScript = pos::CreateColdStakingScript(stakerKey.GetPubKey().GetID(), ownerKey.GetPubKey().GetID());
    const CScript contract = GetScriptForDestination(WitnessV0ScriptHash(coldScript));
    const CAmount principal = 1000 * COIN;

    CMutableTransaction funding;
    funding.vout.emplace_back(principal, contract);
    const CTransaction fundingTx(funding);

    CBasicKeyStore stakerStore;
    BOOST_REQUIRE(stakerStore.AddKey(stakerKey));
    BOOST_REQUIRE(stakerStore.AddCScript(coldScript));
    BOOST_REQUIRE(stakerStore.AddCScript(contract));
    BOOST_CHECK(IsMine(stakerStore, contract) == ISMINE_SPENDABLE);

    CMutableTransaction coinstake;
    coinstake.vin.emplace_back(COutPoint(fundingTx.GetHash(), 0));
    coinstake.vout.emplace_back(principal, contract);
    BOOST_REQUIRE(SignSignature(stakerStore, fundingTx, coinstake, 0, SIGHASH_ALL));
    BOOST_REQUIRE_EQUAL(coinstake.vin[0].scriptWitness.stack.size(), 4U);
    BOOST_CHECK(coinstake.vin[0].scriptWitness.stack[2] == std::vector<unsigned char>{0x01});

    CMutableTransaction theft;
    theft.vin.emplace_back(COutPoint(fundingTx.GetHash(), 0));
    theft.vout.emplace_back(principal - 1000, P2PKH(stakerKey));
    BOOST_CHECK(!SignSignature(stakerStore, fundingTx, theft, 0, SIGHASH_ALL));

    CBasicKeyStore ownerStore;
    BOOST_REQUIRE(ownerStore.AddKey(ownerKey));
    BOOST_REQUIRE(ownerStore.AddKey(stakerKey));
    BOOST_REQUIRE(ownerStore.AddCScript(coldScript));
    BOOST_REQUIRE(ownerStore.AddCScript(contract));
    BOOST_CHECK(IsMine(ownerStore, contract) == ISMINE_SPENDABLE);

    CMutableTransaction combinedStake;
    combinedStake.vin.emplace_back(COutPoint(fundingTx.GetHash(), 0));
    combinedStake.vout.emplace_back(principal, contract);
    BOOST_REQUIRE(SignSignature(ownerStore, fundingTx, combinedStake, 0, SIGHASH_ALL));
    BOOST_REQUIRE_EQUAL(combinedStake.vin[0].scriptWitness.stack.size(), 4U);
    BOOST_CHECK(combinedStake.vin[0].scriptWitness.stack[2] == std::vector<unsigned char>{0x01});

    CMutableTransaction withdrawal;
    withdrawal.vin.emplace_back(COutPoint(fundingTx.GetHash(), 0));
    withdrawal.vout.emplace_back(principal - 1000, P2PKH(ownerKey));
    BOOST_REQUIRE(SignSignature(ownerStore, fundingTx, withdrawal, 0, SIGHASH_ALL));
    BOOST_REQUIRE_EQUAL(withdrawal.vin[0].scriptWitness.stack.size(), 4U);
    BOOST_CHECK(withdrawal.vin[0].scriptWitness.stack[2].empty());
}

BOOST_AUTO_TEST_CASE(cold_staking_script_and_taproot_staking_tests)
{
    // 1. Generate keys for Staker and Owner
    CKey stakerKey, ownerKey;
    stakerKey.MakeNewKey(true);
    ownerKey.MakeNewKey(true);

    CPubKey stakerPubKey = stakerKey.GetPubKey();
    CPubKey ownerPubKey = ownerKey.GetPubKey();

    const CKeyID stakerKeyId = stakerPubKey.GetID();
    const CKeyID ownerKeyId = ownerPubKey.GetID();

    // 2. Create Cold Staking Redeem Script
    CScript coldScript = pos::CreateColdStakingScript(stakerKeyId, ownerKeyId);
    BOOST_CHECK_EQUAL(coldScript.size(), 54U);

    // Verify IsColdStakingScript pattern matching
    CKeyID parsedStakerId, parsedOwnerId;
    BOOST_CHECK(pos::IsColdStakingScript(coldScript, parsedStakerId, parsedOwnerId));
    BOOST_CHECK(parsedStakerId == stakerKeyId);
    BOOST_CHECK(parsedOwnerId == ownerKeyId);

    // Invalid script should fail pattern match
    CScript dummyScript = CScript() << OP_DUP << OP_HASH160 << ToByteVector(stakerKeyId) << OP_EQUALVERIFY << OP_CHECKSIG;
    BOOST_CHECK(!pos::IsColdStakingScript(dummyScript, parsedStakerId, parsedOwnerId));

    // 3. Wrap in P2WSH (SegWit v0 ScriptHash)
    uint256 coldScriptHash;
    CSHA256().Write(coldScript.data(), coldScript.size()).Finalize(coldScriptHash.begin());
    CScript p2wshScript = CScript() << OP_0 << ToByteVector(coldScriptHash);

    // Create a mock coinstake transaction using this P2WSH output
    CMutableTransaction txCoinStake;
    txCoinStake.vin.resize(1);
    txCoinStake.vout.resize(1);
    txCoinStake.vout[0].scriptPubKey = p2wshScript;
    txCoinStake.vout[0].nValue = 1000 * COIN;

    // Simulate staking spend: witness stack contains [sig, stakerPubKey, 1, coldScript]
    std::vector<unsigned char> dummySig(71, 0x30); // dummy DER signature
    txCoinStake.vin[0].scriptWitness.stack.push_back(dummySig);
    txCoinStake.vin[0].scriptWitness.stack.push_back(ToByteVector(stakerPubKey));
    txCoinStake.vin[0].scriptWitness.stack.push_back(std::vector<unsigned char>{0x01}); // OP_TRUE
    txCoinStake.vin[0].scriptWitness.stack.push_back(ToByteVector(coldScript));

    // Verify that GetPubKeysFromCoinStakeTx successfully extracts the STAKER pubkey (not owner)
    std::vector<CPubKey> pubkeys;
    CTransactionRef txRef = MakeTransactionRef(txCoinStake);
    BOOST_CHECK(pos::GetPubKeysFromCoinStakeTx(txRef, pubkeys));
    BOOST_REQUIRE_EQUAL(pubkeys.size(), 1U);
    BOOST_CHECK(pubkeys[0] == stakerPubKey);
    BOOST_CHECK(pubkeys[0] != ownerPubKey);

    Consensus::Params gatedConsensus = CreateChainParams(CBaseChainParams::REGTEST)->GetConsensus();
    gatedConsensus.ColdStakingHeight = 1000;
    pubkeys.clear();
    BOOST_CHECK(!pos::GetPubKeysFromCoinStakeTx(txRef, pubkeys, 999, gatedConsensus));
    BOOST_CHECK(pos::GetPubKeysFromCoinStakeTx(txRef, pubkeys, 1000, gatedConsensus));

    // 4. Taproot (P2TR) staking test
    CKey taprootKey;
    taprootKey.MakeNewKey(true);
    CPubKey taprootPubKey = taprootKey.GetPubKey();
    XOnlyPubKey xpubkey(taprootPubKey);

    CScript p2trScript = CScript() << OP_1 << ToByteVector(xpubkey);

    CMutableTransaction txTaprootStake;
    txTaprootStake.vin.resize(1);
    txTaprootStake.vout.resize(1);
    txTaprootStake.vout[0].scriptPubKey = p2trScript;
    txTaprootStake.vout[0].nValue = 1000 * COIN;

    std::vector<CPubKey> trPubkeys;
    CTransactionRef txTRRef = MakeTransactionRef(txTaprootStake);
    BOOST_CHECK(pos::GetPubKeysFromCoinStakeTx(txTRRef, trPubkeys));
    BOOST_REQUIRE_EQUAL(trPubkeys.size(), 1U);
    BOOST_CHECK(XOnlyPubKey(trPubkeys[0]) == xpubkey);
}

BOOST_AUTO_TEST_SUITE_END()
