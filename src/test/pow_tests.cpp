// Copyright (c) 2015-2018 The Bitcoin Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <pow.h>
#include <random.h>
#include <util.h>
#include <test/test_xpchain.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(pow_tests, BasicTestingSetup)

/*

    Current Mainnet Difficulty is temporary value.
    It is restore old value when mainnet start.

    serisia

*/

/* Test calculation of next difficulty target with no constraints applying */

//TODO:new value for xpc

//BOOST_AUTO_TEST_CASE(get_next_work)
//{
//    const auto chainParams = CreateChainParams(CBaseChainParams::MAIN);
//    int64_t nLastRetargetTime = 1261130161; // Block #30240
//    CBlockIndex pindexLast;
//    pindexLast.nHeight = 32255;
//    pindexLast.nTime = 1262152739;  // Block #32255
//    pindexLast.nBits = 0x1d00ffff;
//   BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), 0x1d00d86aU);
//}

/* Test the constraint on the upper bound for next work */
BOOST_AUTO_TEST_CASE(get_next_work_pow_limit)
{
    const auto chainParams = CreateChainParams(CBaseChainParams::MAIN);
    int64_t nLastRetargetTime = 1231006505; // Block #0
    CBlockIndex pindexLast;
    pindexLast.nHeight = 2015;
    pindexLast.nTime = 1233061996;  // Block #2015
    pindexLast.nBits = 0x1d00ffff;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), 0x1d00ffffU);
}

BOOST_AUTO_TEST_CASE(testnet_genesis_uses_easy_pow_limit)
{
    const auto mainParams = CreateChainParams(CBaseChainParams::MAIN);
    const auto testParams = CreateChainParams(CBaseChainParams::TESTNET);
    const auto regtestParams = CreateChainParams(CBaseChainParams::REGTEST);

    BOOST_CHECK(testParams->GenesisBlock().GetHash() != mainParams->GenesisBlock().GetHash());
    BOOST_CHECK(testParams->GenesisBlock().GetHash() != regtestParams->GenesisBlock().GetHash());
    BOOST_CHECK_EQUAL(testParams->GenesisBlock().GetHash().GetHex(), "17e8ac05fcd037f9b1fe25da878bc6b8e1ebca64083a7fe3168bf509a74a53be");
    BOOST_CHECK_EQUAL(testParams->MessageStart()[0], 0xfa);
    BOOST_CHECK_EQUAL(testParams->MessageStart()[1], 0xbf);
    BOOST_CHECK_EQUAL(testParams->MessageStart()[2], 0xb5);
    BOOST_CHECK_EQUAL(testParams->MessageStart()[3], 0xda);
    BOOST_REQUIRE_EQUAL(testParams->DNSSeeds().size(), 2U);
    BOOST_CHECK_EQUAL(testParams->DNSSeeds()[0], "158.179.20.33");
    BOOST_CHECK_EQUAL(testParams->DNSSeeds()[1], "207.211.156.219");

    CBlockIndex previous;
    previous.nHeight = 0;
    previous.nTime = testParams->GenesisBlock().nTime;
    previous.nBits = testParams->GenesisBlock().nBits;

    CBlockHeader candidate;
    candidate.nTime = previous.nTime + 1;

    const unsigned int expected =
        UintToArith256(testParams->GetConsensus().powLimit).GetCompact();
    BOOST_CHECK_EQUAL(
        GetNextWorkRequired(&previous, &candidate, testParams->GetConsensus()),
        expected);
    BOOST_CHECK_EQUAL(previous.nBits, expected);
}

BOOST_AUTO_TEST_CASE(testnet_pow_to_pos_transition_uses_validation_difficulty)
{
    const auto testParams = CreateChainParams(CBaseChainParams::TESTNET);
    const Consensus::Params& consensus = testParams->GetConsensus();
    const unsigned int powLimit = UintToArith256(consensus.powLimit).GetCompact();

    CBlockIndex beforeSwitch;
    beforeSwitch.nHeight = consensus.nSwitchHeight - 1;
    beforeSwitch.nTime = testParams->GenesisBlock().nTime + beforeSwitch.nHeight;
    beforeSwitch.nBits = powLimit;

    CBlockIndex switchBlock;
    switchBlock.pprev = &beforeSwitch;
    switchBlock.nHeight = consensus.nSwitchHeight;
    switchBlock.nTime = beforeSwitch.nTime + 1;
    switchBlock.nBits = powLimit;

    CBlockHeader firstPoS;
    firstPoS.nTime = switchBlock.nTime + consensus.nPowTargetSpacing * 2 + 1;

    // The first PoS candidate is validated while its previous index is still a
    // PoW-height index. Its producer must therefore use GetNextWorkRequired,
    // including the testnet delayed-block rule, rather than call the retarget
    // calculation directly.
    BOOST_CHECK_EQUAL(GetNextWorkRequired(&switchBlock, &firstPoS, consensus), powLimit);
    BOOST_CHECK_NE(
        CalculateNextWorkRequired(&switchBlock, beforeSwitch.GetBlockTime(), consensus),
        GetNextWorkRequired(&switchBlock, &firstPoS, consensus));
}

/* Test the constraint on the lower bound for actual time taken */
//BOOST_AUTO_TEST_CASE(get_next_work_lower_limit_actual)
//{
//    const auto chainParams = CreateChainParams(CBaseChainParams::MAIN);
//    int64_t nLastRetargetTime = 1279008237; // Block #66528
//    CBlockIndex pindexLast;
//    pindexLast.nHeight = 68543;
//    pindexLast.nTime = 1279297671;  // Block #68543
//    pindexLast.nBits = 0x1c05a3f4;
//    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), 0x1c0168fdU);
//}

/* Test the constraint on the upper bound for actual time taken */
//BOOST_AUTO_TEST_CASE(get_next_work_upper_limit_actual)
//{
//    const auto chainParams = CreateChainParams(CBaseChainParams::MAIN);
//    int64_t nLastRetargetTime = 1263163443; // NOTE: Not an actual block time
//    CBlockIndex pindexLast;
//    pindexLast.nHeight = 46367;
//    pindexLast.nTime = 1269211443;  // Block #46367
//    pindexLast.nBits = 0x1c387f6f;
//    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), 0x1d00e1fdU);
//}

BOOST_AUTO_TEST_CASE(GetBlockProofEquivalentTime_test)
{
    const auto chainParams = CreateChainParams(CBaseChainParams::MAIN);
    std::vector<CBlockIndex> blocks(10000);
    for (int i = 0; i < 10000; i++) {
        blocks[i].pprev = i ? &blocks[i - 1] : nullptr;
        blocks[i].nHeight = i;
        blocks[i].nTime = 1269211443 + i * chainParams->GetConsensus().nPowTargetSpacing;
        blocks[i].nBits = 0x207fffff; /* target 0x7fffff000... */
        blocks[i].nChainWork = i ? blocks[i - 1].nChainWork + GetBlockProof(blocks[i - 1]) : arith_uint256(0);
    }

    for (int j = 0; j < 1000; j++) {
        CBlockIndex *p1 = &blocks[InsecureRandRange(10000)];
        CBlockIndex *p2 = &blocks[InsecureRandRange(10000)];
        CBlockIndex *p3 = &blocks[InsecureRandRange(10000)];

        int64_t tdiff = GetBlockProofEquivalentTime(*p1, *p2, *p3, chainParams->GetConsensus());
        BOOST_CHECK_EQUAL(tdiff, p1->GetBlockTime() - p2->GetBlockTime());
    }
}

BOOST_AUTO_TEST_SUITE_END()
