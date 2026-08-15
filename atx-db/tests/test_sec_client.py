from __future__ import annotations

import pytest
from urllib3.util.retry import Retry

from atx_db import security_master


def test_sec_session_has_governed_retry_policy() -> None:
    session = security_master.sec_session("atx-test test@example.com")
    adapter = session.get_adapter("https://www.sec.gov/")

    assert adapter.max_retries.total == 5
    assert adapter.max_retries.status == 5
    assert adapter.max_retries.allowed_methods == frozenset({"GET", "HEAD"})
    assert adapter.max_retries.status_forcelist == security_master.SEC_RETRY_STATUS_CODES
    assert adapter.max_retries.respect_retry_after_header


def test_sec_request_limiter_paces_below_official_ceiling(monkeypatch) -> None:
    clock_values = iter((100.0, 100.0, 100.0, 100.11))
    sleeps: list[float] = []
    monkeypatch.setattr(security_master.time, "monotonic", lambda: next(clock_values))
    monkeypatch.setattr(security_master.time, "sleep", sleeps.append)
    monkeypatch.setattr(security_master, "_SEC_NEXT_REQUEST_AT", 0.0)

    security_master._wait_for_sec_request_slot()
    security_master._wait_for_sec_request_slot()

    assert sleeps == [pytest.approx(security_master.SEC_REQUEST_INTERVAL_SECONDS)]
    assert security_master.SEC_REQUEST_INTERVAL_SECONDS > 0.1


def test_retry_policy_type_is_urllib3_retry() -> None:
    session = security_master.sec_session("atx-test test@example.com")
    assert isinstance(session.get_adapter("https://www.sec.gov/").max_retries, Retry)
