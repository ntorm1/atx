"""Ownership and short-interest research features (task P9, fix round 1).

For one sealed R2a monthly panel run (one basis) this module builds one immutable,
content-addressed *ownership version*: the formation-aligned table
``research_ownership_features`` (dense over the panel's eligible lines: linked primary
lines ``owner_basis='linked_primary'`` and eligible lines without an owner link
``owner_basis='unlinked_line'``, the R2b I1 universe of price-line features; the shape of
P3's ``research_event_features`` for the planned R2b adapter) and the identity rows it used
(``research_ownership_identity``). Identity comes from :mod:`atx_db.ownership_identity`.

Features (:data:`OWNERSHIP_FEATURES`)
------------------------------------
13F features need the owner: on an unlinked line they are NULL with ``no_owner_link``.
Quarter R* = the latest calendar quarter end whose filing deadline, ``R + 45 days`` plus
the FC1 46 h floor, has passed at the formation cutoff and that is at most
``max_13f_age_days`` old; R* - 1 = the previous calendar quarter end.

``io_ratio_13f``
    Institutional ownership: the 13F **common-equity** shares (eligible holdings of
    :func:`atx_db.ownership_identity.holding_exclusion_sql`: ``SH``, no put/call, a common
    ``title_of_class``, a valid non-placeholder CUSIP) summed over every manager's position
    state in the owner's mapped CUSIPs at R* / the line's **verified** share count at R* (the
    ``market_daily_metrics`` row of the last session on or before R*, newest revision
    visible at the cutoff, ``shares_source='dei'``); otherwise NULL with ``unverified_shares``
    / ``missing_market_row``. IO > 1 is kept (never clipped) and flagged
    ``value_flag='io_above_one'``.
``io_change_13f``
    IO(R*) - IO(R* - 1), both as known at the same cutoff; the weaker identity of the two.
``breadth_change_13f``
    Chen, Hong and Stein (2002): (managers holding the stock at R* - managers holding it at
    R* - 1) / managers with a visible full filing in both quarters, counting only those.

A manager-quarter's position state at a cutoff: the latest visible FULL filing (original or
RESTATEMENT) plus the visible ADD-NEW-HOLDINGS amendments after it (the
:mod:`atx_db.thirteenf_amendments` sequencing); filings are deduplicated by accession. A
filing's clock is the later of its EDGAR acceptance (when ``sec_submissions`` has it) and
filed date + 46 h; the 45-day filing lag is inherent.

FINRA short interest (line-level: every eligible line; the latest settlement mapped to the
line whose publication clock is at or before the cutoff and at most ``max_si_age_days``
old):

``short_interest_ratio``
    Short interest / the line's verified share count at the last session on or before the
    settlement (same gate as IO; an unlinked line has no DEI count: ``unverified_shares``). A
    split window is covered by that gate: ``market_daily`` never labels a count ``dei`` across
    an unabsorbed split (``archive_split_adjusted`` / ``split_unresolved`` instead).
``days_to_cover_si``
    Short interest / FINRA's average daily volume (``missing_adv`` when not positive);
    NULL with ``split_in_period`` when FINRA's ``stock_split_flag`` is set or an R1d exact
    split ratio (the A8 real-split rule) goes ex on the line inside the reporting period
    (after the symbol's previous settlement, at most 16 days back, through the settlement):
    SI and the period ADV are then on mixed share bases.

Short interest is visible only from its **publication** clock, never the settlement: the
later of the loader's ``available_at`` and settlement + ``si_publication_business_days``
business days (default 8; FINRA disseminates about seven business days after settlement,
after the close) at 22:00 UTC; formations trade at the next session. A row FINRA revised
(``revision_flag`` set: the loader keeps only the revised values) is visible only from the
modeled publication of the **next** settlement cycle, never at the original's clock; with no
next cycle in the data it is withheld.
Business days are the observed XNYS sessions inside the calendar and weekdays outside it.

Every row carries ``owner_basis``, ``source_period``, ``source_clock``, ``identity_basis``
(the weakest mapping basis/confidence among the owner's CUSIPs, or
``finra_symbol_date_line``), ``sample_conditioning`` (13F: ``cusip_survivor_conditioned``
when any CUSIP used was mapped through a current snapshot, else ``dated_name_window``) and
``reason``: ``valid``, ``unverified_shares``, ``missing_market_row``, ``no_owner_link``,
``no_mapped_13f_holding`` (no visible eligible 13F position in a CUSIP mapped to the owner
for that quarter: unknown, never zero IO -- nobody held the stock, or its CUSIP did not map,
e.g. a non-surviving CUSIP without a dated name window), ``not_held_both_quarters``, ``no_continuing_managers``,
``previous_quarter_<reason>``, ``missing_adv``, ``split_in_period``,
``no_recent_13f_quarter`` or ``no_recent_short_interest``.

Versions and seal checks follow P3: ``ownership_version`` = sha256(spec incl. panel seal,
parameters, input fingerprints; code digest). Seal checks: no value visible after its
cutoff; no short-interest value before its publication clock or on its settlement day; no
13F value before its quarter's deadline clock; every 13F value's owner mapped for its
quarter; no owner feature on an unlinked line; grain. Blockers label the reconstructed
identity, the modeled FINRA calendar, revised FINRA rows, unlinked lines and the
survivor-conditioned 13F coverage (``thirteenf_identity_survivor_conditioned:<n>`` while any
13F value is survivor-conditioned, and whenever no dated name window (0327) exists).
"""

from __future__ import annotations

import contextlib
import datetime as dt
import hashlib
import json
import re
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import duckdb

from .. import ownership_identity as _identity
from .._fundamental_clock import FUNDAMENTAL_CLOCK_POLICY
from ..market_daily import MARKET_DAILY_SOURCE_NAME, MARKET_DAILY_STRICT_SOURCE_NAME, _real_split_sql
from . import panel as _panel
from .features import OWNER_BASIS_LINKED, OWNER_BASIS_UNLINKED, UNLINKED_COHORT_REASONS, UNLINKED_LINES_BLOCKER
from .store import ResearchStore

QUERY_VERSION = "research-ownership-features-v2"
OWNERSHIP_SCHEMA_VERSION = 2
KNOWN_SCHEMA_VERSIONS = (1, 2)

FEATURE_IO = "io_ratio_13f"
FEATURE_IO_CHANGE = "io_change_13f"
FEATURE_BREADTH = "breadth_change_13f"
FEATURE_SIR = "short_interest_ratio"
FEATURE_DTC = "days_to_cover_si"
THIRTEENF_FEATURES = (FEATURE_IO, FEATURE_IO_CHANGE, FEATURE_BREADTH)
SI_FEATURES = (FEATURE_SIR, FEATURE_DTC)
OWNERSHIP_FEATURES = (*THIRTEENF_FEATURES, *SI_FEATURES)

VALID = "valid"
REASON_UNVERIFIED = _panel.UNVERIFIED_SHARES_REASON
REASON_NO_MARKET_ROW = "missing_market_row"
REASON_NO_OWNER = "no_owner_link"
REASON_NO_CUSIP = "no_mapped_13f_holding"
REASON_NOT_BOTH = "not_held_both_quarters"
REASON_NO_CONTINUING = "no_continuing_managers"
REASON_NO_ADV = "missing_adv"
REASON_SPLIT = "split_in_period"
REASON_NO_13F = "no_recent_13f_quarter"
REASON_NO_SI = "no_recent_short_interest"
FLAG_IO_ABOVE_ONE = "io_above_one"
SI_IDENTITY_BASIS = "finra_symbol_date_line"
SI_CONDITIONING = "line_level"
VERIFIED_SHARES_SOURCES = _panel.VERIFIED_SHARES_SOURCES

FILING_DEADLINE_DAYS = 45
EVIDENCE_FLOOR_HOURS = 46
PUBLICATION_HOUR = 22
SI_PERIOD_MAX_DAYS = 16

STATUS_BUILDING = "building"
STATUS_SEALED = "sealed"
STATUS_EMPTY = "sealed_empty"
STATUS_FAILED = "failed"
SEALED_STATUSES = (STATUS_SEALED, STATUS_EMPTY)

IDENTITY_BLOCKER = "ownership_identity_reconstructed_modeled_not_certified"
PUBLICATION_BLOCKER = "short_interest_publication_modeled_business_days"
RECONSTRUCTED_BLOCKER = "reconstructed_identity_universe_and_availability_not_certifiable"
SURVIVOR_BLOCKER = "thirteenf_identity_survivor_conditioned"
REVISED_BLOCKER = "finra_revised_rows_modeled_next_cycle"

REQUIRED_WAREHOUSE_TABLES = ("trading_calendar", "market_daily_metrics", "equity_daily_bars", "sec_company_tickers")
THIRTEENF_TABLES = ("thirteenf_submissions", "thirteenf_holdings")
_VERSION_TABLES = ("research_ownership_features", "research_ownership_identity", "research_ownership_versions")
_TEMP_TABLES = ("_ow_cal", "_ow_days", "_ow_bday", "_ow_forms", "_ow_lines", "_ow_owners", "_ow_filings",
                "_ow_form_q", "_ow_positions", "_ow_vals", "_ow_si", "_ow_si_rows", "_ow_si_splits", "_ow_shares_at",
                "_ow_state", "_ow_managers", "_ow_share_keys", "_ow_si_pick", "_oi_cusip_periods", "_oi_cusip_owner",
                "_oi_names", "_oi_current_cusips", "_oi_flagged", "_oi_ticker_links", "_oi_primary_diag",
                "_oi_name_links", "_oi_raw", "_oi_filers", "_oi_cusip_status", "_oi_eligible", "_oi_si_keys",
                "_oi_si_line")
_ID = re.compile(r"^[a-z][a-z0-9_]{0,63}$")
_PKG = Path(__file__).resolve().parents[1]
_CODE_FILES = (Path(__file__), _PKG / "ownership_identity.py", _PKG / "research" / "panel.py",
               _PKG / "_fundamental_clock.py")
_DIGEST_CHUNK = 12
_VALS_COLUMNS = ("formation_date DATE, security_id VARCHAR, owner_cik VARCHAR, feature_id VARCHAR, value DOUBLE, "
                 "reason VARCHAR, available_at TIMESTAMP, source_period DATE, source_clock TIMESTAMP, "
                 "value_flag VARCHAR, identity_basis VARCHAR, sample_conditioning VARCHAR")


class OwnershipStoreError(ValueError):
    """The ownership store cannot build or validate a version under its contract."""


def _canonical(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False, default=str)


def _sha(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _sql_list(values: Sequence[str]) -> str:
    return ",".join("'" + value.replace("'", "''") + "'" for value in values)


def _code_sha() -> str:
    digest = hashlib.sha256()
    for path in _CODE_FILES:
        digest.update(path.name.encode("utf-8"))
        digest.update(path.read_bytes().replace(b"\r\n", b"\n"))
    return digest.hexdigest()


def _now() -> dt.datetime:
    return dt.datetime.now(dt.UTC).replace(tzinfo=None)


def _calendar_keys() -> tuple[str, str]:
    from ..delisting import _TRADING_CALENDAR_ID, _TRADING_CALENDAR_SOURCE

    return _TRADING_CALENDAR_ID, _TRADING_CALENDAR_SOURCE


# ---------------------------------------------------------------------------
# Schema
# ---------------------------------------------------------------------------

def ensure_ownership_schema(con: Any) -> None:
    """Create or upgrade the ``research_ownership_*`` tables (schema v2); refuse a newer unknown schema."""
    con.execute("""
        CREATE TABLE IF NOT EXISTS research_ownership_schema (
            version INTEGER PRIMARY KEY, name VARCHAR NOT NULL, applied_at TIMESTAMP NOT NULL)""")
    versions = {int(row[0]) for row in con.execute("SELECT version FROM research_ownership_schema").fetchall()}
    if versions - set(KNOWN_SCHEMA_VERSIONS):
        raise RuntimeError(f"research ownership schema has unknown versions {sorted(versions)}; code is older")
    if OWNERSHIP_SCHEMA_VERSION in versions:
        return
    con.execute("""
        CREATE TABLE IF NOT EXISTS research_ownership_versions (
            ownership_version VARCHAR PRIMARY KEY,
            status VARCHAR NOT NULL,
            basis VARCHAR NOT NULL,
            panel_run_id VARCHAR NOT NULL,
            panel_sha256 VARCHAR NOT NULL,
            query_version VARCHAR NOT NULL,
            spec_json VARCHAR NOT NULL,
            spec_sha256 VARCHAR NOT NULL,
            code_sha256 VARCHAR NOT NULL,
            inputs_json VARCHAR NOT NULL,
            blockers_json VARCHAR NOT NULL,
            diagnostic_json VARCHAR,
            identity_sha256 VARCHAR,
            features_sha256 VARCHAR,
            created_at TIMESTAMP NOT NULL,
            finished_at TIMESTAMP
        );
        CREATE TABLE IF NOT EXISTS research_ownership_identity (
            ownership_version VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            source_key VARCHAR NOT NULL,
            source_period DATE NOT NULL,
            name_key VARCHAR,
            owner_cik VARCHAR,
            security_id VARCHAR,
            identity_basis VARCHAR,
            confidence VARCHAR,
            reason VARCHAR NOT NULL
        );
        CREATE TABLE IF NOT EXISTS research_ownership_features (
            ownership_version VARCHAR NOT NULL,
            formation_date DATE NOT NULL,
            security_id VARCHAR NOT NULL,
            owner_cik VARCHAR,
            feature_id VARCHAR NOT NULL,
            raw_value DOUBLE,
            reason VARCHAR NOT NULL,
            available_at TIMESTAMP,
            source_period DATE,
            source_clock TIMESTAMP,
            value_flag VARCHAR,
            identity_basis VARCHAR
        );
    """)
    # v1 -> v2 in place (no-ops on a fresh store): owner basis, sample conditioning, nullable owner.
    con.execute("ALTER TABLE research_ownership_features ADD COLUMN IF NOT EXISTS owner_basis VARCHAR")
    con.execute("ALTER TABLE research_ownership_features ADD COLUMN IF NOT EXISTS sample_conditioning VARCHAR")
    con.execute("ALTER TABLE research_ownership_identity ADD COLUMN IF NOT EXISTS sample_conditioning VARCHAR")
    nullable = con.execute("""
        SELECT is_nullable FROM duckdb_columns()
        WHERE table_name='research_ownership_features' AND column_name='owner_cik' AND NOT internal
          AND database_name=current_database()""").fetchone()
    if nullable is not None and not nullable[0]:
        con.execute("ALTER TABLE research_ownership_features ALTER COLUMN owner_cik DROP NOT NULL")
    con.execute("INSERT INTO research_ownership_schema VALUES (?, ?, ?)",
                [OWNERSHIP_SCHEMA_VERSION, "ownership_features_v2_line_level_conditioning", _now()])


# ---------------------------------------------------------------------------
# Options, spec and version identity
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class OwnershipFeatureOptions:
    """What determines an ownership version (``formation_chunk`` is execution only)."""

    panel_run_id: str
    max_13f_age_days: int = 200
    max_si_age_days: int = 45
    si_publication_business_days: int = 8
    formation_chunk: int = 12
    verify_panel: bool = False


@dataclass(frozen=True)
class OwnershipFeatureResult:
    ownership_version: str
    status: str
    reused: bool
    feature_rows: int
    blockers: tuple[str, ...]
    diagnostic: dict[str, Any]


def _validate_options(options: OwnershipFeatureOptions) -> OwnershipFeatureOptions:
    if not isinstance(options.panel_run_id, str) or not _ID.fullmatch(options.panel_run_id):
        raise OwnershipStoreError("panel_run_id must be a lower-case identifier")
    for label, value, low, high in (("max_13f_age_days", options.max_13f_age_days, 46, 1000),
                                    ("max_si_age_days", options.max_si_age_days, 1, 400),
                                    ("si_publication_business_days", options.si_publication_business_days, 1, 30),
                                    ("formation_chunk", options.formation_chunk, 1, 600)):
        if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
            raise OwnershipStoreError(f"{label} must be an integer {low}..{high}")
    return options


def _panel_context(store: ResearchStore, options: OwnershipFeatureOptions) -> dict[str, Any]:
    row = store.con.execute("SELECT status, basis, panel_sha256 FROM research_panel_runs WHERE run_id=?",
                            [options.panel_run_id]).fetchone()
    if row is None:
        raise OwnershipStoreError(f"panel run {options.panel_run_id!r} is absent")
    status, basis, panel_sha = row
    if status not in ("complete", "untestable_strict") or not panel_sha:
        raise OwnershipStoreError(f"panel run {options.panel_run_id} is {status!r}; only sealed complete or "
                                  "untestable_strict runs carry ownership features")
    if options.verify_panel:
        _panel.validate_research_panel(store, options.panel_run_id)
    market = MARKET_DAILY_STRICT_SOURCE_NAME if basis == _panel.BASIS_STRICT else MARKET_DAILY_SOURCE_NAME
    return {"status": str(status), "basis": str(basis), "panel_sha256": str(panel_sha), "market_source": market}


def _input_fingerprints(store: ResearchStore, calendar: tuple[str, str], market_source: str) -> dict[str, Any]:
    queries: dict[str, tuple[str, list[Any]]] = {
        "trading_calendar": ("""
            SELECT count(*), min(trade_date), max(trade_date), sum(date_diff('day', DATE '1970-01-01', trade_date))
            FROM trading_calendar WHERE calendar_id=? AND source=? AND is_open""", list(calendar)),
        "market_daily_metrics": ("SELECT count(*), max(available_at), sum(shares_outstanding), "
                                 "count(DISTINCT shares_source) FROM market_daily_metrics WHERE source=?",
                                 [market_source]),
        "equity_daily_bars": ("SELECT count(*), max(trade_date), count(DISTINCT symbol) FROM equity_daily_bars", []),
        "sec_company_tickers": ("SELECT count(*), count(DISTINCT cik), count(DISTINCT title) FROM sec_company_tickers",
                                []),
    }
    optional = {  # table -> content aggregate; max(source_loaded_at) is appended when the column exists
        "thirteenf_submissions": "count(*), max(period_of_report), max(filing_date)",
        "thirteenf_holdings": "count(*), sum(share_quantity)",
        "thirteenf_cover_pages": "count(*)",
        "finra_short_interest": "count(*), max(settlement_date), sum(current_short_position_quantity)",
        "security_identifier_history": "count(*)",
        "securities": "count(*)",
        "security_identity_evidence": "count(*), max(valid_from)",
        "sec_submissions": "count(*)",
    }
    for table, aggregate in optional.items():
        if store.warehouse_has(table):
            loaded = ", max(source_loaded_at)" if store.warehouse_has(table, "source_loaded_at") else ""
            queries[table] = (f"SELECT {aggregate}{loaded} FROM {table}", [])
    for table in ("equity_daily_bars", "sec_company_tickers"):
        if store.warehouse_has(table, "source_loaded_at"):
            sql, params = queries[table]
            queries[table] = (sql.replace(" FROM ", ", max(source_loaded_at) FROM ", 1), params)
    out: dict[str, Any] = {}
    for table, (sql, params) in queries.items():
        values = store.con.execute(sql, params).fetchone()
        out[table] = [None if value is None else str(value) for value in values]
    return out


def _spec(options: OwnershipFeatureOptions, context: dict[str, Any], calendar: tuple[str, str],
          inputs: dict[str, Any]) -> dict[str, Any]:
    return {
        "query_version": QUERY_VERSION,
        "panel_run_id": options.panel_run_id,
        "panel_sha256": context["panel_sha256"],
        "basis": context["basis"],
        "market_source": context["market_source"],
        "verified_shares_sources": list(VERIFIED_SHARES_SOURCES),
        "universe": {"owner_bases": [OWNER_BASIS_LINKED, OWNER_BASIS_UNLINKED],
                     "unlinked_cohort_reasons": list(UNLINKED_COHORT_REASONS),
                     "owner_features": list(THIRTEENF_FEATURES), "line_features": list(SI_FEATURES)},
        "thirteenf": {"filing_deadline_days": FILING_DEADLINE_DAYS, "max_age_days": options.max_13f_age_days,
                      "clock": f"max(acceptance, filed + {EVIDENCE_FLOOR_HOURS}h) ({FUNDAMENTAL_CLOCK_POLICY})",
                      "positions": "eligible common-equity holdings; latest visible FULL + later visible ADD",
                      "common_title_pattern": _identity.COMMON_TITLE_PATTERN,
                      "non_common_title_pattern": _identity.NON_COMMON_TITLE_PATTERN,
                      "breadth": "chen_hong_stein_2002_continuing_managers_with_full_filings"},
        "short_interest": {"max_age_days": options.max_si_age_days,
                           "publication_business_days": options.si_publication_business_days,
                           "publication_hour_utc": PUBLICATION_HOUR, "line_lookback_days": _identity.SI_LOOKBACK_DAYS,
                           "revised_rows": "next_settlement_cycle_publication",
                           "split_in_period": "stock_split_flag_or_a8_real_split_rule",
                           "period_max_days": SI_PERIOD_MAX_DAYS},
        "identity": {"bases": list(_identity.IDENTITY_BASES), "availability": _identity.MAPPING_AVAILABILITY,
                     "min_current_filers": _identity.MIN_CURRENT_FILERS,
                     "name_dominance_min": _identity.NAME_DOMINANCE_MIN},
        "calendar": list(calendar),
        "features": list(OWNERSHIP_FEATURES),
        "inputs": inputs,
        "duckdb": duckdb.__version__,
    }


# ---------------------------------------------------------------------------
# Staging
# ---------------------------------------------------------------------------

def _stage_panel(con: Any, run_id: str, calendar: tuple[str, str]) -> dict[str, int]:
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _ow_cal AS
        SELECT trade_date, CAST(row_number() OVER (ORDER BY trade_date) AS INTEGER) AS session_number
        FROM (SELECT DISTINCT trade_date FROM trading_calendar WHERE calendar_id=? AND source=? AND is_open)
    """, list(calendar))
    bounds = con.execute("SELECT min(trade_date), max(trade_date) FROM _ow_cal").fetchone()
    if bounds is None or bounds[0] is None:
        raise OwnershipStoreError("the session calendar is empty")
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _ow_days AS
        WITH series AS (
            SELECT CAST(generate_series AS DATE) AS day
            FROM generate_series(DATE '1993-01-01', CAST(? AS DATE) + 400, INTERVAL 1 DAY)
        ), marked AS (
            SELECT s.day, CASE WHEN s.day BETWEEN CAST(? AS DATE) AND CAST(? AS DATE) THEN c.trade_date IS NOT NULL
                               ELSE isodow(s.day) <= 5 END AS is_business
            FROM series s LEFT JOIN _ow_cal c ON c.trade_date=s.day
        )
        SELECT day, is_business, sum(CASE WHEN is_business THEN 1 ELSE 0 END) OVER (ORDER BY day) AS business_le
        FROM marked
    """, [bounds[1], bounds[0], bounds[1]])
    con.execute("CREATE OR REPLACE TEMP TABLE _ow_bday AS SELECT business_le, min(day) AS day FROM _ow_days "
                "WHERE is_business GROUP BY business_le")
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _ow_forms AS
        SELECT formation_date, cutoff FROM research_panel_calendar
        WHERE run_id=? AND status=? AND formation_date IS NOT NULL AND cutoff IS NOT NULL
    """, [run_id, _panel.CALENDAR_FORMED])
    # R2b I1: linked primary lines plus eligible lines without a usable owner link.
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ow_lines AS
        SELECT * FROM (
            SELECT c.formation_date, f.cutoff, c.security_id,
                   CASE WHEN c.cohort_reason='valid' AND coalesce(c.primary_line, false) THEN c.owner_cik END
                       AS owner_cik,
                   CASE WHEN c.cohort_reason='valid' AND coalesce(c.primary_line, false) AND c.owner_cik IS NOT NULL
                        THEN '{OWNER_BASIS_LINKED}'
                        WHEN c.cohort_reason IN ({_sql_list(UNLINKED_COHORT_REASONS)}) THEN '{OWNER_BASIS_UNLINKED}'
                        END AS owner_basis
            FROM research_panel_cohort c JOIN _ow_forms f ON f.formation_date=c.formation_date
            WHERE c.run_id=? AND coalesce(c.eligible, false)
        ) WHERE owner_basis IS NOT NULL
    """, [run_id])
    con.execute(f"CREATE OR REPLACE TEMP TABLE _ow_owners AS SELECT DISTINCT owner_cik FROM _ow_lines "
                f"WHERE owner_basis='{OWNER_BASIS_LINKED}'")
    formations, lines, unlinked, owners = con.execute(f"""
        SELECT (SELECT count(*) FROM _ow_forms), (SELECT count(*) FROM _ow_lines),
               (SELECT count(*) FROM _ow_lines WHERE owner_basis='{OWNER_BASIS_UNLINKED}'),
               (SELECT count(*) FROM _ow_owners)""").fetchone()
    return {"formations": int(formations), "line_formations": int(lines), "unlinked_line_formations": int(unlinked),
            "owners": int(owners)}


def _shares_sql(market_source: str) -> str:
    """``_ow_shares_at(security_id, anchor_date, cutoff) -> session, clock, shares, shares_source``.

    Callers stage ``_ow_share_keys(security_id, anchor_date, cutoff)``; the result is the newest
    ``market_daily_metrics`` revision of the last session on or before ``anchor_date`` visible
    at ``cutoff`` (every column from that one revision, NULLs included: ``arg_max_null``).
    """
    return f"""
        CREATE OR REPLACE TEMP TABLE _ow_shares_at AS
        WITH keyed AS (
            SELECT k.*, s.trade_date AS session FROM _ow_share_keys k
            ASOF LEFT JOIN _ow_cal s ON k.anchor_date >= s.trade_date
        )
        SELECT k.security_id, k.anchor_date, k.cutoff, k.session,
               max(m.available_at) AS clock,
               CAST(arg_max_null(m.shares_outstanding, (m.available_at, m.market_daily_id)) AS DOUBLE) AS shares,
               arg_max_null(m.shares_source, (m.available_at, m.market_daily_id)) AS shares_source,
               count(m.market_daily_id) AS market_rows
        FROM keyed k
        LEFT JOIN market_daily_metrics m
          ON m.security_id=k.security_id AND m.trade_date=k.session AND m.available_at <= k.cutoff
         AND m.as_of_date <= CAST(k.cutoff AS DATE) AND m.source='{market_source.replace("'", "''")}'
        GROUP BY k.security_id, k.anchor_date, k.cutoff, k.session
    """


def _share_reason_sql(alias: str) -> str:
    verified = _sql_list(VERIFIED_SHARES_SOURCES)
    return f"""CASE WHEN coalesce({alias}.market_rows, 0) = 0 THEN '{REASON_NO_MARKET_ROW}'
                    WHEN {alias}.shares_source IS NULL OR {alias}.shares_source NOT IN ({verified})
                         OR NOT coalesce({alias}.shares > 0, false) THEN '{REASON_UNVERIFIED}'
                    ELSE '{VALID}' END"""


def _stage_identity(store: ResearchStore, version: str, has_13f: bool, has_si: bool) -> dict[str, Any]:
    con = store.con
    out: dict[str, Any] = {}
    if has_13f:
        out["cusip_periods"] = _identity.stage_13f_cusip_periods(con)
        out["cusip_owner_map"] = _identity.stage_cusip_owner_map(con)
        with store.transaction():  # audit: every unmapped CUSIP-period and the mappings to panel owners
            con.execute(f"""
                INSERT INTO research_ownership_identity (ownership_version, source, source_key, source_period,
                    name_key, owner_cik, security_id, identity_basis, confidence, reason, sample_conditioning)
                SELECT ?, '13f', cusip, report_period, name_key, owner_cik, NULL, identity_basis, confidence, reason,
                       sample_conditioning
                FROM _oi_cusip_owner
                WHERE reason<>'{_identity.MAPPED}' OR owner_cik IN (SELECT owner_cik FROM _ow_owners)
            """, [version])
    else:
        con.execute("CREATE OR REPLACE TEMP TABLE _oi_cusip_owner (cusip VARCHAR, report_period DATE, name_key VARCHAR, "
                    "owner_cik VARCHAR, identity_basis VARCHAR, confidence VARCHAR, reason VARCHAR, "
                    "sample_conditioning VARCHAR)")
    if has_si:
        out["si_line_map"] = _identity.stage_si_line_map(con)
        with store.transaction():  # audit: rows mapped to panel lines (unmapped rows are counted only)
            con.execute(f"""
                INSERT INTO research_ownership_identity (ownership_version, source, source_key, source_period,
                    name_key, owner_cik, security_id, identity_basis, confidence, reason, sample_conditioning)
                SELECT ?, 'finra', symbol, settlement_date, symbol_key, NULL, security_id,
                       '{SI_IDENTITY_BASIS}', NULL, reason, '{SI_CONDITIONING}'
                FROM _oi_si_line
                WHERE reason='{_identity.SI_MAPPED}' AND security_id IN (SELECT security_id FROM _ow_lines)
            """, [version])
    return out


def _stage_filings(store: ResearchStore) -> dict[str, int]:
    """``_ow_filings``: quarter-end 13F-HR filings (one per accession) with manager, mode, sequence and clock."""
    con = store.con
    cover = store.warehouse_has("thirteenf_cover_pages")
    acceptance = store.warehouse_has("sec_submissions", "acceptance_datetime")
    cover_join = ("LEFT JOIN thirteenf_cover_pages c ON c.accession_number=s.accession_number "
                  "AND c.source_period=s.source_period") if cover else ""
    is_amend = ("coalesce(lower(trim(c.is_amendment)) IN ('true', '1', 'yes', 'y'), "
                "upper(trim(s.submission_type)) LIKE '%/A')") if cover else "upper(trim(s.submission_type)) LIKE '%/A'"
    amend_no = "try_cast(nullif(trim(c.amendment_no), '') AS INTEGER)" if cover else "CAST(NULL AS INTEGER)"
    amend_type = "upper(coalesce(nullif(trim(c.amendment_type), ''), ''))" if cover else "''"
    accepted_cte = ("""accepted AS (
            SELECT x.accession_number, max(x.acceptance_datetime) AS accepted_at FROM sec_submissions x
            SEMI JOIN thirteenf_submissions t ON t.accession_number=x.accession_number GROUP BY 1
        ),""" if acceptance else
                    "accepted AS (SELECT CAST(NULL AS VARCHAR) AS accession_number, "
                    "CAST(NULL AS TIMESTAMP) AS accepted_at WHERE false),")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ow_filings AS
        WITH {accepted_cte} normalized AS (
            SELECT s.accession_number, s.source_period, lpad(regexp_replace(trim(s.cik), '[^0-9]', '', 'g'), 10, '0')
                       AS manager_cik,
                   s.period_of_report AS report_period, s.filing_date, {is_amend} AS is_amendment,
                   {amend_no} AS amendment_no, {amend_type} AS amendment_type
            FROM thirteenf_submissions s {cover_join}
            WHERE upper(trim(s.submission_type)) IN ('13F-HR', '13F-HR/A') AND s.cik IS NOT NULL
              AND s.period_of_report IS NOT NULL AND s.filing_date IS NOT NULL
              AND {_identity.quarter_end_sql('s.period_of_report')}
            -- an accession present in two data-set periods is one filing (the latest data set)
            QUALIFY row_number() OVER (PARTITION BY s.accession_number ORDER BY s.source_period DESC) = 1
        )
        SELECT n.accession_number, n.source_period, n.manager_cik, n.report_period, n.filing_date,
               CASE WHEN NOT n.is_amendment OR n.amendment_type LIKE '%RESTATEMENT%' THEN 'FULL' ELSE 'ADD' END
                   AS filing_mode,
               CAST(row_number() OVER (PARTITION BY n.manager_cik, n.report_period
                                       ORDER BY n.filing_date, coalesce(n.amendment_no, 0), n.accession_number)
                    AS INTEGER) AS filing_seq,
               greatest(a.accepted_at, CAST(n.filing_date AS TIMESTAMP) + INTERVAL {EVIDENCE_FLOOR_HOURS} HOUR)
                   AS clock
        FROM normalized n LEFT JOIN accepted a ON a.accession_number=n.accession_number
    """)
    filings = int(con.execute("SELECT count(*) FROM _ow_filings").fetchone()[0])
    rejected = con.execute(f"""
        SELECT count(*) FROM thirteenf_submissions s
        WHERE upper(trim(s.submission_type)) IN ('13F-HR', '13F-HR/A') AND s.period_of_report IS NOT NULL
          AND NOT {_identity.quarter_end_sql('s.period_of_report')}""").fetchone()
    return {"filings": filings, "rejected_non_quarter_end_filings": int(rejected[0])}


def _load_quarter(con: Any, quarter: dt.date) -> int:
    """Append one quarter's mapped eligible common positions to ``_ow_positions`` (streamed by quarter)."""
    con.execute(f"""
        INSERT INTO _ow_positions
        SELECT f.manager_cik, f.report_period, f.accession_number, m.owner_cik, sum(h.share_quantity) AS shares
        FROM _ow_filings f
        JOIN thirteenf_holdings h ON h.accession_number=f.accession_number AND h.source_period=f.source_period
        JOIN _oi_cusip_owner m
          ON m.cusip={_identity.cusip_key_sql('h.cusip')} AND m.report_period=f.report_period
         AND m.reason='{_identity.MAPPED}'
        WHERE f.report_period=? AND {_identity.holding_exclusion_sql('h')} IS NULL
          AND m.owner_cik IN (SELECT owner_cik FROM _ow_owners)
        GROUP BY f.manager_cik, f.report_period, f.accession_number, m.owner_cik
    """, [quarter])
    return int(con.execute("SELECT count(*) FROM _ow_positions WHERE report_period=?", [quarter]).fetchone()[0])


def _thirteenf_formation(con: Any, formation: dt.date, cutoff: dt.datetime, current: dt.date,
                         previous: dt.date, market_source: str) -> None:
    """Append the three 13F features of one formation's linked lines to ``_ow_vals``."""
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _ow_state AS
        WITH vis AS (
            SELECT * FROM _ow_filings WHERE report_period IN (?, ?) AND clock <= ?
        ), base AS (
            SELECT manager_cik, report_period, max(filing_seq) FILTER (WHERE filing_mode='FULL') AS base_seq
            FROM vis GROUP BY manager_cik, report_period
        ), live AS (
            SELECT v.* FROM vis v JOIN base b ON b.manager_cik=v.manager_cik AND b.report_period=v.report_period
            WHERE b.base_seq IS NOT NULL
              AND ((v.filing_mode='FULL' AND v.filing_seq=b.base_seq)
                   OR (v.filing_mode='ADD' AND v.filing_seq > b.base_seq))
        )
        SELECT p.report_period, p.owner_cik, p.manager_cik, sum(p.shares) AS shares, max(l.clock) AS clock
        FROM live l JOIN _ow_positions p ON p.accession_number=l.accession_number
        GROUP BY p.report_period, p.owner_cik, p.manager_cik
    """, [current, previous, cutoff])
    # Continuing managers: a visible FULL filing in both quarters (a lone ADD carries no state).
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _ow_managers AS
        SELECT manager_cik FROM _ow_filings
        WHERE report_period IN (?, ?) AND clock <= ? AND filing_mode='FULL'
        GROUP BY manager_cik HAVING count(DISTINCT report_period) = 2
    """, [current, previous, cutoff])
    continuing = int(con.execute("SELECT count(*) FROM _ow_managers").fetchone()[0])
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ow_share_keys AS
        SELECT l.security_id, q.anchor_date, CAST(? AS TIMESTAMP) AS cutoff
        FROM _ow_lines l CROSS JOIN (SELECT CAST(? AS DATE) AS anchor_date UNION ALL SELECT CAST(? AS DATE)) q
        WHERE l.formation_date=? AND l.owner_basis='{OWNER_BASIS_LINKED}'
    """, [cutoff, current, previous, formation])
    con.execute(_shares_sql(market_source))
    deadline = f"CAST(r AS TIMESTAMP) + INTERVAL {FILING_DEADLINE_DAYS} DAY + INTERVAL {EVIDENCE_FLOOR_HOURS} HOUR"
    weaker_basis = ("CASE WHEN c.identity_basis IS NULL OR p.identity_basis IS NULL "
                    "THEN coalesce(c.identity_basis, p.identity_basis) "
                    "WHEN c.identity_rank >= p.identity_rank THEN c.identity_basis ELSE p.identity_basis END")
    weaker_conditioning = (f"CASE WHEN '{_identity.CONDITIONING_SURVIVOR}' IN (c.sample_conditioning, "
                           f"p.sample_conditioning) THEN '{_identity.CONDITIONING_SURVIVOR}' "
                           f"ELSE coalesce(c.sample_conditioning, p.sample_conditioning) END")
    con.execute(f"""
        INSERT INTO _ow_vals
        WITH lines AS (
            SELECT security_id, owner_cik FROM _ow_lines WHERE formation_date=? AND owner_basis='{OWNER_BASIS_LINKED}'
        ), agg AS (
            SELECT report_period, owner_cik, sum(shares) AS inst_shares, max(clock) AS clock,
                   count(DISTINCT manager_cik) FILTER (WHERE shares > 0
                       AND manager_cik IN (SELECT manager_cik FROM _ow_managers)) AS continuing_holders
            FROM _ow_state GROUP BY report_period, owner_cik
        ), basis AS (
            SELECT report_period, owner_cik,
                   CASE WHEN bool_or(identity_basis='{_identity.IDENTITY_NAME_MATCH}') THEN '{_identity.IDENTITY_NAME_MATCH}'
                        ELSE '{_identity.IDENTITY_CUSIP_TICKER}' END || '/' ||
                   CASE WHEN bool_or(confidence='{_identity.CONFIDENCE_LOW}') THEN '{_identity.CONFIDENCE_LOW}'
                        WHEN bool_or(confidence='{_identity.CONFIDENCE_MEDIUM}') THEN '{_identity.CONFIDENCE_MEDIUM}'
                        ELSE '{_identity.CONFIDENCE_HIGH}' END AS identity_basis,
                   CASE WHEN bool_or(confidence='{_identity.CONFIDENCE_LOW}') THEN 3
                        WHEN bool_or(confidence='{_identity.CONFIDENCE_MEDIUM}') THEN 2 ELSE 1 END
                     + CASE WHEN bool_or(identity_basis='{_identity.IDENTITY_NAME_MATCH}') THEN 0.5 ELSE 0 END
                       AS identity_rank,
                   CASE WHEN bool_or(sample_conditioning='{_identity.CONDITIONING_SURVIVOR}')
                        THEN '{_identity.CONDITIONING_SURVIVOR}' ELSE '{_identity.CONDITIONING_DATED}' END
                       AS sample_conditioning
            FROM _oi_cusip_owner
            WHERE reason='{_identity.MAPPED}' AND owner_cik IS NOT NULL AND report_period IN (?, ?)
            GROUP BY report_period, owner_cik
        ), per_q AS (
            SELECT l.security_id, l.owner_cik, q.r, a.inst_shares, a.clock, a.continuing_holders, b.identity_basis,
                   b.identity_rank, b.sample_conditioning, s.shares, s.clock AS share_clock,
                   CASE WHEN a.owner_cik IS NULL THEN '{REASON_NO_CUSIP}' ELSE {_share_reason_sql('s')} END AS reason
            FROM lines l CROSS JOIN (SELECT CAST(? AS DATE) AS r UNION ALL SELECT CAST(? AS DATE)) q
            LEFT JOIN agg a ON a.owner_cik=l.owner_cik AND a.report_period=q.r
            LEFT JOIN basis b ON b.owner_cik=l.owner_cik AND b.report_period=q.r
            LEFT JOIN _ow_shares_at s ON s.security_id=l.security_id AND s.anchor_date=q.r
        ), io AS (
            SELECT *, CASE WHEN reason='{VALID}' THEN inst_shares / shares END AS io,
                   greatest({deadline}, clock, share_clock) AS io_clock
            FROM per_q
        ), cur AS (SELECT * FROM io WHERE r=CAST(? AS DATE)), prev AS (SELECT * FROM io WHERE r=CAST(? AS DATE))
        SELECT CAST(? AS DATE), c.security_id, c.owner_cik, '{FEATURE_IO}', c.io, c.reason,
               CASE WHEN c.reason='{VALID}' THEN c.io_clock END, c.r,
               CAST(c.r AS TIMESTAMP) + INTERVAL {FILING_DEADLINE_DAYS} DAY + INTERVAL {EVIDENCE_FLOOR_HOURS} HOUR,
               CASE WHEN c.io > 1 THEN '{FLAG_IO_ABOVE_ONE}' END, c.identity_basis, c.sample_conditioning
        FROM cur c
        UNION ALL
        SELECT CAST(? AS DATE), c.security_id, c.owner_cik, '{FEATURE_IO_CHANGE}',
               CASE WHEN c.reason='{VALID}' AND p.reason='{VALID}' THEN c.io - p.io END,
               CASE WHEN c.reason<>'{VALID}' THEN c.reason WHEN p.reason<>'{VALID}' THEN 'previous_quarter_' || p.reason
                    ELSE '{VALID}' END,
               CASE WHEN c.reason='{VALID}' AND p.reason='{VALID}' THEN greatest(c.io_clock, p.io_clock) END, c.r,
               CAST(c.r AS TIMESTAMP) + INTERVAL {FILING_DEADLINE_DAYS} DAY + INTERVAL {EVIDENCE_FLOOR_HOURS} HOUR,
               NULL, {weaker_basis}, {weaker_conditioning}
        FROM cur c JOIN prev p ON p.security_id=c.security_id
        UNION ALL
        SELECT CAST(? AS DATE), c.security_id, c.owner_cik, '{FEATURE_BREADTH}',
               CASE WHEN c.inst_shares IS NOT NULL AND p.inst_shares IS NOT NULL AND ? > 0
                    THEN (c.continuing_holders - p.continuing_holders) / CAST(? AS DOUBLE) END,
               CASE WHEN c.inst_shares IS NULL OR p.inst_shares IS NULL THEN '{REASON_NOT_BOTH}'
                    WHEN ? = 0 THEN '{REASON_NO_CONTINUING}' ELSE '{VALID}' END,
               CASE WHEN c.inst_shares IS NOT NULL AND p.inst_shares IS NOT NULL AND ? > 0
                    THEN greatest(CAST(c.r AS TIMESTAMP) + INTERVAL {FILING_DEADLINE_DAYS} DAY
                                  + INTERVAL {EVIDENCE_FLOOR_HOURS} HOUR, c.clock, p.clock) END, c.r,
               CAST(c.r AS TIMESTAMP) + INTERVAL {FILING_DEADLINE_DAYS} DAY + INTERVAL {EVIDENCE_FLOOR_HOURS} HOUR,
               NULL, {weaker_basis}, {weaker_conditioning}
        FROM cur c JOIN prev p ON p.security_id=c.security_id
    """, [formation, current, previous, current, previous, current, previous, formation, formation, formation,
          continuing, continuing, continuing, continuing])


def _build_thirteenf(store: ResearchStore, options: OwnershipFeatureOptions, market_source: str) -> dict[str, Any]:
    con = store.con
    staged = _stage_filings(store)
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ow_form_q AS
        WITH q AS (
            SELECT DISTINCT report_period,
                   CAST(report_period AS TIMESTAMP) + INTERVAL {FILING_DEADLINE_DAYS} DAY
                       + INTERVAL {EVIDENCE_FLOOR_HOURS} HOUR AS ready_at
            FROM _ow_filings
        )
        SELECT f.formation_date, f.cutoff,
               CASE WHEN q.report_period >= f.formation_date - CAST(? AS INTEGER) THEN q.report_period END AS r_cur
        FROM _ow_forms f ASOF LEFT JOIN q ON f.cutoff >= q.ready_at
    """, [options.max_13f_age_days])
    con.execute("CREATE OR REPLACE TEMP TABLE _ow_positions (manager_cik VARCHAR, report_period DATE, "
                "accession_number VARCHAR, owner_cik VARCHAR, shares DOUBLE)")
    loaded: set[dt.date] = set()
    position_rows = streamed = 0
    plan = con.execute("SELECT formation_date, cutoff, r_cur FROM _ow_form_q ORDER BY formation_date").fetchall()
    formed = 0
    for formation, cutoff, current in plan:
        if current is None:
            continue
        previous = con.execute("SELECT CAST(last_day(CAST(? AS DATE) - INTERVAL 3 MONTH) AS DATE)",
                               [current]).fetchone()[0]
        for quarter in (previous, current):
            if quarter not in loaded:
                position_rows += _load_quarter(con, quarter)
                loaded.add(quarter)
                streamed += 1
        for stale in [q for q in loaded if q < previous]:  # sliding window: at most a few quarters staged
            con.execute("DELETE FROM _ow_positions WHERE report_period=?", [stale])
            loaded.discard(stale)
        _thirteenf_formation(con, formation, cutoff, current, previous, market_source)
        formed += 1
    return {**staged, "formations_with_quarter": formed, "quarters_streamed": streamed,
            "position_rows_loaded": position_rows}


def _build_short_interest(store: ResearchStore, options: OwnershipFeatureOptions, market_source: str) -> dict[str, Any]:
    con = store.con
    revised = ("coalesce(trim(CAST(revision_flag AS VARCHAR)), '') <> ''"
               if store.warehouse_has("finra_short_interest", "revision_flag") else "false")
    split_flag = ("coalesce(trim(CAST(stock_split_flag AS VARCHAR)), '') <> ''"
                  if store.warehouse_has("finra_short_interest", "stock_split_flag") else "false")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ow_si_rows AS
        WITH rows AS (  -- one market-class row per symbol-settlement; several classes are not summed
            SELECT symbol, settlement_date, max(current_short_position_quantity) AS short_interest,
                   max(average_daily_volume_quantity) AS adv, max(available_at) AS loader_available_at,
                   bool_or({revised}) AS revised, bool_or({split_flag}) AS split_flag
            FROM finra_short_interest WHERE symbol IS NOT NULL AND settlement_date IS NOT NULL
            GROUP BY symbol, settlement_date HAVING count(*) = 1
        ), cycles AS (  -- FINRA's settlement calendar and each cycle's modeled publication day
            SELECT settlement_date, lead(settlement_date) OVER (ORDER BY settlement_date) AS next_settlement
            FROM (SELECT DISTINCT settlement_date FROM finra_short_interest WHERE settlement_date IS NOT NULL)
        ), published AS (
            SELECT c.settlement_date, c.next_settlement, b.day AS publication_day
            FROM cycles c JOIN _ow_days d ON d.day=c.settlement_date
            JOIN _ow_bday b ON b.business_le=d.business_le + CAST(? AS INTEGER)
        )
        SELECT r.*, m.security_id,
               lag(r.settlement_date) OVER (PARTITION BY r.symbol ORDER BY r.settlement_date) AS prior_settlement,
               -- the revised value is public only with the next cycle; with no next cycle it is
               -- withheld (greatest() skips NULLs, so the loader clock alone would backdate it)
               CASE WHEN r.revised AND n.publication_day IS NULL THEN NULL
                    WHEN r.revised
                    THEN greatest(r.loader_available_at, CAST(n.publication_day AS TIMESTAMP)
                                  + INTERVAL {PUBLICATION_HOUR} HOUR)
                    ELSE greatest(r.loader_available_at, CAST(p.publication_day AS TIMESTAMP)
                                  + INTERVAL {PUBLICATION_HOUR} HOUR) END AS publication_clock
        FROM rows r
        JOIN _oi_si_line m ON m.symbol=r.symbol AND m.settlement_date=r.settlement_date
         AND m.reason='{_identity.SI_MAPPED}'
        JOIN published p ON p.settlement_date=r.settlement_date
        LEFT JOIN published n ON n.settlement_date=p.next_settlement
    """, [options.si_publication_business_days])
    # R1d exact split ratios (the A8 real-split rule) on the mapped lines' own bars.
    if all(store.warehouse_has("equity_daily_bars", column) for column in ("close", "adjusted_close")):
        order = [column for column in ("available_at", "source") if store.warehouse_has("equity_daily_bars", column)]
        rank = f"({', '.join(order)})" if len(order) > 1 else (order[0] if order else "trade_date")
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE _ow_si_splits AS
            SELECT security_id, trade_date FROM (
                SELECT security_id, trade_date,
                       (lag(close) OVER w * adjusted_close) / (lag(adjusted_close) OVER w * close) AS day_factor
                FROM (
                    SELECT security_id, trade_date,
                           arg_max_null(close, {rank}) AS close,
                           arg_max_null(adjusted_close, {rank}) AS adjusted_close
                    FROM equity_daily_bars WHERE security_id IN (SELECT DISTINCT security_id FROM _ow_si_rows)
                    GROUP BY security_id, trade_date
                ) WHERE close > 0 AND adjusted_close > 0
                WINDOW w AS (PARTITION BY security_id ORDER BY trade_date)
            ) WHERE {_real_split_sql('day_factor')}
        """)
    else:
        con.execute("CREATE OR REPLACE TEMP TABLE _ow_si_splits (security_id VARCHAR, trade_date DATE)")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ow_si AS
        SELECT r.* EXCLUDE (prior_settlement),
               r.split_flag OR EXISTS (
                   SELECT 1 FROM _ow_si_splits x
                   WHERE x.security_id=r.security_id AND x.trade_date <= r.settlement_date
                     AND x.trade_date > greatest(coalesce(r.prior_settlement, r.settlement_date - {SI_PERIOD_MAX_DAYS}),
                                                 r.settlement_date - {SI_PERIOD_MAX_DAYS})) AS split_in_period
        FROM _ow_si_rows r
        WHERE r.publication_clock IS NOT NULL
    """)
    formations = [row[0] for row in con.execute("SELECT formation_date FROM _ow_forms ORDER BY 1").fetchall()]
    for start in range(0, len(formations), options.formation_chunk):
        batch = formations[start:start + options.formation_chunk]
        con.execute("""
            CREATE OR REPLACE TEMP TABLE _ow_si_pick AS
            SELECT l.formation_date, l.cutoff, l.security_id, l.owner_cik, s.settlement_date, s.short_interest,
                   s.adv, s.publication_clock, s.split_in_period
            FROM _ow_lines l JOIN _ow_si s
              ON s.security_id=l.security_id AND s.publication_clock <= l.cutoff
             AND s.settlement_date >= l.formation_date - CAST(? AS INTEGER)
            WHERE l.formation_date BETWEEN ? AND ?
            QUALIFY row_number() OVER (PARTITION BY l.formation_date, l.security_id
                                       ORDER BY s.settlement_date DESC, s.symbol) = 1
        """, [options.max_si_age_days, batch[0], batch[-1]])
        con.execute("CREATE OR REPLACE TEMP TABLE _ow_share_keys AS SELECT security_id, settlement_date AS anchor_date, "
                    "cutoff FROM _ow_si_pick")
        con.execute(_shares_sql(market_source))
        con.execute(f"""
            INSERT INTO _ow_vals
            SELECT p.formation_date, p.security_id, p.owner_cik, '{FEATURE_SIR}',
                   CASE WHEN {_share_reason_sql('s')}='{VALID}' THEN p.short_interest / s.shares END,
                   {_share_reason_sql('s')},
                   CASE WHEN {_share_reason_sql('s')}='{VALID}' THEN greatest(p.publication_clock, s.clock) END,
                   p.settlement_date, p.publication_clock, NULL, '{SI_IDENTITY_BASIS}', '{SI_CONDITIONING}'
            FROM _ow_si_pick p
            LEFT JOIN _ow_shares_at s ON s.security_id=p.security_id AND s.anchor_date=p.settlement_date
             AND s.cutoff=p.cutoff
            UNION ALL
            SELECT formation_date, security_id, owner_cik, '{FEATURE_DTC}',
                   CASE WHEN NOT split_in_period AND adv > 0 THEN short_interest / adv END,
                   CASE WHEN split_in_period THEN '{REASON_SPLIT}' WHEN adv > 0 THEN '{VALID}'
                        ELSE '{REASON_NO_ADV}' END,
                   CASE WHEN NOT split_in_period AND adv > 0 THEN publication_clock END,
                   settlement_date, publication_clock, NULL, '{SI_IDENTITY_BASIS}', '{SI_CONDITIONING}'
            FROM _ow_si_pick
        """)
    rows = con.execute("SELECT count(*), count(DISTINCT security_id), count(*) FILTER (WHERE split_in_period) "
                       "FROM _ow_si").fetchone()
    revised = con.execute("SELECT count(*) FILTER (WHERE revised), "
                          "count(*) FILTER (WHERE revised AND publication_clock IS NULL) FROM _ow_si_rows").fetchone()
    multi = con.execute("SELECT count(*) FROM (SELECT 1 FROM finra_short_interest GROUP BY symbol, settlement_date "
                        "HAVING count(*) > 1)").fetchone()
    return {"mapped_settlement_rows": int(rows[0]), "mapped_lines": int(rows[1]), "revised_rows": int(revised[0]),
            "revised_rows_withheld_no_next_cycle": int(revised[1]), "split_in_period_rows": int(rows[2]),
            "multi_market_class_rows_excluded": int(multi[0])}


def _insert_features(store: ResearchStore, version: str, options: OwnershipFeatureOptions) -> int:
    con = store.con
    formations = [row[0] for row in con.execute("SELECT formation_date FROM _ow_forms ORDER BY 1").fetchall()]
    thirteenf = _sql_list(THIRTEENF_FEATURES)
    for start in range(0, len(formations), options.formation_chunk):
        batch = formations[start:start + options.formation_chunk]
        with store.transaction():
            con.execute(f"""
                INSERT INTO research_ownership_features (ownership_version, formation_date, security_id, owner_cik,
                    feature_id, raw_value, reason, available_at, source_period, source_clock, value_flag,
                    identity_basis, owner_basis, sample_conditioning)
                WITH grid AS (
                    SELECT l.formation_date, l.security_id, l.owner_cik, l.owner_basis, f.feature_id
                    FROM _ow_lines l CROSS JOIN (SELECT unnest(?::VARCHAR[]) AS feature_id) f
                    WHERE l.formation_date BETWEEN ? AND ?
                )
                SELECT ?, g.formation_date, g.security_id, g.owner_cik, g.feature_id,
                       CASE WHEN v.reason='{VALID}' THEN v.value END,
                       coalesce(v.reason,
                                CASE WHEN g.feature_id IN ({thirteenf}) AND g.owner_basis<>'{OWNER_BASIS_LINKED}'
                                     THEN '{REASON_NO_OWNER}'
                                     WHEN g.feature_id IN ({thirteenf}) THEN '{REASON_NO_13F}'
                                     ELSE '{REASON_NO_SI}' END),
                       CASE WHEN v.reason='{VALID}' THEN v.available_at END,
                       v.source_period, v.source_clock, v.value_flag, v.identity_basis, g.owner_basis,
                       v.sample_conditioning
                FROM grid g LEFT JOIN _ow_vals v
                  ON v.formation_date=g.formation_date AND v.security_id=g.security_id AND v.feature_id=g.feature_id
            """, [list(OWNERSHIP_FEATURES), batch[0], batch[-1], version])
    rows = con.execute("SELECT count(*) FROM research_ownership_features WHERE ownership_version=?",
                       [version]).fetchone()
    return int(rows[0])


# ---------------------------------------------------------------------------
# Digests, checks
# ---------------------------------------------------------------------------

def _features_digest(con: Any, version: str) -> str:
    formations = [row[0] for row in con.execute(
        "SELECT DISTINCT formation_date FROM research_ownership_features WHERE ownership_version=? ORDER BY 1",
        [version]).fetchall()]
    digests: list[str] = []
    for start in range(0, len(formations), _DIGEST_CHUNK):
        batch = formations[start:start + _DIGEST_CHUNK]
        digests.extend(str(row[1]) for row in con.execute("""
            SELECT formation_date, sha256(string_agg(md5(concat_ws('|', security_id, coalesce(owner_cik, ''),
                       feature_id, coalesce(printf('%.12e', raw_value), ''), reason,
                       coalesce(CAST(available_at AS VARCHAR), ''), coalesce(CAST(source_period AS VARCHAR), ''),
                       coalesce(CAST(source_clock AS VARCHAR), ''), coalesce(value_flag, ''),
                       coalesce(identity_basis, ''), coalesce(owner_basis, ''), coalesce(sample_conditioning, ''))),
                   '' ORDER BY security_id, feature_id))
            FROM research_ownership_features WHERE ownership_version=? AND formation_date BETWEEN ? AND ?
            GROUP BY formation_date ORDER BY formation_date
        """, [version, batch[0], batch[-1]]).fetchall())
    return _sha("\n".join(digests))


def _identity_digest(con: Any, version: str) -> str:
    row = con.execute("""
        SELECT sha256(coalesce(string_agg(md5(concat_ws('|', source, source_key, CAST(source_period AS VARCHAR),
                   coalesce(name_key, ''), coalesce(owner_cik, ''), coalesce(security_id, ''),
                   coalesce(identity_basis, ''), coalesce(confidence, ''), reason,
                   coalesce(sample_conditioning, ''))), '' ORDER BY source, source_key, source_period), ''))
        FROM research_ownership_identity WHERE ownership_version=?
    """, [version]).fetchone()
    return str(row[0])


def _checks(con: Any, version: str) -> dict[str, int]:
    """Point-in-time, identity and grain invariants; every count must be zero."""
    si = _sql_list(SI_FEATURES)
    thirteenf = _sql_list(THIRTEENF_FEATURES)
    checks = {
        "feature_visible_after_cutoff": ("""
            SELECT count(*) FROM research_ownership_features x
            JOIN research_ownership_versions v ON v.ownership_version=x.ownership_version
            JOIN research_panel_calendar c ON c.run_id=v.panel_run_id AND c.formation_date=x.formation_date
            WHERE x.ownership_version=? AND x.available_at IS NOT NULL AND x.available_at > c.cutoff""", [version]),
        "short_interest_before_publication": (f"""
            SELECT count(*) FROM research_ownership_features WHERE ownership_version=? AND feature_id IN ({si})
              AND available_at IS NOT NULL
              AND (source_clock IS NULL OR available_at < source_clock
                   OR source_clock <= CAST(source_period AS TIMESTAMP) + INTERVAL 1 DAY)""", [version]),
        "thirteenf_before_filing_deadline": (f"""
            SELECT count(*) FROM research_ownership_features WHERE ownership_version=? AND feature_id IN ({thirteenf})
              AND available_at IS NOT NULL
              AND available_at < CAST(source_period AS TIMESTAMP) + INTERVAL {FILING_DEADLINE_DAYS} DAY
                                 + INTERVAL {EVIDENCE_FLOOR_HOURS} HOUR""", [version]),
        "thirteenf_value_without_owner_mapping": (f"""
            SELECT count(*) FROM research_ownership_features x WHERE x.ownership_version=?
              AND x.feature_id IN ({thirteenf}) AND x.raw_value IS NOT NULL
              AND NOT EXISTS (SELECT 1 FROM research_ownership_identity i
                              WHERE i.ownership_version=x.ownership_version AND i.source='13f'
                                AND i.owner_cik=x.owner_cik AND i.source_period=x.source_period
                                AND i.reason='{_identity.MAPPED}')""", [version]),
        "owner_feature_on_unlinked_line": (f"""
            SELECT count(*) FROM research_ownership_features WHERE ownership_version=?
              AND feature_id IN ({thirteenf}) AND raw_value IS NOT NULL
              AND owner_basis IS DISTINCT FROM '{OWNER_BASIS_LINKED}'""", [version]),
        "feature_grain": ("""
            SELECT count(*) FROM (SELECT formation_date, security_id, feature_id FROM research_ownership_features
                                  WHERE ownership_version=? GROUP BY ALL HAVING count(*) > 1)""", [version]),
    }
    return {name: int(con.execute(sql, params).fetchone()[0]) for name, (sql, params) in checks.items()}


def validate_ownership_version(store: ResearchStore, ownership_version: str) -> dict[str, int]:
    """Re-run the seal invariants and recompute both content digests of a sealed version."""
    con = store.con
    row = con.execute("SELECT status, identity_sha256, features_sha256, query_version FROM research_ownership_versions "
                      "WHERE ownership_version=?", [ownership_version]).fetchone()
    if row is None:
        raise OwnershipStoreError(f"ownership version {ownership_version} is absent")
    status, identity_sha, features_sha, query_version = row
    if query_version != QUERY_VERSION:
        raise OwnershipStoreError(f"ownership version {ownership_version} was built by {query_version!r}; only "
                                  f"{QUERY_VERSION!r} versions are valid (rebuild it)")
    if status not in SEALED_STATUSES:
        raise OwnershipStoreError(f"ownership version {ownership_version} is {status!r}, not sealed")
    checks = _checks(con, ownership_version)
    bad = {name: n for name, n in checks.items() if n}
    if bad:
        raise OwnershipStoreError(f"ownership version {ownership_version} violates {bad}")
    if _identity_digest(con, ownership_version) != identity_sha:
        raise OwnershipStoreError(f"ownership version {ownership_version}: identity rows differ from their digest")
    if _features_digest(con, ownership_version) != features_sha:
        raise OwnershipStoreError(f"ownership version {ownership_version}: feature rows differ from their digest")
    return checks


# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

def _drop_temps(con: Any) -> None:
    for table in _TEMP_TABLES:
        with contextlib.suppress(duckdb.Error):
            con.execute(f"DROP TABLE IF EXISTS {table}")


def _stored(con: Any, version: str, reused: bool) -> OwnershipFeatureResult:
    status, blockers, diagnostic = con.execute(
        "SELECT status, blockers_json, diagnostic_json FROM research_ownership_versions WHERE ownership_version=?",
        [version]).fetchone()
    rows = con.execute("SELECT count(*) FROM research_ownership_features WHERE ownership_version=?",
                       [version]).fetchone()
    return OwnershipFeatureResult(version, str(status), reused, int(rows[0]), tuple(json.loads(blockers)),
                                  json.loads(diagnostic) if diagnostic else {})


def _reason_counts(con: Any, version: str) -> dict[str, Any]:
    features: dict[str, dict[str, int]] = {}
    for feature, reason, n in con.execute("""
        SELECT feature_id, reason, count(*) FROM research_ownership_features WHERE ownership_version=?
        GROUP BY 1, 2 ORDER BY 1, 2
    """, [version]).fetchall():
        features.setdefault(str(feature), {})[str(reason)] = int(n)
    flagged, survivor = con.execute(f"""
        SELECT count(*) FILTER (WHERE value_flag=?),
               count(*) FILTER (WHERE raw_value IS NOT NULL AND feature_id IN ({_sql_list(THIRTEENF_FEATURES)})
                                AND sample_conditioning=?)
        FROM research_ownership_features WHERE ownership_version=?
    """, [FLAG_IO_ABOVE_ONE, _identity.CONDITIONING_SURVIVOR, version]).fetchone()
    by_basis = {str(basis): int(n) for basis, n in con.execute("""
        SELECT owner_basis, count(DISTINCT formation_date || '|' || security_id) FROM research_ownership_features
        WHERE ownership_version=? GROUP BY 1 ORDER BY 1""", [version]).fetchall()}
    return {"features": features, "io_above_one": int(flagged), "survivor_conditioned_13f_values": int(survivor),
            "line_formations_by_owner_basis": by_basis}


def build_ownership_features(store: ResearchStore, options: OwnershipFeatureOptions) -> OwnershipFeatureResult:
    """Build (or return the sealed) ownership/short-interest feature version of one sealed panel run."""
    options = _validate_options(options)
    missing = [table for table in REQUIRED_WAREHOUSE_TABLES if not store.warehouse_has(table)]
    if missing:
        raise OwnershipStoreError(f"warehouse lacks {missing}")
    con = store.con
    ensure_ownership_schema(con)
    context = _panel_context(store, options)
    calendar = _calendar_keys()
    has_13f = all(store.warehouse_has(table) for table in THIRTEENF_TABLES)
    has_si = store.warehouse_has("finra_short_interest")
    inputs = _input_fingerprints(store, calendar, context["market_source"])
    spec = _spec(options, context, calendar, inputs)
    code_sha = _code_sha()
    spec_json = _canonical(spec)
    version = _sha(_canonical({"spec": spec, "code_sha256": code_sha}))
    existing = con.execute("SELECT status FROM research_ownership_versions WHERE ownership_version=?",
                           [version]).fetchone()
    if existing is not None and existing[0] in SEALED_STATUSES:
        return _stored(con, version, reused=True)
    with store.transaction():
        for table in _VERSION_TABLES:
            con.execute(f"DELETE FROM {table} WHERE ownership_version=?", [version])
        con.execute("""
            INSERT INTO research_ownership_versions (ownership_version, status, basis, panel_run_id, panel_sha256,
                query_version, spec_json, spec_sha256, code_sha256, inputs_json, blockers_json, created_at)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, '[]', ?)
        """, [version, STATUS_BUILDING, context["basis"], options.panel_run_id, context["panel_sha256"],
              QUERY_VERSION, spec_json, _sha(spec_json), code_sha, _canonical(inputs), _now()])
    try:
        diagnostic: dict[str, Any] = {"panel": _stage_panel(con, options.panel_run_id, calendar)}
        con.execute(f"CREATE OR REPLACE TEMP TABLE _ow_vals ({_VALS_COLUMNS})")
        diagnostic["identity"] = _stage_identity(store, version, has_13f, has_si)
        if has_13f:
            diagnostic["thirteenf"] = _build_thirteenf(store, options, context["market_source"])
        if has_si:
            diagnostic["short_interest"] = _build_short_interest(store, options, context["market_source"])
        feature_rows = _insert_features(store, version, options)
        diagnostic["reasons"] = _reason_counts(con, version)
        checks = _checks(con, version)
        diagnostic["checks"] = checks
        bad = {name: n for name, n in checks.items() if n}
        if bad:
            raise OwnershipStoreError(f"ownership version {version} violates {bad}")
        blockers = [IDENTITY_BLOCKER, PUBLICATION_BLOCKER]
        if context["basis"] == _panel.BASIS_RECONSTRUCTED:
            blockers.append(RECONSTRUCTED_BLOCKER)
        if diagnostic["panel"]["unlinked_line_formations"]:
            blockers.append(UNLINKED_LINES_BLOCKER)
        if not has_13f:
            blockers.append("thirteenf_tables_absent")
        if not has_si:
            blockers.append("finra_short_interest_absent")
        owner_map = diagnostic["identity"].get("cusip_owner_map", {})
        unmapped = owner_map.get("unmapped", {})
        if unmapped:
            blockers.append(f"thirteenf_unmapped_cusip_periods:{sum(unmapped.values())}/{owner_map['cusip_periods']}")
        # I-1: 13F coverage through a current snapshot is conditioned on CUSIP survival. The label stays
        # while any value is survivor-conditioned (a partial C9 pilot must not clear it) and whenever no
        # dated name window (0327) exists at all.
        survivor = diagnostic["reasons"]["survivor_conditioned_13f_values"]
        if has_13f and (survivor or not owner_map.get("dated_name_windows")):
            blockers.append(f"{SURVIVOR_BLOCKER}:{survivor}")
        revised = diagnostic.get("short_interest", {}).get("revised_rows", 0)
        if revised:
            blockers.append(f"{REVISED_BLOCKER}:{revised}")
        status = STATUS_SEALED if feature_rows else STATUS_EMPTY
        with store.transaction():
            con.execute("""
                UPDATE research_ownership_versions SET status=?, blockers_json=?, diagnostic_json=?,
                       identity_sha256=?, features_sha256=?, finished_at=? WHERE ownership_version=?
            """, [status, _canonical(blockers), _canonical(diagnostic), _identity_digest(con, version),
                  _features_digest(con, version), _now(), version])
    except Exception as exc:
        with contextlib.suppress(duckdb.Error):
            con.execute("UPDATE research_ownership_versions SET status=?, diagnostic_json=?, finished_at=? "
                        "WHERE ownership_version=?", [STATUS_FAILED, _canonical({"error": repr(exc)[:2000]}),
                                                       _now(), version])
        raise
    finally:
        _drop_temps(con)
    return _stored(con, version, reused=False)


__all__ = [
    "FEATURE_BREADTH",
    "FEATURE_DTC",
    "FEATURE_IO",
    "FEATURE_IO_CHANGE",
    "FEATURE_SIR",
    "FLAG_IO_ABOVE_ONE",
    "OWNERSHIP_FEATURES",
    "OWNERSHIP_SCHEMA_VERSION",
    "QUERY_VERSION",
    "OwnershipFeatureOptions",
    "OwnershipFeatureResult",
    "OwnershipStoreError",
    "build_ownership_features",
    "ensure_ownership_schema",
    "validate_ownership_version",
]
