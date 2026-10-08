"""Offline native-text candidates, never a certification of parties or completion.

One immutable row per document clock avoids v1's later terms/status leaking into
announcement rows. Native source/clock/identity replay admission is required
before publication; raw parser rows carry explicit unsupported reasons.
"""
from __future__ import annotations

import datetime as dt
import re
from dataclasses import dataclass
from decimal import Decimal

from .terminal_mna_v2 import Evidence, utc

SCHEMA = "atx.alpha-panel.events.mna/v2"
_CLAUSE = re.compile(r"converted\s+(?:automatically\s+)?into\s+the\s+right\s+to\s+receive", re.I)
_CASH = re.compile(r"\$\s*(\d+(?:\.\d+)?)\s+(?:per\s+share\s+)?in\s+cash", re.I)
_STOCK = re.compile(r"(\d+(?:\.\d+)?)\s+(?:of\s+a\s+)?shares?\s+of\s+(?:Parent|Acquirer)\s+(?:common\s+)?stock", re.I)
_UNSUPPORTED = re.compile(r"contingent|\bCVR\b|election|proration|fractional|ADS|ordinary shares|subject to|up to", re.I)


@dataclass(frozen=True)
class DealVintage:
    accession: str
    source_id: str
    available_at: dt.datetime
    consideration_type: str | None
    cash_per_share: Decimal | None
    stock_ratio: Decimal | None
    status: str
    evidence: str
    reasons: tuple[str, ...]


def parse_native_candidate(accession: str, text: str, evidence: Evidence) -> DealVintage:
    """Conservative extraction with full unreadable/unsupported population retained.

    Values retain the literal dollar symbol ambiguity; no currency, security,
    share class, announcement date or completion date is inferred here.
    """
    flat = re.sub(r"\s+", " ", text)
    clauses = list(_CLAUSE.finditer(flat))
    reasons = ["native_replay_admission_unavailable", *evidence.errors()]
    cash = ratio = None
    clause = ""
    if len(clauses) != 1:
        reasons.append("missing_consideration_clause" if not clauses else "ambiguous_consideration_clauses")
    else:
        clause = flat[clauses[0].end():clauses[0].end() + 700]
        # Decimal points must not terminate a sentence.
        clause = re.split(r"(?<=[A-Za-z)])\.\s+", clause, maxsplit=1)[0]
        cm, sm = list(_CASH.finditer(clause)), list(_STOCK.finditer(clause))
        if _UNSUPPORTED.search(clause) or len(cm) > 1 or len(sm) > 1:
            reasons.append("ambiguous_or_contingent_consideration")
        elif not cm and not sm:
            reasons.append("unsupported_consideration")
        else:
            cash = Decimal(cm[0].group(1)) if cm else None
            ratio = Decimal(sm[0].group(1)) if sm else None
            if any(x is not None and (not x.is_finite() or x <= 0) for x in (cash, ratio)):
                cash = ratio = None
                reasons.append("invalid_consideration")
    ctype = "mixed" if cash is not None and ratio is not None else "cash" if cash is not None else "stock" if ratio is not None else None
    return DealVintage(accession, evidence.source_id, utc(evidence.available_at), ctype, cash, ratio,
                       "unknown", clause, tuple(sorted(set(reasons))))


def validate_vintages(rows: list[DealVintage]) -> None:
    keys = [(row.accession, row.source_id, utc(row.available_at)) for row in rows]
    if any(not row.accession or not row.source_id for row in rows) or len(set(keys)) != len(keys):
        raise ValueError("nonnull unique document-vintage keys required")
