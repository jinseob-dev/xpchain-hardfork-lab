#!/usr/bin/env python3
# Copyright (c) 2026 The XPChain Community developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""End-to-end regression test for the XPChain proof-of-stake switch.

This is the P0-4 safety net from doc/architecture-separation-roadmap.md. It covers the
parts of proof-of-stake that need real chain state and therefore cannot be reached from
src/test/pos_tests.cpp: CheckProofOfStake, IsCoinStakeTx, the coinstake/coinbase pairing
rules, the block signature soft fork, and the reward paid by ConnectBlock.

The scenario is:

  1. mine the whole proof-of-work range on regtest (heights 1..nSwitchHeight),
  2. show that a proof-of-work block is rejected above the switch height,
  3. let the built-in minter produce the first proof-of-stake block,
  4. check the structural rules from doc/xpchain-pos-consensus.md against that block,
  5. check that a second node that did not create the block accepts it over p2p,
  6. check that the block still validates after -reindex (the on-disk PoS read path).

Steps 5 and 6 are what make this useful for the staged modernization: they revalidate a
proof-of-stake block through the network and the disk paths, not just the mining path.
"""

from decimal import Decimal
import os
import shutil

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
    connect_nodes,
    sync_blocks,
    wait_until,
)

# Must match consensus.nSwitchHeight for regtest in src/chainparams.cpp.
REGTEST_SWITCH_HEIGHT = 1680
# Must match consensus.nStakeMinAge for regtest in src/chainparams.cpp.
REGTEST_STAKE_MIN_AGE = 10


class PoSStakingTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        # Node 0 stakes, node 1 only validates. -txindex keeps CheckProofOfStake on the
        # indexed lookup path rather than the slow UTXO fallback.
        self.extra_args = [
            ['-txindex', '-minting=0'],
            ['-txindex', '-minting=0'],
        ]

    def setup_network(self):
        self.setup_nodes()
        connect_nodes(self.nodes[0], 1)

    def mine_pow_range(self, taproot_address, cold_address, contract):
        """Mine heights 1..nSwitchHeight, which is the whole proof-of-work range."""
        self.log.info("Mining the proof-of-work range up to height %d", REGTEST_SWITCH_HEIGHT)
        # generatetoaddress is capped per call by the RPC's own retry budget, so mine in
        # chunks to keep each call short.
        taproot_blocks = 200
        self.nodes[0].generatetoaddress(taproot_blocks, taproot_address)
        withdrawal_txid = self.check_delegation_split_rpc(contract)
        remaining = REGTEST_SWITCH_HEIGHT - taproot_blocks
        while remaining > 0:
            batch = min(remaining, 250)
            self.nodes[0].generatetoaddress(batch, cold_address)
            remaining -= batch
        assert_equal(self.nodes[0].getblockcount(), REGTEST_SWITCH_HEIGHT)
        sync_blocks(self.nodes)
        return withdrawal_txid

    def check_delegation_split_rpc(self, contract):
        """Create one delegation transaction with an amount-dependent split."""
        self.log.info("Checking automatic delegated-staking UTXO splitting")
        imported = self.nodes[0].createcoldstakingaddress(
            contract['owner'], contract['staker'])
        assert_equal(imported['address'], contract['address'])

        options = {
            'maximum_outputs': 4,
            'minimum_output_amount': 1,
        }
        estimate = self.nodes[0].estimatecoldstakingsplit(1000, options)
        assert_greater_than(estimate['output_count'], 1)
        assert_equal(estimate['output_count'], len(estimate['outputs']))
        assert_equal(sum(estimate['outputs']), Decimal('1000'))

        delegation = self.nodes[0].delegatecoldstaking(contract['address'], 1000, options)
        assert_equal(delegation['output_count'], estimate['output_count'])
        assert_equal(sum(delegation['outputs']), Decimal('1000'))
        raw = self.nodes[0].getrawtransaction(delegation['txid'], True)
        contract_outputs = [output for output in raw['vout']
                            if output['scriptPubKey'].get('addresses') == [contract['address']]]
        assert_equal(len(contract_outputs), delegation['output_count'])
        assert_equal(sum(output['value'] for output in contract_outputs), Decimal('1000'))

        listed = self.nodes[0].listcoldstaking()
        assert_greater_than(len(listed['outputs']), 0)
        assert all(output['owner'] for output in listed['outputs'])

        withdrawal_address = self.nodes[0].getnewaddress('cold withdrawal', 'bech32')
        withdrawal = self.nodes[0].withdrawcoldstaking(
            contract['address'], withdrawal_address, 10)
        withdrawn = self.nodes[0].getrawtransaction(withdrawal['txid'], True)
        withdrawn_outputs = [output for output in withdrawn['vout']
                             if output['scriptPubKey'].get('addresses') == [withdrawal_address]]
        assert_equal(sum(output['value'] for output in withdrawn_outputs), Decimal('10'))
        # The empty selector chooses the owner branch, not the delegated staking branch.
        assert_equal(withdrawn['vin'][0]['txinwitness'][-2], '')
        assert_equal(withdrawn['vin'][0]['txinwitness'][-1], contract['redeemScript'])

        # A request for the exact contract balance is a sweep: the fee is taken
        # from the destination amount, so no unusable fee-sized remainder is left.
        sweep_owner = self.nodes[0].getnewaddress('cold sweep owner', 'bech32')
        sweep_contract = self.nodes[0].createcoldstakingaddress(
            sweep_owner, contract['staker'])
        self.nodes[0].delegatecoldstaking(sweep_contract['address'], 25)
        sweep_view = self.nodes[0].listcoldstaking()
        sweep_balance = sum(
            output['amount'] for output in sweep_view['outputs']
            if output['address'] == sweep_contract['address'])
        assert_equal(sweep_balance, Decimal('25'))

        sweep_destination = self.nodes[0].getnewaddress('cold sweep destination', 'bech32')
        sweep = self.nodes[0].withdrawcoldstaking(
            sweep_contract['address'], sweep_destination, sweep_balance)
        assert_equal(sweep['fee_deducted_from_amount'], True)
        assert_equal(sweep['received_amount'], sweep_balance - sweep['fee'])
        swept = self.nodes[0].getrawtransaction(sweep['txid'], True)
        swept_destination_outputs = [
            output for output in swept['vout']
            if output['scriptPubKey'].get('addresses') == [sweep_destination]]
        swept_contract_outputs = [
            output for output in swept['vout']
            if output['scriptPubKey'].get('addresses') == [sweep_contract['address']]]
        assert_equal(sum(output['value'] for output in swept_destination_outputs),
                     sweep['received_amount'])
        assert_equal(swept_contract_outputs, [])
        return withdrawal['txid']

    def check_cold_stake_role_separation(self, contract, withdrawal_txid):
        """Owner and staker wallets must expose mutually exclusive capabilities."""
        self.log.info("Checking delegated owner/staker role separation")

        owner_view = self.nodes[0].listcoldstaking()
        staker_view = self.nodes[1].listcoldstaking()
        assert_greater_than(len(owner_view['outputs']), 0)
        assert_greater_than(len(staker_view['outputs']), 0)
        assert all(output['owner'] and not output['staker']
                   for output in owner_view['outputs'])
        assert all(not output['owner'] and output['staker']
                   for output in staker_view['outputs'])

        # A staking-only wallet can make blocks but must never be able to take the
        # owner branch of the contract, even when it supplies a valid destination.
        assert_raises_rpc_error(
            -4, "This wallet does not contain the cold-staking owner key",
            self.nodes[1].withdrawcoldstaking,
            contract['address'], contract['staker'], 1)

        # The owner withdrawal created before the remaining PoW range was mined must
        # have propagated and become part of the shared chain.
        withdrawal = self.nodes[1].getrawtransaction(withdrawal_txid, True)
        assert_greater_than(withdrawal['confirmations'], 0)

    def assert_pow_rejected_above_switch(self, address):
        """Above the switch height a template needs a coinstake, which mining RPCs lack.

        BlockAssembler used to assert on the missing coinstake, which aborted the daemon
        for any getblocktemplate or generatetoaddress call on a chain past the switch
        height. It must report a plain RPC error instead.
        """
        self.log.info("Checking that proof-of-work mining is refused above the switch height")
        height_before = self.nodes[0].getblockcount()

        assert_raises_rpc_error(-32603, "Couldn't create new block",
                                self.nodes[0].generatetoaddress, 1, address)
        # getblocktemplate maps a failed template to RPC_OUT_OF_MEMORY, as upstream does.
        assert_raises_rpc_error(-7, "Out of memory",
                                self.nodes[0].getblocktemplate,
                                {'rules': ['segwit']})

        # The node is still alive and the tip did not move.
        assert_equal(self.nodes[0].getblockcount(), height_before)

    def stake_one_block(self):
        """Enable minting on node 0 and wait for the first proof-of-stake block."""
        tip_time = self.nodes[0].getblock(self.nodes[0].getbestblockhash())['time']
        # Move the clock past the tip so the candidate coins clear nStakeMinAge. Staying
        # well inside DEFAULT_MAX_TIP_AGE keeps IsInitialBlockDownload() false, which the
        # minter thread requires.
        mocktime = tip_time + REGTEST_STAKE_MIN_AGE + 300
        self.mocktime = mocktime
        for node in self.nodes:
            node.setmocktime(mocktime)

        self.log.info("Restarting node 0 with minting enabled")
        self.restart_node(0, extra_args=['-txindex', '-minting=1', '-mocktime={}'.format(mocktime)])
        connect_nodes(self.nodes[0], 1)
        self.nodes[0].setmocktime(mocktime)

        wait_until(lambda: self.nodes[0].getblockcount() > REGTEST_SWITCH_HEIGHT, timeout=180)
        staked_hash = self.nodes[0].getblockhash(REGTEST_SWITCH_HEIGHT + 1)

        # Stop staking so the rest of the test runs against a fixed tip.
        self.log.info("Restarting node 0 with minting disabled")
        self.restart_node(0, extra_args=['-txindex', '-minting=0', '-mocktime={}'.format(mocktime)])
        connect_nodes(self.nodes[0], 1)
        self.nodes[0].setmocktime(mocktime)
        return staked_hash

    def check_pos_block_structure(self, block_hash):
        """Assert the rules from doc/xpchain-pos-consensus.md sections 2 and 5."""
        self.log.info("Checking the structure of the first proof-of-stake block")
        block = self.nodes[0].getblock(block_hash, 2)
        assert_equal(block['height'], REGTEST_SWITCH_HEIGHT + 1)

        # Section 2: a PoS block carries a coinbase plus a coinstake.
        assert_greater_than(len(block['tx']), 1)
        coinbase, coinstake = block['tx'][0], block['tx'][1]

        # Section 2: the coinstake spends exactly one input into exactly one output.
        assert_equal(len(coinstake['vin']), 1)
        assert_equal(len(coinstake['vout']), 1)
        assert 'coinbase' not in coinstake['vin'][0]
        staked_script = coinstake['vout'][0]['scriptPubKey']['hex']

        # Section 2.2: with BLOCK_SIGNATURE_ADDITION active the nonce must be zero.
        assert_equal(block['nonce'], 0)

        # Section 2: the coinstake returns the value to the destination it came from.
        prev = self.nodes[0].getrawtransaction(coinstake['vin'][0]['txid'], True)
        staked_out = prev['vout'][coinstake['vin'][0]['vout']]
        assert_equal(staked_out['scriptPubKey']['hex'], staked_script)

        # Section 2.1: the minter builds the multi-recipient coinbase, so vout[0] is the
        # OP_RETURN carrying the recipient count, signature and pubkey, and the last
        # output is the witness commitment. Both must carry no value.
        assert_greater_than(len(coinbase['vout']), 2)
        assert_equal(coinbase['vout'][0]['scriptPubKey']['type'], 'nulldata')
        assert_equal(coinbase['vout'][0]['value'], 0)
        assert_equal(coinbase['vout'][-1]['value'], 0)

        # Section 5: the reward outputs sit between those two and pay the staker.
        reward_outputs = coinbase['vout'][1:-1]
        assert_equal(len(reward_outputs), 1)
        assert_equal(reward_outputs[0]['scriptPubKey']['hex'], staked_script)

        reward = float(reward_outputs[0]['value'])
        assert_greater_than(reward, 0)
        # Section 5: the staking reward is a small yield on the stake, not a subsidy.
        assert_greater_than(float(staked_out['value']), reward)
        return block

    def check_reward_is_immature(self):
        """The reward lands in a coinbase output, so it must start out immature."""
        self.log.info("Checking that the staking reward is immature")
        info = self.nodes[0].getwalletinfo()
        assert_greater_than(float(info['immature_balance']), 0)

    def check_peer_accepts_block(self, block_hash):
        """Node 1 never saw the coinstake being built, so this exercises pure validation."""
        self.log.info("Checking that node 1 accepts the proof-of-stake block over p2p")
        sync_blocks(self.nodes)
        assert_equal(self.nodes[1].getblockcount(), REGTEST_SWITCH_HEIGHT + 1)
        assert_equal(self.nodes[1].getblockhash(REGTEST_SWITCH_HEIGHT + 1), block_hash)

    def check_reindex_revalidates(self, block_hash):
        """-reindex re-runs CheckBlock/ConnectBlock over the PoS block from disk."""
        self.log.info("Checking that node 1 revalidates the proof-of-stake block after -reindex")
        self.restart_node(1, extra_args=['-txindex', '-minting=0', '-reindex', '-mocktime={}'.format(self.mocktime)])
        expected_height = self.nodes[0].getblock(block_hash)['height']
        wait_until(lambda: self.nodes[1].getblockcount() == expected_height, timeout=300)
        assert_equal(self.nodes[1].getbestblockhash(), block_hash)
        connect_nodes(self.nodes[0], 1)

    def check_owner_backup_restores_contract(self, contract):
        """A wallet-file backup must preserve the owner key and contract script."""
        self.log.info("Checking cold-staking owner wallet backup recovery")
        backup_path = os.path.join(self.options.tmpdir, 'cold-owner-wallet.bak')
        restored_name = 'cold-owner-restored'
        restored_wallet = os.path.join(
            self.nodes[0].datadir, 'regtest', 'wallets', restored_name, 'wallet.dat')

        self.nodes[0].backupwallet(backup_path)
        self.nodes[0].createwallet(restored_name)
        self.nodes[0].unloadwallet(restored_name)

        self.stop_node(0)
        shutil.copyfile(backup_path, restored_wallet)
        self.start_node(0, extra_args=[
            '-txindex', '-minting=0', '-wallet={}'.format(restored_name),
            '-mocktime={}'.format(self.mocktime),
        ])
        connect_nodes(self.nodes[0], 1)
        restored_rpc = self.nodes[0].get_wallet_rpc(restored_name)
        restored_rpc.syncwithvalidationinterfacequeue()

        restored_view = restored_rpc.listcoldstaking()
        assert_greater_than(len(restored_view['outputs']), 0)
        assert all(output['address'] == contract['address']
                   for output in restored_view['outputs'])
        assert all(output['owner'] and not output['staker']
                   for output in restored_view['outputs'])

    def stake_one_cold_block(self, contract):
        """Stake on node 1, which has only the delegated staking key."""
        previous_height = self.nodes[0].getblockcount()
        tip_time = self.nodes[0].getblock(self.nodes[0].getbestblockhash())['time']
        mocktime = tip_time + REGTEST_STAKE_MIN_AGE + 300
        self.mocktime = mocktime
        for node in self.nodes:
            node.setmocktime(mocktime)

        self.restart_node(1, extra_args=['-txindex', '-minting=1', '-coldstaketargetage=0',
                                         '-mocktime={}'.format(mocktime)])
        connect_nodes(self.nodes[0], 1)
        self.nodes[1].setmocktime(mocktime)
        wait_until(lambda: self.nodes[1].getblockcount() > previous_height, timeout=180)
        block_hash = self.nodes[1].getblockhash(previous_height + 1)

        self.restart_node(1, extra_args=['-txindex', '-minting=0', '-mocktime={}'.format(mocktime)])
        connect_nodes(self.nodes[0], 1)
        sync_blocks(self.nodes)

        block = self.nodes[0].getblock(block_hash, 2)
        coinstake = block['tx'][1]
        assert_equal(coinstake['vout'][0]['scriptPubKey']['addresses'], [contract['address']])
        reward_outputs = block['tx'][0]['vout'][1:-1]
        assert reward_outputs
        for output in reward_outputs:
            assert_equal(output['scriptPubKey']['addresses'], [contract['address']])
        witness = coinstake['vin'][0]['txinwitness']
        assert_equal(witness[-2], '01')
        assert_equal(witness[-1], contract['redeemScript'])
        prev = self.nodes[0].getrawtransaction(coinstake['vin'][0]['txid'], True)
        assert_equal(prev['vout'][coinstake['vin'][0]['vout']]['value'], coinstake['vout'][0]['value'])
        return block_hash

    def run_test(self):
        # Node 0 owns Taproot coins. Node 1 imports only the staking half of a
        # cold contract; node 0 deliberately does not import that contract.
        address = self.nodes[0].getnewaddress("", "bech32m")
        owner_address = self.nodes[0].getnewaddress("owner", "bech32")
        staker_address = self.nodes[1].getnewaddress("staker", "bech32")
        assert_raises_rpc_error(
            -5, "Taproot owner addresses (txpc1p...) are not supported",
            self.nodes[1].createcoldstakingaddress, address, staker_address)
        assert_raises_rpc_error(
            -5, "Taproot staker addresses (txpc1p...) are not supported",
            self.nodes[1].createcoldstakingaddress, owner_address, address)
        contract = self.nodes[1].createcoldstakingaddress(owner_address, staker_address)
        assert_equal(self.nodes[1].getaddressinfo(contract['address'])['ismine'], True)
        assert_equal(self.nodes[0].getaddressinfo(contract['address'])['ismine'], False)

        withdrawal_txid = self.mine_pow_range(address, contract['address'], contract)
        self.check_cold_stake_role_separation(contract, withdrawal_txid)
        self.assert_pow_rejected_above_switch(address)

        staked_hash = self.stake_one_block()
        self.check_pos_block_structure(staked_hash)
        self.check_reward_is_immature()
        self.check_peer_accepts_block(staked_hash)

        cold_hash = self.stake_one_cold_block(contract)
        self.check_reindex_revalidates(cold_hash)
        self.check_owner_backup_restores_contract(contract)

        self.log.info("Taproot, delegated cold staking, propagation, reindex and owner recovery all verified")


if __name__ == '__main__':
    PoSStakingTest().main()
