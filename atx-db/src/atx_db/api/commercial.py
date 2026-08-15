"""Commercial policy primitives: pricing, idempotency, limits, and quota accounting."""

from __future__ import annotations

import datetime as dt
import json
import threading
from collections import defaultdict, deque
from collections.abc import Mapping
from dataclasses import dataclass
from decimal import Decimal
from pathlib import Path
from typing import Protocol

from ..connection import open_duckdb_connection


def _utc_now() -> dt.datetime:
    return dt.datetime.now(dt.UTC)


def _aware_utc(value: dt.datetime) -> dt.datetime:
    return value.replace(tzinfo=dt.UTC) if value.tzinfo is None else value.astimezone(dt.UTC)


def month_start(value: dt.datetime | None = None) -> dt.datetime:
    current = value or _utc_now()
    current = _aware_utc(current)
    return current.replace(day=1, hour=0, minute=0, second=0, microsecond=0)


@dataclass(frozen=True)
class UnitPrice:
    dataset: str
    schema: str
    mode: str
    currency: str
    billing_unit: str
    unit_price_per_gb: Decimal | None
    status: str
    valid_from: dt.datetime
    valid_to: dt.datetime | None

    def public(self) -> dict[str, object]:
        return {
            "dataset": self.dataset,
            "schema": self.schema,
            "mode": self.mode,
            "currency": self.currency,
            "billing_unit": self.billing_unit,
            "unit_price_per_gb": (None if self.unit_price_per_gb is None else float(self.unit_price_per_gb)),
            "status": self.status,
            "valid_from": self.valid_from,
            "valid_to": self.valid_to,
        }


class PricingCatalog(Protocol):
    def get(self, dataset: str, schema: str, *, mode: str = "historical") -> UnitPrice | None: ...

    def list(self, *, dataset: str | None = None) -> list[UnitPrice]: ...


class InMemoryPricingCatalog:
    def __init__(self, prices: Mapping[tuple[str, str, str], Decimal | int | float | None] | None = None) -> None:
        valid_from = dt.datetime(1900, 1, 1, tzinfo=dt.UTC)
        self._prices = {
            key: UnitPrice(
                dataset=key[0],
                schema=key[1],
                mode=key[2],
                currency="USD",
                billing_unit="uncompressed_arrow_bytes",
                unit_price_per_gb=None if value is None else Decimal(str(value)),
                status="contract_required" if value is None else "active",
                valid_from=valid_from,
                valid_to=None,
            )
            for key, value in (prices or {}).items()
        }

    def get(self, dataset: str, schema: str, *, mode: str = "historical") -> UnitPrice | None:
        return self._prices.get((dataset, schema, mode))

    def list(self, *, dataset: str | None = None) -> list[UnitPrice]:
        return sorted(
            (price for price in self._prices.values() if dataset is None or price.dataset == dataset),
            key=lambda price: (price.dataset, price.schema, price.mode),
        )


class DuckDBPricingCatalog:
    def __init__(self, database_path: Path | str) -> None:
        self.database_path = Path(database_path)

    def get(self, dataset: str, schema: str, *, mode: str = "historical") -> UnitPrice | None:
        with open_duckdb_connection(self.database_path, read_only=True) as conn:
            row = conn.execute(
                """
                SELECT dataset_id,schema_code,mode,currency,billing_unit,
                       unit_price_per_gb,status,valid_from,valid_to
                FROM api_unit_price_catalog
                WHERE dataset_id = ? AND schema_code = ? AND mode = ?
                  AND valid_from <= now() AND (valid_to IS NULL OR valid_to > now())
                ORDER BY valid_from DESC LIMIT 1
                """,
                [dataset, schema, mode],
            ).fetchone()
        return None if row is None else _unit_price_from_row(row)

    def list(self, *, dataset: str | None = None) -> list[UnitPrice]:
        condition = "true" if dataset is None else "dataset_id = ?"
        parameters: list[object] = [] if dataset is None else [dataset]
        with open_duckdb_connection(self.database_path, read_only=True) as conn:
            rows = conn.execute(
                f"""
                SELECT dataset_id,schema_code,mode,currency,billing_unit,
                       unit_price_per_gb,status,valid_from,valid_to
                FROM api_unit_price_catalog
                WHERE {condition}
                  AND valid_from <= now() AND (valid_to IS NULL OR valid_to > now())
                QUALIFY row_number() OVER (
                    PARTITION BY dataset_id,schema_code,mode,currency
                    ORDER BY valid_from DESC
                ) = 1
                ORDER BY dataset_id,schema_code,mode
                """,
                parameters,
            ).fetchall()
        return [_unit_price_from_row(row) for row in rows]


def _unit_price_from_row(row: tuple[object, ...]) -> UnitPrice:
    return UnitPrice(
        dataset=str(row[0]),
        schema=str(row[1]),
        mode=str(row[2]),
        currency=str(row[3]),
        billing_unit=str(row[4]),
        unit_price_per_gb=None if row[5] is None else Decimal(str(row[5])),
        status=str(row[6]),
        valid_from=_aware_utc(row[7]),  # type: ignore[arg-type]
        valid_to=None if row[8] is None else _aware_utc(row[8]),  # type: ignore[arg-type]
    )


def calculate_cost(billable_bytes: int, price: UnitPrice | None) -> Decimal | None:
    if price is None or price.status != "active" or price.unit_price_per_gb is None:
        return None
    return (Decimal(billable_bytes) / Decimal(1_000_000_000) * price.unit_price_per_gb).quantize(Decimal("0.000000001"))


@dataclass(frozen=True)
class RateLimitDecision:
    allowed: bool
    limit: int | None
    remaining: int | None
    retry_after_seconds: int | None


class RateLimiter(Protocol):
    def consume(self, account_id: str, key_id: str, dataset: str, limit: int | None) -> RateLimitDecision: ...


class InMemoryRateLimiter:
    """Thread-safe sliding-window limiter for the single-node serving adapter."""

    def __init__(self) -> None:
        self._events: dict[tuple[str, str, str], deque[dt.datetime]] = defaultdict(deque)
        self._lock = threading.Lock()

    def consume(self, account_id: str, key_id: str, dataset: str, limit: int | None) -> RateLimitDecision:
        if limit is None:
            return RateLimitDecision(True, None, None, None)
        now = _utc_now()
        cutoff = now - dt.timedelta(minutes=1)
        key = (account_id, key_id, dataset)
        with self._lock:
            events = self._events[key]
            while events and events[0] <= cutoff:
                events.popleft()
            if len(events) >= limit:
                retry = max(1, int((events[0] + dt.timedelta(minutes=1) - now).total_seconds()) + 1)
                return RateLimitDecision(False, limit, 0, retry)
            events.append(now)
            return RateLimitDecision(True, limit, limit - len(events), None)


@dataclass(frozen=True)
class IdempotencyRecord:
    account_id: str
    endpoint: str
    key: str
    request_sha256: str
    state: str
    resource_type: str | None
    resource_id: str | None
    response_status: int | None
    response_json: str | None
    expires_at: dt.datetime


class IdempotencyConflict(ValueError):
    pass


class IdempotencyInProgress(RuntimeError):
    pass


class IdempotencyStore(Protocol):
    def begin(
        self,
        account_id: str,
        endpoint: str,
        key: str,
        request_sha256: str,
        *,
        ttl: dt.timedelta,
    ) -> IdempotencyRecord | None: ...

    def complete(
        self,
        account_id: str,
        endpoint: str,
        key: str,
        *,
        resource_type: str,
        resource_id: str,
        response_status: int,
        response: Mapping[str, object],
    ) -> None: ...

    def abort(self, account_id: str, endpoint: str, key: str) -> None: ...


class InMemoryIdempotencyStore:
    def __init__(self) -> None:
        self._records: dict[tuple[str, str, str], IdempotencyRecord] = {}
        self._lock = threading.Lock()

    def begin(
        self,
        account_id: str,
        endpoint: str,
        key: str,
        request_sha256: str,
        *,
        ttl: dt.timedelta,
    ) -> IdempotencyRecord | None:
        index = (account_id, endpoint, key)
        now = _utc_now()
        with self._lock:
            existing = self._records.get(index)
            if existing is not None and existing.expires_at <= now:
                del self._records[index]
                existing = None
            if existing is not None:
                return _validate_replay(existing, request_sha256)
            self._records[index] = IdempotencyRecord(
                account_id=account_id,
                endpoint=endpoint,
                key=key,
                request_sha256=request_sha256,
                state="started",
                resource_type=None,
                resource_id=None,
                response_status=None,
                response_json=None,
                expires_at=now + ttl,
            )
            return None

    def complete(
        self,
        account_id: str,
        endpoint: str,
        key: str,
        *,
        resource_type: str,
        resource_id: str,
        response_status: int,
        response: Mapping[str, object],
    ) -> None:
        index = (account_id, endpoint, key)
        with self._lock:
            existing = self._records[index]
            self._records[index] = IdempotencyRecord(
                **{
                    **existing.__dict__,
                    "state": "completed",
                    "resource_type": resource_type,
                    "resource_id": resource_id,
                    "response_status": response_status,
                    "response_json": json.dumps(response, default=str, sort_keys=True, separators=(",", ":")),
                }
            )

    def abort(self, account_id: str, endpoint: str, key: str) -> None:
        with self._lock:
            self._records.pop((account_id, endpoint, key), None)


class DuckDBIdempotencyStore:
    def __init__(self, database_path: Path | str) -> None:
        self.database_path = Path(database_path)
        self._lock = threading.Lock()

    def begin(
        self,
        account_id: str,
        endpoint: str,
        key: str,
        request_sha256: str,
        *,
        ttl: dt.timedelta,
    ) -> IdempotencyRecord | None:
        now = _utc_now()
        expires_at = now + ttl
        with self._lock, open_duckdb_connection(self.database_path) as conn:
            conn.execute(
                """
                DELETE FROM saas_idempotency_records
                WHERE account_id = ? AND endpoint = ? AND idempotency_key = ? AND expires_at <= ?
                """,
                [account_id, endpoint, key, now],
            )
            inserted = conn.execute(
                """
                INSERT INTO saas_idempotency_records (
                    account_id,endpoint,idempotency_key,request_sha256,state,expires_at
                ) VALUES (?,?,?,?, 'started', ?)
                ON CONFLICT DO NOTHING
                RETURNING idempotency_key
                """,
                [account_id, endpoint, key, request_sha256, expires_at],
            ).fetchone()
            if inserted is not None:
                return None
            row = conn.execute(
                """
                SELECT account_id,endpoint,idempotency_key,request_sha256,state,
                       resource_type,resource_id,response_status,response_json,expires_at
                FROM saas_idempotency_records
                WHERE account_id = ? AND endpoint = ? AND idempotency_key = ?
                """,
                [account_id, endpoint, key],
            ).fetchone()
        assert row is not None
        return _validate_replay(_idempotency_from_row(row), request_sha256)

    def complete(
        self,
        account_id: str,
        endpoint: str,
        key: str,
        *,
        resource_type: str,
        resource_id: str,
        response_status: int,
        response: Mapping[str, object],
    ) -> None:
        with self._lock, open_duckdb_connection(self.database_path) as conn:
            conn.execute(
                """
                UPDATE saas_idempotency_records
                SET state = 'completed',resource_type = ?,resource_id = ?,response_status = ?,
                    response_json = ?,completed_at = now(),updated_at = now()
                WHERE account_id = ? AND endpoint = ? AND idempotency_key = ? AND state = 'started'
                """,
                [
                    resource_type,
                    resource_id,
                    response_status,
                    json.dumps(response, default=str, sort_keys=True, separators=(",", ":")),
                    account_id,
                    endpoint,
                    key,
                ],
            )

    def abort(self, account_id: str, endpoint: str, key: str) -> None:
        with self._lock, open_duckdb_connection(self.database_path) as conn:
            conn.execute(
                """
                DELETE FROM saas_idempotency_records
                WHERE account_id = ? AND endpoint = ? AND idempotency_key = ? AND state = 'started'
                """,
                [account_id, endpoint, key],
            )


def _validate_replay(record: IdempotencyRecord, request_sha256: str) -> IdempotencyRecord:
    if record.request_sha256 != request_sha256:
        raise IdempotencyConflict("idempotency key was already used with different request parameters")
    if record.state != "completed":
        raise IdempotencyInProgress("a request with this idempotency key is still in progress")
    return record


def _idempotency_from_row(row: tuple[object, ...]) -> IdempotencyRecord:
    return IdempotencyRecord(
        account_id=str(row[0]),
        endpoint=str(row[1]),
        key=str(row[2]),
        request_sha256=str(row[3]),
        state=str(row[4]),
        resource_type=None if row[5] is None else str(row[5]),
        resource_id=None if row[6] is None else str(row[6]),
        response_status=None if row[7] is None else int(str(row[7])),
        response_json=None if row[8] is None else str(row[8]),
        expires_at=_aware_utc(row[9]),  # type: ignore[arg-type]
    )
