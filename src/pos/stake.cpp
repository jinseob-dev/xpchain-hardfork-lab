// Copyright (c) 2018-2026 The XPChain Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/stake.h>

#include <consensus/merkle.h>
#include <hash.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/interpreter.h>
#include <script/standard.h>
#include <util.h>
#include <utilstrencodings.h>
#include <validation.h>

#include <vector>

namespace pos {

bool IsDestinationSame(const CScript& a, const CScript& b)
{
    txnouttype aType, bType;
    std::vector<std::vector<unsigned char>> aSol, bSol;

    if (!Solver(a, aType, aSol) || !Solver(b, bType, bSol)) {
        return false;
    }

    if (aSol.size() != 1 || bSol.size() != 1) {
        return false;
    }

    CTxDestination aDest, bDest;
    if (!ExtractDestination(a, aDest) || !ExtractDestination(b, bDest)) {
        return false;
    }

    if (!(aDest == bDest)) {
        return false;
    }

    return true;
}

bool IsCoinStakeTx(CTransactionRef tx, const Consensus::Params &consensusParams, uint256 &hashBlock,
                   CTransactionRef& prevTx)
{
    if (tx->vin.size() != 1) {
        return error("%s: coinstake has too many inputs", __func__);
    }
    if (tx->vout.size() != 1) {
        return error("%s: coinstake has too many outputs", __func__);
    }

    if (!GetTransaction(tx->vin[0].prevout.hash, prevTx, consensusParams, hashBlock, true)) {
        return error("%s: unknown coinstake input", __func__);
    }

    if (prevTx->GetHash() != tx->vin[0].prevout.hash) {
        return error("%s: invalid coinstake input hash", __func__);
    }

    if (!pos::IsDestinationSame(prevTx->vout[tx->vin[0].prevout.n].scriptPubKey, tx->vout[0].scriptPubKey)) {
        return error("%s: invalid coinstake output", __func__);
    }

    return true;
}

CScript CreateColdStakingScript(const CKeyID& stakingKeyId, const CKeyID& ownerKeyId)
{
    CScript script;
    script << OP_IF
           << OP_DUP << OP_HASH160 << ToByteVector(stakingKeyId) << OP_EQUALVERIFY
           << OP_CHECKCOLDSTAKEVERIFY << OP_CHECKSIG
           << OP_ELSE
           << OP_DUP << OP_HASH160 << ToByteVector(ownerKeyId) << OP_EQUALVERIFY << OP_CHECKSIG
           << OP_ENDIF;
    return script;
}

bool IsColdStakingScript(const CScript& script, CKeyID& stakingKeyId, CKeyID& ownerKeyId)
{
    return MatchColdStakingScript(script, stakingKeyId, ownerKeyId);
}

bool IsColdStakingCoinStake(const CTransactionRef& txCoinStake, CKeyID& stakingKeyId, CKeyID& ownerKeyId)
{
    if (!txCoinStake || txCoinStake->vin.empty() || txCoinStake->vout.empty() ||
        txCoinStake->vin[0].scriptWitness.stack.size() < 2) {
        return false;
    }

    // Only the delegated staking branch is a coinstake. The empty selector is
    // the owner's withdrawal branch and must never receive a consensus minting
    // allowance.
    const auto& witnessStack = txCoinStake->vin[0].scriptWitness.stack;
    if (witnessStack[witnessStack.size() - 2] != std::vector<unsigned char>{0x01}) {
        return false;
    }

    txnouttype type;
    std::vector<std::vector<unsigned char>> solutions;
    if (!Solver(txCoinStake->vout[0].scriptPubKey, type, solutions) ||
        type != TX_WITNESS_V0_SCRIPTHASH || solutions.size() != 1) {
        return false;
    }

    const std::vector<unsigned char>& rawScript = witnessStack.back();
    const CScript witnessScript(rawScript.begin(), rawScript.end());
    if (!IsColdStakingScript(witnessScript, stakingKeyId, ownerKeyId)) {
        return false;
    }

    uint256 scriptHash;
    CSHA256().Write(witnessScript.data(), witnessScript.size()).Finalize(scriptHash.begin());
    return solutions[0] == ToByteVector(scriptHash);
}

bool CheckColdStakingRewardOutputs(const CTransactionRef& txCoinStake,
                                   const std::vector<std::pair<CScript, CAmount>>& rewardValues,
                                   int nHeight, const Consensus::Params& consensusParams)
{
    if (nHeight < consensusParams.ColdStakingHeight) {
        return true;
    }

    CKeyID stakingKey, ownerKey;
    if (!IsColdStakingCoinStake(txCoinStake, stakingKey, ownerKey)) {
        return true;
    }

    // V2 compounds the entire reward into the coinstake output. No separate
    // coinbase reward output, including a zero-valued placeholder, is allowed.
    if (nHeight >= consensusParams.ColdStakingCompoundHeight) {
        return rewardValues.empty();
    }

    // Cold-staking v1 has no operator commission: every reward output must
    // remain under the delegator's original contract. A future commission
    // model requires a distinct script version and activation rule.
    for (const auto& reward : rewardValues) {
        if (reward.first != txCoinStake->vout[0].scriptPubKey) {
            return false;
        }
    }
    return true;
}

static bool GetPubKeyFromScript(CScript scriptPubKey, const CTxIn& txIn, std::vector<CPubKey>& vPubKey,
                                bool allowColdStaking, bool allowTaproot, int depth = 0)
{
    assert(depth <= 2);

    // Check if this script itself is a cold staking redeem script
    CKeyID stakingKeyId, ownerKeyId;
    if (allowColdStaking && IsColdStakingScript(scriptPubKey, stakingKeyId, ownerKeyId)) {
        // Block is staked and signed by the delegate staker key.
        // Check witness stack (for P2WSH cold staking)
        for (const auto& item : txIn.scriptWitness.stack) {
            CPubKey pk(item.begin(), item.end());
            if (pk.IsValid() && pk.GetID() == stakingKeyId) {
                vPubKey.push_back(pk);
                return true;
            }
        }
        // Fallback: check scriptSig stack (for P2SH cold staking)
        std::vector<std::vector<unsigned char>> stack;
        if (EvalScript(stack, txIn.scriptSig, SCRIPT_VERIFY_NONE, BaseSignatureChecker(), SigVersion::BASE)) {
            for (const auto& item : stack) {
                CPubKey pk(item.begin(), item.end());
                if (pk.IsValid() && pk.GetID() == stakingKeyId) {
                    vPubKey.push_back(pk);
                    return true;
                }
            }
        }
        return false;
    }

    txnouttype type;
    std::vector<std::vector<unsigned char>> vSolutions;
    if (!Solver(scriptPubKey, type, vSolutions)) {
        return false;
    }
    vPubKey.clear();
    switch (type) {
        case TX_PUBKEY:
            vPubKey.push_back(CPubKey(vSolutions[0].begin(), vSolutions[0].end()));
            break;
        case TX_MULTISIG:
            for (auto itr = vSolutions.begin() + 1; itr != vSolutions.end() - 1; itr++) {
                vPubKey.push_back(CPubKey(itr->begin(), itr->end()));
            }
            break;
        case TX_PUBKEYHASH:
        {
            std::vector<std::vector<unsigned char>> stack;
            if (!EvalScript(stack, txIn.scriptSig, SCRIPT_VERIFY_NONE, BaseSignatureChecker(), SigVersion::BASE)) {
                return false;
            }
            vPubKey.push_back(CPubKey(stack.back().begin(), stack.back().end()));
        }
        break;
        case TX_WITNESS_V0_KEYHASH:
            vPubKey.push_back(CPubKey(txIn.scriptWitness.stack.back().begin(), txIn.scriptWitness.stack.back().end()));
            break;
        case TX_WITNESS_V1_TAPROOT:
        {
            if (!allowTaproot) {
                return false;
            }
            if (vSolutions.empty() || vSolutions[0].size() != 32) {
                return false;
            }
            XOnlyPubKey xpubkey(vSolutions[0]);
            CPubKey pubkey = xpubkey.GetCorrespondingPubKey();
            if (!pubkey.IsValid()) {
                return false;
            }
            vPubKey.push_back(pubkey);
        }
        break;
        case TX_SCRIPTHASH:
        {
            std::vector<std::vector<unsigned char>> stack;
            if (!EvalScript(stack, txIn.scriptSig, SCRIPT_VERIFY_NONE, BaseSignatureChecker(), SigVersion::BASE)) {
                return false;
            }
            CScript redeemScript;
            redeemScript = CScript(stack.back().begin(), stack.back().end());
            if (!GetPubKeyFromScript(redeemScript, txIn, vPubKey, allowColdStaking, allowTaproot, depth + 1)) {
                return false;
            }
        }
        break;
        case TX_WITNESS_V0_SCRIPTHASH:
        {
            std::vector<std::vector<unsigned char>> stack;
            stack = txIn.scriptWitness.stack;
            if (stack.empty()) {
                return false;
            }
            CScript redeemScript;
            redeemScript = CScript(stack.back().begin(), stack.back().end());
            if (!GetPubKeyFromScript(redeemScript, txIn, vPubKey, allowColdStaking, allowTaproot, depth + 1)) {
                return false;
            }
        }
        break;
        default:
            return false;
    }
    return true;
}

bool GetPubKeysFromCoinStakeTx(const CTransactionRef& txCoinStake, std::vector<CPubKey>& vPubKeys)
{
    if (!GetPubKeyFromScript(txCoinStake->vout[0].scriptPubKey, txCoinStake->vin[0], vPubKeys, true, true)) {
        return false;
    }

    for (const CPubKey& v_pub_key : vPubKeys) {
        if (!v_pub_key.IsValid())
            return false;
    }

    return true;
}

bool GetPubKeysFromCoinStakeTx(const CTransactionRef& txCoinStake, std::vector<CPubKey>& vPubKeys,
                               int nHeight, const Consensus::Params& consensusParams)
{
    if (txCoinStake->vin.empty() || txCoinStake->vout.empty()) {
        return false;
    }
    if (!GetPubKeyFromScript(txCoinStake->vout[0].scriptPubKey, txCoinStake->vin[0], vPubKeys,
                             nHeight >= consensusParams.ColdStakingHeight,
                             nHeight >= consensusParams.TaprootHeight)) {
        return false;
    }
    for (const CPubKey& pubkey : vPubKeys) {
        if (!pubkey.IsValid()) return false;
    }
    return true;
}

bool MakeBlockHashExcludedSignature(const CBlock& block, uint256& hashBlock, std::vector<unsigned char>& sig)
{
    const CScript& scriptSig = block.vtx[0]->vin[0].scriptSig;

    auto itr = scriptSig.begin();
    opcodetype op;
    // get last element
    while (GetScriptOp(itr, scriptSig.end(), op, &sig)) {
        if (itr == scriptSig.end()) {
            break;
        }
    }

    if (op >= OP_PUSHDATA1) {
        return error("MakeBlockHashExcludedSignature(): the last element of scriptSig is not signature");
    }

    CMutableTransaction txCoinBase(*block.vtx[0]);
    txCoinBase.vin[0].scriptSig = CScript(scriptSig.begin(), scriptSig.end() - (op + 1));
    CBlock cpBlock = block;
    cpBlock.vtx[0] = MakeTransactionRef(std::move(txCoinBase));
    cpBlock.hashMerkleRoot = BlockMerkleRoot(cpBlock);

    hashBlock = cpBlock.GetBlockHeader().GetHash();

    return true;
}

bool CheckBlockSignature(const CBlock& block, int nHeight, const Consensus::Params& consensusParams)
{
    std::vector<CPubKey> pubkeys;
    if (!GetPubKeysFromCoinStakeTx(block.vtx[1], pubkeys, nHeight, consensusParams)) {
        return error("CheckBlockSignature(): could not get the public key");
    }
    uint256 hashBlock;
    std::vector<unsigned char> signature;
    if (!MakeBlockHashExcludedSignature(block, hashBlock, signature)) {
        return error("CheckBlockSignature(): could not get the signature and hashblock");
    }
    for (const CPubKey& pubkey : pubkeys) {
        if (pubkey.Verify(hashBlock, signature))
            return true;
        // BIP340 Schnorr signature support for Taproot block staking
        if (signature.size() == 64) {
            XOnlyPubKey xpk(pubkey);
            if (xpk.VerifySchnorr(hashBlock, signature))
                return true;
        }
    }
    return error("CheckBlockSignature(): Verify Failed signature = %s, hashblock = %s", HexStr(signature), HexStr(hashBlock));
}

} // namespace pos
