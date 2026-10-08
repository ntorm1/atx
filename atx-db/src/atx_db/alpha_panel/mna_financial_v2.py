"""Conservative native financial clauses with public complete-inventory replay.

Native monetary/date/party text is distinct from external security/price basis
admission. No filing-item code or issuer primary line certifies financial terms.
Only explicitly named common-share, unconditional cash/USD or share conversion
sentences are supported. Unknown classes, alternatives and dates are retained.
"""
from __future__ import annotations

import datetime as dt
import hashlib
import re
from dataclasses import dataclass, fields
from decimal import Decimal
from pathlib import Path
from typing import Mapping

from . import mna_native_v2 as N
from . import terminal_mna_v2 as T

SCHEMA = "atx.alpha-panel.events.mna/v2"

_DATE = r"(?:January|February|March|April|May|June|July|August|September|October|November|December) \d{1,2}, \d{4}"
_COMPLETE = re.compile(r"\bOn (?P<date>" + _DATE + r"), (?P<target>[A-Z][\w&.,' -]{1,100}?) "
                       r"(?:completed|consummated) (?:the |its )?(?:previously announced )?"
                       r"(?:merger|business combination)(?: with (?P<acquirer>[A-Z][\w&.,' -]{1,100}?))?(?:\.| pursuant to)")
_SETTLE = re.compile(r"\bOn (?P<date>" + _DATE + r"), (?:the )?(?:cash consideration was paid|"
                     r"stock consideration was delivered|merger consideration was paid and delivered)\b")
_ANNOUNCE = re.compile(r"\bOn (?P<date>" + _DATE + r"), (?P<target>[A-Z][\w&.,' -]{1,100}?) "
                       r"announced (?:the |its )?(?:merger|business combination) with (?P<acquirer>[A-Z][\w&.,' -]{1,100}?)(?:\.| pursuant to)")
_TERMINATE = re.compile(r"\bOn (?P<date>" + _DATE + r"), (?P<target>[A-Z][\w&.,' -]{1,100}?) "
                        r"terminated (?:the |its )?(?:merger|business combination) with (?P<acquirer>[A-Z][\w&.,' -]{1,100}?)(?:\.| pursuant to)")
_CONVERT = re.compile(r"\b(?:was|were|is|will be) (?:automatically )?converted into (?:the )?(?:ultimate )?right to receive\b", re.I)
_NAMED_CLASS = re.compile(r"\beach (?:issued and outstanding )?share of (?:the )?"
                         r"(?P<issuer>[A-Z][\w&.,' -]{1,100}?) (?P<class>(?:Class [A-Z] )?common stock)\b")
_USD = re.compile(r"\b(?:USD|US\$|U\.S\. dollars?)\s*(?P<amount>\d+(?:\.\d+)?)\s+(?:per share\s+)?in cash\b", re.I)
_STOCK = re.compile(r"(?P<ratio>\d+(?:\.\d+)?) (?:of a )?shares? of "
                    r"(?P<issuer>[A-Z][\w&.,' -]{1,100}?) (?P<class>(?:Class [A-Z] )?common stock)\b")
_BLOCKS = {
    "conditional_consideration": re.compile(r"\bif\b|\bprovided\s+that\b|\bunless\b|\bsubject\s+to\b|\bconditions?\b", re.I),
    "elected_or_alternative_consideration": re.compile(r"\belect\w*\b|\bproration\b|\bor\b|\bin lieu\b", re.I),
    "ads_or_unit_conversion_unqualified": re.compile(r"\bADSs?\b|American Depositary|depositary receipts|ordinary shares", re.I),
    "contingent_or_adjustable_consideration": re.compile(r"\bCVRs?\b|contingent|subject to adjustment|up to", re.I),
    "fractional_treatment_unqualified": re.compile(r"fractional", re.I),
    "preferred_or_compensation_class": re.compile(r"preferred|restricted stock|stock options|stock appreciation", re.I),
    "scaled_or_non_per_share_consideration": re.compile(r"\b(?:million|billion)\b|\bper (?:hundred|thousand|[0-9,]+) shares\b", re.I),
}


@dataclass(frozen=True)
class FinancialVintage:
    accession: str
    source_id: str
    source_sha256: str
    available_at: dt.datetime
    announce_date: dt.date | None
    termination_date: dt.date | None
    completion_date: dt.date | None
    settlement_date: dt.date | None
    target_name: str | None
    target_share_class: str | None
    acquirer_name: str | None
    acquirer_share_class: str | None
    consideration_type: str | None
    cash_per_share: Decimal | None
    cash_currency: str | None
    stock_ratio: Decimal | None
    status: str
    consideration_clauses: tuple[str, ...]
    reasons: tuple[str, ...]


def _date(value: str) -> dt.date | None:
    try:
        return dt.datetime.strptime(value, "%B %d, %Y").date()
    except ValueError:
        return None


def _sentences(text: str) -> list[str]:
    # Retain decimal amounts, abbreviated company names and dates. Boundaries
    # require a word-ending sentence followed by a capitalized subject/heading.
    return re.split(r"(?<=[a-z)])\.\s+(?=[A-Z]|each\b)", re.sub(r"\s+", " ", text).strip())


def parse_financial(text: str, pin: N.NativePin) -> FinancialVintage:
    flat = re.sub(r"\s+", " ", text)
    reasons = []
    complete = list(_COMPLETE.finditer(flat))
    settle = list(_SETTLE.finditer(flat))
    announced = list(_ANNOUNCE.finditer(flat))
    terminated = list(_TERMINATE.finditer(flat))
    termination_date = _date(terminated[0].group("date")) if len(terminated) == 1 else None
    announce_date = _date(announced[0].group("date")) if len(announced) == 1 else None
    completion = _date(complete[0].group("date")) if len(complete) == 1 else None
    settlement = _date(settle[0].group("date")) if len(settle) == 1 else None
    target = complete[0].group("target").strip() if len(complete) == 1 else None
    acquirer = complete[0].group("acquirer") if len(complete) == 1 else None
    if len(announced) == 1:
        if target and target != announced[0].group("target").strip():
            reasons.append("native_announcement_completion_party_mismatch")
        target = target or announced[0].group("target").strip()
        acquirer = acquirer or announced[0].group("acquirer").strip()
    if completion is None:
        reasons.append("native_completion_unqualified")
    if settlement is None:
        reasons.append("native_settlement_unqualified")
    if completion and settlement and settlement < completion:
        reasons.append("native_settlement_before_completion")
    if completion and termination_date:
        reasons.append("ambiguous_native_completion_termination")
    clauses = tuple(sentence for sentence in _sentences(flat) if _CONVERT.search(sentence))
    for clause in clauses:
        for reason, pattern in _BLOCKS.items():
            if pattern.search(clause):
                reasons.append(reason)
    cash = ratio = None
    currency = target_class = acquirer_class = ctype = None
    if len(clauses) != 1 or len(list(_CONVERT.finditer(flat))) != 1:
        reasons.append("missing_or_ambiguous_native_share_clauses")
    else:
        clause = clauses[0]
        named = list(_NAMED_CLASS.finditer(clause))
        if len(named) != 1:
            reasons.append("native_target_name_class_unqualified")
        elif target is not None and named[0].group("issuer").strip() != target:
            reasons.append("native_target_party_mismatch")
        else:
            target = named[0].group("issuer").strip()
            target_class = named[0].group("class")
        amounts = list(_USD.finditer(clause))
        tail = clause[_CONVERT.search(clause).end():].strip().rstrip(".")
        stocks = list(_STOCK.finditer(tail))
        # Require the supported monetary clauses to consume the entire tail.
        # Identity/replay cannot make an unknown qualifier unconditional.
        residue = _USD.sub("", _STOCK.sub("", tail))
        residue = re.sub(r"\b(?:and|without interest)\b|[(),;.]|\bi\b|\bii\b", "", residue, flags=re.I).strip()
        if residue:
            reasons.append("unsupported_qualified_consideration_tail")
        if len(amounts) > 1 or len(stocks) > 1 or (not amounts and not stocks):
            reasons.append("native_consideration_unqualified")
        if not reasons or set(reasons) <= {"native_settlement_unqualified", "native_completion_unqualified"}:
            cash = Decimal(amounts[0].group("amount")) if amounts else None
            currency = "USD" if amounts else None
            ratio = Decimal(stocks[0].group("ratio")) if stocks else None
            if stocks:
                stock_issuer = stocks[0].group("issuer").strip()
                if acquirer and acquirer != stock_issuer:
                    reasons.append("native_acquirer_party_mismatch")
                    ratio = None
                else:
                    acquirer = stock_issuer
                    acquirer_class = stocks[0].group("class")
            if any(value is not None and value <= 0 for value in (cash, ratio)):
                reasons.append("invalid_native_consideration")
                cash = ratio = None
            if ((cash is not None and (cash >= 10000 or cash.as_tuple().exponent < -12))
                or (ratio is not None and (ratio >= 1000 or ratio.as_tuple().exponent < -12))):
                reasons.append("unsupported_native_numeric_range_or_precision")
                cash = ratio = None
            ctype = "mixed" if cash is not None and ratio is not None else "cash" if cash is not None else "stock" if ratio is not None else None
    if pin.clock_basis == "retrieval_floor":
        reasons.append("historical_public_clock_unqualified_retrieval_floor")
    return FinancialVintage(pin.accession, pin.source_id, pin.sha256, T.utc(pin.available_at),
                            announce_date, termination_date, completion, settlement, target, target_class, acquirer, acquirer_class,
                            ctype, cash, currency, ratio, "unknown" if completion and termination_date else "terminated" if termination_date else "completed" if completion else "pending" if announce_date else "unknown",
                            clauses, tuple(sorted(set(reasons))))


def financial_code_pins() -> dict[str, str]:
    return {**N.code_pins(), "mna_financial_v2": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}


def replay_financial(objects: Mapping[str, bytes], pins: tuple[N.NativePin, ...], *,
                     admission_id: str, expected_code: Mapping[str, str], bulk_clocks=None,
                     expected_auxiliary=None, clock_receipts=None) -> list[FinancialVintage]:
    before = financial_code_pins()
    if dict(expected_code) != before:
        raise ValueError("complete independent financial reader/code identity required")
    # Native identity/clock admission happens before financial computation.
    N.replay_native(objects, pins, admission_id=admission_id, expected_code=N.code_pins(), bulk_clocks=bulk_clocks,
                    expected_auxiliary=expected_auxiliary, clock_receipts=clock_receipts)
    rows = []
    for pin in pins:
        text = objects[pin.source_id].decode("utf-8", errors="strict")
        if pin.clock_basis == "native_sec_acceptance":
            text = re.findall(r"(?s)<DOCUMENT>(.*?)</DOCUMENT>", text)[0]
        parser = N._Text()
        parser.feed(text)
        parser.close()
        rows.append(parse_financial(" ".join(parser.parts), pin))
    if financial_code_pins() != before:
        raise ValueError("financial reader/code changed during replay")
    return rows


def validate_financial_rows(rows: list[dict], objects: Mapping[str, bytes], pins: tuple[N.NativePin, ...], *,
                            admission_id: str, expected_code: Mapping[str, str], bulk_clocks=None,
                            expected_auxiliary=None, clock_receipts=None) -> None:
    names = {field.name for field in fields(FinancialVintage)}
    if any(set(row) != names for row in rows):
        raise ValueError("complete financial schema required")
    actual = [FinancialVintage(**row) for row in rows]
    expected = replay_financial(objects, pins, admission_id=admission_id, expected_code=expected_code, bulk_clocks=bulk_clocks,
                                expected_auxiliary=expected_auxiliary, clock_receipts=clock_receipts)
    key = lambda row: (row.accession, row.source_id, row.available_at)
    keys = [key(row) for row in actual]
    if len(keys) != len(set(keys)) or sorted(actual, key=key) != sorted(expected, key=key):
        raise ValueError("complete unique financial native output replay mismatch")
