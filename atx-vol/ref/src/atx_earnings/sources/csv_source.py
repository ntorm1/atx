from __future__ import annotations

import csv
from collections import defaultdict
from datetime import date, datetime, timezone
from pathlib import Path

from ..db import EarningsDatabase
from ..models import DateStatus, EarningsEvent, MarketSession


def load_csv_snapshots(
    database: EarningsDatabase,
    path: str | Path,
    *,
    source: str = "csv-confirmed",
    observed_at: datetime | None = None,
) -> list[int]:
    """Import a vendor/IR calendar without weakening PIT semantics.

    Required columns: symbol,event_date. Optional columns mirror EarningsEvent.
    Each date in the file becomes an atomic snapshot. To represent an explicitly
    empty date, include a row with only event_date populated.
    """
    observed_at = observed_at or datetime.now(timezone.utc).replace(microsecond=0)
    grouped: dict[date, list[EarningsEvent]] = defaultdict(list)
    raw = Path(path).read_text(encoding="utf-8-sig")
    for index, row in enumerate(csv.DictReader(raw.splitlines()), start=2):
        if not row.get("event_date"):
            raise ValueError(f"CSV row {index} has no event_date")
        event_date = date.fromisoformat(row["event_date"].strip())
        if not row.get("symbol", "").strip():
            grouped[event_date]
            continue
        grouped[event_date].append(
            EarningsEvent(
                symbol=row["symbol"],
                event_date=event_date,
                source_record_key=row.get("source_record_key")
                or f"{row['symbol']}:{event_date.isoformat()}:{index}",
                company_name=_none(row.get("company_name")),
                session=_enum(MarketSession, row.get("session"), MarketSession.UNKNOWN),
                date_status=_enum(DateStatus, row.get("date_status"), DateStatus.CONFIRMED),
                fiscal_quarter_ending=_date(row.get("fiscal_quarter_ending")),
                eps_estimate=_float(row.get("eps_estimate")),
                reported_eps=_float(row.get("reported_eps")),
                surprise_percent=_float(row.get("surprise_percent")),
                estimator_count=_int(row.get("estimator_count")),
                eps_prior_year=_float(row.get("eps_prior_year")),
                prior_year_report_date=_date(row.get("prior_year_report_date")),
                currency=_none(row.get("currency")),
                raw=dict(row),
            )
        )
    ids = []
    for calendar_date, events in sorted(grouped.items()):
        ids.append(
            database.add_earnings_snapshot(
                source=source,
                calendar_date=calendar_date,
                events=events,
                requested_at=observed_at,
                observed_at=observed_at,
                raw_payload=raw,
            )
        )
    return ids


def _none(value: str | None) -> str | None:
    return value.strip() if value and value.strip() else None


def _date(value: str | None) -> date | None:
    return date.fromisoformat(value.strip()) if value and value.strip() else None


def _float(value: str | None) -> float | None:
    return float(value) if value and value.strip() else None


def _int(value: str | None) -> int | None:
    return int(value) if value and value.strip() else None


def _enum(enum_type, value: str | None, default):
    return enum_type(value.strip().upper()) if value and value.strip() else default
