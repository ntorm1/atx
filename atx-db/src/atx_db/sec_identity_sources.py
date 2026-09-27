"""Pure adapters for already sealed SEC identity rows; never fetch or scan archives.

Official layouts: sec.gov/files/insider_transactions_readme.pdf (SUBMISSION),
sec.gov/files/financial-statement-data-sets.pdf (SUB). Insider has no acceptance
column. FSDS instance prefixes and current submissions tickers are not verified
historical security-line evidence. Vintage/publication proof is supplied separately.
"""
from __future__ import annotations

import datetime as dt
import re
from collections.abc import Iterable, Mapping
from dataclasses import dataclass

from .security_link import FilingEvidence, SEAL, cik_key, symbol_key, utc


@dataclass(frozen=True)
class FilingClock:
    accepted_at: dt.datetime | None = None
    published_at: dt.datetime | None = None
    revision_available_at: dt.datetime | None = None
    revision_status: str = "unverified"
    raw: str = ""


@dataclass(frozen=True)
class SourceProvenance:
    sha256: str
    locator: str
    observed_at: dt.datetime


def acceptance_timestamp(raw: str, naive_timezone: dt.tzinfo | None = None) -> dt.datetime | None:
    """No invented filed-day clock. Naive EDGAR stamps require declared timezone.

    Callers must supply a fixed offset or a separately resolved zone/fold; a fixed
    dissemination lag does not upgrade acceptance to verified public availability.
    """
    if not raw.strip():
        return None
    parsed = (dt.datetime.strptime(raw, "%Y%m%d%H%M%S") if re.fullmatch(r"\d{14}", raw)
              else dt.datetime.fromisoformat(raw.replace("Z", "+00:00")))
    if parsed.tzinfo is None:
        if naive_timezone is None:
            return None
        a, b = parsed.replace(tzinfo=naive_timezone, fold=0), parsed.replace(tzinfo=naive_timezone, fold=1)
        if a.utcoffset() != b.utcoffset():
            raise ValueError("ambiguous/nonexistent local acceptance time requires explicit offset")
        parsed = a
    return utc(parsed)


def _filed(raw: str) -> dt.date:
    if re.fullmatch(r"\d{8}", raw):
        result = dt.datetime.strptime(raw, "%Y%m%d").date()
    elif re.fullmatch(r"\d{2}-[A-Za-z]{3}-\d{4}", raw):
        day, month, year = raw.upper().split("-")
        months = "JAN FEB MAR APR MAY JUN JUL AUG SEP OCT NOV DEC".split()
        result = dt.date(int(year), months.index(month) + 1, int(day))
    else:
        result = dt.date.fromisoformat(raw)
    if result >= SEAL:
        raise ValueError("unsealed SEC filing row")
    return result


def insider_submission_rows(rows: Iterable[Mapping[str, str]], source: SourceProvenance,
                            clocks: Mapping[str, FilingClock]) -> list[FilingEvidence]:
    result = []
    for row in rows:
        accession = row["ACCESSION_NUMBER"]
        clock = clocks.get(accession, FilingClock())
        result.append(FilingEvidence(
            f"insider:{source.sha256}:{accession}", accession, cik_key(row["ISSUERCIK"]),
            row["ISSUERTRADINGSYMBOL"], _filed(row["FILING_DATE"]), clock.accepted_at,
            source.sha256, source.locator + "#" + accession, source.observed_at,
            clock.published_at, clock.revision_available_at, clock.revision_status,
            accepted_raw=clock.raw))
    return result


def fsds_submission_rows(rows: Iterable[Mapping[str, str]], source: SourceProvenance,
                         clocks: Mapping[str, FilingClock],
                         naive_timezone: dt.tzinfo | None = None) -> list[FilingEvidence]:
    result = []
    for row in rows:
        accession = row["adsh"]
        raw = row.get("accepted", "")
        parsed = acceptance_timestamp(raw, naive_timezone)
        clock = clocks.get(accession, FilingClock(accepted_at=parsed, raw=raw))
        if parsed is not None and clock.accepted_at is not None and utc(parsed) != utc(clock.accepted_at):
            raise ValueError("conflicting accession acceptance timestamps")
        # Do not turn a filename heuristic into dated ticker proof. Co-registrant
        # CIKs/aciks and prevrpt are not used to backdate or prune historical state.
        prefix = row.get("instance", "").split("-", 1)[0]
        ticker = symbol_key(prefix) if prefix else "UNKNOWN"
        result.append(FilingEvidence(
            f"fsds:{source.sha256}:{accession}", accession, cik_key(row["cik"]), ticker,
            _filed(row["filed"]), clock.accepted_at, source.sha256,
            source.locator + "#" + accession, source.observed_at, clock.published_at,
            clock.revision_available_at, clock.revision_status, "instance-prefix-candidate", raw))
    return result


def dated_submission_rows(rows: Iterable[Mapping[str, str]], source: SourceProvenance,
                          clocks: Mapping[str, FilingClock]) -> list[FilingEvidence]:
    """Normalized original filing/header projection with an explicit issuer symbol.

    Input requires `issuer_trading_symbol` and `symbol_evidence_locator`; top-level
    current `tickers` arrays are intentionally not accepted as a substitute.
    """
    result = []
    for row in rows:
        accession = row["accession_number"]
        clock = clocks.get(accession, FilingClock())
        locator = row["symbol_evidence_locator"]
        if not locator:
            raise ValueError("dated issuer symbol requires original filing locator")
        result.append(FilingEvidence(
            f"submission:{source.sha256}:{accession}", accession, cik_key(row["issuer_cik"]),
            row["issuer_trading_symbol"], _filed(row["filing_date"]), clock.accepted_at,
            source.sha256, locator, source.observed_at, clock.published_at,
            clock.revision_available_at, clock.revision_status, accepted_raw=clock.raw))
    return result
