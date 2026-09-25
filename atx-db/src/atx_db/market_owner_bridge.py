"""Explicit price-line -> accounting-owner bridge for the daily market panel.

Why this exists
---------------
Price bars and accounting content live in different identity namespaces:

* a *price line* is one ``equity_daily_bars.security_id``. The broad
  ticker-history loader assigns ``SEC-CIK-##########`` to the first vendor line
  whose **current** symbol maps to a CIK and ``TBLTICKERHISTORY-*`` to every
  other line (secondary share classes of the same issuer, delisted/renamed
  names whose current symbol no longer maps, symbol-reuse collisions);
* an *accounting owner* is the ``security_id`` under which Company Facts
  content was materialized: ``SEC-COMPANYFACTS-UNRESOLVED-CIK-##########`` for
  archive facts filed before any dated CIK identifier history, ``SEC-CIK-*``
  for facts resolved through dated history, and one CIK can have both.

Joining the two on equal ``security_id`` (the pre-A5 panel) silently drops
fundamentals for every secondary class line and for issuers whose content is
keyed by the unresolved-CIK owner. This module replaces that implicit join
with an explicit, inspectable bridge

    (price_security_id, owner_security_id, cik, share_class_symbol,
     valid_from, valid_to, available_at, identity_basis)

plus lineage columns (``availability_basis``, ``link_method``,
``unlinked_reason``, ``evidence_observed_at``) and the owner's *members*: every
content ``security_id`` that belongs to the owner (all ids whose normalized CIK
equals the owner's CIK). ``valid_to`` is exclusive.

Modes
-----
``strict``
    Only dated identity evidence (:class:`OwnerLinkEvidence` with
    ``evidence_status='verified_dated'`` and ``availability_status='verified'``,
    artifact hash and locator present). ``identity_basis='verified_dated'``,
    ``availability_basis='verified'``; a link's ``available_at`` is the
    earliest covering evidence clock, never earlier. No qualified evidence
    source exists yet (C9/C10), so strict mode is empty unless the caller
    supplies evidence. It is the only mode that may count toward certification.
``reconstructed``
    The current SEC ticker map backcast over each price line's whole history
    (RX1, run5 default). ``identity_basis='current_ticker_unverified'`` and
    ``availability_basis='modeled'``: the link is *modeled* as known from the
    line's first bar cutoff (``first_trade_date + 22h``) although it was only
    observed at the ticker snapshot (``evidence_observed_at``). It is research
    and coverage-measurement input, never verified historical identity.

Reconstructed link rules, in precedence order, for a line whose last traded
symbol is normalized (upper case; ``.``, ``/`` and blanks -> ``-``):

1. ``current_sec_ticker``: the symbol is a current SEC ticker of exactly one
   CIK, and this line is that symbol's latest holder (latest last bar, then
   most bar rows, then id). Former holders of a reused symbol are unlinked as
   ``symbol_held_by_other_line``; a symbol mapped to several CIKs is unlinked
   as ``ambiguous_current_ticker``. A secondary class line
   (``TBLTICKERHISTORY-7`` trading the current class-B ticker of X) links to
   ``SEC-CIK-X`` with its class symbol.
2. ``cik_security_id``: no current SEC ticker matches, but the line id is itself
   ``SEC-CIK-##########`` (assigned upstream by a current-symbol map).
3. ``shared_security_id``: no CIK evidence at all, but accounting content is
   keyed by the line's own id -- the legacy equal-id join, retained so
   non-SEC identity namespaces keep working.
4. otherwise unlinked as ``no_current_ticker`` (delisted/renamed lines whose
   last symbol no longer maps) -- counted, never silently dropped.

DEI shares
----------
A DEI share count is an issuer-level (or undetermined-class) count. The bridge
marks ``dei_shares_eligible`` only for a line that is its owner's *sole*
concurrently trading linked line; owners with overlapping linked lines
(multi-class, units/warrants) keep their line-level archive shares until A8
defines a per-class share basis. ``concurrent_owner_lines`` exposes that count.
"""

from __future__ import annotations

import datetime as dt
import re
from collections import defaultdict
from collections.abc import Iterable, Sequence
from dataclasses import dataclass, field, replace
from functools import cached_property
from itertools import pairwise

from .connection import DuckDBStore
from .warehouse import cik_security_id

__all__ = [
    "AVAILABILITY_MODELED",
    "AVAILABILITY_VERIFIED",
    "BRIDGE_VALUE_COLUMNS",
    "IDENTITY_BASIS_CURRENT_TICKER",
    "IDENTITY_BASIS_VERIFIED_DATED",
    "LINK_CIK_SECURITY_ID",
    "LINK_CURRENT_SEC_TICKER",
    "LINK_DATED_EVIDENCE",
    "LINK_SHARED_SECURITY_ID",
    "MEMBER_VALUE_COLUMNS",
    "OWNER_MODES",
    "OWNER_MODE_RECONSTRUCTED",
    "OWNER_MODE_STRICT",
    "BridgeRow",
    "MarketOwnerBridge",
    "OwnerLinkEvidence",
    "PriceLine",
    "bridge_value_params",
    "build_market_owner_bridge",
    "classify_reconstructed",
    "classify_strict",
    "member_value_params",
    "normalize_cik",
    "normalize_symbol",
    "values_relation_sql",
]

OWNER_MODE_STRICT = "strict"
OWNER_MODE_RECONSTRUCTED = "reconstructed"
OWNER_MODES = (OWNER_MODE_STRICT, OWNER_MODE_RECONSTRUCTED)

IDENTITY_BASIS_CURRENT_TICKER = "current_ticker_unverified"
IDENTITY_BASIS_VERIFIED_DATED = "verified_dated"
AVAILABILITY_MODELED = "modeled"
AVAILABILITY_VERIFIED = "verified"

LINK_CURRENT_SEC_TICKER = "current_sec_ticker"
LINK_CIK_SECURITY_ID = "cik_security_id"
LINK_SHARED_SECURITY_ID = "shared_security_id"
LINK_DATED_EVIDENCE = "dated_evidence"

UNLINKED_NO_CURRENT_TICKER = "no_current_ticker"
UNLINKED_SYMBOL_HELD_BY_OTHER_LINE = "symbol_held_by_other_line"
UNLINKED_AMBIGUOUS_CURRENT_TICKER = "ambiguous_current_ticker"
UNLINKED_NO_DATED_EVIDENCE = "no_dated_evidence"

#: Modeled reconstructed-link availability: the first bar's end-of-day cutoff
#: (same ``trade_date + 22h`` convention as ``market_daily.END_OF_DAY_HOURS``).
_MODELED_LINK_HOURS = 22

#: Column layout of the per-batch ``owner_bridge`` VALUES relation consumed by
#: ``market_daily.build_market_daily_sql`` (name, DuckDB type).
BRIDGE_VALUE_COLUMNS: tuple[tuple[str, str], ...] = (
    ("price_security_id", "VARCHAR"),
    ("owner_key", "VARCHAR"),
    ("valid_from", "DATE"),
    ("valid_to", "DATE"),
    ("available_at", "TIMESTAMP"),
    ("dei_shares_eligible", "BOOLEAN"),
    ("identity_basis", "VARCHAR"),
)
#: Column layout of the per-batch ``owner_members`` VALUES relation.
MEMBER_VALUE_COLUMNS: tuple[tuple[str, str], ...] = (
    ("owner_key", "VARCHAR"),
    ("member_security_id", "VARCHAR"),
)

_SEC_CIK_ID = re.compile(r"SEC-CIK-(\d{10})")
_SYMBOL_SEPARATORS = re.compile(r"[./\s]+")
_CIK_DIGITS = re.compile(r"\d{1,10}")


def normalize_symbol(symbol: str | None) -> str | None:
    """Upper-case, trim, and fold ``.``/``/``/blank class separators to ``-``."""
    if symbol is None:
        return None
    key = _SYMBOL_SEPARATORS.sub("-", str(symbol).strip().upper()).strip("-")
    return key or None


def normalize_cik(cik: object) -> str | None:
    """Ten-digit zero-padded CIK, or ``None`` for anything non-numeric."""
    if cik is None:
        return None
    text = str(cik).strip()
    if not _CIK_DIGITS.fullmatch(text) or int(text) == 0:
        return None
    return f"{int(text):010d}"


@dataclass(frozen=True)
class PriceLine:
    """One price line (``equity_daily_bars.security_id``) and its trading span."""

    price_security_id: str
    last_symbol: str | None
    first_trade_date: dt.date
    last_trade_date: dt.date
    bar_rows: int


@dataclass(frozen=True)
class OwnerLinkEvidence:
    """One dated identity fact linking a price line to an issuer/owner.

    ``valid_to`` is exclusive. ``available_at`` is the evidence clock (when the
    fact was knowable); the bridge never makes a link available earlier.
    """

    price_security_id: str
    cik: str | None
    valid_from: dt.date
    available_at: dt.datetime
    evidence_status: str
    availability_status: str
    artifact_sha256: str
    source_locator: str
    valid_to: dt.date | None = None
    owner_security_id: str | None = None
    share_class_symbol: str | None = None
    source_published_at: dt.datetime | None = None
    evidence_id: str | None = None


@dataclass(frozen=True)
class BridgeRow:
    """One bridge interval for a price line; unlinked lines have one row with a reason."""

    price_security_id: str
    owner_security_id: str | None
    cik: str | None
    share_class_symbol: str | None
    valid_from: dt.date | None
    valid_to: dt.date | None
    available_at: dt.datetime | None
    identity_basis: str | None
    availability_basis: str | None
    link_method: str | None
    unlinked_reason: str | None = None
    evidence_observed_at: dt.datetime | None = None
    dei_shares_eligible: bool = False
    concurrent_owner_lines: int = 0

    @property
    def linked(self) -> bool:
        return self.owner_security_id is not None


@dataclass(frozen=True)
class MarketOwnerBridge:
    """The resolved bridge plus owner membership, batching and reporting helpers."""

    mode: str
    lines: dict[str, PriceLine]
    rows: tuple[BridgeRow, ...]
    owner_members: dict[str, tuple[str, ...]]
    evidence_supplied: int = 0
    rejected_evidence: dict[str, int] = field(default_factory=dict)
    ticker_snapshot_observed_at: dt.datetime | None = None
    ambiguous_content_ids: int = 0
    #: Every accounting-content id seen (lets summary() flag owners with no
    #: content without another warehouse scan).
    content_ids: frozenset[str] = frozenset()

    @property
    def identity_basis(self) -> str:
        return IDENTITY_BASIS_VERIFIED_DATED if self.mode == OWNER_MODE_STRICT else IDENTITY_BASIS_CURRENT_TICKER

    @property
    def availability_basis(self) -> str:
        return AVAILABILITY_VERIFIED if self.mode == OWNER_MODE_STRICT else AVAILABILITY_MODELED

    def line_ids(self) -> list[str]:
        return sorted(self.lines)

    @cached_property
    def _by_line(self) -> dict[str, tuple[BridgeRow, ...]]:
        # Rows are already sorted by (price line, valid_from); index once so a
        # ~30K-line bridge is not rescanned for each of ~150 batches.
        grouped: dict[str, list[BridgeRow]] = defaultdict(list)
        for row in self.rows:
            grouped[row.price_security_id].append(row)
        return {line: tuple(rows) for line, rows in grouped.items()}

    def linked_rows(self, price_security_ids: Iterable[str]) -> list[BridgeRow]:
        return [
            row for price_id in sorted(set(price_security_ids)) for row in self._by_line.get(price_id, ()) if row.linked
        ]

    def members_for(self, rows: Iterable[BridgeRow]) -> list[tuple[str, str]]:
        owners = sorted({row.owner_security_id for row in rows if row.owner_security_id is not None})
        return [(owner, member) for owner in owners for member in self.owner_members.get(owner, (owner,))]

    def owner_aligned_batches(self, identifiers: Sequence[str], batch_size: int) -> list[tuple[str, ...]]:
        """Pack identifiers into batches that never split one owner's lines.

        Every price line that links (in any interval) to the same owner lands in
        the same batch, so issuer-level aggregation over an owner's class lines
        (A8) can run inside one statement. A group larger than ``batch_size``
        forms its own batch. Single-line groups keep the sorted-id order.
        """
        size = max(1, int(batch_size))
        parent: dict[str, str] = {}

        def find(node: str) -> str:
            parent.setdefault(node, node)
            while parent[node] != node:
                parent[node] = parent[parent[node]]
                node = parent[node]
            return node

        ordered = sorted(dict.fromkeys(identifiers))
        wanted = set(ordered)
        for identifier in ordered:
            find("line:" + identifier)
        for row in self.rows:
            if row.linked and row.price_security_id in wanted:
                left, right = find("line:" + row.price_security_id), find("owner:" + str(row.owner_security_id))
                if left != right:
                    parent[right] = left
        groups: dict[str, list[str]] = defaultdict(list)
        for identifier in ordered:
            groups[find("line:" + identifier)].append(identifier)
        batches: list[tuple[str, ...]] = []
        current: list[str] = []
        for group in sorted(groups.values(), key=lambda members: members[0]):
            if current and len(current) + len(group) > size:
                batches.append(tuple(current))
                current = []
            current.extend(group)
            if len(current) >= size:
                batches.append(tuple(current))
                current = []
        if current:
            batches.append(tuple(current))
        return batches

    def summary(self, identifiers: Iterable[str] | None = None) -> dict[str, object]:
        """Linked/unlinked accounting for the refreshed lines (JSON-serializable)."""
        wanted = set(self.lines) if identifiers is None else set(identifiers)
        by_line = self._by_line
        linked_by_method: dict[str, int] = defaultdict(int)
        unlinked_by_reason: dict[str, int] = defaultdict(int)
        linked_lines = unlinked_lines = linked_bar_rows = unlinked_bar_rows = 0
        secondary = dei_withheld = id_disagreements = 0
        owners: set[str] = set()
        multi_line_owners: set[str] = set()
        for price_id in sorted(wanted):
            line = self.lines.get(price_id)
            rows = by_line.get(price_id, ())
            linked = [row for row in rows if row.linked]
            bar_rows = line.bar_rows if line is not None else 0
            if linked:
                linked_lines += 1
                linked_bar_rows += bar_rows
                linked_by_method[str(linked[0].link_method)] += 1
                for row in linked:
                    owners.add(str(row.owner_security_id))
                    if row.concurrent_owner_lines > 1:
                        multi_line_owners.add(str(row.owner_security_id))
                if any(row.owner_security_id != price_id for row in linked):
                    secondary += 1
                if not any(row.dei_shares_eligible for row in linked):
                    dei_withheld += 1
                match = _SEC_CIK_ID.fullmatch(price_id)
                if match is not None and any(row.cik not in (None, match.group(1)) for row in linked):
                    id_disagreements += 1
            elif line is not None:
                unlinked_lines += 1
                unlinked_bar_rows += bar_rows
                reason = rows[0].unlinked_reason if rows else UNLINKED_NO_CURRENT_TICKER
                unlinked_by_reason[str(reason)] += 1
        without_content = sum(
            1 for owner in owners if not any(member in self.content_ids for member in self.owner_members.get(owner, ()))
        )
        summary: dict[str, object] = {
            "mode": self.mode,
            "identity_basis": self.identity_basis,
            "availability_basis": self.availability_basis,
            "lines": linked_lines + unlinked_lines,
            "requested_without_bars": len(wanted - set(self.lines)),
            "linked_lines": linked_lines,
            "unlinked_lines": unlinked_lines,
            "linked_bar_rows": linked_bar_rows,
            "unlinked_bar_rows": unlinked_bar_rows,
            "linked_by_method": dict(sorted(linked_by_method.items())),
            "unlinked_by_reason": dict(sorted(unlinked_by_reason.items())),
            "secondary_lines_linked": secondary,
            "owners": len(owners),
            "multi_line_owners": len(multi_line_owners),
            "owners_without_accounting_content": without_content,
            "dei_shares_withheld_lines": dei_withheld,
            "line_id_cik_disagreements": id_disagreements,
            "ambiguous_content_owner_ids": self.ambiguous_content_ids,
            "ticker_snapshot_observed_at": (
                self.ticker_snapshot_observed_at.isoformat() if self.ticker_snapshot_observed_at else None
            ),
        }
        if self.mode == OWNER_MODE_STRICT:
            summary["evidence"] = {
                "supplied": self.evidence_supplied,
                "rejected_by_reason": dict(sorted(self.rejected_evidence.items())),
            }
        return summary


# ---------------------------------------------------------------------------
# Pure classification (no warehouse access; unit-testable)
# ---------------------------------------------------------------------------


def _modeled_available_at(first_trade_date: dt.date) -> dt.datetime:
    return dt.datetime.combine(first_trade_date, dt.time(_MODELED_LINK_HOURS))


def _content_ciks(content: dict[str, frozenset[str]]) -> tuple[dict[str, list[str]], int]:
    """CIK -> content ids whose (single) normalized CIK equals it; count ambiguous ids."""
    by_cik: dict[str, list[str]] = defaultdict(list)
    ambiguous = 0
    for security_id, ciks in content.items():
        if len(ciks) == 1:
            by_cik[next(iter(ciks))].append(security_id)
        elif len(ciks) > 1:
            ambiguous += 1
    return by_cik, ambiguous


def _owner_members(
    rows: Sequence[BridgeRow], content: dict[str, frozenset[str]]
) -> tuple[dict[str, tuple[str, ...]], int]:
    by_cik, ambiguous = _content_ciks(content)
    members: dict[str, set[str]] = defaultdict(set)
    for row in rows:
        if row.owner_security_id is None:
            continue
        owner = row.owner_security_id
        members[owner].add(owner)
        if row.link_method != LINK_SHARED_SECURITY_ID and row.cik is not None:
            members[owner].update(by_cik.get(row.cik, ()))
    return {owner: tuple(sorted(ids)) for owner, ids in sorted(members.items())}, ambiguous


def _with_share_eligibility(rows: list[BridgeRow], lines: dict[str, PriceLine]) -> tuple[BridgeRow, ...]:
    """Count each owner's concurrently trading linked lines; DEI only for a sole line."""
    owner_lines: dict[str, set[str]] = defaultdict(set)
    for row in rows:
        if row.owner_security_id is not None:
            owner_lines[row.owner_security_id].add(row.price_security_id)
    resolved: list[BridgeRow] = []
    for row in rows:
        if row.owner_security_id is None:
            resolved.append(row)
            continue
        line = lines[row.price_security_id]
        concurrent = sum(
            1
            for other_id in owner_lines[row.owner_security_id]
            if lines[other_id].first_trade_date <= line.last_trade_date
            and line.first_trade_date <= lines[other_id].last_trade_date
        )
        resolved.append(replace(row, concurrent_owner_lines=concurrent, dei_shares_eligible=concurrent == 1))
    return tuple(sorted(resolved, key=lambda row: (row.price_security_id, row.valid_from or dt.date.min)))


def classify_reconstructed(
    lines: Sequence[PriceLine],
    tickers: Sequence[tuple[str, str, dt.datetime | None]],
    content: dict[str, frozenset[str]],
) -> tuple[tuple[BridgeRow, ...], dict[str, tuple[str, ...]], int]:
    """Current-ticker backcast bridge. ``tickers`` rows are ``(cik, ticker, observed_at)``.

    Returns ``(rows, owner_members, ambiguous_content_ids)``.
    """
    index: dict[str, dict[str, tuple[str, dt.datetime | None]]] = defaultdict(dict)
    for raw_cik, raw_ticker, observed_at in tickers:
        cik, key = normalize_cik(raw_cik), normalize_symbol(raw_ticker)
        if cik is None or key is None:
            continue
        ticker = str(raw_ticker).strip().upper()
        previous = index[key].get(cik)
        if previous is None or (observed_at or dt.datetime.min) > (previous[1] or dt.datetime.min):
            index[key][cik] = (ticker, observed_at)

    holders: dict[str, str] = {}
    matched: dict[str, list[PriceLine]] = defaultdict(list)
    for line in lines:
        key = normalize_symbol(line.last_symbol)
        if key is not None and key in index:
            matched[key].append(line)
    for key, candidates in matched.items():
        best = min(candidates, key=lambda c: (-c.last_trade_date.toordinal(), -c.bar_rows, c.price_security_id))
        holders[key] = best.price_security_id

    rows: list[BridgeRow] = []
    for line in sorted(lines, key=lambda item: item.price_security_id):
        price_id = line.price_security_id
        key = normalize_symbol(line.last_symbol)
        linked = {
            "price_security_id": price_id,
            "valid_from": line.first_trade_date,
            "valid_to": None,
            "available_at": _modeled_available_at(line.first_trade_date),
            "identity_basis": IDENTITY_BASIS_CURRENT_TICKER,
            "availability_basis": AVAILABILITY_MODELED,
        }
        unlinked = {
            "price_security_id": price_id,
            "owner_security_id": None,
            "cik": None,
            "share_class_symbol": None,
            "valid_from": None,
            "valid_to": None,
            "available_at": None,
            "identity_basis": None,
            "availability_basis": None,
            "link_method": None,
        }
        if key is not None and key in index:
            owners = index[key]
            if len(owners) > 1:
                rows.append(BridgeRow(**unlinked, unlinked_reason=UNLINKED_AMBIGUOUS_CURRENT_TICKER))
            elif holders[key] != price_id:
                rows.append(BridgeRow(**unlinked, unlinked_reason=UNLINKED_SYMBOL_HELD_BY_OTHER_LINE))
            else:
                cik, (ticker, observed_at) = next(iter(owners.items()))
                rows.append(
                    BridgeRow(
                        **linked,
                        owner_security_id=cik_security_id(cik),
                        cik=cik,
                        share_class_symbol=ticker,
                        link_method=LINK_CURRENT_SEC_TICKER,
                        evidence_observed_at=observed_at,
                    )
                )
            continue
        match = _SEC_CIK_ID.fullmatch(price_id)
        if match is not None:
            rows.append(
                BridgeRow(
                    **linked,
                    owner_security_id=price_id,
                    cik=match.group(1),
                    share_class_symbol=line.last_symbol,
                    link_method=LINK_CIK_SECURITY_ID,
                )
            )
        elif price_id in content:
            ciks = content[price_id]
            rows.append(
                BridgeRow(
                    **linked,
                    owner_security_id=price_id,
                    cik=next(iter(ciks)) if len(ciks) == 1 else None,
                    share_class_symbol=line.last_symbol,
                    link_method=LINK_SHARED_SECURITY_ID,
                )
            )
        else:
            rows.append(BridgeRow(**unlinked, unlinked_reason=UNLINKED_NO_CURRENT_TICKER))

    by_id = {line.price_security_id: line for line in lines}
    resolved = _with_share_eligibility(rows, by_id)
    members, ambiguous = _owner_members(resolved, content)
    return resolved, members, ambiguous


def _evidence_rejection(evidence: OwnerLinkEvidence, lines: dict[str, PriceLine]) -> str | None:
    if not (evidence.price_security_id or "").strip():
        return "missing_price_line"
    if evidence.evidence_status != "verified_dated":
        return "evidence_not_verified_dated"
    if evidence.availability_status != "verified":
        return "availability_not_verified"
    if not (evidence.artifact_sha256 or "").strip() or not (evidence.source_locator or "").strip():
        return "missing_artifact_or_locator"
    if evidence.valid_from is None:
        return "missing_valid_from"
    if evidence.valid_to is not None and evidence.valid_to <= evidence.valid_from:
        return "empty_or_inverted_interval"
    if evidence.available_at is None:
        return "missing_available_at"
    if evidence.source_published_at is not None and evidence.available_at < evidence.source_published_at:
        return "available_before_publication"
    if normalize_cik(evidence.cik) is None and not (evidence.owner_security_id or "").strip():
        return "missing_owner"
    if evidence.price_security_id not in lines:
        return "price_line_without_bars"
    return None


def _owner_key(evidence: OwnerLinkEvidence) -> str:
    owner = (evidence.owner_security_id or "").strip()
    if owner:
        return owner
    cik = normalize_cik(evidence.cik)
    assert cik is not None  # guaranteed by _evidence_rejection
    return cik_security_id(cik)


def _overlaps(left: OwnerLinkEvidence, right: OwnerLinkEvidence) -> bool:
    left_end = left.valid_to or dt.date.max
    right_end = right.valid_to or dt.date.max
    return left.valid_from < right_end and right.valid_from < left_end


def _segments(evidence: list[OwnerLinkEvidence], owner: str) -> list[BridgeRow]:
    """Split one owner's (possibly overlapping) evidence into disjoint intervals.

    A bar in a segment can use the link once *any* covering evidence is
    available, so the segment's ``available_at`` is the minimum covering clock
    -- the clock of a specific evidence row, never earlier than it.
    """
    bounds = sorted({ev.valid_from for ev in evidence} | {ev.valid_to for ev in evidence if ev.valid_to is not None})
    open_ended = any(ev.valid_to is None for ev in evidence)
    edges: list[tuple[dt.date, dt.date | None]] = list(pairwise(bounds))
    if open_ended:
        edges.append((bounds[-1], None))
    segments: list[BridgeRow] = []
    for start, end in edges:
        covering = [
            ev
            for ev in evidence
            if ev.valid_from <= start and (ev.valid_to is None or (end is not None and ev.valid_to >= end))
        ]
        if not covering:
            continue
        first = min(covering, key=lambda ev: (ev.available_at, ev.evidence_id or "", ev.source_locator))
        cik = normalize_cik(first.cik)
        candidate = BridgeRow(
            price_security_id=first.price_security_id,
            owner_security_id=owner,
            cik=cik,
            share_class_symbol=first.share_class_symbol,
            valid_from=start,
            valid_to=end,
            available_at=first.available_at,
            identity_basis=IDENTITY_BASIS_VERIFIED_DATED,
            availability_basis=AVAILABILITY_VERIFIED,
            link_method=LINK_DATED_EVIDENCE,
            evidence_observed_at=first.available_at,
        )
        previous = segments[-1] if segments else None
        if (
            previous is not None
            and previous.valid_to == start
            and (previous.available_at, previous.share_class_symbol, previous.cik)
            == (candidate.available_at, candidate.share_class_symbol, candidate.cik)
        ):
            segments[-1] = replace(previous, valid_to=end)
        else:
            segments.append(candidate)
    return segments


def classify_strict(
    lines: Sequence[PriceLine],
    evidence: Sequence[OwnerLinkEvidence],
    content: dict[str, frozenset[str]],
) -> tuple[tuple[BridgeRow, ...], dict[str, tuple[str, ...]], int, dict[str, int]]:
    """Dated-evidence bridge. Returns ``(rows, members, ambiguous_content_ids, rejected)``.

    Evidence is validated per row; overlapping intervals that assign one price
    line to *different* owners are rejected as ``conflicting_evidence``.
    Lines without accepted evidence are unlinked as ``no_dated_evidence``.
    """
    by_id = {line.price_security_id: line for line in lines}
    rejected: dict[str, int] = defaultdict(int)
    accepted: dict[str, list[OwnerLinkEvidence]] = defaultdict(list)
    for item in evidence:
        reason = _evidence_rejection(item, by_id)
        if reason is not None:
            rejected[reason] += 1
        else:
            accepted[item.price_security_id].append(item)

    rows: list[BridgeRow] = []
    for line in sorted(lines, key=lambda item: item.price_security_id):
        items = accepted.get(line.price_security_id, [])
        conflicted: set[int] = set()
        for i, left in enumerate(items):
            for j in range(i + 1, len(items)):
                right = items[j]
                if _owner_key(left) != _owner_key(right) and _overlaps(left, right):
                    conflicted.update((i, j))
        if conflicted:
            rejected["conflicting_evidence"] += len(conflicted)
        kept = [item for index, item in enumerate(items) if index not in conflicted]
        by_owner: dict[str, list[OwnerLinkEvidence]] = defaultdict(list)
        for item in kept:
            by_owner[_owner_key(item)].append(item)
        line_rows = [segment for owner, group in sorted(by_owner.items()) for segment in _segments(group, owner)]
        if line_rows:
            rows.extend(line_rows)
        else:
            rows.append(
                BridgeRow(
                    price_security_id=line.price_security_id,
                    owner_security_id=None,
                    cik=None,
                    share_class_symbol=None,
                    valid_from=None,
                    valid_to=None,
                    available_at=None,
                    identity_basis=None,
                    availability_basis=None,
                    link_method=None,
                    unlinked_reason=UNLINKED_NO_DATED_EVIDENCE,
                )
            )
    resolved = _with_share_eligibility(rows, by_id)
    members, ambiguous = _owner_members(resolved, content)
    return resolved, members, ambiguous, dict(sorted(rejected.items()))


# ---------------------------------------------------------------------------
# Warehouse reads (bounded aggregates; no security-day frames)
# ---------------------------------------------------------------------------


def _table_exists(store: DuckDBStore, table: str) -> bool:
    row = store.con.execute(
        "SELECT count(*) FROM duckdb_tables() WHERE table_name = ? AND NOT temporary", [table]
    ).fetchone()
    return bool(row and row[0])


def _read_lines(store: DuckDBStore) -> list[PriceLine]:
    # One streaming aggregate over the bar table (one row per line); same
    # ``close > 0`` population the panel refreshes when unscoped.
    rows = store.con.execute(
        """
        SELECT security_id,
               arg_max(symbol, (trade_date, source)) AS last_symbol,
               min(trade_date) AS first_trade_date,
               max(trade_date) AS last_trade_date,
               count(*) AS bar_rows
        FROM equity_daily_bars
        WHERE close > 0 AND trade_date IS NOT NULL
        GROUP BY security_id
        ORDER BY security_id
        """
    ).fetchall()
    return [PriceLine(str(r[0]), r[1], r[2], r[3], int(r[4])) for r in rows]


def _read_tickers(store: DuckDBStore) -> list[tuple[str, str, dt.datetime | None]]:
    if not _table_exists(store, "sec_company_tickers"):
        return []
    return [
        (str(cik), str(ticker), loaded_at)
        for cik, ticker, loaded_at in store.con.execute(
            "SELECT cik, ticker, source_loaded_at FROM sec_company_tickers WHERE cik IS NOT NULL AND ticker IS NOT NULL"
        ).fetchall()
    ]


def _quoted_list(codes: Sequence[str]) -> str:
    return ", ".join("'" + code.replace("'", "''") + "'" for code in codes) if codes else "''"


def _read_content(
    store: DuckDBStore, *, item_codes: Sequence[str], metric_codes: Sequence[str], derived_source: str
) -> dict[str, frozenset[str]]:
    """Every accounting-content ``security_id`` the panel reads, with its normalized CIK(s).

    CIK comes from the content row's ``cik`` column, else from the id itself
    when it is ``SEC-CIK-*`` or ``SEC-COMPANYFACTS-UNRESOLVED-CIK-*``.
    Aggregated DISTINCT scans only (one row per id/CIK pair).
    """
    rows = store.con.execute(
        f"""
        WITH content AS (
            SELECT DISTINCT security_id, cik FROM fundamental_standardized
            WHERE basis IN ('quarterly', 'instant') AND available_at IS NOT NULL
              AND canonical_code IN ({_quoted_list(item_codes)})
            UNION
            SELECT DISTINCT security_id, cik FROM shares_outstanding_history
            WHERE share_count_type = 'shares_outstanding' AND taxonomy = 'dei' AND available_at IS NOT NULL
            UNION
            SELECT DISTINCT security_id, CAST(NULL AS VARCHAR) FROM derived_metric_values
            WHERE source = ? AND metric_code IN ({_quoted_list(metric_codes)})
        )
        SELECT security_id,
               list_distinct(list(coalesce(
                   CASE WHEN regexp_full_match(trim(cik), '[0-9]{{1,10}}') AND try_cast(trim(cik) AS BIGINT) > 0
                        THEN lpad(CAST(try_cast(trim(cik) AS BIGINT) AS VARCHAR), 10, '0') END,
                   nullif(lpad(regexp_extract(security_id,
                       '^SEC-(?:COMPANYFACTS-UNRESOLVED-)?CIK-([0-9]{{1,10}})$', 1), 10, '0'), '0000000000')
               ))) AS ciks
        FROM content
        WHERE security_id IS NOT NULL
        GROUP BY security_id
        """,
        [derived_source],
    ).fetchall()
    return {str(security_id): frozenset(c for c in (ciks or []) if c) for security_id, ciks in rows}


def build_market_owner_bridge(
    store: DuckDBStore,
    *,
    mode: str = OWNER_MODE_RECONSTRUCTED,
    evidence: Sequence[OwnerLinkEvidence] | None = None,
    item_codes: Sequence[str] = (),
    metric_codes: Sequence[str] = (),
    derived_source: str,
) -> MarketOwnerBridge:
    """Resolve every price line's accounting owner under ``mode``.

    Reads three bounded aggregates: one row per price line, the current SEC
    ticker snapshot, and one row per accounting-content id. ``evidence`` is
    used only in strict mode; ``None`` means no qualified evidence source is
    wired yet, so the strict bridge links nothing.
    """
    if mode not in OWNER_MODES:
        raise ValueError(f"unknown owner mode {mode!r}; expected one of {OWNER_MODES}")
    lines = _read_lines(store)
    content = _read_content(store, item_codes=item_codes, metric_codes=metric_codes, derived_source=derived_source)
    by_id = {line.price_security_id: line for line in lines}
    if mode == OWNER_MODE_STRICT:
        supplied = tuple(evidence or ())
        rows, members, ambiguous, rejected = classify_strict(lines, supplied, content)
        bridge = MarketOwnerBridge(
            mode=mode,
            lines=by_id,
            rows=rows,
            owner_members=members,
            evidence_supplied=len(supplied),
            rejected_evidence=rejected,
            ambiguous_content_ids=ambiguous,
        )
    else:
        tickers = _read_tickers(store)
        rows, members, ambiguous = classify_reconstructed(lines, tickers, content)
        observed = [loaded for _cik, _ticker, loaded in tickers if loaded is not None]
        bridge = MarketOwnerBridge(
            mode=mode,
            lines=by_id,
            rows=rows,
            owner_members=members,
            ticker_snapshot_observed_at=max(observed) if observed else None,
            ambiguous_content_ids=ambiguous,
        )
    return replace(bridge, content_ids=frozenset(content))


# ---------------------------------------------------------------------------
# SQL helpers for the per-batch VALUES relations
# ---------------------------------------------------------------------------


def values_relation_sql(columns: Sequence[tuple[str, str]], row_count: int) -> str:
    """A typed, parameterized VALUES relation (or an empty typed relation).

    Rows are bound, never interpolated; the relation is a CTE body, so it
    creates no temporary catalog object across the connection recycle.
    """
    names = ", ".join(name for name, _kind in columns)
    if row_count <= 0:
        typed_nulls = ", ".join(f"CAST(NULL AS {kind}) AS {name}" for name, kind in columns)
        return f"SELECT {typed_nulls} WHERE false"
    row_sql = "(" + ", ".join(f"CAST(? AS {kind})" for _name, kind in columns) + ")"
    return f"SELECT * FROM (VALUES {', '.join([row_sql] * row_count)}) AS v({names})"


def bridge_value_params(rows: Sequence[BridgeRow]) -> list[object]:
    params: list[object] = []
    for row in rows:
        params.extend(
            [
                row.price_security_id,
                row.owner_security_id,
                row.valid_from,
                row.valid_to,
                row.available_at,
                row.dei_shares_eligible,
                row.identity_basis,
            ]
        )
    return params


def member_value_params(members: Sequence[tuple[str, str]]) -> list[object]:
    return [value for pair in members for value in pair]
