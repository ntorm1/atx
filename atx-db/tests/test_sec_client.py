from __future__ import annotations

import time

import pytest
import requests
from requests.adapters import HTTPAdapter

from atx_db import sec_http, security_master


class _CountingLimiter:
    def __init__(self) -> None:
        self.acquired = 0

    def acquire(self, label: str | None = None) -> None:
        self.acquired += 1


class _Response:
    def __init__(self, status: int) -> None:
        self.status_code = status
        self.headers: dict[str, str] = {}

    def close(self) -> None:
        pass


def test_sec_session_sends_only_the_approved_user_agent() -> None:
    session = security_master.sec_session(None)
    assert session.headers["User-Agent"] == sec_http.APPROVED_SEC_USER_AGENT
    with pytest.raises(ValueError, match="ATX_SEC_USER_AGENT"):
        security_master.sec_session("atx-test test@example.com")


def test_every_attempt_takes_a_limiter_token(monkeypatch) -> None:
    limiter = _CountingLimiter()
    adapter = sec_http.sec_session(limiter=limiter).get_adapter("https://www.sec.gov/")  # type: ignore[arg-type]
    responses = iter((_Response(503), _Response(429), _Response(200)))
    monkeypatch.setattr(HTTPAdapter, "send", lambda self, request, **kwargs: next(responses))
    monkeypatch.setattr(sec_http.time, "sleep", lambda _seconds: None)
    assert adapter.send(requests.Request("GET", "https://www.sec.gov/x").prepare()).status_code == 200
    assert limiter.acquired == 3
    assert sec_http.SEC_RETRY_STATUS_CODES == frozenset({429, 500, 502, 503, 504})


def test_limiter_handles_share_one_host_wide_spacing(tmp_path) -> None:
    lock = tmp_path / ".sec_rate.lock"
    first, second = sec_http.SecRateLimiter(lock_path=lock), sec_http.SecRateLimiter(lock_path=lock)
    grants = []
    for limiter in (first, second, first, second, first, second):
        limiter.acquire()
        grants.append(time.time())
    assert min(b - a for a, b in zip(grants, grants[1:])) >= 0.2 - 0.005
    with pytest.raises(ValueError):
        sec_http.SecRateLimiter(rate_per_s=6.0, lock_path=lock)
