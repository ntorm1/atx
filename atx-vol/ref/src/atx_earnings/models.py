from __future__ import annotations

from dataclasses import dataclass, field
from datetime import date, datetime, timezone
from enum import Enum
from typing import Any


class MarketSession(str, Enum):
    BMO = "BMO"
    AMC = "AMC"
    DURING_MARKET = "DURING_MARKET"
    UNKNOWN = "UNKNOWN"


class DateStatus(str, Enum):
    ESTIMATED = "ESTIMATED"
    CONFIRMED = "CONFIRMED"
    REPORTED = "REPORTED"
    UNKNOWN = "UNKNOWN"


def utc_now() -> datetime:
    return datetime.now(timezone.utc)


def iso_datetime(value: datetime | str | None = None) -> str:
    if value is None:
        value = utc_now()
    if isinstance(value, str):
        value = datetime.fromisoformat(value.replace("Z", "+00:00"))
    if value.tzinfo is None:
        value = value.replace(tzinfo=timezone.utc)
    return value.astimezone(timezone.utc).isoformat().replace("+00:00", "Z")


def normalize_symbol(symbol: str) -> str:
    """Use a single canonical spelling while retaining share-class dots."""
    return symbol.strip().upper().replace("/", ".").replace("-", ".")


@dataclass(frozen=True)
class EarningsEvent:
    symbol: str
    event_date: date
    source_record_key: str
    company_name: str | None = None
    session: MarketSession = MarketSession.UNKNOWN
    date_status: DateStatus = DateStatus.UNKNOWN
    fiscal_quarter_ending: date | None = None
    eps_estimate: float | None = None
    reported_eps: float | None = None
    surprise_percent: float | None = None
    estimator_count: int | None = None
    eps_prior_year: float | None = None
    prior_year_report_date: date | None = None
    currency: str | None = None
    raw: dict[str, Any] = field(default_factory=dict)

    def __post_init__(self) -> None:
        object.__setattr__(self, "symbol", normalize_symbol(self.symbol))


@dataclass(frozen=True)
class UniverseMembership:
    symbol: str
    valid_from: date
    valid_to: date | None
    company_name: str | None = None
    sector: str | None = None
    sub_industry: str | None = None
    cik: str | None = None

    def __post_init__(self) -> None:
        object.__setattr__(self, "symbol", normalize_symbol(self.symbol))


@dataclass(frozen=True)
class RefreshReport:
    source: str
    start: date
    end: date
    successful_dates: int
    failed_dates: int
    observations: int
