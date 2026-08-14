from __future__ import annotations

import datetime as dt
from decimal import Decimal

import pytest

from atx_db.api.commercial import (
    IdempotencyConflict,
    IdempotencyInProgress,
    InMemoryIdempotencyStore,
    InMemoryRateLimiter,
    UnitPrice,
    calculate_cost,
)


def test_idempotency_is_account_scoped_rejects_mutation_and_reports_in_progress() -> None:
    store = InMemoryIdempotencyStore()
    ttl = dt.timedelta(hours=24)

    assert store.begin("account-a", "batch.submit_job", "retry-1", "hash-a", ttl=ttl) is None
    with pytest.raises(IdempotencyInProgress):
        store.begin("account-a", "batch.submit_job", "retry-1", "hash-a", ttl=ttl)
    assert store.begin("account-b", "batch.submit_job", "retry-1", "hash-b", ttl=ttl) is None

    store.complete(
        "account-a",
        "batch.submit_job",
        "retry-1",
        resource_type="batch_job",
        resource_id="job-1",
        response_status=202,
        response={"job_id": "job-1"},
    )
    replay = store.begin("account-a", "batch.submit_job", "retry-1", "hash-a", ttl=ttl)
    assert replay is not None
    assert replay.resource_id == "job-1"
    with pytest.raises(IdempotencyConflict):
        store.begin("account-a", "batch.submit_job", "retry-1", "different", ttl=ttl)


def test_rate_limiter_is_scoped_by_account_key_and_dataset() -> None:
    limiter = InMemoryRateLimiter()

    first = limiter.consume("account", "key-a", "dataset-a", 1)
    blocked = limiter.consume("account", "key-a", "dataset-a", 1)
    other_key = limiter.consume("account", "key-b", "dataset-a", 1)
    other_dataset = limiter.consume("account", "key-a", "dataset-b", 1)

    assert first.allowed and first.remaining == 0
    assert not blocked.allowed and blocked.retry_after_seconds is not None
    assert other_key.allowed
    assert other_dataset.allowed


def test_cost_uses_decimal_gigabytes_and_unpriced_contracts_return_none() -> None:
    active = UnitPrice(
        dataset="ATX.US.FUNDAMENTALS",
        schema="standardized",
        mode="historical",
        currency="USD",
        billing_unit="uncompressed_arrow_bytes",
        unit_price_per_gb=Decimal("2.50"),
        status="active",
        valid_from=dt.datetime(2020, 1, 1, tzinfo=dt.UTC),
        valid_to=None,
    )
    contract = UnitPrice(**{**active.__dict__, "unit_price_per_gb": None, "status": "contract_required"})

    assert calculate_cost(1_000_000_000, active) == Decimal("2.500000000")
    assert calculate_cost(100_000_000, active) == Decimal("0.250000000")
    assert calculate_cost(1_000_000_000, contract) is None
