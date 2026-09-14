// Copyright (c) 2026 The XPChain Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <consensus/params.h>
#include <warnings.h>

#include <boost/test/unit_test.hpp>

#include <vector>

BOOST_AUTO_TEST_SUITE(warnings_tests)

BOOST_AUTO_TEST_CASE(known_stale_retarget_fork)
{
    Consensus::Params params{};
    params.PoSRetargetFixHeight = 451;

    std::vector<uint256> hashes(4);
    hashes[0] = uint256S("0x450");
    hashes[1] = uint256S("0xae20ad929456b8336fd22840ca970aec6738ce3ea74c24ecac93d171f613bf52");
    hashes[2] = uint256S("0x452");
    hashes[3] = uint256S("0x453");
    params.PoSRetargetLegacyForkBlock = hashes[1];

    std::vector<CBlockIndex> fork(4);
    for (size_t i = 0; i < fork.size(); ++i) {
        fork[i].nHeight = 450 + i;
        fork[i].pprev = i == 0 ? nullptr : &fork[i - 1];
        fork[i].phashBlock = &hashes[i];
    }

    BOOST_CHECK(!IsKnownStaleRetargetFork(nullptr, 677, params));
    BOOST_CHECK(!IsKnownStaleRetargetFork(&fork.back(), 524, params));
    BOOST_CHECK(IsKnownStaleRetargetFork(&fork.back(), 525, params));

    params.PoSRetargetLegacyForkBlock = uint256S("0x01");
    BOOST_CHECK(!IsKnownStaleRetargetFork(&fork.back(), 677, params));

    params.PoSRetargetLegacyForkBlock.SetNull();
    BOOST_CHECK(!IsKnownStaleRetargetFork(&fork.back(), 677, params));
}

BOOST_AUTO_TEST_SUITE_END()
