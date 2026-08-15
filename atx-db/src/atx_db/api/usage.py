"""Request-level usage metering interfaces."""

from __future__ import annotations

import datetime as dt
import threading
from dataclasses import dataclass
from decimal import Decimal
from pathlib import Path
from typing import Protocol

from ..connection import open_duckdb_connection


@dataclass(frozen=True)
class UsageEvent:
    request_id: str
    account_id: str
    key_id: str
    endpoint: str
    dataset: str | None
    schema: str | None
    started_at: dt.datetime
    finished_at: dt.datetime
    status_code: int
    record_count: int
    response_bytes: int
    billable_bytes: int = 0
    cost_usd: Decimal | None = None
    billing_mode: str | None = None


class UsageLedger(Protocol):
    def record(self, event: UsageEvent) -> None: ...

    def billable_bytes_since(self, account_id: str, dataset: str, since: dt.datetime) -> int: ...


class InMemoryUsageLedger:
    """Thread-safe development ledger implementing the durable sink contract."""

    def __init__(self) -> None:
        self._events: list[UsageEvent] = []
        self._lock = threading.Lock()

    def record(self, event: UsageEvent) -> None:
        with self._lock:
            self._events.append(event)

    def events(self) -> tuple[UsageEvent, ...]:
        with self._lock:
            return tuple(self._events)

    def billable_bytes_since(self, account_id: str, dataset: str, since: dt.datetime) -> int:
        with self._lock:
            return sum(
                event.billable_bytes
                for event in self._events
                if event.account_id == account_id and event.dataset == dataset and event.finished_at >= since
            )


class DuckDBUsageLedger:
    """Durable single-node usage ledger for development and private deployments."""

    def __init__(self, database_path: Path | str) -> None:
        self.database_path = Path(database_path)
        self._lock = threading.Lock()

    def record(self, event: UsageEvent) -> None:
        with self._lock, open_duckdb_connection(self.database_path) as conn:
            conn.execute(
                """
                INSERT INTO saas_usage_events (
                    request_id,account_id,key_id,endpoint,dataset_id,schema_code,
                    started_at,finished_at,status_code,record_count,response_bytes,
                    billable_bytes,cost_usd,billing_mode
                ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)
                ON CONFLICT (request_id) DO UPDATE SET
                    finished_at = excluded.finished_at,
                    status_code = excluded.status_code,
                    record_count = excluded.record_count,
                    response_bytes = excluded.response_bytes,
                    billable_bytes = excluded.billable_bytes,
                    cost_usd = excluded.cost_usd,
                    billing_mode = excluded.billing_mode
                """,
                [
                    event.request_id,
                    event.account_id,
                    event.key_id,
                    event.endpoint,
                    event.dataset,
                    event.schema,
                    event.started_at,
                    event.finished_at,
                    event.status_code,
                    event.record_count,
                    event.response_bytes,
                    event.billable_bytes,
                    event.cost_usd,
                    event.billing_mode,
                ],
            )

    def billable_bytes_since(self, account_id: str, dataset: str, since: dt.datetime) -> int:
        with self._lock, open_duckdb_connection(self.database_path, read_only=True) as conn:
            row = conn.execute(
                """
                SELECT coalesce(sum(billable_bytes), 0)
                FROM saas_usage_events
                WHERE account_id = ? AND dataset_id = ? AND finished_at >= ?
                """,
                [account_id, dataset, since],
            ).fetchone()
        assert row is not None
        return int(row[0])
