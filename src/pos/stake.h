// Copyright (c) 2018-2026 The XPChain Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef XPCHAIN_POS_STAKE_H
#define XPCHAIN_POS_STAKE_H

#include <consensus/params.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/script.h>
#include <uint256.h>

#include <vector>

namespace pos {

bool IsCoinStakeTx(CTransactionRef tx, const Consensus::Params &consensusParams, uint256 &hashBlock, CTransactionRef& prevTx);
bool IsDestinationSame(const CScript& prevTxOut, const CScript& coinStakeTxOut);

/** Cold Staking script helper functions */
CScript CreateColdStakingScript(const CKeyID& stakingKeyId, const CKeyID& ownerKeyId);
bool IsColdStakingScript(const CScript& script, CKeyID& stakingKeyId, CKeyID& ownerKeyId);

bool GetPubKeysFromCoinStakeTx(const CTransactionRef& txCoinStake, std::vector<CPubKey>& vPubKeys);
bool MakeBlockHashExcludedSignature(const CBlock& block, uint256& hashBlock, std::vector<unsigned char>& sig);
bool CheckBlockSignature(const CBlock& block, const Consensus::Params& consensusParams);

} // namespace pos

#endif // XPCHAIN_POS_STAKE_H
