"""Conservative M&A terminal valuation core; no acquisition or implicit admission.

Inputs must be independently admitted original-source observations. This module
does not turn receipts, hashes, filing item codes, or caller booleans into source
certification. ``provenance_errors`` supplied by the native reader/admission
validator are retained, and an absent admission identifier fails closed. The
actual native source reader and immutable independently reviewed admission
contract are prerequisites to publishing a certified v2 stage.

Value coverage includes 2020+; terminal returns are computed only before 2023.
No return here certifies a complete consumer label window or residual proceeds.
"""
from __future__ import annotations

import datetime as dt
from dataclasses import dataclass, replace
from decimal import Decimal
from typing import Iterable

UTC = dt.timezone.utc
HOLDOUT = dt.date(2023, 1, 1)
SCHEMA = "atx.alpha-panel.delisting.events/v2"
ORIGINAL_MNA_DENOMINATOR = 2267


def utc(value: dt.datetime) -> dt.datetime:
    if value.tzinfo is None or value.utcoffset() is None:
        raise ValueError("UTC source clock required")
    return value.astimezone(UTC)


@dataclass(frozen=True)
class Evidence:
    """Reader output, not a certificate: independent native admission is required."""
    source_id: str
    source_sha256: str
    available_at: dt.datetime
    admission_id: str | None = None
    provenance_errors: tuple[str, ...] = ()

    def errors(self) -> tuple[str, ...]:
        errors = list(self.provenance_errors)
        if not self.source_id or len(self.source_sha256) != 64 or any(
            c not in "0123456789abcdef" for c in self.source_sha256
        ):
            errors.append("missing_native_source_identity")
        if not self.admission_id:
            errors.append("missing_independent_native_admission")
        utc(self.available_at)
        return tuple(errors)


@dataclass(frozen=True)
class Candidate:
    security_id: int
    last_session: dt.date
    available_at: dt.datetime
    cause: str = "mna"
    continued: bool = False


@dataclass(frozen=True)
class Price:
    security_id: int
    session_date: dt.date
    close: Decimal
    currency: str
    share_class: str
    unit: str
    basis: str
    evidence: Evidence


@dataclass(frozen=True)
class Terms:
    deal_id: str
    target_security_id: int
    target_share_class: str
    target_unit: str
    target_basis: str
    currency: str
    consideration_type: str
    cash_per_share: Decimal | None
    stock_ratio: Decimal | None
    acquirer_security_id: int | None
    acquirer_share_class: str | None
    acquirer_unit: str | None
    acquirer_basis: str | None
    terminal_date: dt.date
    settlement_date: dt.date
    status: str
    evidence: Evidence
    completion_evidence: Evidence
    cvr: bool = False
    election: bool = False
    fractional_treatment: str = "not_applicable"


@dataclass(frozen=True)
class TerminalValue:
    security_id: int
    last_session: dt.date
    available_at: dt.datetime
    deal_id: str | None
    proceeds_per_share: Decimal | None
    currency: str | None
    dlret: Decimal | None
    dlret_exact: bool
    dlret_imputed: bool
    reason: tuple[str, ...]
    source_ids: tuple[str, ...]
    settlement_date: dt.date | None


def _positive(value: Decimal | None) -> bool:
    return isinstance(value, Decimal) and value.is_finite() and value > 0


def value_terminal(candidate: Candidate, terms: Terms | None, target: Price | None,
                   acquirer: Price | None = None) -> TerminalValue:
    """Proceeds and conditional exact return; unknown amounts are always NULL.

    All consideration/price units and basis tokens must match exactly. Foreign
    currency and split-adjusted inputs need prior signed native normalization;
    this core never guesses an FX rate, conversion date, or adjustment factor.
    """
    clocks = [utc(candidate.available_at)]
    reasons: list[str] = []
    sources: list[str] = []
    if candidate.continued:
        reasons.append("continued_line")
    if candidate.cause != "mna":
        reasons.append("not_mna")
    if terms is None:
        reasons.append("missing_terminal_terms")
    else:
        for ev in (terms.evidence, terms.completion_evidence):
            clocks.append(utc(ev.available_at))
            reasons.extend(ev.errors())
            sources.append(ev.source_id)
        if terms.target_security_id != candidate.security_id:
            reasons.append("target_security_mismatch")
        if terms.status != "completed":
            reasons.append("not_source_proven_completed")
        if terms.terminal_date < candidate.last_session or terms.settlement_date < terms.terminal_date:
            reasons.append("invalid_terminal_settlement_order")
        if terms.cvr or terms.election:
            reasons.append("contingent_or_elected_consideration")
        if terms.consideration_type not in ("cash", "stock", "mixed"):
            reasons.append("unsupported_consideration")
        if not all((terms.target_share_class, terms.target_unit, terms.target_basis, terms.currency)):
            reasons.append("missing_target_units_class_currency_basis")
        if terms.consideration_type in ("cash", "mixed") and not _positive(terms.cash_per_share):
            reasons.append("missing_cash_terms")
        if terms.consideration_type == "cash" and terms.stock_ratio is not None:
            reasons.append("contradictory_cash_terms")
        if terms.consideration_type == "stock" and terms.cash_per_share is not None:
            reasons.append("contradictory_stock_terms")
        if terms.consideration_type in ("stock", "mixed"):
            if not _positive(terms.stock_ratio):
                reasons.append("missing_stock_ratio")
            if terms.fractional_treatment != "exact_per_target_share":
                reasons.append("unqualified_fractional_treatment")
            if acquirer is None:
                reasons.append("missing_acquirer_terminal_close")
            else:
                clocks.append(utc(acquirer.evidence.available_at))
                reasons.extend(acquirer.evidence.errors())
                sources.append(acquirer.evidence.source_id)
                if (acquirer.security_id != terms.acquirer_security_id
                    or acquirer.session_date != terms.terminal_date):
                    reasons.append("acquirer_security_or_date_mismatch")
                if (acquirer.share_class, acquirer.unit, acquirer.basis, acquirer.currency) != (
                    terms.acquirer_share_class, terms.acquirer_unit, terms.acquirer_basis, terms.currency):
                    reasons.append("acquirer_class_unit_basis_currency_mismatch")
                if not _positive(acquirer.close):
                    reasons.append("invalid_acquirer_close")
    proceeds = None
    if not reasons and terms is not None:
        proceeds = terms.cash_per_share if terms.consideration_type == "cash" else (
            (terms.cash_per_share or Decimal(0)) + acquirer.close * terms.stock_ratio
        )
    # Missing target mark prevents a return, but need not erase qualified proceeds.
    if target is None:
        reasons.append("missing_target_last_close")
    else:
        clocks.append(utc(target.evidence.available_at))
        reasons.extend(target.evidence.errors())
        sources.append(target.evidence.source_id)
        if target.security_id != candidate.security_id or target.session_date != candidate.last_session:
            reasons.append("target_security_or_last_session_mismatch")
        if terms is not None and (target.share_class, target.unit, target.basis, target.currency) != (
            terms.target_share_class, terms.target_unit, terms.target_basis, terms.currency):
            reasons.append("target_class_unit_basis_currency_mismatch")
        if not _positive(target.close):
            reasons.append("invalid_target_close")
    if candidate.last_session >= HOLDOUT or (terms is not None and (
        terms.terminal_date >= HOLDOUT or terms.settlement_date >= HOLDOUT
    )):
        reasons.append("return_holdout")
    # This scaffold has no admitted original-source reader/replay contract yet.
    # Never let a caller manufacture certification using admission_id or hashes.
    reasons.append("native_replay_admission_unavailable")
    dlret = None
    return TerminalValue(candidate.security_id, candidate.last_session, max(clocks),
                         terms.deal_id if terms else None, proceeds, terms.currency if terms else None,
                         dlret, dlret is not None, False, tuple(sorted(set(reasons))),
                         tuple(sorted(set(sources))), terms.settlement_date if terms else None)


def offline_terminal_return(proceeds: Decimal, target_last_close: Decimal, *,
                            last_session: dt.date, terminal_date: dt.date,
                            settlement_date: dt.date) -> Decimal:
    """Formula only for offline fixtures; this is NOT source/label certification.

    Actual builders must not call this until independent native replay is landed
    and the controller has frozen/authorized the real return computation.
    """
    if not all(day < HOLDOUT for day in (last_session, terminal_date, settlement_date)):
        raise ValueError("return_holdout")
    if not last_session <= terminal_date <= settlement_date:
        raise ValueError("invalid_terminal_settlement_order")
    if not _positive(proceeds) or not _positive(target_last_close):
        raise ValueError("positive finite qualified proceeds and target close required")
    return proceeds / target_last_close - Decimal(1)


def preserve_population(candidates: Iterable[Candidate], terms: dict[int, list[Terms]],
                        target_prices: dict[int, Price], acquirer_prices: dict[int, Price]
                        ) -> list[TerminalValue]:
    """One outcome per complete candidate key; ambiguous deal links stay NULL."""
    outcomes = []
    keys = set()
    for candidate in candidates:
        key = (candidate.security_id, candidate.last_session, utc(candidate.available_at))
        if key in keys:
            raise ValueError("duplicate candidate vintage key")
        keys.add(key)
        matches = terms.get(candidate.security_id, [])
        match = matches[0] if len(matches) == 1 else None
        outcome = value_terminal(candidate, match, target_prices.get(candidate.security_id),
                                 acquirer_prices.get(match.acquirer_security_id) if match else None)
        if len(matches) > 1:
            outcome = replace(outcome, reason=tuple(sorted(set(outcome.reason + ("ambiguous_deal_link",)))))
        outcomes.append(outcome)
    return outcomes


def complete_window_eligible(value: TerminalValue, *, start_mark: dt.date, entry: dt.date,
                             exit: dt.date, calendar: tuple[dt.date, ...],
                             residual_treatment_source: Evidence | None) -> bool:
    """Independent calendar and dated residual treatment are mandatory.

    This check is a necessary eligibility condition, not a forward-label builder.
    Native intermediate prices/actions still require consumer replay admission.
    """
    if tuple(sorted(set(calendar))) != calendar:
        raise ValueError("calendar must contain unique ordered actual sessions")
    if any(day not in calendar for day in (start_mark, entry, exit)):
        return False
    if not start_mark < entry <= exit < HOLDOUT:
        return False
    if calendar.index(entry) != calendar.index(start_mark) + 1:
        return False
    return (value.dlret_exact and value.settlement_date is not None
            and entry <= value.last_session <= value.settlement_date <= exit
            and residual_treatment_source is not None and not residual_treatment_source.errors())


def gates(*, candidates: int, exact: int, deals: int, consideration: int,
          checked: int, correct: int) -> dict[str, dict[str, object]]:
    """Numeric gates without selected-subset denominators or invented PASS."""
    if any(not isinstance(x, int) or x < 0 for x in (candidates, exact, deals, consideration, checked, correct)):
        raise ValueError("nonnegative integer counts required")
    if exact > candidates or consideration > deals or correct > checked:
        raise ValueError("numerator exceeds denominator")
    return {
        "population_reconciliation": {"status": "PASS" if candidates == ORIGINAL_MNA_DENOMINATOR else "FAIL",
                                      "measured": candidates, "original": ORIGINAL_MNA_DENOMINATOR},
        "exact_dlret": {"status": "PASS" if candidates == ORIGINAL_MNA_DENOMINATOR and exact / candidates >= .8 else "FAIL",
                        "measured": exact / candidates if candidates else None, "denominator": candidates},
        "consideration": {"status": "PASS" if deals and consideration / deals >= .8 else "FAIL",
                          "measured": consideration / deals if deals else None, "denominator": deals},
        "handcheck": {"status": "PASS" if checked >= 200 and correct / checked >= .95 else "FAIL",
                      "measured": correct / checked if checked else None, "denominator": checked},
    }
