// Copyright (c) 2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/wallet.h>

#include <algorithm>
#include <amount.h>
#include <chain.h>
#include <consensus/validation.h>
#include <crypto/ripemd160.h>
#include <interfaces/handler.h>
#include <net.h>
#include <key_io.h>
#include <outputtype.h>
#include <policy/feerate.h>
#include <policy/fees.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <script/ismine.h>
#include <script/standard.h>
#include <support/allocators/secure.h>
#include <sync.h>
#include <timedata.h>
#include <ui_interface.h>
#include <uint256.h>
#include <validation.h>
#include <wallet/feebumper.h>
#include <wallet/fees.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>
#include <wallet/walletutil.h>

namespace interfaces {
const uint32_t BIP32_HARDENED_KEY_LIMIT = 0x80000000;
namespace {

class PendingWalletTxImpl : public PendingWalletTx
{
public:
    PendingWalletTxImpl(CWallet& wallet) : m_wallet(wallet), m_key(&wallet) {}

    const CTransaction& get() override { return *m_tx; }

    int64_t getVirtualSize() override { return GetVirtualTransactionSize(*m_tx); }

    bool commit(WalletValueMap value_map,
        WalletOrderForm order_form,
        std::string from_account,
        std::string& reject_reason) override
    {
        LOCK2(cs_main, m_wallet.cs_wallet);
        CValidationState state;
        if (!m_wallet.CommitTransaction(m_tx, std::move(value_map), std::move(order_form), std::move(from_account), m_key, g_connman.get(), state)) {
            reject_reason = state.GetRejectReason();
            return false;
        }
        return true;
    }

    CTransactionRef m_tx;
    CWallet& m_wallet;
    CReserveKey m_key;
};

//! Construct wallet tx struct.
WalletTx MakeWalletTx(CWallet& wallet, const CWalletTx& wtx)
{
    WalletTx result;
    result.tx = wtx.tx;
    result.txin_is_mine.reserve(wtx.tx->vin.size());
    for (const auto& txin : wtx.tx->vin) {
        result.txin_is_mine.emplace_back(wallet.IsMine(txin));
    }
    result.txout_is_mine.reserve(wtx.tx->vout.size());
    result.txout_address.reserve(wtx.tx->vout.size());
    result.txout_address_is_mine.reserve(wtx.tx->vout.size());
    for (const auto& txout : wtx.tx->vout) {
        result.txout_is_mine.emplace_back(wallet.IsMine(txout));
        result.txout_address.emplace_back();
        result.txout_address_is_mine.emplace_back(ExtractDestination(txout.scriptPubKey, result.txout_address.back()) ?
                                                      IsMine(wallet, result.txout_address.back()) :
                                                      ISMINE_NO);
    }
    result.credit = wtx.GetCredit(ISMINE_ALL);
    result.debit = wtx.GetDebit(ISMINE_ALL);
    result.change = wtx.GetChange();
    result.time = wtx.GetTxTime();
    result.value_map = wtx.mapValue;
    result.is_coinbase = wtx.IsCoinBase();
    return result;
}

//! Construct wallet tx status struct.
WalletTxStatus MakeWalletTxStatus(const CWalletTx& wtx)
{
    WalletTxStatus result;
    auto mi = ::mapBlockIndex.find(wtx.hashBlock);
    CBlockIndex* block = mi != ::mapBlockIndex.end() ? mi->second : nullptr;
    result.block_height = (block ? block->nHeight : std::numeric_limits<int>::max());
    result.blocks_to_maturity = wtx.GetBlocksToMaturity();
    result.depth_in_main_chain = wtx.GetDepthInMainChain();
    result.time_received = wtx.nTimeReceived;
    result.lock_time = wtx.tx->nLockTime;
    result.is_final = CheckFinalTx(*wtx.tx);
    result.is_trusted = wtx.IsTrusted();
    result.is_abandoned = wtx.isAbandoned();
    result.is_coinbase = wtx.IsCoinBase();
    result.is_in_main_chain = wtx.IsInMainChain();
    return result;
}

//! Construct wallet TxOut struct.
WalletTxOut MakeWalletTxOut(CWallet& wallet, const CWalletTx& wtx, int n, int depth)
{
    WalletTxOut result;
    result.txout = wtx.tx->vout[n];
    result.time = wtx.GetTxTime();
    result.depth_in_main_chain = depth;
    result.is_spent = wallet.IsSpent(wtx.GetHash(), n);
    return result;
}

class WalletImpl : public Wallet
{
public:
    WalletImpl(const std::shared_ptr<CWallet>& wallet) : m_shared_wallet(wallet), m_wallet(*wallet.get()) {}

    bool encryptWallet(const SecureString& wallet_passphrase) override
    {
        return m_wallet.EncryptWallet(wallet_passphrase);
    }
    bool isCrypted() override { return m_wallet.IsCrypted(); }
    bool lock() override { return m_wallet.Lock(); }
    bool unlock(const SecureString& wallet_passphrase) override { return m_wallet.Unlock(wallet_passphrase); }
    bool isLocked() override { return m_wallet.IsLocked(); }
    bool changeWalletPassphrase(const SecureString& old_wallet_passphrase,
        const SecureString& new_wallet_passphrase) override
    {
        return m_wallet.ChangeWalletPassphrase(old_wallet_passphrase, new_wallet_passphrase);
    }
    void abortRescan() override { m_wallet.AbortRescan(); }
    bool addScript(const CScript& script) override
    {
        LOCK(m_wallet.cs_wallet);
        return m_wallet.AddCScript(script);
    }
    bool isColdStakingDestination(const CTxDestination& dest) override
    {
        LOCK(m_wallet.cs_wallet);
        const CScript output = GetScriptForDestination(dest);
        txnouttype type;
        std::vector<std::vector<unsigned char>> solutions;
        if (!Solver(output, type, solutions) || type != TX_WITNESS_V0_SCRIPTHASH || solutions.size() != 1) return false;
        uint160 scriptId;
        CRIPEMD160().Write(solutions[0].data(), solutions[0].size()).Finalize(scriptId.begin());
        CScript witnessScript;
        CKeyID stakingKey, ownerKey;
        return m_wallet.GetCScript(CScriptID(scriptId), witnessScript) &&
               MatchColdStakingScript(witnessScript, stakingKey, ownerKey);
    }
    bool importMnemonicSeed(const std::vector<unsigned char>& seed_bytes, const MnemonicImportOptions& options) override
    {
        LOCK2(cs_main, m_wallet.cs_wallet);

        // Legacy wallets (non-HD) must be upgraded before importing an HD seed.
        if (!m_wallet.IsHDEnabled()) {
            if (!m_wallet.CanSupportFeature(FEATURE_HD)) {
                LogPrintf("importMnemonicSeed: wallet does not support HD feature\n");
                return false;
            }
            m_wallet.SetMinVersion(FEATURE_HD);
            LogPrintf("importMnemonicSeed: upgraded legacy wallet to HD\n");
        }

        CExtKey masterKey;
        masterKey.SetSeed(seed_bytes.data(), seed_bytes.size());

        if (options.use_bip44) {
            try {
                WalletBatch batch(m_wallet.GetDatabase());
                CKeyID seed_id = masterKey.key.GetPubKey().GetID();

                CExtKey purposeKey;
                if (!masterKey.Derive(purposeKey, 44 | BIP32_HARDENED_KEY_LIMIT)) {
                    LogPrintf("importMnemonicSeed: BIP44 purpose key derivation failed\n");
                    return false;
                }

                CExtKey coinTypeKey;
                if (!purposeKey.Derive(coinTypeKey, options.bip44_coin_type | BIP32_HARDENED_KEY_LIMIT)) {
                    LogPrintf("importMnemonicSeed: BIP44 coin type key derivation failed\n");
                    return false;
                }

                CExtKey accountKey;
                if (!coinTypeKey.Derive(accountKey, 0 | BIP32_HARDENED_KEY_LIMIT)) {
                    LogPrintf("importMnemonicSeed: BIP44 account key derivation failed\n");
                    return false;
                }

                const uint32_t gap_limit = std::max<uint32_t>(1, std::min(options.gap_limit, 2000u));
                int imported = 0;
                int new_keys = 0;

                for (int change = 0; change <= 1; ++change) {
                    CExtKey changeKey;
                    if (!accountKey.Derive(changeKey, change)) {
                        LogPrintf("importMnemonicSeed: BIP44 change key derivation failed for change=%d\n", change);
                        continue;
                    }

                    for (uint32_t i = 0; i < gap_limit; ++i) {
                        CExtKey childKey;
                        if (!changeKey.Derive(childKey, i)) {
                            LogPrintf("importMnemonicSeed: BIP44 child key derivation failed for index=%u\n", i);
                            continue;
                        }

                        CKey key = childKey.key;
                        CPubKey pubkey = key.GetPubKey();

                        CKeyMetadata metadata;
                        metadata.nCreateTime = 1;
                        metadata.hd_seed_id = seed_id;
                        metadata.hdKeypath = "m/44'/" + std::to_string(options.bip44_coin_type) + "'/0'/" +
                            std::to_string(change) + "/" + std::to_string(i);

                        if (m_wallet.HaveKey(pubkey.GetID())) {
                            m_wallet.LearnAllRelatedScripts(pubkey);
                            ++imported;
                            continue;
                        }

                        m_wallet.LoadKeyMetadata(pubkey.GetID(), metadata);

                        if (!m_wallet.AddKeyPubKeyWithDB(batch, key, pubkey)) {
                            LogPrintf("importMnemonicSeed: AddKeyPubKeyWithDB failed for %s\n", metadata.hdKeypath);
                            continue;
                        }

                        m_wallet.LearnAllRelatedScripts(pubkey);
                        for (const auto& dest : GetAllDestinationsForKey(pubkey)) {
                            m_wallet.SetAddressBook(dest, "", "receive");
                        }
                        ++imported;
                        ++new_keys;
                    }
                }

                if (new_keys == 0) {
                    LogPrintf("importMnemonicSeed: BIP44 import did not add any new keys\n");
                    return false;
                }

                m_wallet.MarkDirty();
                LogPrintf("importMnemonicSeed: BIP44 import complete — %d keys processed (%d new)\n", imported, new_keys);
            } catch (const std::exception& e) {
                LogPrintf("importMnemonicSeed (BIP44): unexpected error — %s\n", e.what());
                return false;
            }
            return true;
        }

        CKey key = masterKey.key;

        try {
            CPubKey master_pub_key = m_wallet.DeriveNewSeed(key);
            m_wallet.SetHDSeed(master_pub_key);
            m_wallet.NewKeyPool();
            m_wallet.MarkDirty();
        } catch (const std::runtime_error& e) {
            LogPrintf("importMnemonicSeed: failed — %s\n", e.what());
            return false;
        } catch (const std::exception& e) {
            LogPrintf("importMnemonicSeed: unexpected error — %s\n", e.what());
            return false;
        }
        return true;
    }
    int64_t rescanFromTime(int64_t start_time) override
    {
        WalletRescanReserver reserver(&m_wallet);
        if (!reserver.reserve()) {
            return 0;
        }
        return m_wallet.RescanFromTime(start_time, reserver, true /* update */);
    }
    bool rescanBlockchain(int start_height, int stop_height) override
    {
        WalletRescanReserver reserver(&m_wallet);
        if (!reserver.reserve()) {
            LogPrintf("rescanBlockchain: wallet rescan already in progress\n");
            return false;
        }

        CBlockIndex* pindexStart = nullptr;
        CBlockIndex* pindexStop = nullptr;
        CBlockIndex* pChainTip = nullptr;
        {
            LOCK(cs_main);
            pindexStart = chainActive.Genesis();
            pChainTip = chainActive.Tip();
            if (!pindexStart || !pChainTip) {
                return false;
            }

            if (start_height > 0) {
                pindexStart = chainActive[start_height];
                if (!pindexStart) {
                    LogPrintf("rescanBlockchain: invalid start_height %d\n", start_height);
                    return false;
                }
            }

            if (stop_height >= 0) {
                pindexStop = chainActive[stop_height];
                if (!pindexStop) {
                    LogPrintf("rescanBlockchain: invalid stop_height %d\n", stop_height);
                    return false;
                }
                if (pindexStop->nHeight < pindexStart->nHeight) {
                    LogPrintf("rescanBlockchain: stop_height before start_height\n");
                    return false;
                }
            }
        }

        if (fPruneMode) {
            LOCK(cs_main);
            CBlockIndex* block = pindexStop ? pindexStop : pChainTip;
            while (block && block->nHeight >= pindexStart->nHeight) {
                if (!(block->nStatus & BLOCK_HAVE_DATA)) {
                    LogPrintf("rescanBlockchain: cannot rescan beyond pruned blocks\n");
                    return false;
                }
                block = block->pprev;
            }
        }

        CBlockIndex* stopBlock = m_wallet.ScanForWalletTransactions(pindexStart, pindexStop, reserver, true);
        if (m_wallet.IsAbortingRescan()) {
            LogPrintf("rescanBlockchain: rescan aborted by user\n");
            return false;
        }
        if (stopBlock) {
            LogPrintf("rescanBlockchain: rescan stopped early at height %d\n", stopBlock->nHeight);
            return false;
        }
        return true;
    }
    bool backupWallet(const std::string& filename) override { return m_wallet.BackupWallet(filename); }
    std::string getWalletName() override { return m_wallet.GetName(); }
    bool getKeyFromPool(bool internal, CPubKey& pub_key) override
    {
        return m_wallet.GetKeyFromPool(pub_key, internal);
    }
    bool getPubKey(const CKeyID& address, CPubKey& pub_key) override { return m_wallet.GetPubKey(address, pub_key); }
    bool getPrivKey(const CKeyID& address, CKey& key) override { return m_wallet.GetKey(address, key); }
    bool isSpendable(const CTxDestination& dest) override { return IsMine(m_wallet, dest) & ISMINE_SPENDABLE; }
    bool haveWatchOnly() override { return m_wallet.HaveWatchOnly(); };
    bool setAddressBook(const CTxDestination& dest, const std::string& name, const std::string& purpose) override
    {
        return m_wallet.SetAddressBook(dest, name, purpose);
    }
    bool delAddressBook(const CTxDestination& dest) override
    {
        return m_wallet.DelAddressBook(dest);
    }
    bool getAddress(const CTxDestination& dest,
        std::string* name,
        isminetype* is_mine,
        std::string* purpose) override
    {
        LOCK(m_wallet.cs_wallet);
        auto it = m_wallet.mapAddressBook.find(dest);
        if (it == m_wallet.mapAddressBook.end()) {
            return false;
        }
        if (name) {
            *name = it->second.name;
        }
        if (is_mine) {
            *is_mine = IsMine(m_wallet, dest);
        }
        if (purpose) {
            *purpose = it->second.purpose;
        }
        return true;
    }
    std::vector<WalletAddress> getAddresses() override
    {
        LOCK(m_wallet.cs_wallet);
        std::vector<WalletAddress> result;
        for (const auto& item : m_wallet.mapAddressBook) {
            result.emplace_back(item.first, IsMine(m_wallet, item.first), item.second.name, item.second.purpose);
        }
        return result;
    }
    void learnRelatedScripts(const CPubKey& key, OutputType type) override { m_wallet.LearnRelatedScripts(key, type); }
    bool addDestData(const CTxDestination& dest, const std::string& key, const std::string& value) override
    {
        LOCK(m_wallet.cs_wallet);
        return m_wallet.AddDestData(dest, key, value);
    }
    bool eraseDestData(const CTxDestination& dest, const std::string& key) override
    {
        LOCK(m_wallet.cs_wallet);
        return m_wallet.EraseDestData(dest, key);
    }
    std::vector<std::string> getDestValues(const std::string& prefix) override
    {
        return m_wallet.GetDestValues(prefix);
    }
    void lockCoin(const COutPoint& output) override
    {
        LOCK2(cs_main, m_wallet.cs_wallet);
        return m_wallet.LockCoin(output);
    }
    void unlockCoin(const COutPoint& output) override
    {
        LOCK2(cs_main, m_wallet.cs_wallet);
        return m_wallet.UnlockCoin(output);
    }
    bool isLockedCoin(const COutPoint& output) override
    {
        LOCK2(cs_main, m_wallet.cs_wallet);
        return m_wallet.IsLockedCoin(output.hash, output.n);
    }
    bool isSpent(const uint256& hash, unsigned int n) override
    {
        LOCK2(cs_main, m_wallet.cs_wallet);
        return m_wallet.IsSpent(hash, n);
    }
    void listLockedCoins(std::vector<COutPoint>& outputs) override
    {
        LOCK2(cs_main, m_wallet.cs_wallet);
        return m_wallet.ListLockedCoins(outputs);
    }
    std::unique_ptr<PendingWalletTx> createTransaction(const std::vector<CRecipient>& recipients,
        const CCoinControl& coin_control,
        bool sign,
        int& change_pos,
        CAmount& fee,
        std::string& fail_reason) override
    {
        LOCK2(cs_main, m_wallet.cs_wallet);
        auto pending = MakeUnique<PendingWalletTxImpl>(m_wallet);
        if (!m_wallet.CreateTransaction(recipients, pending->m_tx, pending->m_key, fee, change_pos,
                fail_reason, coin_control, sign)) {
            return {};
        }
        return std::move(pending);
    }
    bool transactionCanBeAbandoned(const uint256& txid) override { return m_wallet.TransactionCanBeAbandoned(txid); }
    bool abandonTransaction(const uint256& txid) override
    {
        LOCK2(cs_main, m_wallet.cs_wallet);
        return m_wallet.AbandonTransaction(txid);
    }
    bool transactionCanBeBumped(const uint256& txid) override
    {
        return feebumper::TransactionCanBeBumped(&m_wallet, txid);
    }
    bool createBumpTransaction(const uint256& txid,
        const CCoinControl& coin_control,
        CAmount total_fee,
        std::vector<std::string>& errors,
        CAmount& old_fee,
        CAmount& new_fee,
        CMutableTransaction& mtx) override
    {
        return feebumper::CreateTransaction(&m_wallet, txid, coin_control, total_fee, errors, old_fee, new_fee, mtx) ==
               feebumper::Result::OK;
    }
    bool signBumpTransaction(CMutableTransaction& mtx) override { return feebumper::SignTransaction(&m_wallet, mtx); }
    bool commitBumpTransaction(const uint256& txid,
        CMutableTransaction&& mtx,
        std::vector<std::string>& errors,
        uint256& bumped_txid) override
    {
        return feebumper::CommitTransaction(&m_wallet, txid, std::move(mtx), errors, bumped_txid) ==
               feebumper::Result::OK;
    }
    CTransactionRef getTx(const uint256& txid) override
    {
        LOCK2(::cs_main, m_wallet.cs_wallet);
        auto mi = m_wallet.mapWallet.find(txid);
        if (mi != m_wallet.mapWallet.end()) {
            return mi->second.tx;
        }
        return {};
    }
    WalletTx getWalletTx(const uint256& txid) override
    {
        LOCK2(::cs_main, m_wallet.cs_wallet);
        auto mi = m_wallet.mapWallet.find(txid);
        if (mi != m_wallet.mapWallet.end()) {
            return MakeWalletTx(m_wallet, mi->second);
        }
        return {};
    }
    std::vector<WalletTx> getWalletTxs() override
    {
        LOCK2(::cs_main, m_wallet.cs_wallet);
        std::vector<WalletTx> result;
        result.reserve(m_wallet.mapWallet.size());
        for (const auto& entry : m_wallet.mapWallet) {
            result.emplace_back(MakeWalletTx(m_wallet, entry.second));
        }
        return result;
    }
    bool tryGetTxStatus(const uint256& txid,
        interfaces::WalletTxStatus& tx_status,
        int& num_blocks,
        int64_t& adjusted_time) override
    {
        TRY_LOCK(::cs_main, locked_chain);
        if (!locked_chain) {
            return false;
        }
        TRY_LOCK(m_wallet.cs_wallet, locked_wallet);
        if (!locked_wallet) {
            return false;
        }
        auto mi = m_wallet.mapWallet.find(txid);
        if (mi == m_wallet.mapWallet.end()) {
            return false;
        }
        num_blocks = ::chainActive.Height();
        adjusted_time = GetAdjustedTime();
        tx_status = MakeWalletTxStatus(mi->second);
        return true;
    }
    WalletTx getWalletTxDetails(const uint256& txid,
        WalletTxStatus& tx_status,
        WalletOrderForm& order_form,
        bool& in_mempool,
        int& num_blocks,
        int64_t& adjusted_time) override
    {
        LOCK2(::cs_main, m_wallet.cs_wallet);
        auto mi = m_wallet.mapWallet.find(txid);
        if (mi != m_wallet.mapWallet.end()) {
            num_blocks = ::chainActive.Height();
            adjusted_time = GetAdjustedTime();
            in_mempool = mi->second.InMempool();
            order_form = mi->second.vOrderForm;
            tx_status = MakeWalletTxStatus(mi->second);
            return MakeWalletTx(m_wallet, mi->second);
        }
        return {};
    }
    WalletBalances getBalances() override
    {
        WalletBalances result;
        result.balance = m_wallet.GetBalance();
        result.unconfirmed_balance = m_wallet.GetUnconfirmedBalance();
        result.immature_balance = m_wallet.GetImmatureBalance();
        result.have_watch_only = m_wallet.HaveWatchOnly();
        if (result.have_watch_only) {
            result.watch_only_balance = m_wallet.GetBalance(ISMINE_WATCH_ONLY);
            result.unconfirmed_watch_only_balance = m_wallet.GetUnconfirmedWatchOnlyBalance();
            result.immature_watch_only_balance = m_wallet.GetImmatureWatchOnlyBalance();
        }
        return result;
    }
    bool tryGetBalances(WalletBalances& balances, int& num_blocks) override
    {
        TRY_LOCK(cs_main, locked_chain);
        if (!locked_chain) return false;
        TRY_LOCK(m_wallet.cs_wallet, locked_wallet);
        if (!locked_wallet) {
            return false;
        }
        balances = getBalances();
        num_blocks = ::chainActive.Height();
        return true;
    }
    CAmount getBalance() override { return m_wallet.GetBalance(); }
    CAmount getAvailableBalance(const CCoinControl& coin_control) override
    {
        return m_wallet.GetAvailableBalance(&coin_control);
    }
    isminetype txinIsMine(const CTxIn& txin) override
    {
        LOCK2(::cs_main, m_wallet.cs_wallet);
        return m_wallet.IsMine(txin);
    }
    isminetype txoutIsMine(const CTxOut& txout) override
    {
        LOCK2(::cs_main, m_wallet.cs_wallet);
        return m_wallet.IsMine(txout);
    }
    CAmount getDebit(const CTxIn& txin, isminefilter filter) override
    {
        LOCK2(::cs_main, m_wallet.cs_wallet);
        return m_wallet.GetDebit(txin, filter);
    }
    CAmount getCredit(const CTxOut& txout, isminefilter filter) override
    {
        LOCK2(::cs_main, m_wallet.cs_wallet);
        return m_wallet.GetCredit(txout, filter);
    }
    CoinsList listCoins() override
    {
        LOCK2(::cs_main, m_wallet.cs_wallet);
        CoinsList result;
        for (const auto& entry : m_wallet.ListCoins()) {
            auto& group = result[entry.first];
            for (const auto& coin : entry.second) {
                group.emplace_back(
                    COutPoint(coin.tx->GetHash(), coin.i), MakeWalletTxOut(m_wallet, *coin.tx, coin.i, coin.nDepth));
            }
        }
        return result;
    }
    std::vector<WalletTxOut> getCoins(const std::vector<COutPoint>& outputs) override
    {
        LOCK2(::cs_main, m_wallet.cs_wallet);
        std::vector<WalletTxOut> result;
        result.reserve(outputs.size());
        for (const auto& output : outputs) {
            result.emplace_back();
            auto it = m_wallet.mapWallet.find(output.hash);
            if (it != m_wallet.mapWallet.end()) {
                int depth = it->second.GetDepthInMainChain();
                if (depth >= 0) {
                    result.back() = MakeWalletTxOut(m_wallet, it->second, output.n, depth);
                }
            }
        }
        return result;
    }
    CAmount getRequiredFee(unsigned int tx_bytes) override { return GetRequiredFee(m_wallet, tx_bytes); }
    CAmount getMinimumFee(unsigned int tx_bytes,
        const CCoinControl& coin_control,
        int* returned_target,
        FeeReason* reason) override
    {
        FeeCalculation fee_calc;
        CAmount result;
        result = GetMinimumFee(m_wallet, tx_bytes, coin_control, ::mempool, ::feeEstimator, &fee_calc);
        if (returned_target) *returned_target = fee_calc.returnedTarget;
        if (reason) *reason = fee_calc.reason;
        return result;
    }
    unsigned int getConfirmTarget() override { return m_wallet.m_confirm_target; }
    bool hdEnabled() override { return m_wallet.IsHDEnabled(); }
    bool IsWalletFlagSet(uint64_t flag) override { return m_wallet.IsWalletFlagSet(flag); }
    OutputType getDefaultAddressType() override { return m_wallet.m_default_address_type; }
    OutputType getDefaultChangeType() override { return m_wallet.m_default_change_type; }
    bool isLegacy() override { 
        return m_wallet.GetDatabase().Format() == "berkeley"; 
    }
    std::string databaseFormat() override { return m_wallet.GetDatabase().Format(); }
    bool isEncryptedAtRest() override { return m_wallet.GetDatabase().EncryptedAtRest(); }
    bool isDescriptor() override { return m_wallet.IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS); }
    std::unique_ptr<Handler> handleUnload(UnloadFn fn) override
    {
        return MakeHandler(m_wallet.NotifyUnload.connect(fn));
    }
    std::unique_ptr<Handler> handleShowProgress(ShowProgressFn fn) override
    {
        return MakeHandler(m_wallet.ShowProgress.connect(fn));
    }
    std::unique_ptr<Handler> handleStatusChanged(StatusChangedFn fn) override
    {
        return MakeHandler(m_wallet.NotifyStatusChanged.connect([fn](CCryptoKeyStore*) { fn(); }));
    }
    std::unique_ptr<Handler> handleAddressBookChanged(AddressBookChangedFn fn) override
    {
        return MakeHandler(m_wallet.NotifyAddressBookChanged.connect(
            [fn](CWallet*, const CTxDestination& address, const std::string& label, bool is_mine,
                const std::string& purpose, ChangeType status) { fn(address, label, is_mine, purpose, status); }));
    }
    std::unique_ptr<Handler> handleTransactionChanged(TransactionChangedFn fn) override
    {
        return MakeHandler(m_wallet.NotifyTransactionChanged.connect(
            [fn](CWallet*, const uint256& txid, ChangeType status) { fn(txid, status); }));
    }
    std::unique_ptr<Handler> handleWatchOnlyChanged(WatchOnlyChangedFn fn) override
    {
        return MakeHandler(m_wallet.NotifyWatchonlyChanged.connect(fn));
    }
    bool setRewardDistributionPcts(const std::vector<std::pair<std::string, std::uint8_t>>& pcts) override
    {
        return m_wallet.SetRewardDistributionPcts(pcts);
    }
    bool delRewardDistributionPcts() override
    {
        return m_wallet.DelRewardDistributionPcts();
    }
    std::vector<std::pair<std::string, std::uint8_t>> getRewardDistributionPcts() override
    {
        LOCK(m_wallet.cs_wallet);
        return m_wallet.vRewardDistributionPcts;
    }

    std::shared_ptr<CWallet> m_shared_wallet;
    CWallet& m_wallet;
};

} // namespace

std::unique_ptr<Wallet> MakeWallet(const std::shared_ptr<CWallet>& wallet) { return MakeUnique<WalletImpl>(wallet); }

} // namespace interfaces
