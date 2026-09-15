#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""通行会话与凭证判定单元测试。"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import passage as ps


class FakeClock(object):
    def __init__(self, t=1000.0):
        self.t = t

    def __call__(self):
        return self.t

    def advance(self, dt):
        self.t += dt


class TestPassageSession(unittest.TestCase):
    def setUp(self):
        self.clock = FakeClock()
        self.s = ps.PassageSession(ttl_s=30.0, clock=self.clock)

    def test_inactive_before_start(self):
        self.assertFalse(self.s.is_active())
        self.assertFalse(self.s.try_open(ps.CREDENTIAL_CARD))

    def test_start_makes_session_active(self):
        self.s.start()
        self.assertTrue(self.s.is_active())
        self.assertFalse(self.s.snapshot()["opened"])

    def test_first_credential_wins(self):
        self.s.start()
        self.assertTrue(self.s.try_open(ps.CREDENTIAL_PLATE))
        self.assertFalse(self.s.try_open(ps.CREDENTIAL_CARD))
        snap = self.s.snapshot()
        self.assertTrue(snap["opened"])
        self.assertEqual(snap["credential"], ps.CREDENTIAL_PLATE)

    def test_session_expires_after_ttl(self):
        """回归：会话原先没有超时，超时后必须失效。"""
        self.s.start()
        self.clock.advance(29.0)
        self.assertTrue(self.s.is_active())
        self.clock.advance(2.0)
        self.assertFalse(self.s.is_active())
        self.assertFalse(self.s.try_open(ps.CREDENTIAL_CARD))

    def test_expired_session_lets_next_card_open_fresh(self):
        """车走了之后下一辆车刷卡，不应被记成上一次会话的凭证。"""
        self.s.start()
        self.s.try_open(ps.CREDENTIAL_PLATE)
        self.clock.advance(120.0)
        self.s.start()
        self.assertTrue(self.s.try_open(ps.CREDENTIAL_CARD))
        self.assertEqual(self.s.snapshot()["credential"], ps.CREDENTIAL_CARD)

    def test_finish_ends_session(self):
        self.s.start()
        self.s.finish()
        self.assertFalse(self.s.is_active())
        self.assertFalse(self.s.try_open(ps.CREDENTIAL_CARD))

    def test_restart_resets_state(self):
        self.s.start()
        self.s.try_open(ps.CREDENTIAL_PLATE)
        self.s.start()
        snap = self.s.snapshot()
        self.assertTrue(snap["active"])
        self.assertFalse(snap["opened"])
        self.assertIsNone(snap["credential"])

    def test_operator_recorded(self):
        self.s.start()
        self.s.try_open(ps.CREDENTIAL_REMOTE, operator="web-admin")
        self.assertEqual(self.s.snapshot()["operator"], "web-admin")


class TestResolveCredential(unittest.TestCase):
    def test_explicit_reason_wins(self):
        self.assertEqual(ps.resolve_credential("card"), ps.CREDENTIAL_CARD)
        self.assertEqual(ps.resolve_credential("plate"), ps.CREDENTIAL_PLATE)
        self.assertEqual(ps.resolve_credential("remote"), ps.CREDENTIAL_REMOTE)

    def test_falls_back_to_recent_card(self):
        self.assertEqual(ps.resolve_credential("", last_card_age_s=0.4), ps.CREDENTIAL_CARD)
        self.assertNotEqual(ps.resolve_credential("", last_card_age_s=30.0), ps.CREDENTIAL_CARD)

    def test_falls_back_to_recent_remote_command(self):
        self.assertEqual(ps.resolve_credential("", remote_request_age_s=1.0),
                         ps.CREDENTIAL_REMOTE)

    def test_unknown_when_nothing_recent(self):
        """回归：旧实现统一记成 card_or_remote，无法审计。"""
        self.assertEqual(ps.resolve_credential(""), ps.CREDENTIAL_UNKNOWN)

    def test_unknown_reason_string_is_ignored(self):
        self.assertEqual(ps.resolve_credential("bogus"), ps.CREDENTIAL_UNKNOWN)


if __name__ == "__main__":
    unittest.main(verbosity=2)
