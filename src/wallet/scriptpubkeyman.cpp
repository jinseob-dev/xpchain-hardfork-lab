// Copyright (c) 2019-2020 The XPChain Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// XPChain adaptation — uses XPChain's CCryptoKeyStore/CBasicKeyStore hierarchy
// and txnouttype (not XPChain Core v0.21's TxoutType enum class).

#include <keystore.h>
#include <key_io.h>
#include <outputtype.h>
#include <script/descriptor.h>
#include <script/ismine.h>
#include <script/sign.h>
#include <script/standard.h>
#include <util.h>
#include <util/bip32.h>
#include <utilstrencodings.h>
#include <wallet/crypter.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/walletdb.h>
#include <wallet/walletutil.h>

#include <sstream>
#include <string>


// Local helper: extract pubkey from a P2PK script (mirrors keystore.cpp's static ExtractPubKey)
static bool ExtractPubKey(const CScript& dest, CPubKey& pubKeyOut)
{
    std::vector<std::vector<unsigned char>> solutions;
    txnouttype type;
    if (!Solver(dest, type, solutions)) return false;
    if (type == TX_PUBKEY && solutions.size() == 1) {
        pubKeyOut = CPubKey(solutions[0]);
        return pubKeyOut.IsFullyValid();
    }
    return false;
}


//! Value for the first BIP32 hardened derivation. Can be used as a bit mask.
const uint32_t BIP32_HARDENED_KEY_LIMIT = 0x80000000;

// ─── GetAffectedKeys ─────────────────────────────────────────────────────────

std::vector<CKeyID> GetAffectedKeys(const CScript& spk, const SigningProvider& provider)
{
    std::vector<std::vector<unsigned char>> solutions;
    txnouttype type;
    Solver(spk, type, solutions);

    std::vector<CKeyID> keyids;
    if (type == TX_PUBKEY) {
        if (!solutions.empty()) {
            CPubKey pk(solutions[0]);
            if (pk.IsValid()) keyids.push_back(pk.GetID());
        }
    } else if (type == TX_PUBKEYHASH) {
        if (!solutions.empty()) {
            keyids.push_back(CKeyID(uint160(solutions[0])));
        }
    } else if (type == TX_MULTISIG) {
        for (size_t i = 1; i + 1 < solutions.size(); ++i) {
            CPubKey pk(solutions[i]);
            if (pk.IsValid()) keyids.push_back(pk.GetID());
        }
    }
    return keyids;
}

// ─── IsMine helpers ───────────────────────────────────────────────────────────

namespace {

enum class IsMineSigVersion { TOP = 0, P2SH = 1, WITNESS_V0 = 2 };
enum class IsMineResult   { NO = 0, WATCH_ONLY = 1, SPENDABLE = 2, INVALID = 3 };

bool PermitsUncompressed(IsMineSigVersion sv)
{
    return sv == IsMineSigVersion::TOP || sv == IsMineSigVersion::P2SH;
}

bool HaveKeys(const std::vector<std::vector<unsigned char>>& pubkeys,
              const LegacyScriptPubKeyMan& ks)
{
    for (const auto& pk : pubkeys) {
        if (!ks.HaveKey(CPubKey(pk).GetID())) return false;
    }
    return true;
}

IsMineResult IsMineInner(const LegacyScriptPubKeyMan& ks,
                          const CScript& scriptPubKey,
                          IsMineSigVersion sv,
                          bool recurse_scripthash = true)
{
    IsMineResult ret = IsMineResult::NO;

    std::vector<std::vector<unsigned char>> vSols;
    txnouttype type;
    Solver(scriptPubKey, type, vSols);

    CKeyID keyID;
    switch (type) {
    case TX_NONSTANDARD:
    case TX_NULL_DATA:
    case TX_WITNESS_UNKNOWN:
        break;

    case TX_WITNESS_V1_TAPROOT:
    {
        if (sv != IsMineSigVersion::TOP) return IsMineResult::INVALID;
        // Construct the 33-byte compressed pubkey (X-only + parity bit)
        // Try both parity 0x02 and 0x03
        unsigned char vch[33];
        memcpy(vch + 1, vSols[0].data(), 32);

        vch[0] = 0x02;
        if (ks.HaveKey(CPubKey(vch, vch + 33).GetID())) {
            ret = std::max(ret, IsMineResult::SPENDABLE);
        } else {
            vch[0] = 0x03;
            if (ks.HaveKey(CPubKey(vch, vch + 33).GetID())) {
                ret = std::max(ret, IsMineResult::SPENDABLE);
            }
        }
        break;
    }

    case TX_COLDSTAKE:
        if (sv != IsMineSigVersion::WITNESS_V0) return IsMineResult::INVALID;
        if (ks.HaveKey(CKeyID(uint160(vSols[0]))) ||
            ks.HaveKey(CKeyID(uint160(vSols[1])))) {
            ret = std::max(ret, IsMineResult::SPENDABLE);
        }
        break;

    case TX_PUBKEY:
        keyID = CPubKey(vSols[0]).GetID();
        if (!PermitsUncompressed(sv) && vSols[0].size() != 33)
            return IsMineResult::INVALID;
        if (ks.HaveKey(keyID))
            ret = std::max(ret, IsMineResult::SPENDABLE);
        break;

    case TX_WITNESS_V0_KEYHASH: {
        if (sv == IsMineSigVersion::WITNESS_V0)  return IsMineResult::INVALID;
        if (sv == IsMineSigVersion::TOP &&
            !ks.HaveCScript(CScriptID(CScript() << OP_0 << vSols[0])))
            break;
        ret = std::max(ret, IsMineInner(
            ks, GetScriptForDestination(WitnessV0KeyHash(uint160(vSols[0]))),
            IsMineSigVersion::WITNESS_V0));
        break;
    }

    case TX_PUBKEYHASH:
        keyID = CKeyID(uint160(vSols[0]));
        if (!PermitsUncompressed(sv)) {
            CPubKey pk;
            if (ks.GetPubKey(keyID, pk) && !pk.IsCompressed())
                return IsMineResult::INVALID;
        }
        if (ks.HaveKey(keyID))
            ret = std::max(ret, IsMineResult::SPENDABLE);
        break;

    case TX_SCRIPTHASH: {
        if (sv != IsMineSigVersion::TOP) return IsMineResult::INVALID;
        CScriptID scriptID = CScriptID(uint160(vSols[0]));
        CScript subscript;
        if (ks.GetCScript(scriptID, subscript))
            ret = std::max(ret,
                recurse_scripthash
                    ? IsMineInner(ks, subscript, IsMineSigVersion::P2SH)
                    : IsMineResult::SPENDABLE);
        break;
    }

    case TX_WITNESS_V0_SCRIPTHASH: {
        if (sv == IsMineSigVersion::WITNESS_V0) return IsMineResult::INVALID;
        if (sv == IsMineSigVersion::TOP &&
            !ks.HaveCScript(CScriptID(CScript() << OP_0 << vSols[0])))
            break;
        uint160 hash;
        CRIPEMD160().Write(&vSols[0][0], vSols[0].size()).Finalize(hash.begin());
        CScriptID scriptID = CScriptID(hash);
        CScript subscript;
        if (ks.GetCScript(scriptID, subscript))
            ret = std::max(ret,
                recurse_scripthash
                    ? IsMineInner(ks, subscript, IsMineSigVersion::WITNESS_V0)
                    : IsMineResult::SPENDABLE);
        break;
    }

    case TX_MULTISIG: {
        if (sv == IsMineSigVersion::TOP) break;
        std::vector<std::vector<unsigned char>> keys(vSols.begin() + 1, vSols.end() - 1);
        if (!PermitsUncompressed(sv)) {
            for (const auto& k : keys)
                if (k.size() != 33) return IsMineResult::INVALID;
        }
        if (HaveKeys(keys, ks))
            ret = std::max(ret, IsMineResult::SPENDABLE);
        break;
    }

    default:
        break;
    }

    if (ret == IsMineResult::NO && ks.HaveWatchOnly(scriptPubKey))
        ret = std::max(ret, IsMineResult::WATCH_ONLY);

    return ret;
}
} // namespace

// ─── isminetype IsMine ────────────────────────────────────────────────────────

isminetype LegacyScriptPubKeyMan::IsMine(const CScript& script) const
{
    switch (IsMineInner(*this, script, IsMineSigVersion::TOP)) {
    case IsMineResult::INVALID:
    case IsMineResult::NO:        return ISMINE_NO;
    case IsMineResult::WATCH_ONLY:return ISMINE_WATCH_ONLY;
    case IsMineResult::SPENDABLE: return ISMINE_SPENDABLE;
    }
    assert(false);
}

// ─── Encryption ───────────────────────────────────────────────────────────────

bool LegacyScriptPubKeyMan::CheckDecryptionKey(const CKeyingMaterial& master_key, bool accept_no_keys)
{
    LOCK(cs_KeyStore);
    assert(mapKeys.empty());

    bool keyPass = mapCryptedKeys.empty();
    bool keyFail = false;
    CryptedKeyMap::const_iterator mi = mapCryptedKeys.begin();
    WalletBatch batch(m_storage.GetDatabase());
    for (; mi != mapCryptedKeys.end(); ++mi) {
        const CPubKey& vchPubKey = mi->second.first;
        const std::vector<unsigned char>& vchCryptedSecret = mi->second.second;
        CKey key;
        if (!DecryptKey(master_key, vchCryptedSecret, vchPubKey, key)) {
            keyFail = true;
            break;
        }
        keyPass = true;
        if (fDecryptionThoroughlyChecked) break;
        else {
            batch.WriteCryptedKey(vchPubKey, vchCryptedSecret, mapKeyMetadata[vchPubKey.GetID()]);
        }
    }
    if (keyPass && keyFail) {
        LogPrintf("The wallet is probably corrupted: Some keys decrypt but not all.\n");
        throw std::runtime_error("Error unlocking wallet: some keys decrypt but not all. Wallet may be corrupt.");
    }
    if (keyFail || (!keyPass && !accept_no_keys)) return false;
    fDecryptionThoroughlyChecked = true;
    return true;
}

bool LegacyScriptPubKeyMan::Encrypt(const CKeyingMaterial& master_key, WalletBatch* batch)
{
    LOCK(cs_KeyStore);
    encrypted_batch = batch;
    if (!mapCryptedKeys.empty()) { encrypted_batch = nullptr; return false; }

    KeyMap keys_to_encrypt;
    keys_to_encrypt.swap(mapKeys);
    for (const auto& mKey : keys_to_encrypt) {
        const CKey& key = mKey.second;
        CPubKey vchPubKey = key.GetPubKey();
        CKeyingMaterial vchSecret(key.begin(), key.end());
        std::vector<unsigned char> vchCryptedSecret;
        if (!EncryptSecret(master_key, vchSecret, vchPubKey.GetHash(), vchCryptedSecret)) {
            encrypted_batch = nullptr; return false;
        }
        if (!AddCryptedKey(vchPubKey, vchCryptedSecret)) {
            encrypted_batch = nullptr; return false;
        }
    }
    encrypted_batch = nullptr;
    return true;
}

// ─── Address pool ─────────────────────────────────────────────────────────────

bool LegacyScriptPubKeyMan::GetNewDestination(const OutputType type, CTxDestination& dest, std::string& error)
{
    LOCK(cs_KeyStore);
    error.clear();
    CPubKey new_key;
    if (!GetKeyFromPool(new_key, type)) {
        error = _("Error: Keypool ran out, please call keypoolrefill first");
        return false;
    }
    LearnRelatedScripts(new_key, type);
    dest = GetDestinationForKey(new_key, type);
    return true;
}

bool LegacyScriptPubKeyMan::GetReservedDestination(const OutputType type, bool internal,
    CTxDestination& address, int64_t& index, CKeyPool& keypool)
{
    LOCK(cs_KeyStore);
    if (!CanGetAddresses(internal)) return false;
    if (!ReserveKeyFromKeyPool(index, keypool, internal)) return false;
    address = GetDestinationForKey(keypool.vchPubKey, type);
    return true;
}

bool LegacyScriptPubKeyMan::TopUpInactiveHDChain(const CKeyID seed_id, int64_t index, bool internal)
{
    LOCK(cs_KeyStore);
    if (m_storage.IsLocked()) return false;
    auto it = m_inactive_hd_chains.find(seed_id);
    if (it == m_inactive_hd_chains.end()) return false;
    CHDChain& chain = it->second;

    int64_t target_size = std::max(gArgs.GetArg("-keypool", (int64_t)DEFAULT_KEYPOOL_SIZE), (int64_t)1);
    int64_t kp_size = (internal ? chain.nInternalChainCounter : chain.nExternalChainCounter) - (index + 1);
    int64_t missing = std::max(target_size - kp_size, (int64_t)0);

    if (missing > 0) {
        WalletBatch batch(m_storage.GetDatabase());
        for (int64_t i = missing; i > 0; --i) GenerateNewKey(batch, chain, internal);
        WalletLogPrintf("inactive seed id %s added %d %s keys\n",
                        HexStr(seed_id), missing, internal ? "internal" : "");
    }
    return true;
}

void LegacyScriptPubKeyMan::MarkUnusedAddresses(const CScript& script)
{
    LOCK(cs_KeyStore);
    for (const auto& keyid : GetAffectedKeys(script, *this)) {
        auto mi = m_pool_key_to_index.find(keyid);
        if (mi != m_pool_key_to_index.end()) {
            WalletLogPrintf("%s: Detected a used keypool key, marking keys as used\n", __func__);
            MarkReserveKeysAsUsed(mi->second);
            if (!TopUp()) WalletLogPrintf("%s: Topping up keypool failed (locked wallet)\n", __func__);
        }
        auto it = mapKeyMetadata.find(keyid);
        if (it != mapKeyMetadata.end()) {
            CKeyMetadata meta = it->second;
            if (!meta.hd_seed_id.IsNull() && meta.hd_seed_id != m_hd_chain.seed_id) {
                bool internal_chain = (meta.key_origin.path[1] & ~BIP32_HARDENED_KEY_LIMIT) != 0;
                int64_t idx = meta.key_origin.path[2] & ~BIP32_HARDENED_KEY_LIMIT;
                if (!TopUpInactiveHDChain(meta.hd_seed_id, idx, internal_chain))
                    WalletLogPrintf("%s: Adding inactive seed keys failed\n", __func__);
            }
        }
    }
}

void LegacyScriptPubKeyMan::UpgradeKeyMetadata()
{
    LOCK(cs_KeyStore);
    if (m_storage.IsLocked() || m_storage.IsWalletFlagSet(WALLET_FLAG_KEY_ORIGIN_METADATA)) return;

    auto batch = std::unique_ptr<WalletBatch>(new WalletBatch(m_storage.GetDatabase()));
    for (auto& meta_pair : mapKeyMetadata) {
        CKeyMetadata& meta = meta_pair.second;
        if (!meta.hd_seed_id.IsNull() && !meta.has_key_origin && meta.hdKeypath != "s") {
            CKey key;
            GetKey(meta.hd_seed_id, key);
            CExtKey masterKey;
            masterKey.SetSeed(key.begin(), key.size());
            CKeyID master_id = masterKey.key.GetPubKey().GetID();
            std::copy(master_id.begin(), master_id.begin() + 4, meta.key_origin.fingerprint);
            if (!ParseHDKeypath(meta.hdKeypath, meta.key_origin.path))
                throw std::runtime_error("Invalid stored hdKeypath");
            meta.has_key_origin = true;
            if (meta.nVersion < CKeyMetadata::VERSION_WITH_KEY_ORIGIN)
                meta.nVersion = CKeyMetadata::VERSION_WITH_KEY_ORIGIN;
            CPubKey pubkey;
            if (GetPubKey(meta_pair.first, pubkey))
                batch->WriteKeyMetadata(meta, pubkey, true);
        }
    }
}

bool LegacyScriptPubKeyMan::SetupGeneration(bool force)
{
    if ((CanGenerateKeys() && !force) || m_storage.IsLocked()) return false;
    SetHDSeed(GenerateNewSeed());
    if (!NewKeyPool()) return false;
    return true;
}

bool LegacyScriptPubKeyMan::IsHDEnabled() const
{
    return !m_hd_chain.seed_id.IsNull();
}

bool LegacyScriptPubKeyMan::CanGetAddresses(bool internal) const
{
    LOCK(cs_KeyStore);
    bool keypool_has_keys;
    if (internal && m_storage.CanSupportFeature(FEATURE_HD_SPLIT))
        keypool_has_keys = setInternalKeyPool.size() > 0;
    else
        keypool_has_keys = KeypoolCountExternalKeys() > 0;
    if (!keypool_has_keys) return CanGenerateKeys();
    return keypool_has_keys;
}

bool LegacyScriptPubKeyMan::Upgrade(int prev_version, int new_version, std::string& error)
{
    LOCK(cs_KeyStore);
    bool hd_upgrade = false;
    bool split_upgrade = false;
    if (IsFeatureSupported(new_version, FEATURE_HD) && !IsHDEnabled()) {
        WalletLogPrintf("Upgrading wallet to HD\n");
        m_storage.SetMinVersion(FEATURE_HD);
        CPubKey masterPubKey = GenerateNewSeed();
        SetHDSeed(masterPubKey);
        hd_upgrade = true;
    }
    if (!IsFeatureSupported(prev_version, FEATURE_HD_SPLIT) &&
         IsFeatureSupported(new_version, FEATURE_HD_SPLIT)) {
        WalletLogPrintf("Upgrading wallet to HD chain split\n");
        m_storage.SetMinVersion(FEATURE_PRE_SPLIT_KEYPOOL);
        split_upgrade = FEATURE_HD_SPLIT > prev_version;
        if (m_hd_chain.nVersion < CHDChain::VERSION_HD_CHAIN_SPLIT) {
            m_hd_chain.nVersion = CHDChain::VERSION_HD_CHAIN_SPLIT;
            if (!WalletBatch(m_storage.GetDatabase()).WriteHDChain(m_hd_chain))
                throw std::runtime_error(std::string(__func__) + ": writing chain failed");
        }
    }
    if (split_upgrade) MarkPreSplitKeys();
    if (hd_upgrade) {
        if (!TopUp()) {
            error = "Unable to generate keys";
            return false;
        }
    }
    return true;
}

bool LegacyScriptPubKeyMan::HavePrivateKeys() const
{
    LOCK(cs_KeyStore);
    return !mapKeys.empty() || !mapCryptedKeys.empty();
}

void LegacyScriptPubKeyMan::RewriteDB()
{
    LOCK(cs_KeyStore);
    setInternalKeyPool.clear();
    setExternalKeyPool.clear();
    m_pool_key_to_index.clear();
}

static int64_t GetOldestKeyTimeInPool(const std::set<int64_t>& setKeyPool, WalletBatch& batch)
{
    if (setKeyPool.empty()) return GetTime();
    CKeyPool keypool;
    int64_t nIndex = *setKeyPool.begin();
    if (!batch.ReadPool(nIndex, keypool))
        throw std::runtime_error(std::string(__func__) + ": read oldest key in keypool failed");
    assert(keypool.vchPubKey.IsValid());
    return keypool.nTime;
}

int64_t LegacyScriptPubKeyMan::GetOldestKeyPoolTime() const
{
    LOCK(cs_KeyStore);
    WalletBatch batch(m_storage.GetDatabase());
    int64_t oldestKey = GetOldestKeyTimeInPool(setExternalKeyPool, batch);
    if (IsHDEnabled() && m_storage.CanSupportFeature(FEATURE_HD_SPLIT)) {
        oldestKey = std::max(GetOldestKeyTimeInPool(setInternalKeyPool, batch), oldestKey);
        if (!set_pre_split_keypool.empty())
            oldestKey = std::max(GetOldestKeyTimeInPool(set_pre_split_keypool, batch), oldestKey);
    }
    return oldestKey;
}

size_t LegacyScriptPubKeyMan::KeypoolCountExternalKeys() const
{
    LOCK(cs_KeyStore);
    return setExternalKeyPool.size() + set_pre_split_keypool.size();
}

unsigned int LegacyScriptPubKeyMan::GetKeyPoolSize() const
{
    LOCK(cs_KeyStore);
    return setInternalKeyPool.size() + setExternalKeyPool.size() + set_pre_split_keypool.size();
}

int64_t LegacyScriptPubKeyMan::GetTimeFirstKey() const
{
    LOCK(cs_KeyStore);
    return nTimeFirstKey;
}

std::unique_ptr<SigningProvider> LegacyScriptPubKeyMan::GetSolvingProvider(const CScript& script) const
{
    return std::unique_ptr<SigningProvider>(new LegacySigningProvider(*this));
}

bool LegacyScriptPubKeyMan::CanProvide(const CScript& script, SignatureData& sigdata)
{
    IsMineResult ismine = IsMineInner(*this, script, IsMineSigVersion::TOP, false);
    if (ismine == IsMineResult::SPENDABLE || ismine == IsMineResult::WATCH_ONLY) return true;
    ProduceSignature(*this, DUMMY_SIGNATURE_CREATOR, script, sigdata);
    if (!sigdata.signatures.empty()) {
        bool has_privkeys = false;
        for (const auto& key_sig_pair : sigdata.signatures)
            has_privkeys |= HaveKey(key_sig_pair.first);
        return has_privkeys;
    }
    return false;
}

bool LegacyScriptPubKeyMan::SignTransaction(CMutableTransaction& tx,
    const std::map<COutPoint, Coin>& coins, int sighash,
    std::map<int, std::string>& input_errors) const
{
    // Collect spent outputs for Taproot sighash
    std::vector<CTxOut> spent_outputs;
    for (const auto& input : tx.vin) {
        auto it = coins.find(input.prevout);
        if (it != coins.end()) {
            spent_outputs.push_back(it->second.out);
        } else {
            spent_outputs.push_back(CTxOut()); // empty placeholder
        }
    }

    bool needs_txdata = false;
    for (const auto& out : spent_outputs) {
        int version;
        std::vector<unsigned char> program;
        if (out.scriptPubKey.IsWitnessProgram(version, program)) {
            needs_txdata = true;
            break;
        }
    }

    if (needs_txdata) {
        PrecomputedTransactionData txdata(tx);
        txdata.InitTaproot(tx, std::move(spent_outputs));

        for (size_t i = 0; i < tx.vin.size(); ++i) {
            auto coin_it = coins.find(tx.vin[i].prevout);
            if (coin_it == coins.end()) {
                input_errors[i] = "Input not found or already spent";
                continue;
            }
            const CScript& scriptPubKey = coin_it->second.out.scriptPubKey;
            const CAmount amount = coin_it->second.out.nValue;

            SignatureData sigdata;
            if (!ProduceSignature(*this,
                    MutableTransactionSignatureCreator(&tx, i, amount, &txdata, sighash),
                    scriptPubKey, sigdata)) {
                input_errors[i] = "Signing failed";
                continue;
            }
            UpdateInput(tx.vin[i], sigdata);
        }
    } else {
        // Pure legacy signing
        for (size_t i = 0; i < tx.vin.size(); ++i) {
            auto coin_it = coins.find(tx.vin[i].prevout);
            if (coin_it == coins.end()) {
                input_errors[i] = "Input not found or already spent";
                continue;
            }
            const CScript& scriptPubKey = coin_it->second.out.scriptPubKey;
            const CAmount amount = coin_it->second.out.nValue;

            SignatureData sigdata;
            if (!ProduceSignature(*this,
                    MutableTransactionSignatureCreator(&tx, i, amount, sighash),
                    scriptPubKey, sigdata)) {
                input_errors[i] = "Signing failed";
                continue;
            }
            UpdateInput(tx.vin[i], sigdata);
        }
    }
    return input_errors.empty();
}

SigningResult LegacyScriptPubKeyMan::SignMessage(const std::string& message,
    const CKeyID& keyid, std::string& str_sig) const
{
    CKey key;
    if (!GetKey(keyid, key)) return SigningResult::PRIVATE_KEY_NOT_AVAILABLE;

    // Simple message signing: prepend the XPChain/XPChain message magic prefix
    // (same as CMessageSigner in earlier versions)
    static const std::string MSG_MAGIC = "XPChain Signed Message:\n";
    CHashWriter ss(SER_GETHASH, 0);
    ss << MSG_MAGIC;
    ss << message;
    uint256 hash = ss.GetHash();

    std::vector<unsigned char> sig;
    if (!key.SignCompact(hash, sig)) return SigningResult::SIGNING_FAILED;

    str_sig = EncodeBase64(sig.data(), sig.size());
    return SigningResult::OK;
}

std::unique_ptr<CKeyMetadata> LegacyScriptPubKeyMan::GetMetadata(const CTxDestination& dest) const
{
    LOCK(cs_KeyStore);
    CKeyID key_id = GetKeyForDestination(*this, dest);
    if (!key_id.IsNull()) {
        auto it = mapKeyMetadata.find(key_id);
        if (it != mapKeyMetadata.end())
            return std::unique_ptr<CKeyMetadata>(new CKeyMetadata(it->second));
    }
    CScript spk = GetScriptForDestination(dest);
    auto it = m_script_metadata.find(CScriptID(spk));
    if (it != m_script_metadata.end())
        return std::unique_ptr<CKeyMetadata>(new CKeyMetadata(it->second));
    return nullptr;
}

uint256 LegacyScriptPubKeyMan::GetID() const
{
    // Use SHA256 of the HD chain seed id as unique identifier; fall back to all-ones
    if (!m_hd_chain.seed_id.IsNull()) {
        uint256 id;
        CSHA256().Write(m_hd_chain.seed_id.begin(), 20).Finalize(id.begin());
        return id;
    }
    return uint256S("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
}

void LegacyScriptPubKeyMan::SetInternal(bool internal)
{
    // no-op for legacy
}

void LegacyScriptPubKeyMan::UpdateTimeFirstKey(int64_t nCreateTime)
{
    AssertLockHeld(cs_KeyStore);
    if (nCreateTime <= 1) {
        nTimeFirstKey = 1;
    } else if (!nTimeFirstKey || nCreateTime < nTimeFirstKey) {
        nTimeFirstKey = nCreateTime;
    }
}

// ─── Key management ───────────────────────────────────────────────────────────

bool LegacyScriptPubKeyMan::LoadKey(const CKey& key, const CPubKey& pubkey)
{
    return AddKeyPubKeyInner(key, pubkey);
}

bool LegacyScriptPubKeyMan::AddKeyPubKey(const CKey& secret, const CPubKey& pubkey)
{
    LOCK(cs_KeyStore);
    WalletBatch batch(m_storage.GetDatabase());
    return LegacyScriptPubKeyMan::AddKeyPubKeyWithDB(batch, secret, pubkey);
}

bool LegacyScriptPubKeyMan::AddKeyPubKeyWithDB(WalletBatch& batch, const CKey& secret, const CPubKey& pubkey)
{
    AssertLockHeld(cs_KeyStore);
    assert(!m_storage.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS));

    bool needsDB = !encrypted_batch;
    if (needsDB) encrypted_batch = &batch;
    if (!AddKeyPubKeyInner(secret, pubkey)) {
        if (needsDB) encrypted_batch = nullptr;
        return false;
    }
    if (needsDB) encrypted_batch = nullptr;

    // Remove from watch-only if present
    CScript script;
    script = GetScriptForDestination(CKeyID(pubkey.GetID()));
    if (HaveWatchOnly(script)) RemoveWatchOnly(script);
    script = GetScriptForRawPubKey(pubkey);
    if (HaveWatchOnly(script)) RemoveWatchOnly(script);

    if (!m_storage.HasEncryptionKeys()) {
        return batch.WriteKey(pubkey, secret.GetPrivKey(), mapKeyMetadata[pubkey.GetID()]);
    }
    m_storage.UnsetBlankWalletFlag(batch);
    return true;
}

bool LegacyScriptPubKeyMan::LoadCScript(const CScript& redeemScript)
{
    if (redeemScript.size() > MAX_SCRIPT_ELEMENT_SIZE) {
        std::string strAddr = EncodeDestination(CScriptID(redeemScript));
        WalletLogPrintf("%s: Warning: redeemScript of size %i exceeds max %i, address %s unredeemable.\n",
                        __func__, redeemScript.size(), MAX_SCRIPT_ELEMENT_SIZE, strAddr);
        return true;
    }
    return CBasicKeyStore::AddCScript(redeemScript);
}

void LegacyScriptPubKeyMan::LoadKeyMetadata(const CKeyID& keyID, const CKeyMetadata& meta)
{
    LOCK(cs_KeyStore);
    UpdateTimeFirstKey(meta.nCreateTime);
    mapKeyMetadata[keyID] = meta;
}

void LegacyScriptPubKeyMan::LoadScriptMetadata(const CScriptID& script_id, const CKeyMetadata& meta)
{
    LOCK(cs_KeyStore);
    UpdateTimeFirstKey(meta.nCreateTime);
    m_script_metadata[script_id] = meta;
}

bool LegacyScriptPubKeyMan::AddKeyPubKeyInner(const CKey& key, const CPubKey& pubkey)
{
    LOCK(cs_KeyStore);
    if (!m_storage.HasEncryptionKeys())
        return CBasicKeyStore::AddKeyPubKey(key, pubkey);
    if (m_storage.IsLocked()) return false;

    std::vector<unsigned char> vchCryptedSecret;
    CKeyingMaterial vchSecret(key.begin(), key.end());
    if (!EncryptSecret(m_storage.GetEncryptionKey(), vchSecret, pubkey.GetHash(), vchCryptedSecret))
        return false;
    if (!AddCryptedKey(pubkey, vchCryptedSecret)) return false;
    return true;
}

bool LegacyScriptPubKeyMan::LoadCryptedKey(const CPubKey& vchPubKey,
    const std::vector<unsigned char>& vchCryptedSecret, bool checksum_valid)
{
    if (!checksum_valid) fDecryptionThoroughlyChecked = false;
    return AddCryptedKeyInner(vchPubKey, vchCryptedSecret);
}

bool LegacyScriptPubKeyMan::AddCryptedKeyInner(const CPubKey& vchPubKey,
    const std::vector<unsigned char>& vchCryptedSecret)
{
    LOCK(cs_KeyStore);
    assert(mapKeys.empty());
    mapCryptedKeys[vchPubKey.GetID()] = std::make_pair(vchPubKey, vchCryptedSecret);
    return true;
}

bool LegacyScriptPubKeyMan::AddCryptedKey(const CPubKey& vchPubKey,
    const std::vector<unsigned char>& vchCryptedSecret)
{
    if (!AddCryptedKeyInner(vchPubKey, vchCryptedSecret)) return false;
    LOCK(cs_KeyStore);
    if (encrypted_batch) {
        return encrypted_batch->WriteCryptedKey(vchPubKey, vchCryptedSecret,
                                                 mapKeyMetadata[vchPubKey.GetID()]);
    }
    return WalletBatch(m_storage.GetDatabase()).WriteCryptedKey(
        vchPubKey, vchCryptedSecret, mapKeyMetadata[vchPubKey.GetID()]);
}

// ─── Override SigningProvider methods ─────────────────────────────────────────

bool LegacyScriptPubKeyMan::HaveKey(const CKeyID& address) const
{
    LOCK(cs_KeyStore);
    if (!mapCryptedKeys.empty()) {
        return mapCryptedKeys.count(address) > 0;
    }
    return CBasicKeyStore::HaveKey(address);
}

bool LegacyScriptPubKeyMan::GetKey(const CKeyID& address, CKey& keyOut) const
{
    LOCK(cs_KeyStore);
    {
        auto mi = mapCryptedKeys.find(address);
        if (mi != mapCryptedKeys.end()) {
            const CPubKey& vchPubKey = mi->second.first;
            const std::vector<unsigned char>& vchCryptedSecret = mi->second.second;
            return DecryptKey(m_storage.GetEncryptionKey(), vchCryptedSecret, vchPubKey, keyOut);
        }
    }
    return CBasicKeyStore::GetKey(address, keyOut);
}

bool LegacyScriptPubKeyMan::GetPubKey(const CKeyID& address, CPubKey& vchPubKeyOut) const
{
    LOCK(cs_KeyStore);
    {
        auto mi = mapCryptedKeys.find(address);
        if (mi != mapCryptedKeys.end()) {
            vchPubKeyOut = mi->second.first;
            return true;
        }
    }
    // Try watch keys
    {
        auto mi = mapWatchKeys.find(address);
        if (mi != mapWatchKeys.end()) {
            vchPubKeyOut = mi->second;
            return true;
        }
    }
    return CBasicKeyStore::GetPubKey(address, vchPubKeyOut);
}

bool LegacyScriptPubKeyMan::AddCScript(const CScript& redeemScript)
{
    WalletBatch batch(m_storage.GetDatabase());
    return AddCScriptWithDB(batch, redeemScript);
}

bool LegacyScriptPubKeyMan::AddCScriptWithDB(WalletBatch& batch, const CScript& script)
{
    if (!CBasicKeyStore::AddCScript(script)) return false;
    if (batch.WriteCScript(Hash160(script), script)) return true;
    return false;
}

bool LegacyScriptPubKeyMan::GetKeyOrigin(const CKeyID& keyid, KeyOriginInfo& info) const
{
    LOCK(cs_KeyStore);
    auto it = mapKeyMetadata.find(keyid);
    if (it != mapKeyMetadata.end() && it->second.has_key_origin) {
        info = it->second.key_origin;
        return true;
    }
    return false;
}

std::set<CKeyID> LegacyScriptPubKeyMan::GetKeys() const
{
    LOCK(cs_KeyStore);
    if (mapCryptedKeys.empty()) return CBasicKeyStore::GetKeys();
    std::set<CKeyID> set_address;
    for (const auto& mi : mapCryptedKeys)
        set_address.insert(mi.first);
    return set_address;
}

// ─── Watchonly ────────────────────────────────────────────────────────────────

bool LegacyScriptPubKeyMan::AddWatchOnly(const CScript& dest)
{
    return AddWatchOnlyInMem(dest);
}

bool LegacyScriptPubKeyMan::AddWatchOnlyInMem(const CScript& dest)
{
    LOCK(cs_KeyStore);
    setWatchOnly.insert(dest);
    CPubKey pubKey;
    if (ExtractPubKey(dest, pubKey)) {
        mapWatchKeys[pubKey.GetID()] = pubKey;
    }
    return true;
}

bool LegacyScriptPubKeyMan::AddWatchOnlyWithDB(WalletBatch& batch, const CScript& dest)
{
    if (!AddWatchOnlyInMem(dest)) return false;
    const CKeyMetadata& meta = m_script_metadata[CScriptID(dest)];
    UpdateTimeFirstKey(meta.nCreateTime);
    NotifyWatchonlyChanged(true);
    if (batch.WriteWatchOnly(dest, meta)) return true;
    return false;
}

bool LegacyScriptPubKeyMan::AddWatchOnlyWithDB(WalletBatch& batch, const CScript& dest, int64_t create_time)
{
    m_script_metadata[CScriptID(dest)].nCreateTime = create_time;
    return AddWatchOnlyWithDB(batch, dest);
}

bool LegacyScriptPubKeyMan::AddWatchOnly(const CScript& dest, int64_t nCreateTime)
{
    m_script_metadata[CScriptID(dest)].nCreateTime = nCreateTime;
    WalletBatch batch(m_storage.GetDatabase());
    return AddWatchOnlyWithDB(batch, dest);
}

bool LegacyScriptPubKeyMan::LoadWatchOnly(const CScript& dest)
{
    return AddWatchOnlyInMem(dest);
}

bool LegacyScriptPubKeyMan::RemoveWatchOnly(const CScript& dest)
{
    LOCK(cs_KeyStore);
    setWatchOnly.erase(dest);
    CPubKey pubKey;
    if (ExtractPubKey(dest, pubKey)) mapWatchKeys.erase(pubKey.GetID());
    if (!HaveWatchOnly()) NotifyWatchonlyChanged(false);
    WalletBatch batch(m_storage.GetDatabase());
    if (!batch.EraseWatchOnly(dest)) return false;
    return true;
}

bool LegacyScriptPubKeyMan::HaveWatchOnly(const CScript& dest) const
{
    LOCK(cs_KeyStore);
    return setWatchOnly.count(dest) > 0;
}

bool LegacyScriptPubKeyMan::HaveWatchOnly() const
{
    LOCK(cs_KeyStore);
    return !setWatchOnly.empty();
}

bool LegacyScriptPubKeyMan::GetWatchPubKey(const CKeyID& address, CPubKey& pubkey_out) const
{
    LOCK(cs_KeyStore);
    auto it = mapWatchKeys.find(address);
    if (it != mapWatchKeys.end()) {
        pubkey_out = it->second;
        return true;
    }
    return false;
}

// ─── Keypool ──────────────────────────────────────────────────────────────────

void LegacyScriptPubKeyMan::LoadKeyPool(int64_t nIndex, const CKeyPool& keypool)
{
    LOCK(cs_KeyStore);
    if (keypool.m_pre_split) {
        set_pre_split_keypool.insert(nIndex);
    } else if (keypool.fInternal) {
        setInternalKeyPool.insert(nIndex);
    } else {
        setExternalKeyPool.insert(nIndex);
    }
    m_max_keypool_index = std::max(m_max_keypool_index, nIndex);
    m_pool_key_to_index[keypool.vchPubKey.GetID()] = nIndex;
}

void LegacyScriptPubKeyMan::AddKeypoolPubkeyWithDB(const CPubKey& pubkey, const bool internal, WalletBatch& batch)
{
    LOCK(cs_KeyStore);
    assert(m_max_keypool_index < std::numeric_limits<int64_t>::max());
    int64_t index = ++m_max_keypool_index;
    if (!batch.WritePool(index, CKeyPool(pubkey, internal))) {
        throw std::runtime_error(std::string(__func__) + ": writing imported pubkey failed");
    }
    if (internal) {
        setInternalKeyPool.insert(index);
    } else {
        setExternalKeyPool.insert(index);
    }
    m_pool_key_to_index[pubkey.GetID()] = index;
}

bool LegacyScriptPubKeyMan::NewKeyPool()
{
    if (m_storage.IsLocked()) return false;
    WalletBatch batch(m_storage.GetDatabase());
    LOCK(cs_KeyStore);
    for (const int64_t nIndex : setInternalKeyPool)
        batch.ErasePool(nIndex);
    setInternalKeyPool.clear();
    for (const int64_t nIndex : setExternalKeyPool)
        batch.ErasePool(nIndex);
    setExternalKeyPool.clear();
    for (const int64_t nIndex : set_pre_split_keypool)
        batch.ErasePool(nIndex);
    set_pre_split_keypool.clear();
    m_pool_key_to_index.clear();
    if (!TopUp()) return false;
    WalletLogPrintf("LegacyScriptPubKeyMan::NewKeyPool rewrote keypool\n");
    return true;
}

bool LegacyScriptPubKeyMan::TopUp(unsigned int kpSize)
{
    if (!CanGenerateKeys()) return false;
    LOCK(cs_KeyStore);
    if (m_storage.IsLocked()) return false;
    unsigned int nTargetSize;
    if (kpSize > 0) nTargetSize = kpSize;
    else nTargetSize = std::max(gArgs.GetArg("-keypool", (int64_t)DEFAULT_KEYPOOL_SIZE), (int64_t)1);

    int64_t missingExternal = std::max((int64_t)nTargetSize - (int64_t)setExternalKeyPool.size(), (int64_t)0);
    int64_t missingInternal = std::max((int64_t)nTargetSize - (int64_t)setInternalKeyPool.size(), (int64_t)0);
    if (!IsHDEnabled() || !m_storage.CanSupportFeature(FEATURE_HD_SPLIT)) missingInternal = 0;

    WalletBatch batch(m_storage.GetDatabase());
    for (int64_t i = missingExternal; i > 0; --i)
        GenerateNewKey(batch, m_hd_chain, false);
    for (int64_t i = missingInternal; i > 0; --i)
        GenerateNewKey(batch, m_hd_chain, true);

    if (missingInternal + missingExternal > 0) {
        WalletLogPrintf("keypool added %d keys (%d internal), size=%u (%u internal)\n",
                        missingInternal + missingExternal, missingInternal,
                        setInternalKeyPool.size() + setExternalKeyPool.size() + set_pre_split_keypool.size(),
                        setInternalKeyPool.size());
    }
    return true;
}

void LegacyScriptPubKeyMan::MarkPreSplitKeys()
{
    WalletBatch batch(m_storage.GetDatabase());
    for (auto it = setExternalKeyPool.begin(); it != setExternalKeyPool.end();) {
        int64_t nIndex = *it;
        CKeyPool keypool;
        if (!batch.ReadPool(nIndex, keypool) || !keypool.vchPubKey.IsValid() ||
            !HaveKey(keypool.vchPubKey.GetID())) {
            throw std::runtime_error(std::string(__func__) + ": read keypool entry failed");
        }
        keypool.m_pre_split = true;
        if (!batch.WritePool(nIndex, keypool)) {
            throw std::runtime_error(std::string(__func__) + ": writing keypool entry failed");
        }
        set_pre_split_keypool.insert(nIndex);
        it = setExternalKeyPool.erase(it);
    }
}

bool LegacyScriptPubKeyMan::ReserveKeyFromKeyPool(int64_t& nIndex, CKeyPool& keypool, bool fRequestedInternal)
{
    nIndex = -1;
    keypool.vchPubKey = CPubKey();
    LOCK(cs_KeyStore);
    if (!IsLocked()) TopUp();

    bool fReturningInternal = IsHDEnabled() &&
                               m_storage.CanSupportFeature(FEATURE_HD_SPLIT) &&
                               fRequestedInternal;
    bool use_split_keypool = set_pre_split_keypool.empty();
    std::set<int64_t>& setKeyPool = use_split_keypool
        ? (fReturningInternal ? setInternalKeyPool : setExternalKeyPool)
        : set_pre_split_keypool;

    if (setKeyPool.empty()) return false;
    auto it = setKeyPool.begin();
    nIndex = *it;
    setKeyPool.erase(it);

    // Note: poolType (internal/external) is tracked via keypool.fInternal
    bool poolType_internal = fReturningInternal; (void)poolType_internal;
    if (!WalletBatch(m_storage.GetDatabase()).ReadPool(nIndex, keypool))
        throw std::runtime_error(std::string(__func__) + ": read failed");
    if (!HaveKey(keypool.vchPubKey.GetID()))
        throw std::runtime_error(std::string(__func__) + ": unknown key in key pool");
    assert(keypool.vchPubKey.IsValid());
    m_pool_key_to_index.erase(keypool.vchPubKey.GetID());
    m_index_to_reserved_key[nIndex] = keypool.vchPubKey.GetID();
    WalletLogPrintf("keypool reserve %d\n", nIndex);
    return true;
}

void LegacyScriptPubKeyMan::KeepDestination(int64_t nIndex, const OutputType& type)
{
    LOCK(cs_KeyStore);
    auto it = m_index_to_reserved_key.find(nIndex);
    assert(it != m_index_to_reserved_key.end());
    m_index_to_reserved_key.erase(it);
    WalletLogPrintf("keypool keep %d\n", nIndex);
}

void LegacyScriptPubKeyMan::ReturnDestination(int64_t nIndex, bool fInternal, const CTxDestination&)
{
    LOCK(cs_KeyStore);
    auto it = m_index_to_reserved_key.find(nIndex);
    assert(it != m_index_to_reserved_key.end());
    CKeyID& pubkey_id = it->second;
    WalletBatch batch(m_storage.GetDatabase());
    CKeyPool keypool;
    batch.ReadPool(nIndex, keypool);
    m_pool_key_to_index[pubkey_id] = nIndex;
    m_index_to_reserved_key.erase(it);
    if (fInternal) setInternalKeyPool.insert(nIndex);
    else if (!set_pre_split_keypool.empty()) set_pre_split_keypool.insert(nIndex);
    else setExternalKeyPool.insert(nIndex);
    WalletLogPrintf("keypool return %d\n", nIndex);
    NotifyCanGetAddressesChanged();
}

void LegacyScriptPubKeyMan::MarkReserveKeysAsUsed(int64_t keypool_id)
{
    AssertLockHeld(cs_KeyStore);
    bool internal = setInternalKeyPool.count(keypool_id);
    if (!internal) assert(setExternalKeyPool.count(keypool_id) || set_pre_split_keypool.count(keypool_id));
    std::set<int64_t>* setKeyPool = internal ? &setInternalKeyPool :
        (set_pre_split_keypool.count(keypool_id) ? &set_pre_split_keypool : &setExternalKeyPool);
    auto it = setKeyPool->begin();
    WalletBatch batch(m_storage.GetDatabase());
    while (it != std::end(*setKeyPool)) {
        const int64_t& index = *it;
        if (index > keypool_id) break;
        CKeyPool keypool;
        if (batch.ReadPool(index, keypool)) {
            m_pool_key_to_index.erase(keypool.vchPubKey.GetID());
        }
        LearnAllRelatedScripts(keypool.vchPubKey);
        batch.ErasePool(index);
        WalletLogPrintf("keypool index %d removed\n", index);
        it = setKeyPool->erase(it);
    }
}

bool LegacyScriptPubKeyMan::GetKeyFromPool(CPubKey& result, const OutputType type, bool internal)
{
    if (!CanGetAddresses(internal)) return false;
    CKeyPool keypool;
    int64_t nIndex;
    if (!ReserveKeyFromKeyPool(nIndex, keypool, internal)) {
        if (m_storage.IsLocked()) return false;
        WalletBatch batch(m_storage.GetDatabase());
        result = GenerateNewKey(batch, m_hd_chain, internal);
        return true;
    }
    KeepDestination(nIndex, type);
    result = keypool.vchPubKey;
    return true;
}

// ─── HD key derivation ────────────────────────────────────────────────────────

CPubKey LegacyScriptPubKeyMan::GenerateNewSeed()
{
    assert(!m_storage.IsLocked());
    CKey key;
    key.MakeNewKey(true);
    return DeriveNewSeed(key);
}

CPubKey LegacyScriptPubKeyMan::DeriveNewSeed(const CKey& key)
{
    int64_t nCreationTime = GetTime();
    CKeyMetadata metadata(nCreationTime);

    CExtKey masterKey;
    masterKey.SetSeed(key.begin(), key.size());

    metadata.hdKeypath = "s";
    metadata.has_key_origin = false;
    metadata.hd_seed_id = masterKey.key.GetPubKey().GetID();

    LOCK(cs_KeyStore);
    CPubKey pubkey = masterKey.key.GetPubKey();
    mapKeyMetadata[pubkey.GetID()] = metadata;
    UpdateTimeFirstKey(nCreationTime);

    WalletBatch batch(m_storage.GetDatabase());
    if (!AddKeyPubKeyWithDB(batch, masterKey.key, pubkey))
        throw std::runtime_error(std::string(__func__) + ": AddKeyPubKey failed");

    return pubkey;
}

void LegacyScriptPubKeyMan::SetHDSeed(const CPubKey& seed)
{
    LOCK(cs_KeyStore);
    CHDChain newHdChain;
    newHdChain.nVersion = m_hd_chain.nVersion;
    newHdChain.seed_id = seed.GetID();
    AddHDChain(newHdChain);
    NotifyCanGetAddressesChanged();
    WalletBatch batch(m_storage.GetDatabase());
    AddKeyOriginWithDB(batch, seed, KeyOriginInfo());
}

bool LegacyScriptPubKeyMan::AddKeyOriginWithDB(WalletBatch& batch, const CPubKey& pubkey, const KeyOriginInfo& info)
{
    LOCK(cs_KeyStore);
    std::copy(info.fingerprint, info.fingerprint + 4, mapKeyMetadata[pubkey.GetID()].key_origin.fingerprint);
    mapKeyMetadata[pubkey.GetID()].key_origin.path = info.path;
    mapKeyMetadata[pubkey.GetID()].has_key_origin = true;
    return batch.WriteKeyMetadata(mapKeyMetadata[pubkey.GetID()], pubkey, true);
}

void LegacyScriptPubKeyMan::AddHDChain(const CHDChain& chain)
{
    LOCK(cs_KeyStore);
    m_hd_chain = chain;
    WalletBatch batch(m_storage.GetDatabase());
    batch.WriteHDChain(chain);
}

void LegacyScriptPubKeyMan::LoadHDChain(const CHDChain& chain)
{
    LOCK(cs_KeyStore);
    m_hd_chain = chain;
}

void LegacyScriptPubKeyMan::AddInactiveHDChain(const CHDChain& chain)
{
    LOCK(cs_KeyStore);
    assert(!chain.seed_id.IsNull());
    m_inactive_hd_chains[chain.seed_id] = chain;
}

void LegacyScriptPubKeyMan::DeriveNewChildKey(WalletBatch& batch, CKeyMetadata& metadata,
    CKey& secret, CHDChain& hd_chain, bool internal)
{
    CKey seed;
    if (!GetKey(hd_chain.seed_id, seed))
        throw std::runtime_error(std::string(__func__) + ": seed not found");

    CExtKey masterKey;
    masterKey.SetSeed(seed.begin(), seed.size());

    CExtKey accountKey;
    if (!masterKey.Derive(accountKey, BIP32_HARDENED_KEY_LIMIT))
        throw std::runtime_error(std::string(__func__) + ": derive account failed");

    CExtKey changeKey;
    if (!accountKey.Derive(changeKey, BIP32_HARDENED_KEY_LIMIT + (internal ? 1 : 0)))
        throw std::runtime_error(std::string(__func__) + ": derive change failed");

    uint32_t nChild;
    CExtKey childKey;
    do {
        nChild = internal
            ? hd_chain.nInternalChainCounter++
            : hd_chain.nExternalChainCounter++;
        changeKey.Derive(childKey, BIP32_HARDENED_KEY_LIMIT + nChild);
    } while (HaveKey(childKey.key.GetPubKey().GetID()));

    secret = childKey.key;

    // Store hd_seed fingerprint
    CKeyID masterID = masterKey.key.GetPubKey().GetID();
    std::copy(masterID.begin(), masterID.begin() + 4, metadata.key_origin.fingerprint);

    // m/0'/change'/nChild'
    metadata.key_origin.path.push_back(BIP32_HARDENED_KEY_LIMIT);
    metadata.key_origin.path.push_back(BIP32_HARDENED_KEY_LIMIT + (internal ? 1 : 0));
    metadata.key_origin.path.push_back(BIP32_HARDENED_KEY_LIMIT + nChild);
    metadata.hdKeypath = FormatHDKeypath(metadata.key_origin.path);
    metadata.has_key_origin = true;
    metadata.hd_seed_id = hd_chain.seed_id;
}

CPubKey LegacyScriptPubKeyMan::GenerateNewKey(WalletBatch& batch, CHDChain& hd_chain, bool internal)
{
    AssertLockHeld(cs_KeyStore);
    assert(!m_storage.IsLocked());

    CKey secret;
    int64_t nCreationTime = GetTime();
    CKeyMetadata metadata(nCreationTime);

    if (IsHDEnabled()) {
        DeriveNewChildKey(batch, metadata, secret, hd_chain, internal);
    } else {
        secret.MakeNewKey(true);
    }

    CPubKey pubkey = secret.GetPubKey();
    assert(secret.VerifyPubKey(pubkey));

    mapKeyMetadata[pubkey.GetID()] = metadata;
    UpdateTimeFirstKey(nCreationTime);

    if (!AddKeyPubKeyWithDB(batch, secret, pubkey))
        throw std::runtime_error(std::string(__func__) + ": AddKeyPubKey failed");

    WalletBatch(m_storage.GetDatabase()).WriteHDChain(hd_chain);

    return pubkey;
}

// ─── Script learning ──────────────────────────────────────────────────────────

void LegacyScriptPubKeyMan::LearnRelatedScripts(const CPubKey& key, OutputType output_type)
{
    assert(key.IsCompressed());
    if (output_type == OutputType::P2SH_SEGWIT || output_type == OutputType::BECH32) {
        CScript script = GetScriptForRawPubKey(key);
        // Add P2WPKH
        CScript p2wpkh = GetScriptForDestination(WitnessV0KeyHash(key.GetID()));
        {
            WalletBatch batch(m_storage.GetDatabase());
            AddCScriptWithDB(batch, p2wpkh);
            if (output_type == OutputType::P2SH_SEGWIT) {
                // Also add P2SH-P2WPKH
                CScript p2sh_p2wpkh = GetScriptForDestination(CScriptID(p2wpkh));
                AddCScriptWithDB(batch, p2sh_p2wpkh);
            }
        }
    }
}

void LegacyScriptPubKeyMan::LearnAllRelatedScripts(const CPubKey& key)
{
    if (!key.IsCompressed()) return;
    LearnRelatedScripts(key, OutputType::P2SH_SEGWIT);
    LearnRelatedScripts(key, OutputType::BECH32);
}

// ─── Import helpers ───────────────────────────────────────────────────────────

bool LegacyScriptPubKeyMan::ImportScripts(const std::set<CScript> scripts, int64_t timestamp)
{
    WalletBatch batch(m_storage.GetDatabase());
    for (const auto& entry : scripts) {
        CScriptID id(entry);
        if (HaveCScript(id)) continue;
        if (!AddCScriptWithDB(batch, entry)) return false;
    }
    return true;
}

bool LegacyScriptPubKeyMan::ImportPrivKeys(const std::map<CKeyID, CKey>& privkey_map, const int64_t timestamp)
{
    WalletBatch batch(m_storage.GetDatabase());
    for (const auto& entry : privkey_map) {
        const CKey& key = entry.second;
        CPubKey pubkey = key.GetPubKey();
        const CKeyID& id = entry.first;
        assert(id == pubkey.GetID());
        mapKeyMetadata[id].nCreateTime = timestamp;
        if (!AddKeyPubKeyWithDB(batch, key, pubkey)) return false;
        UpdateTimeFirstKey(timestamp);
    }
    return true;
}

bool LegacyScriptPubKeyMan::ImportPubKeys(
    const std::vector<CKeyID>& ordered_pubkeys,
    const std::map<CKeyID, CPubKey>& pubkey_map,
    const std::map<CKeyID, std::pair<CPubKey, KeyOriginInfo>>& key_origins,
    const bool add_keypool, const bool internal, const int64_t timestamp)
{
    WalletBatch batch(m_storage.GetDatabase());
    for (const auto& entry : key_origins) {
        AddKeyOriginWithDB(batch, entry.second.first, entry.second.second);
    }
    for (const CKeyID& id : ordered_pubkeys) {
        auto entry = pubkey_map.find(id);
        if (entry == pubkey_map.end()) continue;
        const CPubKey& pubkey = entry->second;
        CKeyMetadata metadata;
        metadata.nCreateTime = timestamp;
        auto it = key_origins.find(id);
        if (it != key_origins.end()) {
            metadata.key_origin = it->second.second;
            metadata.has_key_origin = true;
        }
        mapKeyMetadata[id] = metadata;
        UpdateTimeFirstKey(timestamp);
        if (add_keypool) {
            AddKeypoolPubkeyWithDB(pubkey, internal, batch);
            NotifyCanGetAddressesChanged();
        }
    }
    return true;
}

bool LegacyScriptPubKeyMan::ImportScriptPubKeys(const std::set<CScript>& script_pub_keys,
    const bool have_solving_data, const int64_t timestamp)
{
    WalletBatch batch(m_storage.GetDatabase());
    for (const CScript& script : script_pub_keys) {
        if (!have_solving_data || !IsMine(script)) {
            CScriptID id(script);
            if (!batch.WriteWatchOnly(script, m_script_metadata[id])) return false;
            if (!AddWatchOnlyInMem(script)) return false;
            NotifyWatchonlyChanged(true);
        }
    }
    return true;
}

bool LegacyScriptPubKeyMan::CanGenerateKeys() const
{
    LOCK(cs_KeyStore);
    return IsHDEnabled() && !m_storage.IsLocked();
}

// ─── DescriptorScriptPubKeyMan ────────────────────────────────────────────────

isminetype DescriptorScriptPubKeyMan::IsMine(const CScript& script) const
{
    LOCK(cs_desc_man);
    for (const auto& entry : m_descriptors) {
        // Expand descriptor to check if script is generated by it
        // This is a simplified version; in full implementation we'd use a cache
        for (const auto& cache_entry : entry.second.cache) {
            if (cache_entry.second == script) return ISMINE_SPENDABLE;
        }
    }
    return ISMINE_NO;
}

bool DescriptorScriptPubKeyMan::GetNewDestination(const OutputType type, CTxDestination& dest, std::string& error)
{
    LOCK(cs_desc_man);
    LogPrintf("DescriptorScriptPubKeyMan::GetNewDestination: Starting for type %d\n", (int)type);
    for (auto& entry : m_descriptors) {
        std::vector<CScript> scripts;
        FlatSigningProvider out;
        try {
            if (entry.second.descriptor->Expand(entry.second.next_index, *this, scripts, out)) {
                if (scripts.empty()) {
                    LogPrintf("DescriptorScriptPubKeyMan::GetNewDestination: Expand returned true but scripts is empty!\n");
                    continue;
                }
                entry.second.cache[entry.second.next_index] = scripts[0];
                ExtractDestination(scripts[0], dest);
                entry.second.next_index++;
                LogPrintf("DescriptorScriptPubKeyMan::GetNewDestination: Successfully generated destination\n");
                return true;
            }
        } catch (const std::exception& e) {
            LogPrintf("DescriptorScriptPubKeyMan::GetNewDestination: Exception during Expand: %s\n", e.what());
        } catch (...) {
            LogPrintf("DescriptorScriptPubKeyMan::GetNewDestination: Unknown exception during Expand\n");
        }
    }
    error = "No suitable descriptor found";
    LogPrintf("DescriptorScriptPubKeyMan::GetNewDestination: Failed - %s\n", error);
    return false;
}

bool DescriptorScriptPubKeyMan::SetupGeneration(bool force)
{
    LOCK(cs_desc_man);
    LogPrintf("DescriptorScriptPubKeyMan::SetupGeneration: Initializing descriptors\n");
    if (!m_descriptors.empty() && !force) {
        LogPrintf("DescriptorScriptPubKeyMan::SetupGeneration: Descriptors already exist, skipping\n");
        return false;
    }

    try {
        CKey seed;
        seed.MakeNewKey(true);
        std::string seed_wif = EncodeSecret(seed);
        LogPrintf("DescriptorScriptPubKeyMan::SetupGeneration: Generated new seed\n");
        
        // tr(WIF)
        std::string tr_desc = "tr(" + seed_wif + ")";
        FlatSigningProvider out_tr;
        std::string error_tr;
        auto desc_tr = Parse(tr_desc, out_tr, error_tr);
        if (desc_tr) {
            LogPrintf("DescriptorScriptPubKeyMan::SetupGeneration: Parsed tr() descriptor\n");
            // Copy keys from out_tr to m_storage_provider
            for (const auto& key_pair : out_tr.keys) {
                m_storage_provider.keys.emplace(key_pair.first, key_pair.second);
            }
            AddDescriptor(std::move(desc_tr), false);
        } else {
            LogPrintf("DescriptorScriptPubKeyMan::SetupGeneration: Failed to parse tr() descriptor: %s\n", error_tr);
        }

        // pkh(WIF) as fallback
        std::string pkh_desc = "pkh(" + seed_wif + ")";
        FlatSigningProvider out_pkh;
        std::string error_pkh;
        auto desc_pkh = Parse(pkh_desc, out_pkh, error_pkh);
        if (desc_pkh) {
            LogPrintf("DescriptorScriptPubKeyMan::SetupGeneration: Parsed pkh() descriptor\n");
            for (const auto& key_pair : out_pkh.keys) {
                m_storage_provider.keys.emplace(key_pair.first, key_pair.second);
            }
            AddDescriptor(std::move(desc_pkh), false);
        } else {
            LogPrintf("DescriptorScriptPubKeyMan::SetupGeneration: Failed to parse pkh() descriptor: %s\n", error_pkh);
        }

        LogPrintf("DescriptorScriptPubKeyMan::SetupGeneration: Finished initialization\n");
        return true;
    } catch (const std::exception& e) {
        LogPrintf("DescriptorScriptPubKeyMan::SetupGeneration: Critical error: %s\n", e.what());
    } catch (...) {
        LogPrintf("DescriptorScriptPubKeyMan::SetupGeneration: Unknown critical error\n");
    }

    return false;
}

bool DescriptorScriptPubKeyMan::SignTransaction(CMutableTransaction& tx,
    const std::map<COutPoint, Coin>& coins, int sighash,
    std::map<int, std::string>& input_errors) const
{
    std::vector<CTxOut> spent_outputs;
    for (const auto& input : tx.vin) {
        auto it = coins.find(input.prevout);
        if (it != coins.end()) {
            spent_outputs.push_back(it->second.out);
        } else {
            spent_outputs.push_back(CTxOut());
        }
    }

    PrecomputedTransactionData txdata(tx);
    txdata.InitTaproot(tx, std::move(spent_outputs));

    for (size_t i = 0; i < tx.vin.size(); ++i) {
        auto coin_it = coins.find(tx.vin[i].prevout);
        if (coin_it == coins.end()) continue;
        
        SignatureData sigdata;
        if (ProduceSignature(*this, MutableTransactionSignatureCreator(&tx, i, coin_it->second.out.nValue, &txdata, sighash), coin_it->second.out.scriptPubKey, sigdata)) {
            UpdateInput(tx.vin[i], sigdata);
        } else {
            input_errors[i] = "Descriptor signing failed";
        }
    }
    return input_errors.empty();
}

SigningResult DescriptorScriptPubKeyMan::SignMessage(const std::string& message, const CKeyID& keyid, std::string& str_sig) const
{
    return SigningResult::SIGNING_FAILED;
}

uint256 DescriptorScriptPubKeyMan::GetID() const
{
    return uint256S("1");
}

void DescriptorScriptPubKeyMan::SetInternal(bool internal)
{
}

bool DescriptorScriptPubKeyMan::AddDescriptor(std::unique_ptr<Descriptor> desc, bool internal)
{
    LOCK(cs_desc_man);
    if (!desc) {
        LogPrintf("DescriptorScriptPubKeyMan::AddDescriptor: Received null descriptor\n");
        return false;
    }

    LogPrintf("DescriptorScriptPubKeyMan::AddDescriptor: Adding descriptor %s\n", desc->ToString());
    
    DescriptorInfo info;
    info.internal = internal;
    
    // Initial expansion to fill cache
    std::vector<CScript> scripts;
    FlatSigningProvider out;
    try {
        for (int i = 0; i < 100; ++i) {
            if (desc->Expand(i, *this, scripts, out)) {
                if (!scripts.empty()) {
                    info.cache[i] = scripts[0];
                }
            }
        }
    } catch (...) {
        LogPrintf("DescriptorScriptPubKeyMan::AddDescriptor: Exception during initial expansion\n");
    }

    info.descriptor = std::move(desc);
    
    uint256 id;
    std::string desc_str = info.descriptor->ToString();
    CSHA256().Write((const unsigned char*)desc_str.c_str(), desc_str.size()).Finalize(id.begin());
    m_descriptors[id] = std::move(info);
    LogPrintf("DescriptorScriptPubKeyMan::AddDescriptor: Descriptor added successfully with ID %s\n", id.GetHex());
    return true;
}

const DescriptorScriptPubKeyMan::DescriptorCache& DescriptorScriptPubKeyMan::GetDescriptorCache(const uint256& id) const
{
    LOCK(cs_desc_man);
    return m_descriptors.at(id).cache;
}
