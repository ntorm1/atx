from __future__ import annotations

from datetime import date, datetime, timedelta
from typing import TypedDict

import httpx
from bs4 import BeautifulSoup  # type: ignore[import-untyped]

from ..models import UniverseMembership, normalize_symbol, utc_now
from .base import UniverseSnapshot


class _Segment(TypedDict):
    valid_from: date
    valid_to: date | None
    company_name: str | None
    sector: str | None
    sub_industry: str | None
    cik: str | None


class WikipediaSP500Source:
    name = "wikipedia-sp500"
    # Printable HTML avoids the JavaScript-oriented edge response while retaining
    # the same two source tables.
    url = "https://en.wikipedia.org/wiki/List_of_S%26P_500_companies?printable=yes"

    def __init__(self, *, timeout: float = 30.0, client: httpx.Client | None = None) -> None:
        self._owns_client = client is None
        self.client = client or httpx.Client(
            timeout=timeout,
            follow_redirects=True,
            headers={
                "User-Agent": (
                    "atx-earnings-ref/0.1 "
                    "(https://github.com/atx-research/atx-earnings-ref)"
                )
            },
        )

    def close(self) -> None:
        if self._owns_client:
            self.client.close()

    def __enter__(self) -> WikipediaSP500Source:
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def fetch(self, *, today: date, lookback_days: int) -> UniverseSnapshot:
        requested_at = utc_now()
        response = self.client.get(self.url)
        response.raise_for_status()
        observed_at = utc_now()
        coverage_start = today - timedelta(days=lookback_days)
        memberships = parse_sp500_html(response.text, today=today, coverage_start=coverage_start)
        return UniverseSnapshot(
            source=self.name,
            universe="sp500",
            requested_at=requested_at,
            observed_at=observed_at,
            coverage_start=coverage_start,
            memberships=tuple(memberships),
            raw_payload=response.text,
        )


def parse_sp500_html(html: str, *, today: date, coverage_start: date) -> list[UniverseMembership]:
    soup = BeautifulSoup(html, "html.parser")
    tables = soup.select("table.wikitable")
    if len(tables) < 2:
        raise ValueError("Wikipedia page does not contain constituent and changes tables")

    current: dict[str, dict[str, str | None]] = {}
    for tr in tables[0].select("tbody tr"):
        cells = tr.find_all(["td", "th"])
        if len(cells) < 2 or cells[0].name == "th":
            continue
        symbol = normalize_symbol(cells[0].get_text(" ", strip=True))
        if not symbol:
            continue
        current[symbol] = {
            "company_name": cells[1].get_text(" ", strip=True) or None,
            "sector": cells[2].get_text(" ", strip=True) if len(cells) > 2 else None,
            "sub_industry": cells[3].get_text(" ", strip=True) if len(cells) > 3 else None,
            "cik": cells[6].get_text(" ", strip=True) if len(cells) > 6 else None,
        }

    changes: list[tuple[date, str | None, str | None, str | None, str | None]] = []
    for tr in tables[1].select("tbody tr"):
        cells = tr.find_all("td")
        if len(cells) < 5:
            continue
        effective = _parse_change_date(cells[0].get_text(" ", strip=True))
        if effective is None or effective > today or effective < coverage_start:
            continue
        added_symbol = _symbol_or_none(cells[1].get_text(" ", strip=True))
        added_name = cells[2].get_text(" ", strip=True) or None
        removed_symbol = _symbol_or_none(cells[3].get_text(" ", strip=True))
        removed_name = cells[4].get_text(" ", strip=True) or None
        changes.append((effective, added_symbol, added_name, removed_symbol, removed_name))

    if not current:
        raise ValueError("Wikipedia constituent table parsed zero symbols")
    return reconstruct_memberships(
        current=current, changes=changes, coverage_start=coverage_start
    )


def reconstruct_memberships(
    *,
    current: dict[str, dict[str, str | None]],
    changes: list[tuple[date, str | None, str | None, str | None, str | None]],
    coverage_start: date,
) -> list[UniverseMembership]:
    segments: dict[str, list[_Segment]] = {}
    for symbol, metadata in current.items():
        segments[symbol] = [
            _Segment(
                valid_from=coverage_start,
                valid_to=None,
                company_name=metadata.get("company_name"),
                sector=metadata.get("sector"),
                sub_industry=metadata.get("sub_industry"),
                cik=metadata.get("cik"),
            )
        ]
    present = set(current)
    open_segment = {symbol: segments[symbol][0] for symbol in current}

    for effective, added, _added_name, removed, removed_name in sorted(
        changes, key=lambda row: row[0], reverse=True
    ):
        # Moving backward across the effective date reverses the published change.
        if added and added in present:
            open_segment[added]["valid_from"] = effective
            present.remove(added)
            del open_segment[added]
        if removed and removed not in present:
            metadata = current.get(removed, {})
            segment = _Segment(
                valid_from=coverage_start,
                valid_to=effective,
                company_name=removed_name or metadata.get("company_name"),
                sector=metadata.get("sector"),
                sub_industry=metadata.get("sub_industry"),
                cik=metadata.get("cik"),
            )
            segments.setdefault(removed, []).append(segment)
            present.add(removed)
            open_segment[removed] = segment

    result: list[UniverseMembership] = []
    for symbol, symbol_segments in segments.items():
        for segment in symbol_segments:
            if segment["valid_to"] is not None and segment["valid_from"] >= segment["valid_to"]:
                continue
            result.append(
                UniverseMembership(
                    symbol=symbol,
                    valid_from=segment["valid_from"],
                    valid_to=segment["valid_to"],
                    company_name=segment.get("company_name"),
                    sector=segment.get("sector"),
                    sub_industry=segment.get("sub_industry"),
                    cik=segment.get("cik"),
                )
            )
    return sorted(result, key=lambda item: (item.symbol, item.valid_from))


def _parse_change_date(text: str) -> date | None:
    for fmt in ("%B %d, %Y", "%Y-%m-%d"):
        try:
            return datetime.strptime(text, fmt).date()
        except ValueError:
            pass
    return None


def _symbol_or_none(value: str) -> str | None:
    return normalize_symbol(value) if value.strip() else None
