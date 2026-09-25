"""Reconstructed historical price-line -> issuer (CIK) links (RI1).

Why this exists
---------------
The market-panel owner bridge (:mod:`atx_db.market_owner_bridge`) links a price
line to its SEC issuer only through the *current* SEC ticker map. Delisted,
acquired and renamed lines have no current ticker, so they carry no
fundamentals in the research panel and no CIK for delisting-reason attribution:
the research cross-section is survivorship-biased and every significance test
inherits that bias. This module reconstructs, from retained dated evidence, a
labeled, scored, point-in-time-honest line -> CIK link for such lines.

It never produces verified identity. Every accepted link is written as a
``security_identity_evidence`` row (migration 0327, L2 §5.1) with
``fact_kind='issuer_link'``, ``evidence_status='reconstructed'`` and
``availability_status='modeled'`` -- research and coverage-measurement input
under RX1, never certification input (the strict bridge rejects it).

Evidence (all retained files; no network)
-----------------------------------------
Measured on the retained files (RI1 report): TickerHistory3 carries no CIK,
issuer name, venue or class (only dated symbols, prices, a vendor share count
and GICS); the Company Facts archive is numeric-only (``dei`` holds
``EntityCommonStockSharesOutstanding`` and ``EntityPublicFloat`` -- no
``TradingSymbol`` or ``EntityRegistrantName``); submissions ``tickers`` and
``exchanges`` and ``company_tickers.json`` list *current* tickers only (a
delisted issuer has none), and ``formerNames`` dates issuer names but no line
carries a name to match them against. There is therefore no dated ticker ->
CIK or name -> line evidence for a delisted line; ticker-name similarity is
kept *out* of the evidence so it can serve as the independent precision probe.
The discriminating dated evidence is numeric and filing-based:

``share_count_match`` (primary, contemporaneous)
    The vendor line's own share count (TickerHistory3 ``shares`` -- **in
    thousands**, measured: AAPL 15,204,137 = 15,204,137,000 on the 10-Q cover)
    equals, within vendor rounding (+-1 thousand), a share count the issuer
    reported in a periodic filing (Company Facts
    ``dei:EntityCommonStockSharesOutstanding`` cover count or
    ``us-gaap:CommonStockSharesOutstanding``), and the vendor run holding that
    count starts in ``[as_of - 7, as_of + 45]`` days (the vendor backfills a new
    count from the cover date; 94% of own-issuer facts, measured) or spans
    ``as_of`` (weight x0.5). One matched quarter is one item. Its weight is
    divided by the CIKs the same vendor run matches (collisions), by the
    line's count changes within +-26 days beyond two (an ETF re-counting daily
    sweeps through values), and cut for quantized counts (x0.1 whole millions,
    x0.25 other multiples of 10,000: fund creation units, rounded counts). Only the
    strongest quarter per **distinct matched count** carries weight: an
    unchanged count re-reported every quarter is one coincidence, not many.
    A pair's ``match_ratio`` (matched / reported issuer quarters inside the
    line's life) must also be consistent.
``terminal_filing_alignment`` (lifecycle, known only at the end)
    The line stopped trading and the issuer filed Form 25/25-NSE within
    ``[-7, +14]`` days (or a Form 15 deregistration within ``[-7, +90]``) of
    the last trade date.
``listing_filing_alignment`` (lifecycle, known at the start)
    The line started after the price file's first date and the issuer filed a
    Form 8-A12B/8-A12G, a final prospectus (424B1/424B4) or a successor
    registration (8-K12B/8-K12G3) within ``[-45, +5]`` days of the first trade.
``symbol_snapshot_match`` (corroboration only)
    A symbol the line traded under is a current SEC ticker of the issuer. Its
    clock is the snapshot's observation time, so it never makes a historical
    link available earlier.

Acceptance (conservative; precision over recall)
------------------------------------------------
A candidate (line, CIK) is accepted when its share weight is at least
``min_share_weight`` (two distinct unambiguous matched counts), its total
weight at least ``accept_weight`` (a third count, or a lifecycle alignment),
and its ``match_ratio`` at least ``min_match_ratio`` (or ``strong_min_ratio``
at ``strong_share_weight``). Symbol matches alone never link. Calibration (RI1
report): decoy runs with every issuer count scaled by 1.0137 and by 0.9871
(the null: same value distribution, no true matches) accept no link. That
bounds *coincidental* matches only -- scaling also removes structural pairs
(co-registrants, holding-company comparatives), which the conflict rule and
the name probe cover instead.

Conflicts (two CIKs for one line-window) are never resolved silently. A
*rival* is any other candidate of the line whose share weight reaches
``competitor_floor`` and whose match ratio reaches ``competitor_min_ratio``
or would be acceptable, with evidence overlapping the candidate's span by
more than ``overlap_tolerance_days`` (or contained in it). The only
resolution rule is dominance: a candidate is accepted over its rivals when
its weight is at least ``dominance_ratio`` times each rival's; the link is
tier ``low`` and names every rival (``competitors``), and every rival is kept
as a ``conflicting`` row (``conflict_dominated``). Without dominance every
side of the conflict is kept as a ``conflicting`` row
(``conflicting_candidates``) and the line stays unlinked.

Confidence tiers (:data:`TIERS`, strongest first)
-------------------------------------------------
The brief's ladder (dated ticker corroborated by name > ticker only > name
only) cannot be built from the retained sources (see above); its analogue on
the evidence that exists is:

``high``
    Uncontested share fingerprint corroborated by an independent evidence
    kind: a terminal or listing filing aligned with the line's own lifecycle,
    or a symbol the line traded under that is a current SEC ticker of the
    issuer.
``medium``
    Uncontested share fingerprint alone (at least ``accept_weight`` of
    distinct matched counts).
``low``
    Accepted over overlapping rival CIKs by the dominance rule.

The A5 current-ticker backcast (``current_ticker_unverified``) is the
ticker-only analogue and stays in the bridge; no name-only link is produced.

**The tier is point in time.** ``tier`` (payload and
:attr:`ReconstructedLink.tier`) is the *final* evaluation label, known only at
``tier_attained_at``. Each link also publishes ``tier_history``: the
``(tier, from)`` steps, where every step is judged only on the items known by
its clock -- a terminal Form 25/15 counts from its own clock, the 2026 SEC
ticker snapshot from its observation, and a rival demotes to ``low`` only once
its own known evidence contests the link. ``tier_at_available_at`` is the
first step. A point-in-time consumer filtering on tier at cutoff ``D`` must use
the step in force at ``D`` (:meth:`ReconstructedLink.tier_at`), never
``tier``: selecting ``tier='high'`` at a 2015 formation because a 2017 Form 25
made the link ``high`` would select on a future delisting.

One CIK per line-interval: accepted candidates of one line have disjoint
evidence spans (a holding-company reorganization or redomicile gives two
sequential CIKs). A segment covers its evidence span, extended by at most
``max_extension_days`` -- to the line's first/last trade when that is closer
or when the issuer's listing/terminal filing dates that end -- never into a
rival's evidence, and hands over to the successor at its successor/listing
filing or first evidence. So a successor issuer without enough evidence of its
own never silently inherits its predecessor's link for years.

Point-in-time honesty
---------------------
Each evidence item has a clock: filing ``filed`` date + 46h (the fundamentals
``sec_filed_date_plus_46h_v1`` convention) and never before the vendor bar's
22:00 cutoff; a listing item at its form's filing clock and never before the
first bar's cutoff; a terminal item at the delisting filing's own acceptance
clock when an exact (offset-bearing, not EDGAR-midnight) ``acceptanceDateTime``
is retained, else ``filed`` + 46h (labelled ``filing_clock_basis``), and never
before the cutoff of the session *after* the last trade -- knowing that a bar
was the last one takes the next session, so a terminal alignment is never
usable at the last bar; the snapshot at its observation. A link's ``available_at`` is the earliest clock at which the
items known by then already pass the acceptance rule -- so a consumer that
joins on ``available_at <= cutoff`` (the bridge's rule) never links a bar with
evidence from its future. ``value_json.evidence_complete_at`` records the clock
of the last item. Conflicts, the match-ratio filter and segment bounds are
judged on all retained evidence: they can only remove or shorten a link, never
make one available earlier (documented residual: a later rival can remove a
link that would have been accepted at the time). The archives themselves are
2026 retrievals whose historical vintage is unverified, hence
``availability_status='modeled'``.

Bridge hook (follow-up after A8; not wired here)
------------------------------------------------
In reconstructed mode, after the current-ticker pass, a line left unlinked
(``no_current_ticker``, ``symbol_held_by_other_line``, ``superseded_cik_line``)
takes the accepted segments of :meth:`ReconstructionResult.owner_links` as
bridge rows (``identity_basis='reconstructed_identity_unverified'``,
``availability_basis='modeled'``, ``link_method=METHOD``, ``available_at`` as
given). Strict mode must keep rejecting them. The same segments also guard the
current-ticker pass: a *stale* current-ticker holder whose accepted segment
names another CIK is a reused ticker (measured: 10 of 104 reconstructable
stale holders, e.g. the 2012-2014 Compuware line linked to today's CPWR
issuer) and should take the reconstructed segment instead.

Resources: stage and match in a private scratch DuckDB (``memory_limit``
<= 384MB, 2 threads; never the warehouse) and reconstruct with
:func:`reconstruct_in_batches`; write evidence rows in bounded transactions
(DuckDB transaction-local storage does not spill). Measured on the retained
files (``scripts/identity_reconstruction_measure.py``, 192MB, batch 500): vendor
staging 75 s, Company Facts staging 743 s (1,000,917 counts), reconstruction of
25,760 lines 46 s; process peak 375 MB private bytes (with
``OPENBLAS_NUM_THREADS=1`` -- numpy's default OpenBLAS alone commits ~515 MB
on a 16-thread host).
"""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import math
import zipfile
from collections import defaultdict
from collections.abc import Iterable, Iterator, Mapping, Sequence
from dataclasses import asdict, dataclass, field, replace
from pathlib import Path

import duckdb

from ._fundamental_clock import FUNDAMENTAL_CLOCK_POLICY
from .historical_identity import EVIDENCE_COLUMNS, normalize_cik
from .market_owner_bridge import OwnerLinkEvidence, normalize_symbol
from .warehouse import cik_security_id

__all__ = [
    "AVAILABILITY_STATUS",
    "CONFLICT_REASONS",
    "EVIDENCE_STATUS",
    "EV_LISTING",
    "EV_SHARES",
    "EV_SYMBOL",
    "EV_TERMINAL",
    "IDENTITY_BASIS",
    "LISTING_FORMS",
    "METHOD",
    "REJECT_BELOW_THRESHOLD",
    "REJECT_CONFLICTING",
    "REJECT_CONFLICT_DOMINATED",
    "REJECT_EMPTY_SEGMENT",
    "REJECT_INCONSISTENT",
    "SHARE_CONCEPTS",
    "SOURCE",
    "TERMINAL_FORMS",
    "TIERS",
    "TIER_HIGH",
    "TIER_LOW",
    "TIER_MEDIUM",
    "VENDOR_NAMESPACE",
    "VENDOR_SHARES_UNIT",
    "Candidate",
    "EvidenceItem",
    "IssuerFiling",
    "IssuerShareFact",
    "ReconstructedLink",
    "ReconstructionParams",
    "ReconstructionResult",
    "RejectedCandidate",
    "VendorLine",
    "VendorShareRun",
    "iter_companyfacts_share_facts",
    "match_share_counts",
    "read_lifecycle_filings",
    "read_sec_ticker_snapshot",
    "read_vendor_lines",
    "reconstruct_in_batches",
    "reconstruct_issuer_links",
    "stage_share_facts",
    "stage_share_runs",
    "stage_vendor_ticker_history",
]

SOURCE = "atx-db identity reconstruction v1"
METHOD = "ri1_share_fingerprint_v1"
EVIDENCE_STATUS = "reconstructed"
AVAILABILITY_STATUS = "modeled"
IDENTITY_BASIS = "reconstructed_identity_unverified"
VENDOR_NAMESPACE = "tickerhistory3.securityID"
#: TickerHistory3 ``shares`` is reported in thousands of shares.
VENDOR_SHARES_UNIT = 1000

EV_SHARES = "share_count_match"
EV_TERMINAL = "terminal_filing_alignment"
EV_LISTING = "listing_filing_alignment"
EV_SYMBOL = "symbol_snapshot_match"
#: Evidence kinds independent of the share fingerprint (they only corroborate it).
_CORROBORATING = (EV_TERMINAL, EV_LISTING, EV_SYMBOL)

TIER_HIGH = "high"
TIER_MEDIUM = "medium"
TIER_LOW = "low"
#: Confidence tiers, strongest first (see the module docstring).
TIERS = (TIER_HIGH, TIER_MEDIUM, TIER_LOW)

REJECT_BELOW_THRESHOLD = "below_threshold"
REJECT_INCONSISTENT = "inconsistent_share_history"
REJECT_CONFLICTING = "conflicting_candidates"
REJECT_CONFLICT_DOMINATED = "conflict_dominated"
REJECT_EMPTY_SEGMENT = "empty_segment"
#: Rejections that are one side of a two-CIK conflict (``evidence_status='conflicting'``).
CONFLICT_REASONS = frozenset({REJECT_CONFLICTING, REJECT_CONFLICT_DOMINATED})

#: Company Facts share-count concepts matched against the vendor count.
SHARE_CONCEPTS = (
    ("dei", "EntityCommonStockSharesOutstanding"),
    ("us-gaap", "CommonStockSharesOutstanding"),
)
#: Exchange delisting notices (tight window) and deregistrations (loose window).
DELISTING_NOTICE_FORMS = ("25", "25-NSE")
DEREGISTRATION_FORMS = ("15-12B", "15-12G", "15-15D", "15F-12B", "15F-12G", "15F-15D")
TERMINAL_FORMS = DELISTING_NOTICE_FORMS + DEREGISTRATION_FORMS
#: Listing registrations, final IPO prospectuses and successor-issuer registrations.
SUCCESSOR_FORMS = ("8-K12B", "8-K12G3")
LISTING_FORMS = ("8-A12B", "8-A12G", "424B1", "424B4", *SUCCESSOR_FORMS)
LIFECYCLE_FORMS = TERMINAL_FORMS + LISTING_FORMS

_BAR_CUTOFF = dt.timedelta(hours=22)
_FILED_AVAILABILITY = dt.timedelta(hours=46)  # FUNDAMENTAL_CLOCK_POLICY
_ONE_DAY = dt.timedelta(days=1)
#: Items kept in value_json per link (the decisive ones first, then latest).
_MAX_JSON_ITEMS = 16


@dataclass(frozen=True)
class ReconstructionParams:
    """Matching and acceptance parameters (their digest is the revision id)."""

    #: Facts below this many shares (subsidiary 100/1,000-share counts, shells) never match.
    min_fact_shares: int = 100_000
    #: Vendor rounding: |vendor thousands - fact / 1000| must be at most this.
    share_abs_tolerance: float = 1.0
    #: A vendor run must start within [as_of - lead, as_of + lag] days ...
    run_start_lead_days: int = 7
    run_start_lag_days: int = 45
    #: ... or span the as_of date (count unchanged since an earlier quarter): weight x this.
    covering_match_weight: float = 0.5
    #: Trials: a run's weight is x min(1, allowance / count changes of the line within +-window days),
    #: so a line that re-counts daily (ETF creations/redemptions) cannot match by sweeping values.
    trial_window_days: int = 26
    trial_allowance: float = 2.0
    #: Quantized counts carry little identity: whole millions (shells, placeholders, SPAC units)
    #: weight x ``round_value_weight``; other multiples of 10,000 (fund creation units of
    #: 25k/50k, rounded balance-sheet counts) x ``quantized_value_weight``. Measured: two
    #: commodity/leveraged ETF lines otherwise matched each other's creation-unit counts.
    round_value_weight: float = 0.1
    quantized_value_weight: float = 0.25
    terminal_notice_window: tuple[int, int] = (-7, 14)
    terminal_deregistration_window: tuple[int, int] = (-7, 90)
    listing_window: tuple[int, int] = (-45, 5)
    #: A line whose last trade is within this many days of the price horizon is still trading.
    active_line_days: int = 30
    #: The price file's first date: a line starting within this many days of it has no listing evidence.
    price_start_grace_days: int = 5
    lifecycle_weight: float = 1.0
    symbol_weight: float = 0.5
    #: Two distinct matched counts at least, and three in total with lifecycle/snapshot corroboration.
    min_share_weight: float = 2.0
    accept_weight: float = 3.0
    #: A segment extends at most this many days beyond its own evidence (a successor issuer
    #: without enough evidence of its own must not inherit its predecessor's link), except up
    #: to the line's first/last trade when a listing/terminal filing dates that end.
    max_extension_days: int = 200
    #: Matched / reported issuer quarters inside the line's life needed to accept ...
    min_match_ratio: float = 0.5
    #: ... or this lower ratio when the share weight alone is this strong (vendor counts that
    #: follow cover counts the archive only has as balance-sheet counts, or partial vendor updates).
    strong_share_weight: float = 4.0
    strong_min_ratio: float = 0.2
    #: A rival (line, CIK) pair competes when its share weight and match ratio reach these.
    competitor_floor: float = 1.0
    competitor_min_ratio: float = 0.25
    dominance_ratio: float = 3.0
    #: Evidence spans overlapping by at most this many days are sequential, not competing
    #: (a span contained in another always competes).
    overlap_tolerance_days: int = 45

    def digest(self) -> str:
        payload = json.dumps(asdict(self), sort_keys=True, default=str)
        return hashlib.sha256(payload.encode()).hexdigest()[:16]


@dataclass(frozen=True)
class VendorLine:
    """One vendor price line (a positive TickerHistory3 ``securityID``)."""

    vendor_id: int
    price_security_id: str
    first_trade_date: dt.date
    last_trade_date: dt.date
    bar_rows: int = 0
    last_symbol: str | None = None
    #: Every (symbol, first date, last date) the line traded under.
    symbols: tuple[tuple[str, dt.date, dt.date], ...] = ()
    #: The CIK the current-ticker bridge links this line to (holdout comparison), if any.
    current_cik: str | None = None


@dataclass(frozen=True)
class VendorShareRun:
    """A maximal run of consecutive bars carrying one vendor share count (thousands)."""

    vendor_id: int
    shares: int
    first_date: dt.date
    last_date: dt.date
    rows: int = 1


@dataclass(frozen=True)
class IssuerShareFact:
    """One reported share count (Company Facts, ``units=shares``)."""

    cik: str
    concept: str
    as_of: dt.date
    value: float
    accession: str | None
    form: str | None
    filed: dt.date


@dataclass(frozen=True)
class IssuerFiling:
    """One lifecycle filing of an issuer (submissions filing metadata)."""

    cik: str
    form: str
    filing_date: dt.date
    accession: str | None = None
    #: Exact EDGAR acceptance clock (naive UTC) when retained; None for date-only/legacy stamps.
    accepted_at: dt.datetime | None = None


@dataclass(frozen=True)
class EvidenceItem:
    """One dated piece of evidence for a (line, CIK) candidate."""

    kind: str
    #: When the item was knowable (PIT clock).
    known_at: dt.datetime
    #: Where the item sits on the line's timeline (None for the snapshot).
    line_date: dt.date | None
    weight: float
    detail: Mapping[str, object] = field(default_factory=dict)


@dataclass(frozen=True)
class Candidate:
    vendor_id: int
    cik: str
    items: tuple[EvidenceItem, ...]
    #: The issuer's reported share-count quarters inside the line's share-data life.
    reported_quarters: int = 0
    #: Matched / reported quarters: a coincidental pair explains few of the issuer's counts.
    match_ratio: float = 0.0

    @property
    def share_weight(self) -> float:
        return _share_weight(self.items)

    @property
    def weight(self) -> float:
        # Lifecycle and symbol items only corroborate a share fingerprint.
        if self.share_weight <= 0:
            return 0.0
        return math.fsum(item.weight for item in self.items)

    @property
    def span(self) -> tuple[dt.date, dt.date] | None:
        return _share_span(self.items)

    @property
    def share_quarters(self) -> int:
        return sum(1 for item in self.items if item.kind == EV_SHARES)


@dataclass(frozen=True)
class ReconstructedLink:
    """One accepted line-interval -> CIK link (``valid_to`` exclusive)."""

    vendor_id: int
    price_security_id: str
    cik: str
    valid_from: dt.date
    valid_to: dt.date
    available_at: dt.datetime
    evidence_complete_at: dt.datetime
    weight: float
    share_weight: float
    tier: str
    symbol: str | None
    items: tuple[EvidenceItem, ...]
    competitors: tuple[tuple[str, float], ...] = ()
    current_cik: str | None = None
    #: Point-in-time ``(tier, from)`` steps; the first starts at ``available_at``, the last is ``tier``.
    tier_history: tuple[tuple[str, dt.datetime], ...] = ()

    @property
    def tier_attained_at(self) -> dt.datetime:
        """When the final ``tier`` was reached (evaluation label before this clock is look-ahead)."""
        return self.tier_history[-1][1] if self.tier_history else self.available_at

    def tier_at(self, cutoff: dt.datetime) -> str | None:
        """The tier in force at ``cutoff`` (None before the link is available)."""
        in_force = [tier for tier, since in self.tier_history if since <= cutoff]
        return in_force[-1] if in_force else None

    @property
    def owner_security_id(self) -> str:
        return cik_security_id(self.cik)

    @property
    def agrees_with_current(self) -> bool | None:
        return None if self.current_cik is None else self.current_cik == self.cik


@dataclass(frozen=True)
class RejectedCandidate:
    """A best-candidate that was not accepted (kept for audit, never linked)."""

    vendor_id: int
    price_security_id: str
    cik: str
    reason: str
    weight: float
    share_weight: float
    span: tuple[dt.date, dt.date] | None
    competitors: tuple[tuple[str, float], ...] = ()
    #: The candidate's own threshold outcome when it was relabelled as one side of a conflict.
    own_reason: str | None = None


@dataclass(frozen=True)
class ReconstructionResult:
    params: ReconstructionParams
    links: tuple[ReconstructedLink, ...]
    rejected: tuple[RejectedCandidate, ...]
    lines_considered: int

    def best_rejected(self) -> list[RejectedCandidate]:
        """The strongest rejected candidate of every line that has no accepted link."""
        linked = {link.vendor_id for link in self.links}
        best: dict[int, RejectedCandidate] = {}
        for item in self.rejected:
            if item.vendor_id in linked:
                continue
            current = best.get(item.vendor_id)
            if current is None or (item.weight, item.cik) > (current.weight, current.cik):
                best[item.vendor_id] = item
        return [best[vendor_id] for vendor_id in sorted(best)]

    def kept_rejections(self) -> list[RejectedCandidate]:
        """Rejected candidates kept as evidence rows.

        Every side of every conflict -- each CIK of an unresolved conflict and
        each rival a dominant link was preferred over -- plus the strongest
        other rejected candidate of every line with neither a link nor a
        conflict row. Weak non-conflicting candidates of linked lines are dropped.
        """
        kept = [item for item in self.rejected if item.reason in CONFLICT_REASONS]
        covered = {item.vendor_id for item in kept}
        kept += [item for item in self.best_rejected() if item.vendor_id not in covered]
        return sorted(kept, key=lambda item: (item.vendor_id, item.cik, item.reason))

    def summary(self) -> dict[str, object]:
        linked = {link.vendor_id for link in self.links}
        holdout = [link for link in self.links if link.current_cik is not None]
        reasons: dict[str, int] = defaultdict(int)
        for item in self.best_rejected():
            reasons[item.reason] += 1
        tiers: dict[str, int] = defaultdict(int)
        for link in self.links:
            tiers[link.tier] += 1
        return {
            "method": METHOD,
            "params_digest": self.params.digest(),
            "lines_considered": self.lines_considered,
            "lines_linked": len(linked),
            "link_segments": len(self.links),
            "multi_segment_lines": len(self.links) - len(linked),
            "links_by_tier": dict(sorted(tiers.items())),
            "unlinked_best_candidate_reasons": dict(sorted(reasons.items())),
            "unresolved_conflict_lines": len(
                {item.vendor_id for item in self.rejected if item.reason == REJECT_CONFLICTING} - linked
            ),
            "dominated_rivals": sum(1 for item in self.rejected if item.reason == REJECT_CONFLICT_DOMINATED),
            "holdout_links": len(holdout),
            "holdout_agree": sum(1 for link in holdout if link.agrees_with_current),
        }

    def owner_links(self, *, artifact_sha256: str = "", source_locator: str = "") -> list[OwnerLinkEvidence]:
        """Accepted segments shaped as bridge evidence (reconstructed/modeled; strict mode rejects them)."""
        return [
            OwnerLinkEvidence(
                price_security_id=link.price_security_id,
                cik=link.cik,
                valid_from=link.valid_from,
                valid_to=link.valid_to,
                available_at=link.available_at,
                evidence_status=EVIDENCE_STATUS,
                availability_status=AVAILABILITY_STATUS,
                artifact_sha256=artifact_sha256,
                source_locator=source_locator or _locator(link),
                owner_security_id=link.owner_security_id,
                share_class_symbol=link.symbol,
                evidence_id=_evidence_id(link.vendor_id, link.cik, link.valid_from, self.params),
            )
            for link in self.links
        ]

    def evidence_rows(
        self,
        *,
        observed_at: dt.datetime,
        source_loaded_at: dt.datetime | None = None,
        run_id: str | None = None,
        artifact_sha256: str | None = None,
        artifacts: Mapping[str, str] | None = None,
        include_rejected: bool = True,
    ) -> list[dict[str, object]]:
        """``security_identity_evidence`` rows (column order of :data:`EVIDENCE_COLUMNS`).

        Accepted segments are ``reconstructed``/``modeled`` with their tier in
        ``value_json``; with ``include_rejected`` the :meth:`kept_rejections`
        follow as ``conflicting`` (one side of a two-CIK conflict) or
        ``unknown`` (below threshold, inconsistent) rows with their
        ``rejection_reason`` and no ``available_at`` -- never linkable.
        ``artifact_sha256`` is the Company Facts archive digest (the source of
        every share fact); ``artifacts`` names every input digest in the payload.
        """
        revision = f"{METHOD}:{self.params.digest()}"
        loaded = source_loaded_at or observed_at
        rows: list[dict[str, object]] = []
        for link in self.links:
            payload = {
                "cik": link.cik,
                "owner_security_id": link.owner_security_id,
                "share_class_symbol": link.symbol,
                "method": METHOD,
                "identity_basis": IDENTITY_BASIS,
                # ``tier`` is the final evaluation label; PIT consumers use ``tier_history``.
                "tier": link.tier,
                "tier_rank": TIERS.index(link.tier),
                "tier_attained_at": link.tier_attained_at.isoformat(),
                "tier_at_available_at": link.tier_history[0][0] if link.tier_history else link.tier,
                "tier_history": [[tier, since.isoformat()] for tier, since in link.tier_history],
                "corroborated_by": sorted({item.kind for item in link.items} & set(_CORROBORATING)),
                "evidence_weight": round(link.weight, 4),
                "share_weight": round(link.share_weight, 4),
                "share_quarters": sum(1 for item in link.items if item.kind == EV_SHARES),
                "evidence_complete_at": link.evidence_complete_at.isoformat(),
                # Sensitivity clocks (never the row's available_at): the first share item's clock and
                # the RX1 current-ticker convention (first bar of the segment, 22:00).
                "first_evidence_at": min(
                    item.known_at for item in link.items if item.kind == EV_SHARES
                ).isoformat(),
                "rx1_first_bar_modeled_at": _at(link.valid_from, _BAR_CUTOFF).isoformat(),
                "current_cik": link.current_cik,
                "competitors": [[cik, round(weight, 4)] for cik, weight in link.competitors],
                "vendor_shares_unit": "thousands",
                "availability_policy": f"pit_cumulative_evidence; filed+46h ({FUNDAMENTAL_CLOCK_POLICY})",
                "items": _json_items(link.items),
                "artifacts": dict(artifacts or {}),
            }
            rows.append(
                _row(
                    evidence_id=_evidence_id(link.vendor_id, link.cik, link.valid_from, self.params),
                    link_vendor=link.vendor_id,
                    security_id=link.price_security_id,
                    cik=link.cik,
                    symbol=link.symbol,
                    payload=payload,
                    valid_from=link.valid_from,
                    valid_to=link.valid_to,
                    locator=_locator(link),
                    artifact_sha256=artifact_sha256,
                    revision=revision,
                    observed_at=observed_at,
                    available_at=link.available_at,
                    status=EVIDENCE_STATUS,
                    rejection=None,
                    run_id=run_id,
                    loaded=loaded,
                )
            )
        if include_rejected:
            for item in self.kept_rejections():
                span = item.span
                payload = {
                    "cik": item.cik,
                    "method": METHOD,
                    "identity_basis": IDENTITY_BASIS,
                    "rejection_reason": item.reason,
                    "own_outcome": item.own_reason,
                    "evidence_weight": round(item.weight, 4),
                    "share_weight": round(item.share_weight, 4),
                    "competitors": [[cik, round(weight, 4)] for cik, weight in item.competitors],
                    "vendor_shares_unit": "thousands",
                    "artifacts": dict(artifacts or {}),
                }
                rows.append(
                    _row(
                        evidence_id=_evidence_id(item.vendor_id, item.cik, span[0] if span else None, self.params)
                        + "-R",
                        link_vendor=item.vendor_id,
                        security_id=item.price_security_id,
                        cik=item.cik,
                        symbol=None,
                        payload=payload,
                        valid_from=span[0] if span else None,
                        valid_to=span[1] + _ONE_DAY if span else None,
                        locator=f"TickerHistory3.parquet:securityID={item.vendor_id}",
                        artifact_sha256=artifact_sha256,
                        revision=revision,
                        observed_at=observed_at,
                        available_at=None,
                        status="conflicting" if item.reason in CONFLICT_REASONS else "unknown",
                        rejection=item.reason,
                        run_id=run_id,
                        loaded=loaded,
                    )
                )
        return rows


# ---------------------------------------------------------------------------------------------
# Row helpers


def _evidence_id(vendor_id: int, cik: str, valid_from: dt.date | None, params: ReconstructionParams) -> str:
    key = f"{METHOD}|{params.digest()}|{vendor_id}|{cik}|{valid_from}"
    return "RI1-" + hashlib.sha256(key.encode()).hexdigest()[:24]


def _locator(link: ReconstructedLink) -> str:
    accessions = [
        str(item.detail.get("accession"))
        for item in link.items
        if item.kind == EV_SHARES and item.detail.get("accession")
    ]
    first = accessions[0] if accessions else "?"
    return (
        f"companyfacts.zip:CIK{link.cik}.json#shares@{first}(+{max(len(accessions) - 1, 0)});"
        f"TickerHistory3.parquet:securityID={link.vendor_id}"
    )


def _json_items(items: Sequence[EvidenceItem]) -> list[dict[str, object]]:
    ordered = sorted(items, key=lambda item: item.known_at)
    kept = ordered if len(ordered) <= _MAX_JSON_ITEMS else ordered[:4] + ordered[-(_MAX_JSON_ITEMS - 4) :]
    return [
        {
            "kind": item.kind,
            "known_at": item.known_at.isoformat(),
            "line_date": item.line_date.isoformat() if item.line_date else None,
            "weight": round(item.weight, 4),
            **{key: (value.isoformat() if isinstance(value, dt.date) else value) for key, value in item.detail.items()},
        }
        for item in kept
    ]


def _row(
    *,
    evidence_id: str,
    link_vendor: int,
    security_id: str,
    cik: str,
    symbol: str | None,
    payload: Mapping[str, object],
    valid_from: dt.date | None,
    valid_to: dt.date | None,
    locator: str,
    artifact_sha256: str | None,
    revision: str,
    observed_at: dt.datetime,
    available_at: dt.datetime | None,
    status: str,
    rejection: str | None,
    run_id: str | None,
    loaded: dt.datetime,
) -> dict[str, object]:
    row: dict[str, object] = {
        "evidence_id": evidence_id,
        "fact_kind": "issuer_link",
        "source": SOURCE,
        "native_key_namespace": VENDOR_NAMESPACE,
        "native_key": str(link_vendor),
        "security_id": security_id,
        "cik": cik,
        "symbol": symbol,
        "value_json": json.dumps(payload, sort_keys=True, default=str),
        "valid_from": valid_from,
        "valid_to": valid_to,
        "source_locator": locator,
        "artifact_sha256": artifact_sha256,
        "source_revision_id": revision,
        "source_time_text": None,
        "source_published_at": None,
        "observed_at": observed_at,
        "available_at": available_at,
        "evidence_status": status,
        "availability_status": AVAILABILITY_STATUS,
        "method": METHOD,
        "rejection_reason": rejection,
        "is_latest_revision": True,
        "run_id": run_id,
        "source_loaded_at": loaded,
    }
    assert list(row) == [name for name, _ in EVIDENCE_COLUMNS]
    return row


# ---------------------------------------------------------------------------------------------
# Staging (private in-memory DuckDB; never the warehouse)

_SHARE_RUNS_DDL = (
    "CREATE TABLE IF NOT EXISTS ri_share_runs (vendor_id BIGINT, shares BIGINT, first_date DATE, "
    "last_date DATE, n BIGINT)"
)
_SHARE_FACTS_DDL = (
    "CREATE TABLE IF NOT EXISTS ri_share_facts (cik VARCHAR, concept VARCHAR, as_of DATE, value DOUBLE, "
    "accession VARCHAR, form VARCHAR, filed DATE)"
)


def stage_share_runs(con: duckdb.DuckDBPyConnection, runs: Iterable[VendorShareRun]) -> None:
    """Stage vendor share runs (fixtures, or rows read elsewhere)."""
    con.execute(_SHARE_RUNS_DDL)
    rows = [(r.vendor_id, r.shares, r.first_date, r.last_date, r.rows) for r in runs]
    if rows:
        con.executemany("INSERT INTO ri_share_runs VALUES (?, ?, ?, ?, ?)", rows)


_SHARE_FACT_FIELDS = ("cik", "concept", "as_of", "value", "accession", "form", "filed")


def stage_share_facts(con: duckdb.DuckDBPyConnection, facts: Iterable[IssuerShareFact], batch: int = 50_000) -> int:
    """Stage issuer share facts in bounded Arrow batches (one INSERT per batch); returns the row count."""
    import pyarrow as pa  # type: ignore[import-untyped]

    con.execute(_SHARE_FACTS_DDL)
    schema = pa.schema(
        [
            ("cik", pa.string()),
            ("concept", pa.string()),
            ("as_of", pa.date32()),
            ("value", pa.float64()),
            ("accession", pa.string()),
            ("form", pa.string()),
            ("filed", pa.date32()),
        ]
    )
    total = 0
    pending: list[IssuerShareFact] = []

    def flush() -> None:
        columns = {name: [getattr(fact, name) for fact in pending] for name in _SHARE_FACT_FIELDS}
        con.register("ri_share_facts_batch", pa.table(columns, schema=schema))
        try:
            con.execute("INSERT INTO ri_share_facts SELECT * FROM ri_share_facts_batch")
        finally:
            con.unregister("ri_share_facts_batch")

    for fact in facts:
        pending.append(fact)
        if len(pending) >= batch:
            flush()
            total += len(pending)
            pending = []
    if pending:
        flush()
        total += len(pending)
    return total


def stage_vendor_ticker_history(con: duckdb.DuckDBPyConnection, parquet_path: str | Path, *, chunks: int = 16) -> None:
    """Stage vendor lines, symbol runs and share-count runs from TickerHistory3.parquet.

    Bounded: per-line and per-(line, symbol) GROUP BYs, and share runs computed
    as gaps-and-islands in ``chunks`` vendor-id slices so no window ever holds
    the whole 32M-row file. Only positive vendor ids (symbol-keyed lines may
    concatenate issuers and are out of scope).
    """
    source = str(parquet_path)
    con.execute(
        """
        CREATE TABLE ri_vendor_lines AS
        SELECT securityID AS vendor_id,
               min(tradingDate) AS first_trade_date, max(tradingDate) AS last_trade_date, count(*) AS bar_rows,
               arg_max(upper(trim(coalesce(nullif(ticker_tk, ''), nullif(todayTicker, '')))), tradingDate)
                   AS last_symbol,
               arg_max(upper(trim(coalesce(nullif(todayTicker, ''), nullif(ticker_tk, '')))), tradingDate)
                   AS today_ticker
        FROM read_parquet(?)
        WHERE securityID > 0 AND close > 0 AND tradingDate IS NOT NULL
          AND coalesce(nullif(trim(ticker_tk), ''), nullif(trim(todayTicker), '')) IS NOT NULL
        GROUP BY securityID
        """,
        [source],
    )
    con.execute(
        """
        CREATE TABLE ri_vendor_symbols AS
        SELECT securityID AS vendor_id, upper(trim(ticker_tk)) AS symbol,
               min(tradingDate) AS first_date, max(tradingDate) AS last_date
        FROM read_parquet(?)
        WHERE securityID > 0 AND close > 0 AND nullif(trim(ticker_tk), '') IS NOT NULL
        GROUP BY 1, 2
        """,
        [source],
    )
    con.execute(_SHARE_RUNS_DDL)
    for part in range(chunks):
        con.execute(
            """
            INSERT INTO ri_share_runs
            WITH s AS (
                SELECT securityID AS vendor_id, tradingDate AS d, shares
                FROM read_parquet(?)
                WHERE securityID > 0 AND shares > 0 AND tradingDate IS NOT NULL AND securityID % ? = ?
            ), marked AS (
                SELECT *, CASE WHEN lag(shares) OVER (PARTITION BY vendor_id ORDER BY d) IS DISTINCT FROM shares
                               THEN 1 ELSE 0 END AS brk
                FROM s
            ), grouped AS (
                SELECT *, sum(brk) OVER (PARTITION BY vendor_id ORDER BY d ROWS UNBOUNDED PRECEDING) AS run_no
                FROM marked
            )
            SELECT vendor_id, shares, min(d), max(d), count(*) FROM grouped GROUP BY vendor_id, run_no, shares
            """,
            [source, chunks, part],
        )


def read_vendor_lines(
    con: duckdb.DuckDBPyConnection,
    line_ids: Mapping[int, str] | None = None,
    current_ciks: Mapping[int, str] | None = None,
) -> list[VendorLine]:
    """Vendor lines from the staged tables. ``line_ids`` maps vendor id -> warehouse price line id

    (``equity_daily_bars.security_id``); the default is the loader's id for a line
    no current ticker maps, ``TBLTICKERHISTORY-<vendor id>``.
    """
    symbols: dict[int, list[tuple[str, dt.date, dt.date]]] = defaultdict(list)
    for vendor_id, symbol, first, last in con.execute(
        "SELECT vendor_id, symbol, first_date, last_date FROM ri_vendor_symbols ORDER BY vendor_id, first_date, symbol"
    ).fetchall():
        symbols[int(vendor_id)].append((str(symbol), first, last))
    lines = []
    for vendor_id, first, last, rows, last_symbol, _today in con.execute(
        "SELECT * FROM ri_vendor_lines ORDER BY vendor_id"
    ).fetchall():
        vid = int(vendor_id)
        lines.append(
            VendorLine(
                vendor_id=vid,
                price_security_id=(line_ids or {}).get(vid, f"TBLTICKERHISTORY-{vid}"),
                first_trade_date=first,
                last_trade_date=last,
                bar_rows=int(rows),
                last_symbol=last_symbol,
                symbols=tuple(symbols.get(vid, ())),
                current_cik=(current_ciks or {}).get(vid),
            )
        )
    return lines


# ---------------------------------------------------------------------------------------------
# Retained-file readers


def _date(value: object) -> dt.date | None:
    if value is None or value == "":
        return None
    if isinstance(value, dt.date):
        return value
    try:
        return dt.date.fromisoformat(str(value)[:10])
    except ValueError:
        return None


def iter_companyfacts_share_facts(
    zip_path: str | Path,
    *,
    ciks: set[str] | None = None,
    concepts: Sequence[tuple[str, str]] = SHARE_CONCEPTS,
) -> Iterator[IssuerShareFact]:
    """Stream share-count facts from the retained Company Facts archive (one member at a time)."""
    wanted = tuple(dict.fromkeys(concepts))
    with zipfile.ZipFile(zip_path) as archive:
        for info in archive.infolist():
            name = info.filename
            if not (name.startswith("CIK") and name.endswith(".json")):
                continue
            try:
                cik = normalize_cik(name[3:-5])
            except ValueError:
                continue
            if ciks is not None and cik not in ciks:
                continue
            data = json.loads(archive.read(info))
            facts = data.get("facts") or {}
            for taxonomy, concept in wanted:
                body = (facts.get(taxonomy) or {}).get(concept) or {}
                for item in (body.get("units") or {}).get("shares") or ():
                    as_of, filed, value = _date(item.get("end")), _date(item.get("filed")), item.get("val")
                    if as_of is None or filed is None or value is None:
                        continue
                    yield IssuerShareFact(
                        cik=cik,
                        concept=f"{taxonomy}:{concept}",
                        as_of=as_of,
                        value=float(value),
                        accession=item.get("accn"),
                        form=item.get("form"),
                        filed=filed,
                    )


def read_lifecycle_filings(
    submissions_zip: str | Path,
    ciks: Iterable[str],
    *,
    forms: Sequence[str] = LIFECYCLE_FORMS,
    since: dt.date = dt.date(2009, 1, 1),
) -> list[IssuerFiling]:
    """Lifecycle filings of the given CIKs from the retained submissions archive (main + history members).

    Uses the bounded directory reader (:class:`_submissions_archive.SubmissionsArchive`);
    only the named members are read.
    """
    from ._submissions_archive import SubmissionsArchive

    wanted = set(forms)
    out: list[IssuerFiling] = []
    with SubmissionsArchive(Path(submissions_zip)) as archive:
        for cik in sorted(set(ciks)):
            name = f"CIK{cik}.json"
            if name not in archive:
                continue
            data = json.loads(archive.read(name))
            blocks = [(data.get("filings") or {}).get("recent") or {}]
            for extra in (data.get("filings") or {}).get("files") or ():
                if (extra.get("filingTo") or "9999") >= since.isoformat() and extra.get("name") in archive:
                    blocks.append(json.loads(archive.read(extra["name"])))
            for block in blocks:
                forms_ = block.get("form") or []
                dates = block.get("filingDate") or []
                accessions = block.get("accessionNumber") or []
                stamps = block.get("acceptanceDateTime") or []
                for index, form in enumerate(forms_):
                    if form not in wanted or index >= len(dates):
                        continue
                    filed = _date(dates[index])
                    if filed is None or filed < since:
                        continue
                    accession = accessions[index] if index < len(accessions) else None
                    accepted = _acceptance_utc(stamps[index]) if index < len(stamps) else None
                    out.append(IssuerFiling(cik, form, filed, accession, accepted))
    return out


def _acceptance_utc(raw: object) -> dt.datetime | None:
    """Exact EDGAR acceptance clock as naive UTC, or None.

    Only an offset-bearing stamp is a clock. A legacy stamp at EDGAR midnight
    Eastern (04:00/05:00 UTC exactly) carries a date, not a time, and is not
    publication evidence (same convention as the historical-identity sources).
    """
    text = str(raw).strip() if raw not in (None, "") else ""
    if not text or not (text.endswith(("Z", "z")) or text[-6:-5] in ("+", "-")):
        return None
    try:
        stamp = dt.datetime.fromisoformat(text[:-1] + "+00:00" if text[-1] in "Zz" else text)
    except ValueError:
        return None
    if stamp.tzinfo is None:
        return None
    utc = stamp.astimezone(dt.UTC).replace(tzinfo=None)
    if utc.time() in (dt.time(4), dt.time(5)):
        return None
    return utc


def read_sec_ticker_snapshot(company_tickers_json: str | Path) -> list[tuple[str, str]]:
    """``(cik, normalized ticker)`` pairs of the retained SEC ticker snapshot (current tickers only)."""
    payload = json.loads(Path(company_tickers_json).read_text(encoding="utf-8"))
    pairs = set()
    for value in payload.values():
        key = normalize_symbol(value.get("ticker"))
        if key:
            pairs.add((normalize_cik(value["cik_str"]), key))
    return sorted(pairs)


# ---------------------------------------------------------------------------------------------
# Matching


def match_share_counts(
    con: duckdb.DuckDBPyConnection,
    params: ReconstructionParams,
    *,
    vendor_ids: Iterable[int] | None = None,
) -> list[tuple[object, ...]]:
    """Share-count matches: one row per (vendor line, CIK, fact ``as_of``) -- the best fact/run pair.

    A vendor run matches a fact when its count (thousands) is within
    ``share_abs_tolerance`` of ``value / 1000`` and the run starts in
    ``[as_of - lead, as_of + lag]`` (``aligned``) or spans ``as_of``. The
    weight is ``1 / collisions`` (distinct CIKs matching the same run) times
    the covering and round-count discounts. Only (line, CIK) pairs whose
    share weight reaches ``min(min_share_weight, competitor_floor)`` are
    returned -- weaker pairs can neither link nor compete -- so the result
    stays small (bounded SQL; nothing per bar reaches Python).

    Columns: vendor_id, cik, concept, as_of, value, filed, accession, form,
    shares, run_first, run_last, aligned, collisions, weight.
    """
    lead, lag = params.run_start_lead_days, params.run_start_lag_days
    floor = min(params.min_share_weight, params.competitor_floor)
    window = int(params.trial_window_days)
    unit = float(VENDOR_SHARES_UNIT)
    run_filter = ""
    arguments: list[object] = []
    if vendor_ids is not None:
        wanted = sorted({int(vendor_id) for vendor_id in vendor_ids})
        if not wanted:
            return []
        run_filter = "WHERE vendor_id IN (SELECT unnest(?::BIGINT[]))"
        arguments.append(wanted)
    arguments += [
        lead,
        lag,
        unit,
        params.min_fact_shares,
        unit,
        params.share_abs_tolerance,
        lead,
        lag,
        params.trial_allowance,
        params.covering_match_weight,
        params.round_value_weight,
        params.quantized_value_weight,
        floor,
        params.min_fact_shares,
        lag,
        lead,
    ]
    # Memory: the (vendor-filtered) runs are the hash-join build side; the fact
    # table is streamed through the probe once, and only matched rows are grouped.
    return con.execute(
        f"""
        WITH runs AS (
            -- Trials: how many count changes the line had around this run. A line that
            -- re-counts daily (an ETF) sweeps through values and matches by chance.
            SELECT *, count(*) OVER (
                PARTITION BY vendor_id ORDER BY first_date
                RANGE BETWEEN INTERVAL {window} DAY PRECEDING AND INTERVAL {window} DAY FOLLOWING) AS nearby
            FROM ri_share_runs
            {run_filter}
        ), raw AS (
            SELECT r.vendor_id, f.cik, f.concept, f.as_of, f.value, f.filed, f.accession, f.form,
                   r.shares, r.first_date AS run_first, r.last_date AS run_last, r.nearby,
                   r.first_date BETWEEN f.as_of - CAST(? AS INTEGER) AND f.as_of + CAST(? AS INTEGER) AS aligned
            FROM ri_share_facts f
            CROSS JOIN (VALUES (-1), (0), (1)) AS k(d)
            JOIN runs r ON r.shares = CAST(floor(f.value / ?) AS BIGINT) + k.d
            WHERE f.value >= ? AND f.as_of IS NOT NULL AND f.filed IS NOT NULL
              AND abs(r.shares - f.value / ?) <= ?
              AND (r.first_date BETWEEN f.as_of - CAST(? AS INTEGER) AND f.as_of + CAST(? AS INTEGER)
                   OR (r.first_date <= f.as_of AND r.last_date >= f.as_of))
        ), matched AS (
            -- One fact per (issuer, concept, as_of, value): its first filing.
            SELECT vendor_id, cik, concept, as_of, value, min(filed) AS filed,
                   arg_min(accession, filed) AS accession, arg_min(form, filed) AS form,
                   shares, run_first, run_last, aligned, nearby
            FROM raw
            GROUP BY vendor_id, cik, concept, as_of, value, shares, run_first, run_last, aligned, nearby
        ), counted AS (
            SELECT m.*, count(DISTINCT m.cik) OVER (PARTITION BY m.vendor_id, m.run_first) AS collisions
            FROM matched m
        ), weighted AS (
            SELECT * EXCLUDE (nearby), (1.0 / collisions)
                      * least(1.0, ? / nearby)
                      * (CASE WHEN aligned THEN 1.0 ELSE ? END)
                      * (CASE WHEN value % 1000000 = 0 THEN ? WHEN value % 10000 = 0 THEN ? ELSE 1.0 END) AS weight
            FROM counted
        ), best AS (
            SELECT * FROM weighted
            QUALIFY row_number() OVER (
                PARTITION BY vendor_id, cik, as_of ORDER BY weight DESC, filed, concept, run_first) = 1
        ), valued AS (
            -- Independent evidence is a *distinct* matched count: an unchanged count
            -- re-reported every quarter (or a flickering vendor run) is one coincidence,
            -- not many. Only the strongest quarter per distinct vendor count carries weight.
            SELECT * REPLACE (CASE WHEN row_number() OVER (
                        PARTITION BY vendor_id, cik, shares ORDER BY weight DESC, as_of) = 1
                    THEN weight ELSE 0.0 END AS weight),
                   weight AS match_weight
            FROM best
        ), pairs AS (
            SELECT vendor_id, cik,
                   count(DISTINCT as_of) FILTER (WHERE concept LIKE 'dei:%') AS matched_dei,
                   count(DISTINCT as_of) FILTER (WHERE concept NOT LIKE 'dei:%') AS matched_other
            FROM valued GROUP BY vendor_id, cik HAVING sum(weight) >= ?
        ), life AS (
            SELECT r.vendor_id, min(r.first_date) AS life_from, max(r.last_date) AS life_to
            FROM ri_share_runs r WHERE r.vendor_id IN (SELECT vendor_id FROM pairs) GROUP BY r.vendor_id
        ), reported AS (
            -- The issuer's reported quarters inside the line's share-data life (the denominator).
            SELECT p.vendor_id, p.cik,
                   count(DISTINCT f.as_of) FILTER (WHERE f.concept LIKE 'dei:%') AS reported_dei,
                   count(DISTINCT f.as_of) FILTER (WHERE f.concept NOT LIKE 'dei:%') AS reported_other
            FROM pairs p JOIN life l USING (vendor_id)
            JOIN ri_share_facts f ON f.cik = p.cik AND f.value >= ? AND f.filed IS NOT NULL
             AND f.as_of BETWEEN l.life_from - CAST(? AS INTEGER) AND l.life_to + CAST(? AS INTEGER)
            GROUP BY p.vendor_id, p.cik
        )
        SELECT b.* EXCLUDE (match_weight), b.match_weight,
               CASE WHEN q.reported_dei > 0 THEN q.reported_dei ELSE q.reported_other END AS reported_quarters,
               least(1.0, CASE WHEN q.reported_dei > 0 THEN p.matched_dei / q.reported_dei
                               ELSE p.matched_other / greatest(q.reported_other, 1) END) AS match_ratio
        FROM valued b JOIN pairs p USING (vendor_id, cik) JOIN reported q USING (vendor_id, cik)
        ORDER BY b.vendor_id, b.cik, b.as_of
        """,
        arguments,
    ).fetchall()


def _at(day: dt.date, delta: dt.timedelta) -> dt.datetime:
    return dt.datetime.combine(day, dt.time()) + delta


def _share_items(
    matches: Iterable[tuple[object, ...]],
) -> tuple[dict[tuple[int, str], list[EvidenceItem]], dict[tuple[int, str], tuple[int, float]]]:
    """Group :func:`match_share_counts` rows into share items and (reported quarters, match ratio) per pair."""
    grouped: dict[tuple[int, str], list[EvidenceItem]] = defaultdict(list)
    stats: dict[tuple[int, str], tuple[int, float]] = {}
    for (vendor_id, cik, concept, as_of, value, filed, accession, form, shares, run_first, _run_last, aligned,
         collisions, weight, match_weight, reported, ratio) in matches:
        assert isinstance(as_of, dt.date) and isinstance(run_first, dt.date) and isinstance(filed, dt.date)
        line_date = max(run_first, as_of)
        pair = (int(vendor_id), str(cik))  # type: ignore[call-overload]
        stats[pair] = (int(reported), float(ratio))  # type: ignore[call-overload,arg-type]
        grouped[pair].append(
            EvidenceItem(
                kind=EV_SHARES,
                # Knowable once the filing is public and the vendor bar carrying the count has closed.
                known_at=max(_at(filed, _FILED_AVAILABILITY), _at(line_date, _BAR_CUTOFF)),
                line_date=line_date,
                weight=float(weight),  # type: ignore[arg-type]
                detail={
                    "concept": concept,
                    "as_of": as_of,
                    "value": value,
                    "vendor_shares_thousands": shares,
                    "run_first": run_first,
                    "filed": filed,
                    "accession": accession,
                    "form": form,
                    "collisions": collisions,
                    "aligned": bool(aligned),
                    "match_weight": round(float(match_weight), 4),  # type: ignore[arg-type]
                },
            )
        )
    return grouped, stats


def _lifecycle_items(
    line: VendorLine,
    cik: str,
    filings: Sequence[IssuerFiling],
    params: ReconstructionParams,
    horizon: dt.date,
    price_start: dt.date,
) -> list[EvidenceItem]:
    items: list[EvidenceItem] = []
    ended = (horizon - line.last_trade_date).days > params.active_line_days
    started = (line.first_trade_date - price_start).days > params.price_start_grace_days
    best_terminal: tuple[int, IssuerFiling] | None = None
    best_listing: tuple[int, IssuerFiling] | None = None
    for filing in filings:
        if ended and filing.form in TERMINAL_FORMS:
            low, high = (
                params.terminal_notice_window
                if filing.form in DELISTING_NOTICE_FORMS
                else params.terminal_deregistration_window
            )
            offset = (filing.filing_date - line.last_trade_date).days
            if low <= offset <= high and (best_terminal is None or abs(offset) < abs(best_terminal[0])):
                best_terminal = (offset, filing)
        if started and filing.form in LISTING_FORMS:
            low, high = params.listing_window
            offset = (filing.filing_date - line.first_trade_date).days
            if low <= offset <= high and (best_listing is None or abs(offset) < abs(best_listing[0])):
                best_listing = (offset, filing)
    if best_terminal is not None:
        offset, filing = best_terminal
        filing_clock, basis = (
            (filing.accepted_at, "acceptance")
            if filing.accepted_at is not None
            else (_at(filing.filing_date, _FILED_AVAILABILITY), "filed_plus_46h")
        )
        items.append(
            EvidenceItem(
                kind=EV_TERMINAL,
                # That the last bar *was* the last is known only at the next session's cutoff.
                known_at=max(filing_clock, _at(_next_session(line.last_trade_date), _BAR_CUTOFF)),
                line_date=line.last_trade_date,
                weight=params.lifecycle_weight,
                detail={"form": filing.form, "filing_date": filing.filing_date, "accession": filing.accession,
                        "offset_days": offset, "filing_clock_basis": basis,
                        "line_end_clock": "last_trade_next_weekday_22h"},
            )
        )
    if best_listing is not None:
        offset, filing = best_listing
        items.append(
            EvidenceItem(
                kind=EV_LISTING,
                known_at=max(_at(filing.filing_date, _FILED_AVAILABILITY), _at(line.first_trade_date, _BAR_CUTOFF)),
                line_date=line.first_trade_date,
                weight=params.lifecycle_weight,
                detail={"form": filing.form, "filing_date": filing.filing_date, "accession": filing.accession,
                        "offset_days": offset},
            )
        )
    return items


def _share_weight(items: Iterable[EvidenceItem]) -> float:
    # fsum: correctly rounded, so thresholds never depend on summation order or interpreter.
    return math.fsum(item.weight for item in items if item.kind == EV_SHARES)


def _share_span(items: Iterable[EvidenceItem]) -> tuple[dt.date, dt.date] | None:
    dates = [item.line_date for item in items if item.kind == EV_SHARES and item.line_date]
    return (min(dates), max(dates)) if dates else None


def _next_session(day: dt.date) -> dt.date:
    """The next weekday (no holiday calendar: a holiday only makes the clock one session early)."""
    following = day + _ONE_DAY
    while following.weekday() >= 5:
        following += _ONE_DAY
    return following


def _passes(items: Sequence[EvidenceItem], params: ReconstructionParams) -> bool:
    return (
        _share_weight(items) >= params.min_share_weight
        and math.fsum(item.weight for item in items) >= params.accept_weight
    )


def _pit_clock(items: Sequence[EvidenceItem], params: ReconstructionParams) -> dt.datetime | None:
    """Earliest clock at which the items known by then pass the acceptance rule."""
    ordered = sorted(items, key=lambda item: item.known_at)
    for index in range(len(ordered)):
        if index + 1 < len(ordered) and ordered[index + 1].known_at == ordered[index].known_at:
            continue
        if _passes(ordered[: index + 1], params):
            return ordered[index].known_at
    return None


def _ratio_acceptable(candidate: Candidate, params: ReconstructionParams) -> bool:
    return candidate.match_ratio >= params.min_match_ratio or (
        candidate.share_weight >= params.strong_share_weight and candidate.match_ratio >= params.strong_min_ratio
    )


def _is_rival(candidate: Candidate, params: ReconstructionParams) -> bool:
    """A (line, CIK) pair strong and consistent enough to contest or bound another CIK's link.

    Every candidate whose ratio would be acceptable is a rival, so two
    acceptable CIKs with overlapping evidence can never both be linked.
    """
    return candidate.share_weight >= params.competitor_floor and (
        candidate.match_ratio >= params.competitor_min_ratio or _ratio_acceptable(candidate, params)
    )


def _overlaps(left: tuple[dt.date, dt.date] | None, right: tuple[dt.date, dt.date] | None, days: int) -> bool:
    """Evidence spans compete: they overlap by more than ``days``, or one lies inside the other."""
    if left is None or right is None:
        return False
    overlap = (min(left[1], right[1]) - max(left[0], right[0])).days
    if overlap < 0:
        return False
    contained = (left[0] <= right[0] and right[1] <= left[1]) or (right[0] <= left[0] and left[1] <= right[1])
    return contained or overlap > days


def _tier(candidate: Candidate, competitors: Sequence[tuple[str, float]]) -> str:
    """Final (evaluation) confidence tier of an accepted candidate (see the module docstring)."""
    if competitors:
        return TIER_LOW
    if any(item.kind in _CORROBORATING for item in candidate.items):
        return TIER_HIGH
    return TIER_MEDIUM


def _tier_known(
    candidate: Candidate, rivals: Sequence[Candidate], clock: dt.datetime, params: ReconstructionParams
) -> str:
    """The tier judged only on the items (own and rivals') known at ``clock``."""
    own = [item for item in candidate.items if item.known_at <= clock]
    own_span = _share_span(own)
    for rival in rivals:
        known = [item for item in rival.items if item.known_at <= clock]
        if _share_weight(known) >= params.competitor_floor and _overlaps(
            own_span, _share_span(known), params.overlap_tolerance_days
        ):
            return TIER_LOW
    if any(item.kind in _CORROBORATING for item in own):
        return TIER_HIGH
    return TIER_MEDIUM


def _tier_history(
    candidate: Candidate, rivals: Sequence[Candidate], available_at: dt.datetime, params: ReconstructionParams
) -> tuple[tuple[str, dt.datetime], ...]:
    """Point-in-time ``(tier, from)`` steps from ``available_at`` on (only changes are recorded)."""
    clocks = {available_at}
    clocks.update(item.known_at for item in candidate.items if item.kind in _CORROBORATING)
    if rivals:  # own and rival share evidence move the contest; without rivals only corroboration matters
        clocks.update(item.known_at for item in candidate.items)
        clocks.update(item.known_at for rival in rivals for item in rival.items)
    steps: list[tuple[str, dt.datetime]] = []
    for clock in sorted(clock for clock in clocks if clock >= available_at):
        tier = _tier_known(candidate, rivals, clock, params)
        if not steps or steps[-1][0] != tier:
            steps.append((tier, clock))
    return tuple(steps)


def _symbol_in(line: VendorLine, segment: tuple[dt.date, dt.date]) -> str | None:
    """The last symbol the line traded under inside ``[from, to)``."""
    inside = [(last, symbol) for symbol, first, last in line.symbols if first < segment[1] and last >= segment[0]]
    return max(inside)[1] if inside else line.last_symbol


def reconstruct_issuer_links(
    con: duckdb.DuckDBPyConnection,
    lines: Sequence[VendorLine],
    *,
    filings: Sequence[IssuerFiling] = (),
    tickers: Sequence[tuple[str, str]] = (),
    ticker_observed_at: dt.datetime | None = None,
    params: ReconstructionParams | None = None,
    horizon: dt.date | None = None,
    price_start: dt.date | None = None,
    matches: Sequence[tuple[object, ...]] | None = None,
) -> ReconstructionResult:
    """Reconstruct line -> CIK links from the staged share runs/facts plus lifecycle and snapshot evidence.

    ``con`` holds ``ri_share_runs`` and ``ri_share_facts`` (see the ``stage_*``
    helpers). ``horizon`` (the price file's last date) decides which lines have
    ended; ``price_start`` its first date. ``matches`` may pass precomputed
    :func:`match_share_counts` rows.
    """
    params = params or ReconstructionParams()
    by_vendor = {line.vendor_id: line for line in lines}
    horizon = horizon or max((line.last_trade_date for line in lines), default=dt.date.max)
    price_start = price_start or min((line.first_trade_date for line in lines), default=dt.date.min)
    rows = match_share_counts(con, params, vendor_ids=by_vendor) if matches is None else matches
    share_items, pair_stats = _share_items(row for row in rows if int(row[0]) in by_vendor)  # type: ignore[call-overload]

    filings_by_cik: dict[str, list[IssuerFiling]] = defaultdict(list)
    for filing in filings:
        filings_by_cik[filing.cik].append(filing)
    tickers_by_cik: dict[str, set[str]] = defaultdict(set)
    for cik, ticker in tickers:
        tickers_by_cik[cik].add(ticker)
    snapshot_at = ticker_observed_at or dt.datetime.combine(horizon, dt.time()) + _BAR_CUTOFF

    candidates: dict[int, list[Candidate]] = defaultdict(list)
    for (vendor_id, cik), items in share_items.items():
        line = by_vendor[vendor_id]
        extra = _lifecycle_items(line, cik, filings_by_cik.get(cik, ()), params, horizon, price_start)
        line_keys = {normalize_symbol(symbol) for symbol, _first, _last in line.symbols} | {
            normalize_symbol(line.last_symbol)
        }
        hits = sorted(key for key in line_keys & tickers_by_cik.get(cik, set()) if key)
        if hits:
            extra.append(
                EvidenceItem(EV_SYMBOL, snapshot_at, None, params.symbol_weight, {"symbols": ",".join(hits)})
            )
        reported, ratio = pair_stats[(vendor_id, cik)]
        ordered = tuple(sorted(items, key=lambda item: item.known_at)) + tuple(extra)
        candidates[vendor_id].append(Candidate(vendor_id, cik, ordered, reported, ratio))

    links: list[ReconstructedLink] = []
    rejected: list[RejectedCandidate] = []
    for vendor_id in sorted(candidates):
        line = by_vendor[vendor_id]
        found = sorted(candidates[vendor_id], key=lambda c: (-c.weight, c.cik))
        accepted: list[tuple[Candidate, tuple[tuple[str, float], ...]]] = []
        rivals_of: dict[str, list[Candidate]] = {}
        line_rejected: list[RejectedCandidate] = []
        for candidate in found:
            rivals_of[candidate.cik] = [
                other
                for other in found
                if other is not candidate
                and _is_rival(other, params)
                and _overlaps(candidate.span, other.span, params.overlap_tolerance_days)
            ]
            competitors = tuple((other.cik, other.weight) for other in rivals_of[candidate.cik])
            if candidate.weight < params.accept_weight or candidate.share_weight < params.min_share_weight:
                reason = REJECT_BELOW_THRESHOLD
            elif not _ratio_acceptable(candidate, params):
                reason = REJECT_INCONSISTENT
            elif any(candidate.weight < params.dominance_ratio * weight for _cik, weight in competitors):
                reason = REJECT_CONFLICTING
            else:
                accepted.append((candidate, competitors))
                continue
            line_rejected.append(
                RejectedCandidate(vendor_id, line.price_security_id, candidate.cik, reason, candidate.weight,
                                  candidate.share_weight, candidate.span, competitors)
            )
        # Both sides of every conflict are kept as conflict rows, whatever a side's own
        # threshold outcome: a rival a link was preferred over (dominance rule) is the
        # losing side of a resolved conflict; a rival that blocks another candidate is one
        # side of an unresolved one.
        dominated = {cik for _candidate, competitors in accepted for cik, _weight in competitors}
        blocking = {cik for item in line_rejected if item.reason == REJECT_CONFLICTING for cik, _w in item.competitors}
        for item in line_rejected:
            if item.cik in dominated:
                item = replace(item, reason=REJECT_CONFLICT_DOMINATED, own_reason=item.reason)
            elif item.cik in blocking and item.reason not in CONFLICT_REASONS:
                item = replace(item, reason=REJECT_CONFLICTING, own_reason=item.reason)
            rejected.append(item)
        if not accepted:
            continue
        # Rejected rivals with real weight bound every segment's extension.
        chosen = {pair[0].cik for pair in accepted}
        rivals = [
            other.span
            for other in found
            if other.cik not in chosen and _is_rival(other, params) and other.span is not None
        ]
        accepted.sort(key=lambda pair: pair[0].span[0])  # type: ignore[index]
        segments = _segments(line, [pair[0] for pair in accepted], rivals, filings_by_cik, params)
        for (candidate, competitors), (start, end) in zip(accepted, segments, strict=True):
            if end <= start:
                rejected.append(
                    RejectedCandidate(vendor_id, line.price_security_id, candidate.cik, REJECT_EMPTY_SEGMENT,
                                      candidate.weight, candidate.share_weight, candidate.span, competitors)
                )
                continue
            clock = _pit_clock(candidate.items, params)
            assert clock is not None
            history = _tier_history(candidate, rivals_of[candidate.cik], clock, params)
            assert history and history[-1][0] == _tier(candidate, competitors)
            links.append(
                ReconstructedLink(
                    vendor_id=vendor_id,
                    price_security_id=line.price_security_id,
                    cik=candidate.cik,
                    valid_from=start,
                    valid_to=end,
                    available_at=clock,
                    evidence_complete_at=max(item.known_at for item in candidate.items),
                    weight=candidate.weight,
                    share_weight=candidate.share_weight,
                    tier=_tier(candidate, competitors),
                    symbol=_symbol_in(line, (start, end)),
                    items=candidate.items,
                    competitors=competitors,
                    current_cik=line.current_cik,
                    tier_history=history,
                )
            )
    return ReconstructionResult(params, tuple(links), tuple(rejected), lines_considered=len(lines))


def reconstruct_in_batches(
    con: duckdb.DuckDBPyConnection,
    lines: Sequence[VendorLine],
    *,
    filings: Sequence[IssuerFiling] = (),
    tickers: Sequence[tuple[str, str]] = (),
    ticker_observed_at: dt.datetime | None = None,
    params: ReconstructionParams | None = None,
    batch_size: int = 500,
) -> Iterator[ReconstructionResult]:
    """:func:`reconstruct_issuer_links` over bounded batches of lines (full-universe runs).

    Every decision is per vendor line (collisions are per vendor run across all
    issuers), so batching is exact; only the horizon and price-file start are
    global and are fixed from all lines first. Callers consume and release each
    batch (evidence rows, summaries) so evidence items never accumulate.
    """
    if not lines:
        return
    horizon = max(line.last_trade_date for line in lines)
    price_start = min(line.first_trade_date for line in lines)
    for start in range(0, len(lines), batch_size):
        yield reconstruct_issuer_links(
            con,
            lines[start : start + batch_size],
            filings=filings,
            tickers=tickers,
            ticker_observed_at=ticker_observed_at,
            params=params,
            horizon=horizon,
            price_start=price_start,
        )


def _segments(
    line: VendorLine,
    ordered: Sequence[Candidate],
    rivals: Sequence[tuple[dt.date, dt.date]],
    filings_by_cik: Mapping[str, Sequence[IssuerFiling]],
    params: ReconstructionParams,
) -> list[tuple[dt.date, dt.date]]:
    """Disjoint ``[from, to)`` intervals for the sequential accepted candidates of one line.

    Each segment covers its own evidence span, extended by at most
    ``max_extension_days`` (to the line's first/last trade when that is closer,
    or when the issuer's listing/terminal filing dates that end), never into
    the territory of a rival CIK's evidence, and hands over to the next
    candidate at its successor/listing filing or first evidence.
    """
    line_start, line_end = line.first_trade_date, line.last_trade_date + _ONE_DAY
    reach = dt.timedelta(days=params.max_extension_days)
    starts: list[dt.date] = []
    ends: list[dt.date] = []
    for index, candidate in enumerate(ordered):
        span = candidate.span
        assert span is not None
        kinds = {item.kind for item in candidate.items}
        if index == 0:
            start = line_start if span[0] - line_start <= reach or EV_LISTING in kinds else span[0] - reach
        else:
            previous = ordered[index - 1].span
            assert previous is not None
            # A successor/listing registration between the predecessor's last and
            # this candidate's first evidence dates the hand-over more precisely.
            handover = [
                filing.filing_date
                for filing in filings_by_cik.get(candidate.cik, ())
                if filing.form in LISTING_FORMS and previous[1] < filing.filing_date <= span[0]
            ]
            start = min(handover) if handover else span[0]
        end = line_end if line_end - span[1] <= reach or EV_TERMINAL in kinds else span[1] + reach
        # Never extend into a rival's evidence: a rival with evidence before/after this
        # candidate's own bounds the extension at the candidate's evidence.
        for rival_start, rival_end in rivals:
            if rival_start < span[0]:
                start = max(start, min(rival_end + _ONE_DAY, span[0]))
            if rival_end > span[1]:
                end = min(end, max(rival_start, span[1] + _ONE_DAY))
        starts.append(max(start, line_start))
        ends.append(min(end, line_end))
    return [
        (starts[index], min(ends[index], starts[index + 1]) if index + 1 < len(ordered) else ends[index])
        for index in range(len(ordered))
    ]
