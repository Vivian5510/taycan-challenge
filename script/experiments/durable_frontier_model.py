#!/usr/bin/env python3
"""Executable model for an ordered durable frontier.

The model separates reservation, application, durability, publication, and
reclamation.  It is intentionally independent of F2 so ordering mistakes can
be tested before changing the engine.
"""

from dataclasses import dataclass
import random
import unittest


@dataclass
class Range:
    first: int
    last: int
    prepared: bool = False
    applied: bool = False
    synced: bool = False
    failed: bool = False


class DurableFrontier:
    def __init__(self):
        self.reserved = 0
        self.applied = 0
        self.durable = 0
        self.published = 0
        self.checkpointed = 0
        self.reclaimable = 0
        self.ranges = {}

    def reserve(self, count):
        if count <= 0:
            raise ValueError("count must be positive")
        first = self.reserved + 1
        self.reserved += count
        item = Range(first, self.reserved)
        self.ranges[first] = item
        return item

    def prepare(self, item):
        self._known(item)
        if item.prepared or item.failed:
            raise ValueError("invalid prepare")
        item.prepared = True

    def apply(self, item):
        self._known(item)
        if not item.prepared or item.applied or item.failed:
            raise ValueError("apply before prepare")
        item.applied = True
        self.applied = max(self.applied, item.last)

    def sync(self, item):
        self._known(item)
        if not item.applied or item.synced or item.failed:
            raise ValueError("sync before apply")
        item.synced = True
        self._advance_durable()

    def fail(self, item):
        self._known(item)
        if item.synced:
            raise ValueError("cannot fail a completed sync")
        item.failed = True

    def publish(self, target=None):
        target = self.durable if target is None else target
        if target > self.durable:
            raise ValueError("cannot publish a non-durable range")
        self.published = max(self.published, target)

    def persist_checkpoint(self, target):
        if target > self.published:
            raise ValueError("checkpoint cannot include unpublished data")
        self.checkpointed = max(self.checkpointed, target)

    def reclaim(self, target):
        if target > self.checkpointed:
            raise ValueError("cannot reclaim beyond the persisted checkpoint")
        self.reclaimable = max(self.reclaimable, target)

    def acked(self, target):
        return 0 < target <= self.published

    def _advance_durable(self):
        while True:
            next_first = self.durable + 1
            item = self.ranges.get(next_first)
            if item is None or not item.synced or item.failed:
                return
            self.durable = item.last

    def _known(self, item):
        if self.ranges.get(item.first) is not item:
            raise ValueError("unknown range")


class DurableFrontierTests(unittest.TestCase):
    def test_out_of_order_sync_waits_for_gap(self):
        frontier = DurableFrontier()
        first = frontier.reserve(3)
        second = frontier.reserve(2)
        for item in (first, second):
            frontier.prepare(item)
            frontier.apply(item)
        frontier.sync(second)
        self.assertEqual(frontier.durable, 0)
        self.assertFalse(frontier.acked(second.last))
        frontier.sync(first)
        self.assertEqual(frontier.durable, second.last)
        self.assertFalse(frontier.acked(second.last))
        frontier.publish()
        self.assertTrue(frontier.acked(second.last))

    def test_out_of_order_apply_still_requires_contiguous_sync(self):
        frontier = DurableFrontier()
        first = frontier.reserve(2)
        second = frontier.reserve(2)
        for item in (first, second):
            frontier.prepare(item)
        frontier.apply(second)
        frontier.sync(second)
        self.assertEqual(frontier.applied, second.last)
        self.assertEqual(frontier.durable, 0)
        frontier.apply(first)
        frontier.sync(first)
        self.assertEqual(frontier.durable, second.last)

    def test_failed_gap_never_lets_later_range_ack(self):
        frontier = DurableFrontier()
        first = frontier.reserve(1)
        second = frontier.reserve(1)
        for item in (first, second):
            frontier.prepare(item)
            frontier.apply(item)
        frontier.fail(first)
        with self.assertRaises(ValueError):
            frontier.sync(first)
        frontier.sync(second)
        self.assertEqual(frontier.durable, 0)
        self.assertFalse(frontier.acked(second.last))

    def test_publish_and_reclaim_follow_durable_frontier(self):
        frontier = DurableFrontier()
        item = frontier.reserve(4)
        frontier.prepare(item)
        frontier.apply(item)
        with self.assertRaises(ValueError):
            frontier.publish(item.last)
        frontier.sync(item)
        frontier.publish(item.last)
        with self.assertRaises(ValueError):
            frontier.reclaim(item.last)
        frontier.persist_checkpoint(item.last)
        frontier.reclaim(item.last)
        self.assertEqual(frontier.reclaimable, item.last)

    def test_random_completion_order_preserves_prefix(self):
        rng = random.Random(20261123)
        frontier = DurableFrontier()
        items = [frontier.reserve(1) for _ in range(64)]
        for item in items:
            frontier.prepare(item)
            frontier.apply(item)
        pending = items[:]
        rng.shuffle(pending)
        completed = set()
        for item in pending:
            frontier.sync(item)
            completed.add(item.first)
            first_gap = next((serial for serial in range(1, 65) if serial not in completed), 65)
            self.assertEqual(frontier.durable, first_gap - 1)
        self.assertEqual(frontier.durable, 64)
        frontier.publish()
        self.assertTrue(frontier.acked(64))

    def test_no_tail_ack_after_crash_cut(self):
        frontier = DurableFrontier()
        items = [frontier.reserve(2) for _ in range(3)]
        for item in items:
            frontier.prepare(item)
            frontier.apply(item)
        frontier.sync(items[0])
        frontier.publish()
        self.assertTrue(frontier.acked(items[0].last))
        self.assertFalse(frontier.acked(items[1].last))
        self.assertEqual(frontier.published, items[0].last)
        self.assertEqual(frontier.reclaimable, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
