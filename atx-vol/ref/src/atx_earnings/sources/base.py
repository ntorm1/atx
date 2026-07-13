from __future__ import annotations

from dataclasses import dataclass
from datetime import date, datetime
from typing import Protocol

from ..models import EarningsEvent, UniverseMembership


@dataclass(frozen=True)
class EarningsSnapshot:
    source: str
    calendar_date: date
    requested_at: datetime
    observed_at: datetime
    events: tuple[EarningsEvent, ...]
    raw_payload: str


@dataclass(frozen=True)
class UniverseSnapshot:
    source: str
    universe: str
    requested_at: datetime
    observed_at: datetime
    coverage_start: date
    memberships: tuple[UniverseMembership, ...]
    raw_payload: str


class EarningsSource(Protocol):
    name: str

    def fetch_date(self, calendar_date: date) -> EarningsSnapshot: ...


class UniverseSource(Protocol):
    name: str

    def fetch(self, *, today: date, lookback_days: int) -> UniverseSnapshot: ...

