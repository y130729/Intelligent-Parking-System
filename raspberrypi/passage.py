#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""单次通行会话（PassageSession）。

一次"车到 -> 开闸 -> 关闸"算一次通行。会话由 STM32 的 CAPTURE 事件开启，
红外检测到车到位即触发。历史上这个会话没有超时，导致：

    车到了、识别失败、车倒着走了 -> 会话仍然 active
    下一辆车刷卡开闸 -> 被误记成上一次会话的凭证

因此这里给会话加了 TTL，超时自动失效；同时把"凭证来源"细化为
plate / card / remote，便于审计（原来的 "card_or_remote" 无法区分
是车主刷卡还是网页端远程开闸）。

本模块为纯逻辑，不依赖串口/相机/网络，可直接单元测试。
"""

import threading
import time

# 凭证来源
CREDENTIAL_PLATE = "plate"
CREDENTIAL_CARD = "card"
CREDENTIAL_REMOTE = "remote"
CREDENTIAL_UNKNOWN = "unknown"

# 一次通行会话的最长存活时间（秒）
DEFAULT_TTL_S = 60.0

# 老固件不带凭证原因时，用"最近是否刷过卡/下发过远程命令"来兜底判断
DEFAULT_CARD_WINDOW_S = 2.0
DEFAULT_REMOTE_WINDOW_S = 5.0


class PassageSession:
    """线程安全的单次通行会话。"""

    def __init__(self, ttl_s=DEFAULT_TTL_S, clock=time.monotonic):
        self._ttl = float(ttl_s)
        self._clock = clock
        self._lock = threading.Lock()
        self._active = False
        self._opened = False
        self._credential = None
        self._operator = None
        self._started_at = None
        self._opened_at = None

    # ---- 生命周期 -------------------------------------------------
    @property
    def ttl(self):
        """会话有效期（秒）。"""
        return self._ttl

    def start(self, now=None):
        """开启新会话（收到 CAPTURE 时调用），会覆盖上一次未结束的会话。"""
        now = self._now(now)
        with self._lock:
            self._active = True
            self._opened = False
            self._credential = None
            self._operator = None
            self._started_at = now
            self._opened_at = None

    def finish(self, now=None):
        """结束会话（关闸或车辆离开时调用）。"""
        with self._lock:
            self._active = False

    def is_active(self, now=None):
        """会话是否仍在有效期内（超时自动失效）。"""
        now = self._now(now)
        with self._lock:
            self._expire_locked(now)
            return self._active

    def try_open(self, credential, now=None, operator=None):
        """尝试用某个凭证开闸。

        返回 True 表示"本次会话由该凭证开启"（首次命中的凭证生效）；
        会话已过期或已经开过闸则返回 False，不会覆盖已有记录。
        """
        now = self._now(now)
        with self._lock:
            self._expire_locked(now)
            if not self._active or self._opened:
                return False
            self._opened = True
            self._credential = credential
            self._operator = operator
            self._opened_at = now
            return True

    def snapshot(self, now=None):
        """当前状态快照，用于日志与云端应答。"""
        now = self._now(now)
        with self._lock:
            self._expire_locked(now)
            return {
                "active": self._active,
                "opened": self._opened,
                "credential": self._credential,
                "operator": self._operator,
                "age_s": None if self._started_at is None else round(now - self._started_at, 1),
            }

    # ---- 内部 -----------------------------------------------------
    def _expire_locked(self, now):
        if self._active and self._started_at is not None and (now - self._started_at) > self._ttl:
            self._active = False

    def _now(self, now):
        return self._clock() if now is None else now


def resolve_credential(reason, last_card_age_s=None, remote_request_age_s=None,
                       card_window_s=DEFAULT_CARD_WINDOW_S,
                       remote_window_s=DEFAULT_REMOTE_WINDOW_S):
    """判定这次开闸的凭证来源。

    优先使用 STM32 明确上报的原因（新固件 BARRIER:OPEN:<reason>）；
    老固件只回 BARRIER:OPEN 时，用最近一次刷卡/远程命令的时间兜底。
    """
    if reason in (CREDENTIAL_PLATE, CREDENTIAL_CARD, CREDENTIAL_REMOTE):
        return reason
    if last_card_age_s is not None and last_card_age_s <= card_window_s:
        return CREDENTIAL_CARD
    if remote_request_age_s is not None and remote_request_age_s <= remote_window_s:
        return CREDENTIAL_REMOTE
    return CREDENTIAL_UNKNOWN
