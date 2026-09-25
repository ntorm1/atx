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

0. Symbol-keyed vendor lines (missing/non-positive vendor id: the loader keys
   them by symbol, so one line can concatenate two issuers) never link:
   ``symbol_keyed_line``.
1. ``current_sec_ticker``: the symbol is a current SEC ticker of exactly one
   CIK (newest snapshot row per ticker), and this line is that symbol's latest
   holder (latest last bar, then most bar rows, then id). Former holders of a
   reused symbol are unlinked as ``symbol_held_by_other_line``; a symbol
   mapped to several CIKs is unlinked as ``ambiguous_current_ticker``. A
   secondary class line (``TBLTICKERHISTORY-7`` trading the current class-B
   ticker of X) links to ``SEC-CIK-X`` with its class symbol.
2. ``cik_security_id``: no current SEC ticker matches, but the line id is itself
   ``SEC-CIK-##########`` (assigned upstream by a current-symbol map). A
   *stale* such line (last bar > ``STALE_LINK_DAYS`` before the bar horizon)
   whose CIK already has a current-ticker holder line is a superseded
   predecessor or a reused-symbol victim: ``superseded_cik_line``.
3. ``shared_security_id``: no CIK evidence at all, but accounting content is
   keyed by the line's own id -- the legacy equal-id join, retained so
   non-SEC identity namespaces keep working
   (``identity_basis='shared_security_id_unverified'``).
4. otherwise unlinked as ``no_current_ticker`` (delisted/renamed lines whose
   last symbol no longer maps) -- counted, never silently dropped.

Stale current-ticker holders stay linked (a delisted issuer that still files
under the same ticker is common) but are counted (``stale_links``).

Share class structure (A5 guard, A8 share basis)
-----------------------------------------------
A DEI count is issuer-level (or an arbitrary per-class pick), so it may only
ever price a line of a single-class issuer. An issuer (CIK) is *multi-class*
when it has more than one **common-equity-class** current SEC ticker -- linked
or not -- or more than one concurrently trading common linked line (strict
owner-key splits included). Every current SEC ticker of the CIK is classified
first:

* ``directory_name``: A2's ``universe_us_listed.classify_security_type`` over
  the newest Nasdaq symbol-directory security name (reused, not forked);
  A2's strict eligible types common/ADR/REIT/LP are common-equity classes;
  preferred, warrant, right, unit, note, ETN, fund, ETF and test are not;
  ``common_unverified`` (no common-share evidence in the name: ZONES,
  capital-trust, agency securities) is not counted, and such a *line* keeps
  DEI/valuation only when it is the issuer's only line and the issuer has no
  common ticker (else ``unverified_class_line``);
* ``class_share`` (A8): a ``common_unverified`` name that states a share class
  (``Class B``/``Series A``, e.g. "Lennar Corporation Class B"), or an SEC
  ticker ``ROOT-X`` (X in A/B/C/K) whose ``ROOT`` or ``ROOT-Y`` is another
  ticker of the same CIK (``ticker_class_suffix``), *is* a common class
  (LEN/LEN-B);
* ``ticker_suffix`` (no directory row): SEC suffix conventions ``-P*``/``-PR*``
  preferred, ``-W``/``-WS``/``-WT`` warrant, ``-U``/``-UN`` unit, ``-R``/``-RT``
  right, and a 5th letter W/U/R/P (or ``WS``) on another ticker of the same CIK;
* ``unclassified``: counted as a common class when a price line carries it or
  when the CIK has no directory row at all (conservative); an unclassified,
  untraded ticker of a directory-covered CIK is an unlisted (OTC-style) line
  and is ignored (``unlisted_untraded``).

Historical sibling classes (A8): a sibling class that was delisted, renamed or
collapsed before the ticker snapshot (DISCA/DISCK before WBD) or never entered
the SEC map (CWEN-A) has no current ticker and is unlinked, so the surviving
line looks single-class. The bridge therefore also scans each linked common
line's *symbol history* (every symbol it traded under, with dates) against the
unlinked lines' symbol histories: two concurrently trading symbols are class
siblings when they share a root and differ only by a class designator --
``ROOT``/``ROOT-X`` or ``ROOT-X``/``ROOT-Y`` (X, Y in A/B/C/K), or a 5-letter
Nasdaq ``ROOTX``/``ROOTY`` (or 4-letter ``ROOT``/``ROOTX``, X, Y in A/B/K). A
sibling whose directory type is non-common (a preferred written ``ADC-A``) is
ignored. The linked line's bridge interval is split at the overlap bounds and
each segment carries ``sibling_lines`` (unbridged sibling classes trading in
it); a segment with siblings is multi-class. This is reconstructed evidence
(symbol structure), used only to *withhold* a share basis, never to link.

Every linked row then carries a ``share_basis`` the panel resolves per bar:

* ``single``: one common class. Reconstructed mode prices it with the owner's
  DEI count behind a DEI/archive basis-and-split guard (see ``market_daily``);
  strict mode assigns no DEI (``strict_no_dei_basis``) and uses the line's own
  vendor count.
* ``multi_class``: the issuer cap is only ``sum(close_i x class shares_i)`` over
  the issuer's bridged class lines trading that day, with each line's own
  vendor count as its class count, and only when every known class is bridged
  and priced and the counts reconcile to an issuer total; otherwise the cap and
  every valuation metric are NULL (``multiclass_unresolved``). DEI never prices
  a class line. Strict mode has no class basis yet (always unresolved).
* ``adr``: an ADR line (A2 directory type ``ADR``) never uses the ordinary-share
  DEI count: the vendor ADS-basis count, else NULL (``adr_ratio_unresolved``);
  ``adr_ratio`` is the ordinary shares per ADS parsed from the directory name
  when it states one, used to reject a vendor count that is not ADS-basis.
* ``withheld``: a non-common (preferred, warrant) or unverified line: its own
  vendor count, no DEI, no valuation (``non_common_line``,
  ``unverified_class_line``).

Non-common tickers never withhold the common line. The panel adds a data-side
guard: a DEI state whose filing carries more than one count or share class
(per-class DEI) makes the row multi-class. Residuals: a multi-class issuer with
one listed class, no historical sibling line and a single issuer-total DEI
count equal to the line's own class count is indistinguishable from a
single-class issuer; unlisted classes (META class B) are outside the
``sum`` by definition.

Reporting currency (A8)
-----------------------
Valuation metrics combine a USD market value with monetary fundamentals, so an
owner whose current monetary facts are not declared in USD is withheld: the
bridge reads, once, every content id with a declared non-USD (or undeclared
on a ``monetary``/``per_share`` item) unit plus every content id of the same
CIKs, and turns the per-filing unit flags into point-in-time status intervals
per owner (``non_usd``, ``mixed``, ``unknown``; see :func:`currency_status`).
Rows with no unit metadata at all (legacy rows, fixtures) carry no currency
evidence and are not withheld.
"""

from __future__ import annotations

import datetime as dt
import re
from collections import defaultdict
from collections.abc import Iterable, Sequence
from dataclasses import dataclass, field, replace
from functools import cached_property
from itertools import groupby, pairwise
from typing import NamedTuple

from .connection import DuckDBStore
from .ticker_history import SOURCE_NAME as TICKER_HISTORY_SOURCE_NAME
from .warehouse import cik_security_id

__all__ = [
    "AVAILABILITY_MODELED",
    "AVAILABILITY_VERIFIED",
    "BRIDGE_VALUE_COLUMNS",
    "CURRENCY_MIXED",
    "CURRENCY_NON_USD",
    "CURRENCY_UNKNOWN",
    "CURRENCY_VALUE_COLUMNS",
    "IDENTITY_BASIS_CURRENT_TICKER",
    "IDENTITY_BASIS_SHARED_ID",
    "IDENTITY_BASIS_VERIFIED_DATED",
    "LINK_CIK_SECURITY_ID",
    "LINK_CURRENT_SEC_TICKER",
    "LINK_DATED_EVIDENCE",
    "LINK_SHARED_SECURITY_ID",
    "MEMBER_VALUE_COLUMNS",
    "OWNER_MODES",
    "OWNER_MODE_RECONSTRUCTED",
    "OWNER_MODE_STRICT",
    "SHARE_BASIS_ADR",
    "SHARE_BASIS_MULTI_CLASS",
    "SHARE_BASIS_SINGLE",
    "SHARE_BASIS_WITHHELD",
    "SINGLE_CLASS_LINK_COLUMNS",
    "STALE_LINK_DAYS",
    "WITHHELD_MULTI_COMMON_CLASS",
    "WITHHELD_NON_COMMON_LINE",
    "WITHHELD_UNVERIFIED_CLASS_LINE",
    "BridgeRow",
    "CurrencyEvent",
    "LineSymbol",
    "MarketOwnerBridge",
    "OwnerLinkEvidence",
    "PriceLine",
    "TickerClass",
    "are_class_siblings",
    "bridge_value_params",
    "build_market_owner_bridge",
    "classify_reconstructed",
    "classify_sec_tickers",
    "classify_strict",
    "currency_status",
    "currency_value_params",
    "member_value_params",
    "normalize_cik",
    "normalize_symbol",
    "parse_ads_ratio",
    "values_relation_sql",
]

OWNER_MODE_STRICT = "strict"
OWNER_MODE_RECONSTRUCTED = "reconstructed"
OWNER_MODES = (OWNER_MODE_STRICT, OWNER_MODE_RECONSTRUCTED)

IDENTITY_BASIS_CURRENT_TICKER = "current_ticker_unverified"
IDENTITY_BASIS_SHARED_ID = "shared_security_id_unverified"
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
UNLINKED_SYMBOL_KEYED_LINE = "symbol_keyed_line"
UNLINKED_SUPERSEDED_CIK_LINE = "superseded_cik_line"

#: A line whose last bar is more than this many calendar days before the bar
#: horizon (latest last bar of any line) is *stale* for the reuse checks.
STALE_LINK_DAYS = 30

#: How a current SEC ticker's security class was determined.
CLASS_BASIS_DIRECTORY = "directory_name"
CLASS_BASIS_SUFFIX = "ticker_suffix"
CLASS_BASIS_CLASS_SUFFIX = "ticker_class_suffix"
CLASS_BASIS_UNCLASSIFIED = "unclassified"
CLASS_BASIS_UNLISTED = "unlisted_untraded"

#: Why a linked line's DEI shares / valuation metrics are withheld.
#: ``multi_common_class`` now marks a multi-class issuer's lines as priced only
#: by the class sum (A8), not a bridge-level withhold.
WITHHELD_MULTI_COMMON_CLASS = "multi_common_class"
WITHHELD_NON_COMMON_LINE = "non_common_line"
WITHHELD_UNVERIFIED_CLASS_LINE = "unverified_class_line"
WITHHELD_STRICT_NO_DEI = "strict_no_dei_basis"
_UNVERIFIED_COMMON = "common_unverified"
#: A8 class evidence: a ``common_unverified`` name or ticker that states a share class.
_CLASS_SHARE = "class_share"
_ADR = "ADR"

#: A8 share basis of a linked row (see the module docstring).
SHARE_BASIS_SINGLE = "single"
SHARE_BASIS_MULTI_CLASS = "multi_class"
SHARE_BASIS_ADR = "adr"
SHARE_BASIS_WITHHELD = "withheld"

#: Owner reporting-currency statuses that withhold valuation metrics.
CURRENCY_NON_USD = "non_usd"
CURRENCY_MIXED = "mixed"
CURRENCY_UNKNOWN = "unknown"

#: Class designators: ``ROOT-X`` separator form, and the Nasdaq fifth letter.
_SEPARATOR_CLASS_LETTERS = frozenset("ABCK")
_FIFTH_LETTER_CLASS_LETTERS = frozenset("ABK")
_SEPARATOR_CLASS = re.compile(r"([A-Z]{1,5})-([A-Z])")
_PLAIN_SYMBOL = re.compile(r"[A-Z]{1,5}")
#: A ``common_unverified`` directory name that states a share class.
_CLASS_NAME = re.compile(r"\b(?:CLASS|SERIES) [A-Z]\b")
_CLASS_NAME_EXCLUSIONS = re.compile(r"\b(?:TRUST|NOTES?|BONDS?|DEBENTURES?|ZONES|DEPOSITARY|INTEREST)\b")

#: SEC ``company_tickers`` suffix conventions for non-common lines, applied to a
#: normalized ticker (separators folded to ``-``) only when the symbol
#: directory has no row for it.
_SUFFIX_CLASS_RULES: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("preferred", re.compile(r"-P(?:R)?-?[A-Z]?$")),
    ("warrant", re.compile(r"-W(?:S|T)?-?[A-Z]?$")),
    ("unit", re.compile(r"-U(?:N)?$")),
    ("right", re.compile(r"-R(?:T)?$")),
)
#: Nasdaq/FINRA fifth-letter codes on a 5-letter ticker whose 4-letter base is
#: another ticker of the same CIK (``WS`` on any longer ticker): warrants,
#: units, rights, preferred series (M-P) and convertible debt (G-I).
_SIBLING_SUFFIX_CLASSES: tuple[tuple[str, str], ...] = (
    ("WS", "warrant"),
    ("W", "warrant"),
    ("U", "unit"),
    ("R", "right"),
    ("P", "preferred"),
    ("O", "preferred"),
    ("N", "preferred"),
    ("M", "preferred"),
    ("G", "note"),
    ("H", "note"),
    ("I", "note"),
)

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
    ("valuation_eligible", "BOOLEAN"),
    ("identity_basis", "VARCHAR"),
    # A8 share basis: the issuer the class sum groups by (CIK, else owner),
    # the row's share basis, the issuer's known common classes, the unbridged
    # sibling class lines trading in this interval, and the ADS ratio.
    ("issuer_key", "VARCHAR"),
    ("share_basis", "VARCHAR"),
    ("issuer_class_lines", "INTEGER"),
    ("sibling_lines", "INTEGER"),
    ("adr_ratio", "DOUBLE"),
    # Row-level identity lineage (A9 columns, written when the table has them).
    ("availability_basis", "VARCHAR"),
    ("link_method", "VARCHAR"),
)
#: Column layout of the per-batch ``owner_members`` VALUES relation.
MEMBER_VALUE_COLUMNS: tuple[tuple[str, str], ...] = (
    ("owner_key", "VARCHAR"),
    ("member_security_id", "VARCHAR"),
)
#: Column layout of :meth:`MarketOwnerBridge.single_class_link_rows` (split
#: evidence lookup for derived metrics, R1e).
SINGLE_CLASS_LINK_COLUMNS: tuple[tuple[str, str], ...] = (
    ("content_security_id", "VARCHAR"),
    ("price_security_id", "VARCHAR"),
    ("issuer_key", "VARCHAR"),
    ("valid_from", "DATE"),
    ("valid_to", "DATE"),
    ("available_at", "TIMESTAMP"),
    ("issuer_multi_class", "BOOLEAN"),
    ("link_method", "VARCHAR"),
    ("identity_basis", "VARCHAR"),
    ("availability_basis", "VARCHAR"),
)
#: Column layout of the per-batch ``currency_guard`` VALUES relation: the
#: owner's non-USD reporting-currency status over ``[from_at, to_at)``.
CURRENCY_VALUE_COLUMNS: tuple[tuple[str, str], ...] = (
    ("owner_key", "VARCHAR"),
    ("from_at", "TIMESTAMP"),
    ("to_at", "TIMESTAMP"),
    ("currency_status", "VARCHAR"),
)

_SEC_CIK_ID = re.compile(r"SEC-CIK-(\d{10})")
_VENDOR_LINE_PREFIX = "TBLTICKERHISTORY-"
_POSITIVE_VENDOR_LINE = re.compile(r"TBLTICKERHISTORY-[1-9]\d*")
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
    #: Missing/non-positive vendor id: the loader keyed the line by symbol.
    symbol_keyed: bool = False


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
    #: False for every line of a multi-common-class issuer (and for a
    #: non-common line) until A8's class basis.
    valuation_eligible: bool = False
    #: Common linked lines of the same issuer (CIK) trading concurrently with this one.
    concurrent_owner_lines: int = 0
    #: max(concurrent common linked lines, common-class SEC tickers) of the issuer.
    issuer_class_lines: int = 0
    #: This line's own security class and how it was determined (see TickerClass).
    security_class: str | None = None
    class_basis: str | None = None
    #: Why DEI/valuation is withheld (``multi_common_class``, ``non_common_line``,
    #: ``strict_no_dei_basis``), or None.
    withheld_reason: str | None = None
    #: A8 share basis (``single``/``multi_class``/``adr``/``withheld``), None if unlinked.
    share_basis: str | None = None
    #: Unbridged sibling class lines trading during this interval (A8).
    sibling_lines: int = 0
    #: Ordinary shares per ADS stated by the directory name (ADR lines only).
    adr_ratio: float | None = None

    @property
    def linked(self) -> bool:
        return self.owner_security_id is not None

    @property
    def issuer_key(self) -> str:
        return _issuer_key(self)


@dataclass(frozen=True)
class TickerClass:
    """Security class of one current SEC ticker of a CIK."""

    cik: str
    ticker: str
    security_class: str
    basis: str
    #: True when the ticker counts as a common-equity class of the issuer.
    counted_common: bool


@dataclass(frozen=True)
class LineSymbol:
    """One symbol a price line traded under, with its first and last bar dates."""

    price_security_id: str
    symbol: str
    first_trade_date: dt.date
    last_trade_date: dt.date


@dataclass(frozen=True)
class CurrencyEvent:
    """Monetary unit flags of one content id's facts for one (period_end, available_at)."""

    security_id: str
    period_end: dt.date
    available_at: dt.datetime
    has_usd: bool
    has_foreign: bool
    has_unknown: bool


class DirectorySnapshot(NamedTuple):
    """Newest symbol-directory classification: symbol -> type, and ADS ratios by symbol key."""

    types: dict[str, str]
    adr_ratios: dict[str, float]


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
    ticker_snapshot_oldest_observed_at: dt.datetime | None = None
    ambiguous_content_ids: int = 0
    #: Every accounting-content id seen (lets summary() flag owners with no
    #: content without another warehouse scan).
    content_ids: frozenset[str] = frozenset()
    #: Content ids with at least one per-class DEI filing (panel withholds
    #: valuation from that filing on; counted as ``withheld_per_class_dei``).
    per_class_dei_ids: frozenset[str] = frozenset()
    #: Classification of every current SEC ticker (for the reason counters).
    ticker_classes: tuple[TickerClass, ...] = ()
    #: Monetary unit events of every content id whose CIK declares a non-USD
    #: (or undeclared) monetary unit anywhere (see :func:`currency_status`).
    currency_events: dict[str, tuple[CurrencyEvent, ...]] = field(default_factory=dict)

    @property
    def identity_basis(self) -> str:
        return IDENTITY_BASIS_VERIFIED_DATED if self.mode == OWNER_MODE_STRICT else IDENTITY_BASIS_CURRENT_TICKER

    @property
    def availability_basis(self) -> str:
        return AVAILABILITY_VERIFIED if self.mode == OWNER_MODE_STRICT else AVAILABILITY_MODELED

    def line_ids(self) -> list[str]:
        return sorted(self.lines)

    @property
    def bar_horizon(self) -> dt.date | None:
        """Latest last bar of any price line (the reconstructed 'now')."""
        return max((line.last_trade_date for line in self.lines.values()), default=None)

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

    @cached_property
    def _owners_by_member(self) -> dict[str, tuple[str, ...]]:
        owners: dict[str, set[str]] = defaultdict(set)
        for owner, members in self.owner_members.items():
            for member in (owner, *members):
                owners[member].add(owner)
        return {member: tuple(sorted(found)) for member, found in owners.items()}

    def single_class_lines(
        self, content_security_id: str, on: dt.date, *, as_of: dt.datetime | None = None
    ) -> tuple[BridgeRow, ...]:
        """The single-common-class price line(s) linked to an accounting id on a date (vendor split evidence).

        PIT-safe split-evidence lookup (R1e): ``content_security_id`` is any
        accounting id -- an owner (``SEC-CIK-*``) or one of its members
        (``SEC-COMPANYFACTS-UNRESOLVED-CIK-*``); ``on`` is the date the price
        line must be linked on (``valid_from <= on < valid_to``); ``as_of`` is
        the clock the link must be available by (default ``on`` + 22h, the bar
        cutoff). Returns the linked rows with ``share_basis='single'`` -- each
        carries ``price_security_id``, ``link_method``, ``identity_basis`` and
        ``availability_basis`` -- or ``()`` when the issuer is multi-class on
        that date (a ``multi_class`` row, including a historical sibling-class
        segment), has only ADR / non-common lines (an ADS price factor is not an
        ordinary-share split), or has no available link. The panel's data-side
        per-class DEI guard is not applied here. Reconstructed links are
        ``current_ticker_unverified`` with modeled availability; never present
        them as verified.
        """
        clock = as_of if as_of is not None else dt.datetime.combine(on, dt.time(_MODELED_LINK_HOURS))
        owners = set(self._owners_by_member.get(content_security_id, ()))
        issuers: set[str] = set()
        for row in self.rows:
            if row.linked and row.owner_security_id in owners:
                issuers.add(_issuer_key(row))
        covering = [
            row
            for row in self.rows
            if row.linked
            and _issuer_key(row) in issuers
            and row.valid_from is not None
            and row.valid_from <= on
            and (row.valid_to is None or on < row.valid_to)
            and row.available_at is not None
            and row.available_at <= clock
        ]
        if any(row.share_basis == SHARE_BASIS_MULTI_CLASS for row in covering):
            return ()
        return tuple(row for row in covering if row.share_basis == SHARE_BASIS_SINGLE)

    def single_class_link_rows(self) -> list[tuple[object, ...]]:
        """Every (member content id, single-class link interval) as :data:`SINGLE_CLASS_LINK_COLUMNS` tuples.

        The set-based form of :meth:`single_class_lines` for binding as a
        VALUES relation: a split-evidence query joins its accounting id and
        date to ``content_security_id`` with ``valid_from <= date < valid_to``
        and ``available_at <= cutoff``, and must also require that no
        ``multi_class`` interval of the same ``issuer_key`` covers the date
        (``issuer_multi_class=true`` rows are included for that purpose).
        """
        out: list[tuple[object, ...]] = []
        for row in self.rows:
            if not row.linked or row.share_basis not in (SHARE_BASIS_SINGLE, SHARE_BASIS_MULTI_CLASS):
                continue
            for member in self.owner_members.get(str(row.owner_security_id), (str(row.owner_security_id),)):
                out.append(
                    (
                        member,
                        row.price_security_id,
                        _issuer_key(row),
                        row.valid_from,
                        row.valid_to,
                        row.available_at,
                        row.share_basis == SHARE_BASIS_MULTI_CLASS,
                        row.link_method,
                        row.identity_basis,
                        row.availability_basis,
                    )
                )
        return out

    def expand_to_issuers(self, identifiers: Iterable[str]) -> list[str]:
        """``identifiers`` plus every price line linked to the same issuer (CIK or owner).

        A multi-class issuer's cap is a sum over all of its bridged class lines,
        so a refresh scoped to one class line must compute (and rewrite) its
        whole issuer group to match an unscoped run.
        """
        wanted = set(identifiers)
        issuers = {_issuer_key(row) for row in self.rows if row.linked and row.price_security_id in wanted}
        wanted.update(row.price_security_id for row in self.rows if row.linked and _issuer_key(row) in issuers)
        return sorted(wanted)

    def currency_intervals(self, rows: Iterable[BridgeRow]) -> list[tuple[str, dt.datetime, dt.datetime | None, str]]:
        """Non-USD reporting-currency intervals ``(owner, from_at, to_at, status)`` for the rows' owners.

        The owner's status at a clock is :func:`currency_status` of the unit
        flags of its latest ``(period_end, available_at)`` monetary filing
        event available by then (all member content ids merged). USD intervals
        are not emitted; intervals are disjoint per owner, ``to_at`` exclusive.
        """
        intervals: list[tuple[str, dt.datetime, dt.datetime | None, str]] = []
        owners = sorted({row.owner_security_id for row in rows if row.owner_security_id is not None})
        for owner in owners:
            events = [
                event
                for member in self.owner_members.get(owner, (owner,))
                for event in self.currency_events.get(member, ())
            ]
            if not events:
                continue
            merged: dict[tuple[dt.date, dt.datetime], tuple[bool, bool, bool]] = {}
            changes: list[tuple[dt.datetime, str | None]] = []
            ordered = sorted(events, key=lambda e: e.available_at)
            for available_at, group in groupby(ordered, key=lambda e: e.available_at):
                for event in group:
                    key = (event.period_end, event.available_at)
                    usd, foreign, unknown = merged.get(key, (False, False, False))
                    merged[key] = (usd or event.has_usd, foreign or event.has_foreign, unknown or event.has_unknown)
                status = currency_status(*merged[max(merged)])
                if not changes or changes[-1][1] != status:
                    changes.append((available_at, status))
            for index, (start, status) in enumerate(changes):
                if status is None:
                    continue
                end = changes[index + 1][0] if index + 1 < len(changes) else None
                intervals.append((owner, start, end, status))
        return intervals

    def owner_aligned_batches(self, identifiers: Sequence[str], batch_size: int) -> list[tuple[str, ...]]:
        """Pack identifiers into batches that never split one owner's lines.

        Every price line that links (in any interval) to the same owner or CIK
        lands in the same batch, so issuer-level aggregation over an issuer's
        class lines (A8) can run inside one statement. A group larger than ``batch_size``
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
                keys = ["owner:" + str(row.owner_security_id)] + (["cik:" + row.cik] if row.cik else [])
                for key in keys:
                    left, right = find("line:" + row.price_security_id), find(key)
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
        linked_by_basis: dict[str, int] = defaultdict(int)
        linked_lines = unlinked_lines = linked_bar_rows = unlinked_bar_rows = 0
        secondary = dei_withheld = valuation_withheld = id_disagreements = stale = per_class_dei = 0
        withheld_by_reason: dict[str, int] = defaultdict(int)
        basis_lines: dict[str, int] = defaultdict(int)
        owners: set[str] = set()
        multi_line_issuers: set[str] = set()
        multi_class_lines = sibling_class_lines = adr_ratio_lines = 0
        horizon = self.bar_horizon
        for price_id in sorted(wanted):
            line = self.lines.get(price_id)
            rows = by_line.get(price_id, ())
            linked = [row for row in rows if row.linked]
            bar_rows = line.bar_rows if line is not None else 0
            if linked:
                linked_lines += 1
                linked_bar_rows += bar_rows
                linked_by_method[str(linked[0].link_method)] += 1
                linked_by_basis[str(linked[0].identity_basis)] += 1
                for basis in sorted({str(row.share_basis) for row in linked}):
                    basis_lines[basis] += 1
                for row in linked:
                    owners.add(str(row.owner_security_id))
                    if row.withheld_reason == WITHHELD_MULTI_COMMON_CLASS:
                        multi_line_issuers.add(_issuer_key(row))
                if any(row.withheld_reason == WITHHELD_MULTI_COMMON_CLASS for row in linked):
                    multi_class_lines += 1
                if any(row.sibling_lines > 0 for row in linked):
                    sibling_class_lines += 1
                if any(row.adr_ratio is not None for row in linked):
                    adr_ratio_lines += 1
                if any(row.owner_security_id != price_id for row in linked):
                    secondary += 1
                if not any(row.dei_shares_eligible for row in linked):
                    dei_withheld += 1
                if not any(row.valuation_eligible for row in linked):
                    valuation_withheld += 1
                    reason = next((row.withheld_reason for row in linked if row.withheld_reason), None)
                    withheld_by_reason[str(reason)] += 1
                elif any(
                    member in self.per_class_dei_ids
                    for row in linked
                    for member in self.owner_members.get(str(row.owner_security_id), ())
                ):
                    # Bridge-eligible, but the panel withholds DEI and valuation
                    # from the first per-class DEI filing of the owner onward.
                    per_class_dei += 1
                if line is not None and horizon is not None and _is_stale(line, horizon):
                    stale += 1
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
            "linked_by_identity_basis": dict(sorted(linked_by_basis.items())),
            "unlinked_by_reason": dict(sorted(unlinked_by_reason.items())),
            "secondary_lines_linked": secondary,
            "owners": len(owners),
            "multi_line_issuers": len(multi_line_issuers),
            "owners_without_accounting_content": without_content,
            "dei_shares_withheld_lines": dei_withheld,
            "valuation_withheld_lines": valuation_withheld,
            # Reason counters (N1/N2): lines withheld at the bridge, by reason,
            # and bridge-eligible lines the per-class DEI guard withholds later.
            # A8: a multi-common-class line is no longer withheld at the bridge;
            # it is priced only by the class sum (never by DEI), and the panel
            # counts its resolved/unresolved rows (``multi_class_lines`` here).
            "withheld_multi_common_class": multi_class_lines,
            "withheld_non_common_line": withheld_by_reason.get(WITHHELD_NON_COMMON_LINE, 0),
            "withheld_unverified_class_line": withheld_by_reason.get(WITHHELD_UNVERIFIED_CLASS_LINE, 0),
            # Upper bound (R2-M2): lines whose owner has *any* per-class DEI
            # filing; the panel treats only rows whose as-of DEI state is
            # per-class as multi-class.
            "withheld_per_class_dei": per_class_dei,
            "multi_class_lines": multi_class_lines,
            "sibling_class_lines": sibling_class_lines,
            "adr_ratio_lines": adr_ratio_lines,
            "share_basis_lines": dict(sorted(basis_lines.items())),
            # Content ids read for the currency guard (declared non-USD or
            # undeclared monetary units, plus every id of the same CIKs).
            "currency_evidence_content_ids": len(self.currency_events),
            # Current SEC tickers (whole snapshot) by class decision.
            "non_common_tickers_ignored": sum(
                1 for item in self.ticker_classes if item.basis != CLASS_BASIS_UNLISTED and not item.counted_common
            ),
            "non_common_tickers_by_basis": _count(
                item.basis
                for item in self.ticker_classes
                if item.basis != CLASS_BASIS_UNLISTED and not item.counted_common
            ),
            "non_common_tickers_by_class": _count(
                item.security_class
                for item in self.ticker_classes
                if item.basis != CLASS_BASIS_UNLISTED and not item.counted_common
            ),
            "unlisted_untraded_tickers_ignored": sum(
                1 for item in self.ticker_classes if item.basis == CLASS_BASIS_UNLISTED
            ),
            "unclassified_tickers_counted_common": sum(
                1 for item in self.ticker_classes if item.basis == CLASS_BASIS_UNCLASSIFIED and item.counted_common
            ),
            "stale_links": stale,
            "stale_link_days": STALE_LINK_DAYS,
            "bar_horizon": horizon.isoformat() if horizon is not None else None,
            "line_id_cik_disagreements": id_disagreements,
            "ambiguous_content_owner_ids": self.ambiguous_content_ids,
            "ticker_snapshot_observed_at": (
                self.ticker_snapshot_observed_at.isoformat() if self.ticker_snapshot_observed_at else None
            ),
            "ticker_snapshot_oldest_observed_at": (
                self.ticker_snapshot_oldest_observed_at.isoformat() if self.ticker_snapshot_oldest_observed_at else None
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


def _issuer_key(row: BridgeRow) -> str:
    """The issuer a linked row belongs to: its CIK, else its owner id."""
    return f"cik:{row.cik}" if row.cik else f"owner:{row.owner_security_id}"


def currency_status(has_usd: bool, has_foreign: bool, has_unknown: bool) -> str | None:
    """Reporting-currency status of one monetary filing event, or None when USD.

    ``has_foreign``: some monetary fact declares an ISO currency other than USD
    (``CAD``, ``EUR/shares``); ``has_usd``: some declares USD; ``has_unknown``:
    some monetary/per-share fact carries no parseable currency. Any foreign unit
    withholds (``mixed`` when USD appears too -- e.g. a convenience
    translation); undeclared units withhold only when nothing declares USD.
    """
    if has_foreign:
        return CURRENCY_MIXED if has_usd else CURRENCY_NON_USD
    if has_unknown and not has_usd:
        return CURRENCY_UNKNOWN
    return None


def _class_families(symbol: str | None) -> list[tuple[str, str, str]]:
    """``(family, root, designator)`` memberships of a symbol that can name a share class.

    ``sep``: ``ROOT-X`` (X in A/B/C/K) and its plain base ``ROOT``; ``nq``: a
    5-letter Nasdaq ``ROOTX`` (X in A/B/K) and its 4-letter base ``ROOT``.
    """
    key = normalize_symbol(symbol)
    if key is None:
        return []
    families: list[tuple[str, str, str]] = []
    match = _SEPARATOR_CLASS.fullmatch(key)
    if match is not None and match.group(2) in _SEPARATOR_CLASS_LETTERS:
        families.append(("sep", match.group(1), match.group(2)))
    elif _PLAIN_SYMBOL.fullmatch(key):
        families.append(("sep", key, ""))
        if len(key) == 5 and key[4] in _FIFTH_LETTER_CLASS_LETTERS:
            families.append(("nq", key[:4], key[4]))
        elif len(key) == 4:
            families.append(("nq", key, ""))
    return families


def are_class_siblings(left: str | None, right: str | None) -> bool:
    """True when two symbols name different share classes of one root (``LEN``/``LEN-B``, ``DISCA``/``DISCK``)."""
    left_families = {(family, root): designator for family, root, designator in _class_families(left)}
    for family, root, designator in _class_families(right):
        other = left_families.get((family, root))
        if other is not None and other != designator:
            return True
    return False


_UNITS = ("ONE", "TWO", "THREE", "FOUR", "FIVE", "SIX", "SEVEN", "EIGHT", "NINE", "TEN", "ELEVEN", "TWELVE")
_TENS = (("FIFTEEN", 15), ("TWENTY", 20), ("TWENTY-FIVE", 25), ("THIRTY", 30), ("FORTY", 40), ("FIFTY", 50))
_NUMBER_WORDS: dict[str, float] = {
    **{word: float(value) for value, word in enumerate(_UNITS, start=1)},
    **{word: float(value) for word, value in _TENS},
    "HUNDRED": 100.0,
}
_FRACTION_WORDS: dict[str, float] = {
    **dict.fromkeys(("ONE-HALF", "ONE HALF"), 0.5),
    **dict.fromkeys(("ONE-QUARTER", "ONE-FOURTH"), 0.25),
    **{"ONE-THIRD": 1 / 3, "ONE-FIFTH": 0.2, "ONE-TENTH": 0.1},
}
_ADS_REPRESENTING = re.compile(r"EACH\s+(?:ADS\s+)?REPRESENT(?:ING|S)\s+(?:AN?\s+)?(?P<rest>.*)$")
_ADS_UNDERLYING = re.compile(r"\b(?:ORDINARY|COMMON)\b|\bSHARES?\b")


def parse_ads_ratio(name: str | None) -> float | None:
    """Ordinary shares per ADS stated by an ADR directory name, or None.

    "... American Depositary Shares, each representing eight Ordinary shares"
    -> 8.0; "(each representing ten (10) Common Shares)" -> 10.0; "each
    representing one-half of one ordinary share" -> 0.5; "1/4" -> 0.25.
    """
    if not name:
        return None
    match = _ADS_REPRESENTING.search(str(name).upper())
    if match is None:
        return None
    rest = match.group("rest").strip()
    if not _ADS_UNDERLYING.search(rest[:80]) or "INTEREST" in rest[:80]:
        return None
    number = re.match(r"\(?(\d+(?:\.\d+)?)\)?(?!\s*/)", rest)
    fraction = re.match(r"(\d+)\s*/\s*(\d+)", rest)
    if fraction is not None and int(fraction.group(2)) > 0:
        return int(fraction.group(1)) / int(fraction.group(2))
    if number is not None:
        value = float(number.group(1))
        return value if value > 0 else None
    for words, value in sorted(_FRACTION_WORDS.items(), key=lambda item: -len(item[0])):
        if rest.startswith(words):
            return value
    for words, value in sorted(_NUMBER_WORDS.items(), key=lambda item: -len(item[0])):
        if re.match(re.escape(words) + r"\b", rest):
            return value
    return None


def _is_stale(line: PriceLine, horizon: dt.date) -> bool:
    return (horizon - line.last_trade_date).days > STALE_LINK_DAYS


def _is_symbol_keyed(price_security_id: str) -> bool:
    return price_security_id.startswith(_VENDOR_LINE_PREFIX) and not _POSITIVE_VENDOR_LINE.fullmatch(price_security_id)


def _ticker_index(
    tickers: Sequence[tuple[str, str, dt.datetime | None]],
) -> tuple[dict[str, dict[str, tuple[str, dt.datetime | None]]], dict[str, int]]:
    """``symbol key -> {cik: (ticker, observed_at)}`` and ``cik -> current ticker count``.

    A ticker keeps only its newest snapshot row(s): the security-master load
    replaces rows per CIK, so a CIK that dropped out of the SEC file leaves a
    stale row behind that must not make a live holder's ticker ambiguous.
    """
    latest: dict[str, dt.datetime] = {}
    rows: list[tuple[str, str, str, dt.datetime | None]] = []
    for raw_cik, raw_ticker, observed_at in tickers:
        cik, key = normalize_cik(raw_cik), normalize_symbol(raw_ticker)
        if cik is None or key is None:
            continue
        rows.append((key, cik, str(raw_ticker).strip().upper(), observed_at))
        stamp = observed_at or dt.datetime.min
        latest[key] = max(latest.get(key, dt.datetime.min), stamp)
    index: dict[str, dict[str, tuple[str, dt.datetime | None]]] = defaultdict(dict)
    for key, cik, ticker, observed_at in rows:
        if (observed_at or dt.datetime.min) < latest[key]:
            continue
        previous = index[key].get(cik)
        if previous is None or ticker < previous[0]:
            index[key][cik] = (ticker, observed_at)
    per_cik: dict[str, set[str]] = defaultdict(set)
    for key, owners in index.items():
        for cik in owners:
            per_cik[cik].add(key)
    return dict(index), {cik: len(keys) for cik, keys in per_cik.items()}


def _count(values: Iterable[str]) -> dict[str, int]:
    counts: dict[str, int] = defaultdict(int)
    for value in values:
        counts[value] += 1
    return dict(sorted(counts.items()))


def _common_equity_types() -> frozenset[str]:
    """A2's strict eligible equity types (common/ADR/REIT/LP with positive evidence).

    A2's ``common_unverified`` (a name surviving every exclusion without
    common-share evidence -- e.g. exchangeable ZONES, capital-trust securities,
    agency bonds) is *not* counted as a common class; see ``_with_class_guards``
    for how such a line itself is treated. Imported lazily:
    ``universe_us_listed`` imports ``market_daily``, which imports this module.
    """
    from .universe_us_listed import ELIGIBLE_SECURITY_TYPES

    return frozenset(ELIGIBLE_SECURITY_TYPES)


def _directory_type(directory: dict[str, str], ticker: str) -> str | None:
    """Directory security type of an SEC ticker, or None when not listed there.

    SEC writes share classes with ``-`` (``BRK-B``) while the Nasdaq directory
    writes them with ``.`` (``BRK.B``) and uses ``-`` for preferreds, so the
    ``.`` form is tried first.
    """
    candidates = [ticker.replace("-", "."), ticker] if "-" in ticker else [ticker]
    for candidate in candidates:
        kind = directory.get(candidate)
        if kind is not None and kind != "unknown":
            return kind
    return None


def _suffix_class(key: str, cik_keys: set[str]) -> str | None:
    """Non-common class implied by an SEC ticker suffix, or None."""
    for label, pattern in _SUFFIX_CLASS_RULES:
        if pattern.search(key):
            return label
    if "-" not in key:
        for suffix, label in _SIBLING_SUFFIX_CLASSES:
            base = key[: -len(suffix)]
            fifth_letter = len(suffix) == 1 and len(key) == 5
            if key.endswith(suffix) and (fifth_letter or len(suffix) > 1) and base in cik_keys:
                return label
    return None


def _is_class_suffix_sibling(key: str, cik_keys: set[str]) -> bool:
    """``ROOT-X`` (X in A/B/C/K) whose ``ROOT`` or another ``ROOT-Y`` is a ticker of the same CIK."""
    match = _SEPARATOR_CLASS.fullmatch(key)
    if match is None or match.group(2) not in _SEPARATOR_CLASS_LETTERS:
        return False
    root = match.group(1)
    return root in cik_keys or any(
        other != key and other.startswith(root + "-") and are_class_siblings(other, key) for other in cik_keys
    )


def classify_sec_tickers(
    tickers: Sequence[tuple[str, str, dt.datetime | None]],
    directory: dict[str, str] | None = None,
    carried_symbols: Iterable[str | None] = (),
) -> tuple[TickerClass, ...]:
    """Classify every current SEC ticker (newest snapshot row) of every CIK.

    ``directory`` maps a Nasdaq symbol-directory symbol (upper case, ``.`` class
    separator) to A2's ``classify_security_type`` label; ``carried_symbols``
    are the last symbols of the price lines in bars. See the module docstring
    for the ``directory_name`` / ``ticker_suffix`` / ``unclassified`` /
    ``unlisted_untraded`` decisions.
    """
    common_types = _common_equity_types()
    directory = directory or {}
    carried = {key for key in (normalize_symbol(symbol) for symbol in carried_symbols) if key}
    index, _counts = _ticker_index(tickers)
    per_cik: dict[str, list[tuple[str, str]]] = defaultdict(list)
    for key, owners in index.items():
        for cik, (ticker, _observed) in owners.items():
            per_cik[cik].append((key, ticker))
    classes: list[TickerClass] = []
    for cik, items in sorted(per_cik.items()):
        cik_keys = {key for key, _ticker in items}
        covered = any(_directory_type(directory, ticker) is not None for _key, ticker in items)
        for key, ticker in sorted(items):
            kind = _directory_type(directory, ticker)
            if kind == _UNVERIFIED_COMMON and _is_class_suffix_sibling(key, cik_keys):
                # A8 (LEN-B): A2 finds no common-share wording, but the ticker
                # is a class designator of another ticker of the same CIK.
                classes.append(TickerClass(cik, ticker, _CLASS_SHARE, CLASS_BASIS_CLASS_SUFFIX, True))
                continue
            if kind is not None:
                counted = kind in common_types or kind == _CLASS_SHARE
                classes.append(TickerClass(cik, ticker, kind, CLASS_BASIS_DIRECTORY, counted))
                continue
            suffix = _suffix_class(key, cik_keys)
            if suffix is not None:
                classes.append(TickerClass(cik, ticker, suffix, CLASS_BASIS_SUFFIX, False))
            elif key in carried or not covered:
                classes.append(TickerClass(cik, ticker, "unclassified", CLASS_BASIS_UNCLASSIFIED, True))
            else:
                # Not in the exchange directory although the issuer is, and no
                # price line trades it: an unlisted (OTC-style) line.
                classes.append(TickerClass(cik, ticker, "unclassified", CLASS_BASIS_UNLISTED, False))
    return tuple(classes)


def _with_class_guards(
    rows: list[BridgeRow],
    lines: dict[str, PriceLine],
    classes: Sequence[TickerClass],
    *,
    strict: bool,
) -> tuple[BridgeRow, ...]:
    """Assign each linked row its share basis and DEI/valuation eligibility.

    An issuer (CIK, else owner id) is multi-class when it has more than one
    common-equity-class current SEC ticker (see :func:`classify_sec_tickers`)
    or more than one concurrently trading common linked line; its lines get
    ``share_basis='multi_class'`` (no DEI; priced only by the class sum).
    Non-common tickers never count. A linked line that is itself non-common
    gets neither DEI nor valuation. An ADR line of a single-class issuer gets
    ``share_basis='adr'`` (no DEI). Strict mode never assigns DEI shares and has
    no class basis yet (a strict multi-class line is valuation-withheld).
    """
    common_types = _common_equity_types()
    by_key = {(item.cik, normalize_symbol(item.ticker)): item for item in classes}
    common_counts: dict[str, int] = defaultdict(int)
    for item in classes:
        if item.counted_common:
            common_counts[item.cik] += 1

    def line_class(row: BridgeRow) -> tuple[str, str, str]:
        """(class, basis, category): category is common / unverified / non_common."""
        key = normalize_symbol(row.share_class_symbol)
        item = by_key.get((row.cik or "", key or ""))
        if item is not None:
            kind, basis = item.security_class, item.basis
        else:
            suffix = _suffix_class(key, set()) if key else None
            kind, basis = (suffix, CLASS_BASIS_SUFFIX) if suffix else ("unclassified", CLASS_BASIS_UNCLASSIFIED)
        if kind in common_types or kind in ("unclassified", _CLASS_SHARE):
            return kind, basis, "common"
        return kind, basis, "unverified" if kind == _UNVERIFIED_COMMON else "non_common"

    kinds = {id(row): line_class(row) for row in rows if row.owner_security_id is not None}
    issuer_lines: dict[str, set[str]] = defaultdict(set)
    for row in rows:
        if row.owner_security_id is not None and kinds[id(row)][2] != "non_common":
            issuer_lines[_issuer_key(row)].add(row.price_security_id)

    line_category = {row.price_security_id: kinds[id(row)][2] for row in rows if row.owner_security_id is not None}

    def concurrent_lines(row: BridgeRow, category: str | None = None) -> int:
        """Linked (non-non-common) lines of the issuer trading concurrently, optionally one category."""
        line = lines[row.price_security_id]
        return sum(
            1
            for other_id in issuer_lines[_issuer_key(row)]
            if lines[other_id].first_trade_date <= line.last_trade_date
            and line.first_trade_date <= lines[other_id].last_trade_date
            and (category is None or other_id == row.price_security_id or line_category.get(other_id) == category)
        )

    resolved: list[BridgeRow] = []
    for row in rows:
        if row.owner_security_id is None:
            resolved.append(row)
            continue
        security_class, basis, category = kinds[id(row)]
        if category == "non_common":
            resolved.append(
                replace(
                    row,
                    security_class=security_class,
                    class_basis=basis,
                    dei_shares_eligible=False,
                    valuation_eligible=False,
                    withheld_reason=WITHHELD_NON_COMMON_LINE,
                    share_basis=SHARE_BASIS_WITHHELD,
                )
            )
            continue
        listed = common_counts.get(row.cik, 0) if row.cik else 0
        if category == "unverified":
            # A name without common-share evidence (ZONES, capital-trust or
            # agency securities): it is the issuer's equity line only when
            # nothing else is -- no common ticker, no other line.
            concurrent = concurrent_lines(row)
            class_lines = max(concurrent, listed + 1)
            single = listed == 0 and concurrent == 1
            reason = None if single else WITHHELD_UNVERIFIED_CLASS_LINE
            share_basis = SHARE_BASIS_SINGLE if single else SHARE_BASIS_WITHHELD
            valuation = single
        else:
            concurrent = concurrent_lines(row, "common")
            class_lines = max(concurrent, listed)
            single = class_lines <= 1
            # A8: a multi-class line is priced only by the issuer class sum,
            # never by DEI; strict mode has no class basis yet.
            reason = None if single else WITHHELD_MULTI_COMMON_CLASS
            if not single:
                share_basis = SHARE_BASIS_MULTI_CLASS
            else:
                share_basis = SHARE_BASIS_ADR if security_class == _ADR else SHARE_BASIS_SINGLE
            valuation = single or not strict
        if single and strict:
            reason = WITHHELD_STRICT_NO_DEI
        resolved.append(
            replace(
                row,
                concurrent_owner_lines=concurrent,
                issuer_class_lines=class_lines,
                security_class=security_class,
                class_basis=basis,
                dei_shares_eligible=single and not strict and share_basis == SHARE_BASIS_SINGLE,
                valuation_eligible=valuation,
                withheld_reason=reason,
                share_basis=share_basis,
            )
        )
    return tuple(sorted(resolved, key=lambda row: (row.price_security_id, row.valid_from or dt.date.min)))


def _with_sibling_segments(
    rows: Sequence[BridgeRow],
    line_symbols: Sequence[LineSymbol],
    directory: dict[str, str] | None = None,
) -> tuple[BridgeRow, ...]:
    """Split linked common rows where an unbridged sibling class line traded concurrently.

    A sibling is an *unlinked* price line that traded under a symbol that is a
    class sibling (:func:`are_class_siblings`) of a symbol this line traded
    under, while both traded (inclusive first/last bar dates). A sibling whose
    symbol the directory types as a non-common instrument is ignored. Each
    resulting segment carries ``sibling_lines`` (distinct siblings trading in
    it); a ``single``/``adr`` segment with siblings becomes ``multi_class`` with
    no DEI (the class sum cannot cover an unbridged class, so the panel
    withholds it). Only the withholding direction is ever inferred.
    """
    if not line_symbols:
        return tuple(rows)
    directory = directory or {}
    common_types = _common_equity_types() | {"unclassified", _CLASS_SHARE, _UNVERIFIED_COMMON}
    linked_lines = {row.price_security_id for row in rows if row.linked}
    by_family: dict[tuple[str, str], list[tuple[str, LineSymbol]]] = defaultdict(list)
    spans_by_line: dict[str, list[LineSymbol]] = defaultdict(list)
    for span in line_symbols:
        spans_by_line[span.price_security_id].append(span)
        if span.price_security_id in linked_lines:
            continue
        kind = _directory_type(directory, normalize_symbol(span.symbol) or "")
        if kind is not None and kind not in common_types:
            continue
        for family, root, designator in _class_families(span.symbol):
            by_family[(family, root)].append((designator, span))

    out: list[BridgeRow] = []
    for row in rows:
        if not row.linked or row.share_basis not in (SHARE_BASIS_SINGLE, SHARE_BASIS_ADR, SHARE_BASIS_MULTI_CLASS):
            out.append(row)
            continue
        start = row.valid_from or dt.date.min
        end = row.valid_to  # exclusive
        intervals: list[tuple[dt.date, dt.date, str]] = []
        for own in spans_by_line.get(row.price_security_id, ()):
            for family, root, designator in _class_families(own.symbol):
                for other_designator, other in by_family.get((family, root), ()):
                    if other_designator == designator or other.price_security_id == row.price_security_id:
                        continue
                    lo = max(own.first_trade_date, other.first_trade_date, start)
                    hi = min(own.last_trade_date, other.last_trade_date) + dt.timedelta(days=1)
                    if end is not None:
                        hi = min(hi, end)
                    if lo < hi:
                        intervals.append((lo, hi, other.price_security_id))
        if not intervals:
            out.append(row)
            continue
        bounds = sorted(
            {start, *(lo for lo, _hi, _id in intervals), *(hi for _lo, hi, _id in intervals)}
            | ({end} if end is not None else set())
        )
        edges: list[tuple[dt.date, dt.date | None]] = list(pairwise(bounds))
        if end is None:
            edges.append((bounds[-1], None))
        segments: list[BridgeRow] = []
        for lo, hi in edges:
            count = len({line for s, e, line in intervals if s <= lo < e})
            basis = row.share_basis if count == 0 else SHARE_BASIS_MULTI_CLASS
            segment = replace(
                row,
                valid_from=lo,
                valid_to=hi,
                sibling_lines=count,
                share_basis=basis,
                dei_shares_eligible=row.dei_shares_eligible and count == 0,
            )
            previous = segments[-1] if segments else None
            if previous is not None and (previous.sibling_lines, previous.share_basis) == (count, basis):
                segments[-1] = replace(previous, valid_to=segment.valid_to)
            else:
                segments.append(segment)
        out.extend(segments)
    return tuple(sorted(out, key=lambda row: (row.price_security_id, row.valid_from or dt.date.min)))


def _with_adr_ratios(rows: Sequence[BridgeRow], adr_ratios: dict[str, float]) -> tuple[BridgeRow, ...]:
    """Attach the directory-stated ordinary shares per ADS to ADR rows."""
    if not adr_ratios:
        return tuple(rows)
    return tuple(
        replace(row, adr_ratio=adr_ratios.get(normalize_symbol(row.share_class_symbol) or ""))
        if row.share_basis == SHARE_BASIS_ADR
        else row
        for row in rows
    )


def classify_reconstructed(
    lines: Sequence[PriceLine],
    tickers: Sequence[tuple[str, str, dt.datetime | None]],
    content: dict[str, frozenset[str]],
    directory: dict[str, str] | None = None,
    line_symbols: Sequence[LineSymbol] = (),
    adr_ratios: dict[str, float] | None = None,
) -> tuple[tuple[BridgeRow, ...], dict[str, tuple[str, ...]], int]:
    """Current-ticker backcast bridge. ``tickers`` rows are ``(cik, ticker, observed_at)``.

    ``directory`` (symbol -> A2 security type) feeds only the class guard;
    ``line_symbols`` (every symbol each line traded under) feeds the
    historical sibling-class guard; ``adr_ratios`` (normalized symbol ->
    ordinary shares per ADS) annotates ADR rows.
    Returns ``(rows, owner_members, ambiguous_content_ids)``.
    """
    index, _counts = _ticker_index(tickers)
    classes = classify_sec_tickers(tickers, directory, (line.last_symbol for line in lines))
    horizon = max((line.last_trade_date for line in lines), default=None)

    def symbol_keyed(line: PriceLine) -> bool:
        return line.symbol_keyed or _is_symbol_keyed(line.price_security_id)

    holders: dict[str, str] = {}
    matched: dict[str, list[PriceLine]] = defaultdict(list)
    for line in lines:
        key = normalize_symbol(line.last_symbol)
        if key is not None and key in index and not symbol_keyed(line):
            matched[key].append(line)
    for key, candidates in matched.items():
        best = min(candidates, key=lambda c: (-c.last_trade_date.toordinal(), -c.bar_rows, c.price_security_id))
        holders[key] = best.price_security_id
    # CIKs that already have a current-ticker holder line (for the rule-2 reuse check).
    held_ciks = {next(iter(index[key])) for key in holders if len(index[key]) == 1}

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
        if symbol_keyed(line):
            rows.append(BridgeRow(**unlinked, unlinked_reason=UNLINKED_SYMBOL_KEYED_LINE))
            continue
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
        if match is not None and horizon is not None and _is_stale(line, horizon) and match.group(1) in held_ciks:
            rows.append(BridgeRow(**unlinked, unlinked_reason=UNLINKED_SUPERSEDED_CIK_LINE))
        elif match is not None:
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
                    **{**linked, "identity_basis": IDENTITY_BASIS_SHARED_ID},
                    owner_security_id=price_id,
                    cik=next(iter(ciks)) if len(ciks) == 1 else None,
                    share_class_symbol=line.last_symbol,
                    link_method=LINK_SHARED_SECURITY_ID,
                )
            )
        else:
            rows.append(BridgeRow(**unlinked, unlinked_reason=UNLINKED_NO_CURRENT_TICKER))

    by_id = {line.price_security_id: line for line in lines}
    resolved = _with_class_guards(rows, by_id, classes, strict=False)
    resolved = _with_adr_ratios(_with_sibling_segments(resolved, line_symbols, directory), adr_ratios or {})
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
    tickers: Sequence[tuple[str, str, dt.datetime | None]] = (),
    directory: dict[str, str] | None = None,
    line_symbols: Sequence[LineSymbol] = (),
) -> tuple[tuple[BridgeRow, ...], dict[str, tuple[str, ...]], int, dict[str, int]]:
    """Dated-evidence bridge. Returns ``(rows, members, ambiguous_content_ids, rejected)``.

    Evidence is validated per row; overlapping intervals that assign one price
    line to *different* owners are rejected as ``conflicting_evidence``.
    Lines without accepted evidence are unlinked as ``no_dated_evidence``.
    ``tickers`` (the current SEC snapshot), ``directory`` and ``line_symbols``
    are used only by the conservative class guards, never to create a link.
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
    classes = classify_sec_tickers(tickers, directory, (line.last_symbol for line in lines))
    resolved = _with_sibling_segments(_with_class_guards(rows, by_id, classes, strict=True), line_symbols, directory)
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
               count(*) AS bar_rows,
               -- ticker-history lines without any positive vendor id were keyed
               -- by symbol upstream and may concatenate issuers.
               coalesce(bool_or(source = ?), false)
                   AND NOT coalesce(bool_or(try_cast(vendor_security_id AS BIGINT) > 0), false) AS symbol_keyed
        FROM equity_daily_bars
        WHERE close > 0 AND trade_date IS NOT NULL
        GROUP BY security_id
        ORDER BY security_id
        """,
        [TICKER_HISTORY_SOURCE_NAME],
    ).fetchall()
    return [PriceLine(str(r[0]), r[1], r[2], r[3], int(r[4]), bool(r[5])) for r in rows]


def _read_tickers(store: DuckDBStore) -> list[tuple[str, str, dt.datetime | None]]:
    if not _table_exists(store, "sec_company_tickers"):
        return []
    return [
        (str(cik), str(ticker), loaded_at)
        for cik, ticker, loaded_at in store.con.execute(
            "SELECT cik, ticker, source_loaded_at FROM sec_company_tickers WHERE cik IS NOT NULL AND ticker IS NOT NULL"
        ).fetchall()
    ]


def _read_directory(store: DuckDBStore) -> DirectorySnapshot:
    """Newest Nasdaq symbol-directory row per symbol, typed by A2's classifier.

    Keys are upper-case symbols with blank/``/`` class separators folded to
    ``.`` (the directory's class convention; ``-`` stays, it marks preferreds
    there). Only the distinct (name, etf, test) tuples are classified. A8: a
    ``common_unverified`` name that states a share class ("... Class B") is
    typed ``class_share``; ADR names that state an ADS ratio yield
    ``adr_ratios`` keyed by :func:`normalize_symbol`.
    """
    if not _table_exists(store, "nasdaq_symbol_directory"):
        return DirectorySnapshot({}, {})
    from .universe_us_listed import classify_security_type

    rows = store.con.execute(
        """
        SELECT upper(trim(symbol)) AS symbol,
               arg_max(security_name, (as_of_date, source_loaded_at)) AS security_name,
               arg_max(etf, (as_of_date, source_loaded_at)) AS etf,
               arg_max(test_issue, (as_of_date, source_loaded_at)) AS test_issue
        FROM nasdaq_symbol_directory
        WHERE nullif(trim(symbol), '') IS NOT NULL
        GROUP BY upper(trim(symbol))
        """
    ).fetchall()
    kinds: dict[tuple[object, object, object], str] = {}
    directory: dict[str, str] = {}
    adr_ratios: dict[str, float] = {}
    for symbol, name, etf, test_issue in rows:
        signature = (name, etf, test_issue)
        if signature not in kinds:
            kind = classify_security_type(name, etf=etf, test_issue=test_issue)
            upper = str(name or "").upper()
            if kind == _UNVERIFIED_COMMON and _CLASS_NAME.search(upper) and not _CLASS_NAME_EXCLUSIONS.search(upper):
                kind = _CLASS_SHARE
            kinds[signature] = kind
        key = re.sub(r"[\s/]+", ".", str(symbol))
        directory[key] = kinds[signature]
        if kinds[signature] == _ADR:
            ratio = parse_ads_ratio(name)
            symbol_key = normalize_symbol(key)
            if ratio is not None and symbol_key is not None:
                adr_ratios[symbol_key] = ratio
    return DirectorySnapshot(directory, adr_ratios)


def _read_line_symbols(store: DuckDBStore) -> list[LineSymbol]:
    """Every (price line, symbol) with its first/last bar date: one bounded aggregate."""
    rows = store.con.execute(
        """
        SELECT security_id, symbol, min(trade_date), max(trade_date)
        FROM equity_daily_bars
        WHERE close > 0 AND trade_date IS NOT NULL AND nullif(trim(symbol), '') IS NOT NULL
        GROUP BY security_id, symbol
        ORDER BY security_id, min(trade_date), symbol
        """
    ).fetchall()
    return [LineSymbol(str(r[0]), str(r[1]), r[2], r[3]) for r in rows]


#: A monetary unit that names an ISO currency (``USD``, ``CAD``, ``EUR/shares``).
_CURRENCY_UNIT_SQL = "regexp_full_match(upper(unit), '[A-Z]{3}(/SHARES)?')"


def _read_currency_events(
    store: DuckDBStore, content: dict[str, frozenset[str]]
) -> dict[str, tuple[CurrencyEvent, ...]]:
    """Monetary unit events of every content id whose CIK declares a non-USD/undeclared unit.

    Two bounded reads: the (few) content ids with a monetary or per-share fact
    whose unit is not USD, then every fact event of those ids and of every id
    sharing their CIK (an owner merges them). USD-only owners are never read.
    """
    if not _table_exists(store, "fundamental_standardized"):
        return {}
    monetary = f"(lower(unit_type) IN ('monetary', 'per_share') OR (unit_type IS NULL AND {_CURRENCY_UNIT_SQL}))"
    flagged = {
        str(row[0])
        for row in store.con.execute(
            f"""
            SELECT DISTINCT security_id FROM fundamental_standardized
            WHERE available_at IS NOT NULL AND security_id IS NOT NULL AND {monetary}
              AND (unit IS NULL OR upper(unit) NOT IN ('USD', 'USD/SHARES'))
            """
        ).fetchall()
    }
    if not flagged:
        return {}
    ciks = {cik for security_id in flagged for cik in content.get(security_id, frozenset())}
    wanted = sorted(flagged | {security_id for security_id, owned in content.items() if owned & ciks})
    events: dict[str, list[CurrencyEvent]] = defaultdict(list)
    for start in range(0, len(wanted), 1000):
        chunk = wanted[start : start + 1000]
        rows = store.con.execute(
            f"""
            SELECT security_id, period_end, available_at,
                   bool_or({_CURRENCY_UNIT_SQL} AND upper(left(unit, 3)) = 'USD'),
                   bool_or({_CURRENCY_UNIT_SQL} AND upper(left(unit, 3)) <> 'USD'),
                   bool_or(unit IS NULL OR NOT {_CURRENCY_UNIT_SQL})
            FROM fundamental_standardized
            WHERE available_at IS NOT NULL AND period_end IS NOT NULL AND {monetary}
              AND security_id IN ({", ".join(["?"] * len(chunk))})
            GROUP BY security_id, period_end, available_at
            """,
            chunk,
        ).fetchall()
        for security_id, period_end, available_at, usd, foreign, unknown in rows:
            events[str(security_id)].append(
                CurrencyEvent(str(security_id), period_end, available_at, bool(usd), bool(foreign), bool(unknown))
            )
    return {
        security_id: tuple(sorted(items, key=lambda e: (e.available_at, e.period_end)))
        for security_id, items in events.items()
    }


def _read_per_class_dei_ids(store: DuckDBStore) -> frozenset[str]:
    """Content ids with a DEI filing carrying several counts or classes (per-class DEI).

    Same partition as the panel's ``shares_filings`` guard; one aggregate.
    """
    rows = store.con.execute(
        """
        SELECT DISTINCT security_id FROM (
            SELECT security_id
            FROM shares_outstanding_history
            WHERE share_count_type = 'shares_outstanding' AND taxonomy = 'dei' AND available_at IS NOT NULL
            GROUP BY security_id, coalesce(accession_number, share_history_id), effective_date, available_at
            HAVING min(share_count) IS DISTINCT FROM max(share_count)
                OR min(coalesce(share_class, '')) <> max(coalesce(share_class, ''))
        )
        """
    ).fetchall()
    return frozenset(str(row[0]) for row in rows)


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

    Reads bounded aggregates only: one row per price line, one per (line,
    symbol), the current SEC ticker snapshot and symbol directory, one row per
    accounting-content id, and the monetary-unit events of the content ids
    that declare a non-USD unit. ``evidence`` is used only in strict mode;
    ``None`` means no qualified evidence source is wired yet, so the strict
    bridge links nothing.
    """
    if mode not in OWNER_MODES:
        raise ValueError(f"unknown owner mode {mode!r}; expected one of {OWNER_MODES}")
    lines = _read_lines(store)
    content = _read_content(store, item_codes=item_codes, metric_codes=metric_codes, derived_source=derived_source)
    tickers = _read_tickers(store)
    snapshot = _read_directory(store)
    directory = snapshot.types
    line_symbols = _read_line_symbols(store)
    observed = [loaded for _cik, _ticker, loaded in tickers if loaded is not None]
    by_id = {line.price_security_id: line for line in lines}
    if mode == OWNER_MODE_STRICT:
        supplied = tuple(evidence or ())
        rows, members, ambiguous, rejected = classify_strict(lines, supplied, content, tickers, directory, line_symbols)
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
        rows, members, ambiguous = classify_reconstructed(
            lines, tickers, content, directory, line_symbols, snapshot.adr_ratios
        )
        bridge = MarketOwnerBridge(
            mode=mode,
            lines=by_id,
            rows=rows,
            owner_members=members,
            ticker_snapshot_observed_at=max(observed) if observed else None,
            ticker_snapshot_oldest_observed_at=min(observed) if observed else None,
            ambiguous_content_ids=ambiguous,
        )
    return replace(
        bridge,
        content_ids=frozenset(content),
        per_class_dei_ids=_read_per_class_dei_ids(store),
        ticker_classes=classify_sec_tickers(tickers, directory, (line.last_symbol for line in lines)),
        currency_events=_read_currency_events(store, content),
    )


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
                row.valuation_eligible,
                row.identity_basis,
                _issuer_key(row),
                row.share_basis,
                max(1, row.issuer_class_lines),
                row.sibling_lines,
                row.adr_ratio,
                row.availability_basis,
                row.link_method,
            ]
        )
    return params


def member_value_params(members: Sequence[tuple[str, str]]) -> list[object]:
    return [value for pair in members for value in pair]


def currency_value_params(intervals: Sequence[tuple[str, dt.datetime, dt.datetime | None, str]]) -> list[object]:
    return [value for interval in intervals for value in interval]
