from __future__ import annotations

import bisect
import json
import time
from collections import defaultdict
from collections.abc import Callable, Iterator, Sequence
from datetime import date, datetime, timedelta
from typing import Any

from .db import EarningsDatabase
from .models import RefreshReport, iso_datetime, utc_now
from .sources.base import EarningsSource, UniverseSource

REFERENCE_EVENT_COLUMNS = (
    "date",
    "session",
    "status",
    "fiscal_quarter_ending",
    "eps_estimate",
    "reported_eps",
    "surprise_percent",
    "estimator_count",
    "source",
    "observed_at",
)


class EarningsService:
    """Orchestrates resumable source collection and PIT reference builds."""

    def __init__(
        self,
        database: EarningsDatabase,
        *,
        earnings_source: EarningsSource,
        universe_source: UniverseSource,
        source_priority: Sequence[str] = ("csv-confirmed", "nasdaq"),
        request_delay_seconds: float = 0.25,
        clock: Callable[[], datetime] = utc_now,
    ) -> None:
        self.database = database
        self.earnings_source = earnings_source
        self.universe_source = universe_source
        self.source_priority = tuple(source_priority)
        self.request_delay_seconds = request_delay_seconds
        self.clock = clock

    def refresh_universe(self, *, today: date, lookback_days: int = 730) -> int:
        snapshot = self.universe_source.fetch(today=today, lookback_days=lookback_days)
        return self.database.add_universe_snapshot(
            source=snapshot.source,
            universe=snapshot.universe,
            memberships=snapshot.memberships,
            requested_at=snapshot.requested_at,
            observed_at=snapshot.observed_at,
            coverage_start=snapshot.coverage_start,
            raw_payload=snapshot.raw_payload,
        )

    def refresh_earnings(
        self,
        start: date,
        end: date,
        *,
        continue_on_error: bool = True,
    ) -> RefreshReport:
        if end < start:
            raise ValueError("end must be on or after start")
        successes = failures = observations = 0
        dates = list(_date_range(start, end))
        for index, calendar_date in enumerate(dates):
            try:
                snapshot = self.earnings_source.fetch_date(calendar_date)
                self.database.add_earnings_snapshot(
                    source=snapshot.source,
                    calendar_date=snapshot.calendar_date,
                    events=snapshot.events,
                    requested_at=snapshot.requested_at,
                    observed_at=snapshot.observed_at,
                    raw_payload=snapshot.raw_payload,
                )
                successes += 1
                observations += len(snapshot.events)
            except Exception as exc:
                failures += 1
                now = self.clock()
                self.database.add_failed_snapshot(
                    source=self.earnings_source.name,
                    scope="earnings_date",
                    scope_key=calendar_date.isoformat(),
                    requested_at=now,
                    observed_at=now,
                    error=f"{type(exc).__name__}: {exc}",
                )
                if not continue_on_error:
                    raise
            if self.request_delay_seconds and index + 1 < len(dates):
                time.sleep(self.request_delay_seconds)
        return RefreshReport(
            source=self.earnings_source.name,
            start=start,
            end=end,
            successful_dates=successes,
            failed_dates=failures,
            observations=observations,
        )

    def materialize_reference(
        self,
        as_of_start: date,
        as_of_end: date,
        *,
        known_at: datetime | str | None = None,
        universe: str = "sp500",
        horizon_days: int = 365,
        max_events: int = 4,
    ) -> int:
        if as_of_end < as_of_start:
            raise ValueError("as_of_end must be on or after as_of_start")
        if max_events != 4:
            raise ValueError("the SQLite wide schema currently supports exactly four event slots")
        known_at_text = iso_datetime(known_at or self.clock())
        universe_snapshot_id, memberships = self.database.universe_intervals(
            universe=universe, known_at=known_at_text
        )
        events = self.database.events_between(
            as_of_start,
            as_of_end + timedelta(days=horizon_days),
            known_at=known_at_text,
        )
        deduped = _dedupe_events(events, self.source_priority)
        by_symbol: dict[str, list[dict[str, Any]]] = defaultdict(list)
        snapshot_ids: set[int] = set()
        for event in deduped:
            by_symbol[event["symbol"]].append(event)
            snapshot_ids.add(int(event["snapshot_id"]))
        for symbol_values in by_symbol.values():
            symbol_values.sort(key=lambda row: (row["event_date"], row["source"]))

        columns = ["build_id", "as_of_date", "symbol", "company_name", "sector"]
        for slot in range(1, 5):
            columns.extend(f"event_{slot}_{name}" for name in REFERENCE_EVENT_COLUMNS)
        placeholders = ",".join("?" for _ in columns)
        insert_sql = f"INSERT INTO reference_rows({','.join(columns)}) VALUES({placeholders})"

        with self.database.transaction() as con:
            cur = con.execute(
                """INSERT INTO reference_builds(
                       as_of_start, as_of_end, known_at, horizon_days, max_events,
                       created_at, universe_snapshot_id, source_snapshot_ids)
                   VALUES(?, ?, ?, ?, ?, ?, ?, ?)""",
                (
                    as_of_start.isoformat(),
                    as_of_end.isoformat(),
                    known_at_text,
                    horizon_days,
                    max_events,
                    iso_datetime(self.clock()),
                    universe_snapshot_id,
                    json.dumps(sorted(snapshot_ids)),
                ),
            )
            if cur.lastrowid is None:
                raise RuntimeError("SQLite did not return a build id")
            build_id = cur.lastrowid
            batch: list[tuple[Any, ...]] = []
            for membership in memberships:
                valid_start = max(as_of_start, date.fromisoformat(membership["valid_from"]))
                valid_to = (
                    date.fromisoformat(membership["valid_to"])
                    if membership["valid_to"]
                    else as_of_end + timedelta(days=1)
                )
                valid_end = min(as_of_end, valid_to - timedelta(days=1))
                if valid_end < valid_start:
                    continue
                symbol_events = by_symbol.get(membership["symbol"], [])
                event_dates = [row["event_date"] for row in symbol_events]
                for as_of_date in _date_range(valid_start, valid_end):
                    start_index = bisect.bisect_left(event_dates, as_of_date.isoformat())
                    horizon_end = (as_of_date + timedelta(days=horizon_days)).isoformat()
                    upcoming = []
                    for event in symbol_events[start_index:]:
                        if event["event_date"] > horizon_end:
                            break
                        upcoming.append(event)
                        if len(upcoming) == max_events:
                            break
                    values: list[Any] = [
                        build_id,
                        as_of_date.isoformat(),
                        membership["symbol"],
                        membership["company_name"],
                        membership["sector"],
                    ]
                    for slot in range(max_events):
                        slot_values: list[Any] = (
                            _event_values(upcoming[slot])
                            if slot < len(upcoming)
                            else [None] * len(REFERENCE_EVENT_COLUMNS)
                        )
                        values.extend(slot_values)
                    batch.append(tuple(values))
                    if len(batch) >= 10_000:
                        con.executemany(insert_sql, batch)
                        batch.clear()
            if batch:
                con.executemany(insert_sql, batch)
        return build_id

    def backfill(
        self,
        *,
        today: date | None = None,
        history_days: int = 730,
        horizon_days: int = 365,
    ) -> dict[str, Any]:
        today = today or self.clock().date()
        job_id = self.database.begin_job("backfill", started_at=self.clock())
        try:
            self.refresh_universe(today=today, lookback_days=history_days)
            as_of_start = today - timedelta(days=history_days)
            report = self.refresh_earnings(as_of_start, today + timedelta(days=horizon_days))
            build_id = self.materialize_reference(
                as_of_start,
                today,
                known_at=self.clock(),
                horizon_days=horizon_days,
            )
            details = {"build_id": build_id, **_report_dict(report)}
            status = "PARTIAL" if report.failed_dates else "SUCCESS"
            self.database.finish_job(
                job_id, status=status, finished_at=self.clock(), details=details
            )
            return {"job_run_id": job_id, "status": status, **details}
        except Exception as exc:
            self.database.finish_job(
                job_id,
                status="FAILED",
                finished_at=self.clock(),
                error=f"{type(exc).__name__}: {exc}",
            )
            raise

    def daily(
        self,
        *,
        today: date | None = None,
        revision_lookback_days: int = 30,
        universe_lookback_days: int = 730,
        horizon_days: int = 365,
    ) -> dict[str, Any]:
        """Refresh upcoming dates and a trailing correction window, then append a build."""
        today = today or self.clock().date()
        job_id = self.database.begin_job("daily", started_at=self.clock())
        try:
            self.refresh_universe(today=today, lookback_days=universe_lookback_days)
            start = today - timedelta(days=revision_lookback_days)
            report = self.refresh_earnings(start, today + timedelta(days=horizon_days))
            build_id = self.materialize_reference(
                start,
                today,
                known_at=self.clock(),
                horizon_days=horizon_days,
            )
            details = {"build_id": build_id, **_report_dict(report)}
            status = "PARTIAL" if report.failed_dates else "SUCCESS"
            self.database.finish_job(
                job_id, status=status, finished_at=self.clock(), details=details
            )
            return {"job_run_id": job_id, "status": status, **details}
        except Exception as exc:
            self.database.finish_job(
                job_id,
                status="FAILED",
                finished_at=self.clock(),
                error=f"{type(exc).__name__}: {exc}",
            )
            raise


def _dedupe_events(rows: Sequence[Any], source_priority: Sequence[str]) -> list[dict[str, Any]]:
    rank = {source: index for index, source in enumerate(source_priority)}
    status_rank = {"REPORTED": 0, "CONFIRMED": 1, "ESTIMATED": 2, "UNKNOWN": 3}
    candidates = [dict(row) for row in rows]
    candidates.sort(
        key=lambda row: (
            row["symbol"],
            status_rank.get(row["date_status"], 9),
            rank.get(row["source"], len(rank)),
            row["event_date"],
        )
    )
    kept: list[dict[str, Any]] = []
    by_symbol: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for candidate in candidates:
        event_day = date.fromisoformat(candidate["event_date"])
        duplicate = False
        for existing in by_symbol[candidate["symbol"]]:
            existing_day = date.fromisoformat(existing["event_date"])
            same_period = (
                not candidate["fiscal_quarter_ending"]
                or not existing["fiscal_quarter_ending"]
                or candidate["fiscal_quarter_ending"] == existing["fiscal_quarter_ending"]
            )
            if same_period and abs((event_day - existing_day).days) <= 7:
                duplicate = True
                break
        if not duplicate:
            by_symbol[candidate["symbol"]].append(candidate)
            kept.append(candidate)
    return kept


def _event_values(event: dict[str, Any]) -> list[Any]:
    return [
        event["event_date"],
        event["session"],
        event["date_status"],
        event["fiscal_quarter_ending"],
        event["eps_estimate"],
        event["reported_eps"],
        event["surprise_percent"],
        event["estimator_count"],
        event["source"],
        event["observed_at"],
    ]


def _date_range(start: date, end: date) -> Iterator[date]:
    current = start
    while current <= end:
        yield current
        current += timedelta(days=1)


def _report_dict(report: RefreshReport) -> dict[str, Any]:
    return {
        "source": report.source,
        "start": report.start.isoformat(),
        "end": report.end.isoformat(),
        "successful_dates": report.successful_dates,
        "failed_dates": report.failed_dates,
        "observations": report.observations,
    }
