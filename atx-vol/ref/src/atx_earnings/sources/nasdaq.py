from __future__ import annotations

import re
import time
from datetime import date, datetime
from typing import Any

import httpx

from ..models import DateStatus, EarningsEvent, MarketSession, utc_now
from .base import EarningsSnapshot


class NasdaqEarningsSource:
    """Public Nasdaq earnings calendar adapter.

    Nasdaq describes these rows as expected dates derived from historical reporting
    patterns, so the adapter deliberately labels them ESTIMATED.  Confirmed company
    announcements can be overlaid with the CSV importer using source priority.
    """

    name = "nasdaq"
    endpoint = "https://api.nasdaq.com/api/calendar/earnings"

    def __init__(
        self,
        *,
        timeout: float = 30.0,
        retries: int = 3,
        client: httpx.Client | None = None,
    ) -> None:
        self.retries = retries
        self._owns_client = client is None
        self.client = client or httpx.Client(
            timeout=timeout,
            follow_redirects=True,
            headers={
                "User-Agent": "Mozilla/5.0 (compatible; atx-earnings-ref/0.1)",
                "Accept": "application/json, text/plain, */*",
                "Accept-Language": "en-US,en;q=0.9",
                "Origin": "https://www.nasdaq.com",
                "Referer": "https://www.nasdaq.com/market-activity/earnings",
            },
        )

    def close(self) -> None:
        if self._owns_client:
            self.client.close()

    def __enter__(self) -> NasdaqEarningsSource:
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def fetch_date(self, calendar_date: date) -> EarningsSnapshot:
        requested_at = utc_now()
        response: httpx.Response | None = None
        for attempt in range(self.retries):
            try:
                response = self.client.get(
                    self.endpoint, params={"date": calendar_date.isoformat()}
                )
                response.raise_for_status()
                break
            except (httpx.HTTPError, httpx.TimeoutException):
                if attempt + 1 >= self.retries:
                    raise
                time.sleep(0.5 * (2**attempt))
        assert response is not None
        observed_at = utc_now()
        payload = response.text
        parsed = response.json()
        events = parse_nasdaq_payload(parsed, calendar_date)
        return EarningsSnapshot(
            source=self.name,
            calendar_date=calendar_date,
            requested_at=requested_at,
            observed_at=observed_at,
            events=tuple(events),
            raw_payload=payload,
        )


def parse_nasdaq_payload(payload: dict[str, Any], calendar_date: date) -> list[EarningsEvent]:
    data = payload.get("data") or {}
    rows = data.get("rows") or []
    if not isinstance(rows, list):
        raise ValueError("Nasdaq payload data.rows is not a list")
    events: list[EarningsEvent] = []
    key_counts: dict[str, int] = {}
    for row in rows:
        if not isinstance(row, dict) or not row.get("symbol"):
            continue
        symbol = _clean_text(row.get("symbol"))
        base_key = f"{symbol}:{calendar_date.isoformat()}"
        key_counts[base_key] = key_counts.get(base_key, 0) + 1
        key = base_key if key_counts[base_key] == 1 else f"{base_key}:{key_counts[base_key]}"
        reported_eps = _parse_number(row.get("eps"))
        events.append(
            EarningsEvent(
                symbol=symbol,
                company_name=_optional_text(row.get("name")),
                event_date=calendar_date,
                source_record_key=key,
                session=_parse_session(row.get("time")),
                date_status=(
                    DateStatus.REPORTED if reported_eps is not None else DateStatus.ESTIMATED
                ),
                fiscal_quarter_ending=_parse_fiscal_period(row.get("fiscalQuarterEnding")),
                eps_estimate=_parse_number(row.get("epsForecast")),
                reported_eps=reported_eps,
                surprise_percent=_parse_number(row.get("surprise")),
                estimator_count=_parse_integer(row.get("noOfEsts")),
                eps_prior_year=_parse_number(row.get("lastYearEPS")),
                prior_year_report_date=_parse_date(row.get("lastYearRptDt")),
                raw=row,
            )
        )
    return events


def _parse_session(value: Any) -> MarketSession:
    text = _clean_text(value).lower().replace("_", "-")
    if "pre-market" in text or "before" in text or text in {"bmo", "am"}:
        return MarketSession.BMO
    if "after-hours" in text or "after market" in text or text in {"amc", "pm"}:
        return MarketSession.AMC
    if "market-hours" in text or "during" in text:
        return MarketSession.DURING_MARKET
    return MarketSession.UNKNOWN


def _parse_number(value: Any) -> float | None:
    text = _clean_text(value)
    if not text or text.lower() in {"n/a", "na", "--"}:
        return None
    negative = text.startswith("(") and text.endswith(")")
    cleaned = re.sub(r"[^0-9.+-]", "", text)
    if not cleaned:
        return None
    number = float(cleaned)
    return -abs(number) if negative else number


def _parse_integer(value: Any) -> int | None:
    number = _parse_number(value)
    return int(number) if number is not None else None


def _parse_date(value: Any) -> date | None:
    text = _clean_text(value)
    for fmt in ("%m/%d/%Y", "%Y-%m-%d", "%b %d, %Y"):
        try:
            return datetime.strptime(text, fmt).date()
        except ValueError:
            pass
    return None


def _parse_fiscal_period(value: Any) -> date | None:
    text = _clean_text(value)
    for fmt in ("%b/%Y", "%m/%Y", "%Y-%m-%d", "%m/%d/%Y"):
        try:
            parsed = datetime.strptime(text, fmt).date()
            return parsed
        except ValueError:
            pass
    return None


def _clean_text(value: Any) -> str:
    return "" if value is None else str(value).replace("$", "").strip()


def _optional_text(value: Any) -> str | None:
    text = _clean_text(value)
    return text or None
