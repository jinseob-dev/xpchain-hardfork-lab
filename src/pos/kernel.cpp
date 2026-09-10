// Copyright (c) 2018-2026 The XPChain Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/kernel.h>
#include <pos/height.h>

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <hash.h>
#include <script/interpreter.h>
#include <streams.h>
#include <uint256.h>
#include <util.h>
#include <validation.h>

#include <algorithm>
#include <vector>

namespace pos {

bool CheckStakeKernelHash(unsigned int nBits, uint32_t nTimeBlockFrom, unsigned int nTxPrevOffset, CAmount nAmount, uint64_t n, uint32_t nTimeTx, uint256& hashProofOfStake, const Consensus::Params& params)
{
    if (nTimeBlockFrom + params.nStakeMinAge > nTimeTx) // Min age requirement
        return false;

    arith_uint256 bnTargetPerCoinDay;
    bnTargetPerCoinDay.SetCompact(nBits);

    int64_t nTimeWeight = std::min((int64_t)nTimeTx - nTimeBlockFrom, params.nStakeMaxAge) - params.nStakeMinAge;

    arith_uint256 bnCoinDayWeight = arith_uint256(nAmount) * nTimeWeight / COIN / (24 * 60 * 60);

    // Calculate hash
    CDataStream ss(SER_GETHASH, 0);

    ss << nBits << nTimeBlockFrom << nTxPrevOffset << nTimeBlockFrom << n << nTimeTx;

    hashProofOfStake = Hash(ss.begin(), ss.end());

    if (arith_uint512(UintToArith256(hashProofOfStake)) > arith_uint512(bnCoinDayWeight) * arith_uint512(bnTargetPerCoinDay))
        return false;

    return true;
}

bool CheckStakeKernelHash(unsigned int nBits, uint32_t nTimeBlockFrom, unsigned int nTxPrevOffset, CAmount nAmount, uint64_t n, uint32_t nTimeTx, uint256& hashProofOfStake)
{
    return CheckStakeKernelHash(nBits, nTimeBlockFrom, nTxPrevOffset, nAmount, n, nTimeTx, hashProofOfStake, Params().GetConsensus());
}

bool CheckStakeKernelHash(unsigned int nBits, const CBlock& blockFrom, unsigned int nTxPrevOffset, const CTxOut& txOutPrev, const COutPoint& prevout, uint32_t nTimeTx, uint256& hashProofOfStake)
{
    return pos::CheckStakeKernelHash(nBits, blockFrom.GetBlockTime(), nTxPrevOffset, txOutPrev.nValue, prevout.n, nTimeTx, hashProofOfStake, Params().GetConsensus());
}

unsigned int GetCoinStakeScriptFlags(int nHeight, const Consensus::Params& params)
{
    unsigned int nFlags = SCRIPT_VERIFY_NONE;
    if (nHeight >= params.TaprootHeight) {
        nFlags |= SCRIPT_VERIFY_TAPROOT;
    }
    if (nHeight >= params.ColdStakingHeight) {
        nFlags |= SCRIPT_VERIFY_COLDSTAKE;
    }
    return nFlags;
}

bool CheckProofOfStakePure(const CTransaction& txCoinStake,
                           const CTransaction& txPrev,
                           unsigned int nBits,
                           uint32_t nTimeTx,
                           uint32_t nTimeBlockFrom,
                           unsigned int nTxPrevOffset,
                           int nHeight,
                           uint256& hashProofOfStake,
                           const Consensus::Params& params)
{
    if (txCoinStake.vin.empty()) {
        return false;
    }
    const CTxIn& txin = txCoinStake.vin[0];
    if (txin.prevout.n >= txPrev.vout.size()) {
        return false;
    }

    // Verify signature
    const unsigned int nFlags = GetCoinStakeScriptFlags(nHeight, params);

    std::vector<CTxOut> spent_outputs;
    spent_outputs.push_back(txPrev.vout[txin.prevout.n]);

    PrecomputedTransactionData txdata;
    if (spent_outputs.size() == txCoinStake.vin.size()) {
        txdata.Init(txCoinStake, std::move(spent_outputs));
    } else {
        txdata = PrecomputedTransactionData(txCoinStake);
    }

    if (!CScriptCheck(txPrev.vout[txin.prevout.n], txCoinStake, 0, nFlags, true, &txdata)()) {
        return error("%s: VerifySignature failed on coinstake %s\n", __func__, txCoinStake.GetHash().ToString());
    }

    return pos::CheckStakeKernelHash(nBits, nTimeBlockFrom, nTxPrevOffset, txPrev.vout[txin.prevout.n].nValue, txin.prevout.n, nTimeTx, hashProofOfStake, params);
}

bool CheckProofOfStake(const CTransactionRef& tx, unsigned int nBits, uint256& hashProofOfStake, unsigned int nBlockTime, int nHeight, const Consensus::Params& params)
{
    const CTxIn& txin = tx->vin[0];

    CTransactionRef txTmp;
    uint256 hash;

    if (!GetTransaction(txin.prevout.hash, txTmp, params, hash, true)) {
        return error("%s: txPrev not found hash = %s\n", __func__, txin.prevout.hash.ToString().c_str());
    }

    if (txTmp->GetHash() != txin.prevout.hash) {
        return false;
    }

    if (hash == uint256()) {
        return false;
    }

    // Get transaction index for the previous transaction
    auto itr = mapBlockIndex.find(hash);
    if (itr == mapBlockIndex.end()) {
        return error("%s: blockindex not found hash = %s\n", __func__, hash.ToString().c_str());
    }

    CBlockIndex* pindex = (*itr).second;
    if (pindex->GetBlockHash() != hash) {
        return false;
    }

    CBlock block;
    if (!ReadBlockFromDisk(block, pindex, params)) {
        return error("%s: block not found db hash = %s\n", __func__, hash.ToString().c_str());
    }

    return CheckProofOfStakePure(*tx, *txTmp, nBits, nBlockTime, block.GetBlockTime(), GetSizeOfCompactSize(block.vtx.size()) + sizeof(CBlockHeader), nHeight, hashProofOfStake, params);
}

} // namespace pos
