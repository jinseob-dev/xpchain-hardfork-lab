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

    def mine_pow_range(self, taproot_address, cold_address):
        """Mine heights 1..nSwitchHeight, which is the whole proof-of-work range."""
        self.log.info("Mining the proof-of-work range up to height %d", REGTEST_SWITCH_HEIGHT)
        # generatetoaddress is capped per call by the RPC's own retry budget, so mine in
        # chunks to keep each call short.
        taproot_blocks = 200
        self.nodes[0].generatetoaddress(taproot_blocks, taproot_address)
        remaining = REGTEST_SWITCH_HEIGHT - taproot_blocks
        while remaining > 0:
            batch = min(remaining, 250)
            self.nodes[0].generatetoaddress(batch, cold_address)
            remaining -= batch
        assert_equal(self.nodes[0].getblockcount(), REGTEST_SWITCH_HEIGHT)
        sync_blocks(self.nodes)

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

    def stake_one_cold_block(self, contract):
        """Stake on node 1, which has only the delegated staking key."""
        previous_height = self.nodes[0].getblockcount()
        tip_time = self.nodes[0].getblock(self.nodes[0].getbestblockhash())['time']
        mocktime = tip_time + REGTEST_STAKE_MIN_AGE + 300
        self.mocktime = mocktime
        for node in self.nodes:
            node.setmocktime(mocktime)

        self.restart_node(1, extra_args=['-txindex', '-minting=1', '-mocktime={}'.format(mocktime)])
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
        contract = self.nodes[1].createcoldstakingaddress(owner_address, staker_address)
        assert_equal(self.nodes[1].getaddressinfo(contract['address'])['ismine'], True)
        assert_equal(self.nodes[0].getaddressinfo(contract['address'])['ismine'], False)

        self.mine_pow_range(address, contract['address'])
        self.assert_pow_rejected_above_switch(address)

        staked_hash = self.stake_one_block()
        self.check_pos_block_structure(staked_hash)
        self.check_reward_is_immature()
        self.check_peer_accepts_block(staked_hash)

        cold_hash = self.stake_one_cold_block(contract)
        self.check_reindex_revalidates(cold_hash)

        self.log.info("Taproot and delegated cold staking, propagation and reindex all verified")


if __name__ == '__main__':
    PoSStakingTest().main()
