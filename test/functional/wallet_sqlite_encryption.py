#!/usr/bin/env python3
# Copyright (c) 2026 The XPChain developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test SQLite wallet encryption, passphrase change (SQLCipher rekey), and loadwallet."""

import os

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)
from test_framework.test_node import ErrorMatch


class WalletSQLiteEncryptionTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def run_test(self):
        node = self.nodes[0]

        self.log.info("createwallet with passphrase encrypts on create (no shutdown)")
        create_pass = "create-on-encrypt-pass"
        node.createwallet("enc_on_create", False, False, create_pass)
        with open(os.path.join(node.datadir, "regtest", "debug.log"), encoding="utf8") as debug_log:
            assert create_pass not in debug_log.read(), "createwallet passphrase must never be written to debug.log"
        created = node.get_wallet_rpc("enc_on_create")
        created_info = created.getwalletinfo()
        assert_equal(created_info["databaseformat"], "sqlite")
        assert "unlocked_until" in created_info
        assert_equal(created_info["unlocked_until"], 0)
        if created_info.get("sqlcipher", False):
            assert_equal(created_info["encrypted_at_rest"], True)
        else:
            assert_equal(created_info["encrypted_at_rest"], False)
        assert_raises_rpc_error(-14, "wallet passphrase entered was incorrect",
                                created.walletpassphrase, "wrong-pass", 10)
        created.walletpassphrase(create_pass, 60)
        created.getnewaddress()
        created.walletlock()

        self.log.info("Create SQLite wallet for encryption test")
        node.createwallet("encrypted.sqlite")
        wallet = node.get_wallet_rpc("encrypted.sqlite")

        info = wallet.getwalletinfo()
        assert_equal(info["databaseformat"], "sqlite")
        assert_equal(info["encrypted_at_rest"], False)

        if not info.get("sqlcipher", False):
            self.log.warning("Skipping at-rest encryption checks: build without SQLCipher")
            return

        if created_info.get("sqlcipher", False):
            enc_path = os.path.join(node.datadir, "regtest", "wallets", "enc_on_create", "wallet.dat")
            with open(enc_path, "rb") as f:
                header = f.read(16)
            assert not header.startswith(b"SQLite format 3"), \
                "createwallet passphrase should SQLCipher-encrypt the wallet file"

        self.log.info("Encrypt SQLite wallet and verify at-rest protection")
        passphrase = "sqlite-at-rest-passphrase"
        address = wallet.getnewaddress("", "legacy")
        privkey = wallet.dumpprivkey(address)

        wallet_path = os.path.join(node.datadir, "regtest", "wallets", "encrypted.sqlite")
        assert os.path.isfile(wallet_path)

        with open(wallet_path, "rb") as f:
            plain_header = f.read(16)
        assert plain_header.startswith(b"SQLite format 3"), "New SQLite wallet should start as plain SQLite"

        # encryptwallet shuts down the node after rewriting the wallet file.
        wallet.encryptwallet(passphrase)
        self.wait_for_node_exit(0, timeout=60)

        with open(wallet_path, "rb") as f:
            encrypted_header = f.read(16)
        assert not encrypted_header.startswith(b"SQLite format 3"), "Encrypted wallet file must not be readable as plain SQLite"

        self.log.info("Restart without -walletdbpassphrase must fail (daemon cannot prompt)")
        self.nodes[0].assert_start_raises_init_error(
            ['-wallet=encrypted.sqlite'],
            'requires -walletdbpassphrase',
            match=ErrorMatch.PARTIAL_REGEX,
        )

        self.log.info("Restart node with correct passphrase")
        self.start_node(0, [
            '-walletdbpassphrase=' + passphrase,
            '-wallet=encrypted.sqlite',
        ])
        wallet = node.get_wallet_rpc("encrypted.sqlite")
        assert_raises_rpc_error(-13, "Please enter the wallet passphrase", wallet.dumpprivkey, address)
        wallet.walletpassphrase(passphrase, 60)
        assert_equal(wallet.dumpprivkey(address), privkey)

        # --- walletpassphrasechange + SQLCipher rekey ---
        passphrase2 = "new-at-rest-passphrase"

        self.log.info("Change passphrase (triggers SQLCipher rekey)")
        wallet.walletpassphrasechange(passphrase, passphrase2)

        self.log.info("Verify new passphrase works for app-layer unlock")
        assert_raises_rpc_error(-14, "wallet passphrase entered was incorrect",
                                wallet.walletpassphrase, passphrase, 10)
        wallet.walletpassphrase(passphrase2, 60)
        assert_equal(wallet.dumpprivkey(address), privkey)
        wallet.walletlock()

        self.log.info("Stop node and verify old passphrase no longer opens the DB file")
        self.stop_node(0)
        self.nodes[0].assert_start_raises_init_error(
            ['-walletdbpassphrase=' + passphrase, '-wallet=encrypted.sqlite'],
            r'file is not a database|Failed to create table',
            match=ErrorMatch.PARTIAL_REGEX,
        )

        self.log.info("Restart with new passphrase after rekey")
        self.start_node(0, [
            '-walletdbpassphrase=' + passphrase2,
            '-wallet=encrypted.sqlite',
        ])
        wallet = node.get_wallet_rpc("encrypted.sqlite")
        wallet.walletpassphrase(passphrase2, 60)
        assert_equal(wallet.dumpprivkey(address), privkey)

        self.log.info("Confirm wallet file is still not plaintext SQLite")
        with open(wallet_path, "rb") as f:
            rekeyed_header = f.read(16)
        assert not rekeyed_header.startswith(b"SQLite format 3"), "Rekeyed wallet must remain encrypted"

        # --- loadwallet with per-wallet dbpassphrase ---
        self.log.info("Unload and reload encrypted wallet via loadwallet dbpassphrase")
        wallet.unloadwallet()
        self.stop_node(0)
        self.start_node(0)  # no -walletdbpassphrase, no -wallet

        assert_raises_rpc_error(
            -4,
            "requires dbpassphrase argument or -walletdbpassphrase",
            node.loadwallet,
            "encrypted.sqlite",
        )
        node.loadwallet("encrypted.sqlite", passphrase2)
        wallet = node.get_wallet_rpc("encrypted.sqlite")
        wallet.walletpassphrase(passphrase2, 60)
        assert_equal(wallet.dumpprivkey(address), privkey)


if __name__ == '__main__':
    WalletSQLiteEncryptionTest().main()
