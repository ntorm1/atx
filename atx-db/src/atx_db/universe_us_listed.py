"""Point-in-time US-listed equity universe (strict and labeled-reconstructed variants).

Implements the Tier-1 spec's ``universe_us_listed(as_of_date)``: securities with at
least one trade in the trailing ``lookback_days`` trading sessions, listed on NYSE,
NASDAQ, NYSE American, ARCA or BATS, whose security type is common stock, an ADR, a
REIT or an LP. ETFs, ETNs, funds/closed-end funds, warrants, rights, units, preferreds
and notes are excluded. Securities with no resolved CIK stay in the universe (it is
the market universe) but carry ``has_cik = false`` so the fundamentals universe can
subset without losing the tail.

Two variants are rebuilt by one :func:`refresh_universe_us_listed` call:

``us_listed_v1`` (strict, point-in-time)
    Every per-session decision is formed at a *decision cutoff* and selects each
    input as of that cutoff **before** revision selection:

    * listing evidence -- the newest ``nasdaq_symbol_directory`` snapshot for the
      bar's symbol with ``as_of_date <= trade_date`` whose own clock
      ``coalesce(available_at, source_loaded_at)`` is at or before the session cutoff
      (``trade_date`` 22:00). When no snapshot dated on/before the session was known
      by then, the decision is *late-known*: its cutoff becomes the earliest clock at
      which such a snapshot was known, and it is emitted with that clock -- never
      backdated to the session;
    * CIK -- only from dated ``security_identifier_history`` rows (``id_type='CIK'``,
      ``valid_from <= trade_date < valid_to``, ``as_of_date <= trade_date``,
      non-NULL ``available_at`` at/before the cutoff). Never from the current
      ``securities.entity_id``. Zero visible values -> ``member_no_cik``; several
      distinct or malformed values -> ``member_conflicting_cik``;
    * market cap -- the newest ``market_daily_metrics`` revision *visible at the
      cutoff* (a later correction is never used, an earlier visible one is never
      discarded because a later one exists).

    The decision's ``available_at`` is the maximum of every selected input clock (bar,
    directory, CIK, market cap) and, for decile-ranked rows, of the ranking cutoff.

``us_listed_reconstructed_v1`` (labeled reconstruction, RX1)
    Current directory attributes are backcast per security through its *latest*
    symbol, CIK is the current identifier mapping, availability is modeled at bar
    close. Every row's ``rules_json`` carries ``evidence_status=reconstructed,
    identity_basis=current_ticker_unverified, availability_basis=modeled_backcast``;
    names without listing evidence are retained with reason
    ``reconstructed_no_listing_evidence`` (type ``unknown``, venue ``UNKNOWN``). It is
    for panel/coverage measurement and research only and must never be presented as
    verified historical membership.

Execution is bounded: only the distinct directory name/flag/venue tuples are
classified in Python. Everything else runs inside DuckDB in per-security hash
batches of at most ``max_bars_per_batch`` bar rows: each batch's decisions are
compressed to gaps-and-islands intervals immediately and dropped, so the only
cross-batch state is the narrow decile population; market-cap deciles (SQL
``ntile``) are then attached to interval starts. No security-day frame is ever
materialized in pandas, so Python memory is independent of the bar count.
``options.as_of_date`` is the knowledge cutoff: inputs whose clock is after the end
of that date, and sessions after it, are invisible to the build.
"""

from __future__ import annotations

import datetime as dt
import math
import re
import time
from collections import Counter
from dataclasses import asdict, dataclass, field

import duckdb
import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .market_daily import MARKET_DAILY_SOURCE_NAME
from .warehouse import json_dumps, quality_check, symbol_key

UNIVERSE_SOURCE_NAME = "atx-db us-listed universe builder"
DEFAULT_US_LISTED_UNIVERSE_ID = "us_listed_v1"
RECONSTRUCTED_US_LISTED_UNIVERSE_ID = "us_listed_reconstructed_v1"

STRICT_VARIANT = "strict"
RECONSTRUCTED_VARIANT = "reconstructed"

CLASSIFICATION_VERSION = "directory_name_regex_v2"
# A session's decision cutoff is trade_date + 22:00 (the same modeled clock the bulk
# price loader stamps on bars).
DECISION_CUTOFF_HOURS = 22
DEFAULT_MAX_BARS_PER_BATCH = 2_000_000

UNKNOWN_SECURITY_TYPE = "unknown"
UNKNOWN_EXCHANGE_CODE = "UNKNOWN"

REASON_MEMBER = "member"
REASON_MEMBER_NO_CIK = "member_no_cik"
REASON_MEMBER_CONFLICTING_CIK = "member_conflicting_cik"
REASON_RECONSTRUCTED_NO_LISTING_EVIDENCE = "reconstructed_no_listing_evidence"
MEMBERSHIP_REASONS: tuple[str, ...] = (
    REASON_MEMBER,
    REASON_MEMBER_NO_CIK,
    REASON_MEMBER_CONFLICTING_CIK,
    REASON_RECONSTRUCTED_NO_LISTING_EVIDENCE,
)

# nasdaq_symbol_directory.exchange is 'NASDAQ' for nasdaqlisted.txt and the raw CQS
# venue letter for otherlisted.txt. 'V' (IEX) is deliberately absent: the spec's
# eligible venue set is NYSE, NASDAQ, NYSE American, ARCA and BATS.
EXCHANGE_CODE_BY_DIRECTORY_EXCHANGE: dict[str, str] = {
    "NASDAQ": "XNAS",
    "N": "XNYS",
    "A": "XASE",
    "P": "ARCX",
    "Z": "BATS",
}
EXCHANGE_LABELS: dict[str, str] = {
    "XNAS": "NASDAQ",
    "XNYS": "NYSE",
    "XASE": "NYSE American",
    "ARCX": "ARCA",
    "BATS": "BATS",
}
ELIGIBLE_EXCHANGE_CODES: tuple[str, ...] = tuple(sorted(EXCHANGE_LABELS))
# Strict eligibility. ``common_unverified`` (no positive common-share evidence in the
# name) is strict-ineligible; the labeled reconstruction retains it as its own type.
ELIGIBLE_SECURITY_TYPES: tuple[str, ...] = ("ADR", "LP", "REIT", "common")
RECONSTRUCTED_ELIGIBLE_SECURITY_TYPES: tuple[str, ...] = (*ELIGIBLE_SECURITY_TYPES, "common_unverified")

# Ordered classification table; first match wins. Every exclusion (ETN, fund /
# closed-end fund, preferred, warrant, right, unit, note) precedes every eligible type,
# so an exclusion can never be masked by an eligible pattern ("XYZ Fund LP" is a fund,
# a preferred ADS is a preferred, a partnership *unit* is a unit).
SECURITY_TYPE_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("ETN", re.compile(r"\bETNS?\b|EXCHANGE[- ]TRADED NOTE")),
    ("fund", re.compile(r"\bETFS?\b|\bFUND\b|CLOSED[- ]END|\bINDEX TRUST\b|\bPORTFOLIO\b")),
    (
        "preferred",
        re.compile(r"\bPREFERRED\b|\bPREFERENCE\b|\bPFD\b|(?<!AMERICAN )DEPOSITARY (?:SHARE|SHS|SHARES)"),
    ),
    ("warrant", re.compile(r"\bWARRANTS?\b|\bWTS?\b")),
    ("right", re.compile(r"\bRIGHTS?\b")),
    ("unit", re.compile(r"\bUNITS?\b")),
    ("note", re.compile(r"\bNOTES?\b|\bDEBENTURES?\b|\bBONDS?\b|\bSUBORDINATED\b")),
    ("ADR", re.compile(r"AMERICAN DEPOSITARY|AMERICAN DEPOSITORY|\bADR\b|\bADS\b")),
    ("REIT", re.compile(r"\bREIT\b|REAL ESTATE INVESTMENT TRUST")),
    ("LP", re.compile(r"\bL\.?P\.?\b|LIMITED PARTNERSHIP")),
)
# ``common`` requires positive common/ordinary-share evidence in the listing name;
# anything else that survived the exclusions is ``common_unverified``.
COMMON_EVIDENCE_PATTERN: re.Pattern[str] = re.compile(
    r"\bCOMMON (?:STOCKS?|SHARES?|SHS)\b"
    r"|\bORDINARY (?:SHARES?|SHS|STOCK)\b"
    r"|\bCAPITAL STOCK\b"
    r"|\bVOTING (?:SHARES?|STOCK)\b"
)


def _clean_name(value: object) -> str:
    if value is None:
        return ""
    try:
        if pd.isna(value):
            return ""
    except (TypeError, ValueError):
        pass
    return " ".join(str(value).upper().split())


def _flag(value: object) -> bool:
    if isinstance(value, bool):
        return value
    if value is None:
        return False
    try:
        if pd.isna(value):
            return False
    except (TypeError, ValueError):
        pass
    return str(value).strip().lower() in {"1", "t", "true", "y", "yes"}


def classify_security_type(
    security_name: object,
    *,
    etf: object = None,
    test_issue: object = None,
) -> str:
    """Classify one listing line into a security type.

    Precedence: the directory's own ``test_issue`` and ``etf`` booleans outrank the
    name; then :data:`SECURITY_TYPE_PATTERNS` (all exclusions before ADR/REIT/LP);
    then ``"common"`` only with positive :data:`COMMON_EVIDENCE_PATTERN` evidence,
    otherwise ``"common_unverified"``. An empty name is ``"unknown"``. Pure and total.
    """

    if _flag(test_issue):
        return "test"
    if _flag(etf):
        return "ETF"
    name = _clean_name(security_name)
    if not name:
        return UNKNOWN_SECURITY_TYPE
    for label, pattern in SECURITY_TYPE_PATTERNS:
        if pattern.search(name):
            return label
    if COMMON_EVIDENCE_PATTERN.search(name):
        return "common"
    return "common_unverified"


def exchange_code_for(directory_exchange: object) -> str | None:
    """Map ``nasdaq_symbol_directory.exchange`` to an eligible MIC-style code, or None."""

    raw = _clean_name(directory_exchange)
    if not raw:
        return None
    return EXCHANGE_CODE_BY_DIRECTORY_EXCHANGE.get(raw)


UNIVERSE_OUTPUT_COLUMNS: tuple[str, ...] = (
    "membership_id",
    "universe_id",
    "security_id",
    "symbol",
    "valid_from",
    "valid_to",
    "available_at",
    "security_type",
    "exchange_code",
    "has_cik",
    "cik",
    "market_cap_decile",
    "reason",
    "rules_json",
    "decision_count",
    "as_of_date",
    "source",
    "run_id",
)

# A new interval opens on any change of these (plus a session gap > lookback_days).
# ``late_key`` is the decision's available_at when that is after its own session
# cutoff (NULL for on-time decisions), so a late-known run never shares an interval --
# and therefore an interval clock -- with on-time decisions or with a later late load.
INTERVAL_STATE_COLUMNS: tuple[str, ...] = (
    "symbol",
    "security_type",
    "exchange_code",
    "has_cik",
    "cik",
    "reason",
    "late_key",
)


@dataclass(frozen=True)
class UniverseUsListedOptions:
    universe_id: str = DEFAULT_US_LISTED_UNIVERSE_ID
    name: str = "US-listed equity universe"
    description: str = (
        "Securities with at least one trade in the trailing lookback window on NYSE, "
        "NASDAQ, NYSE American, ARCA or BATS, restricted to common stock, ADRs, REITs "
        "and LPs. Members without a resolved CIK are retained and flagged."
    )
    lookback_days: int = 20
    market_source: str = MARKET_DAILY_SOURCE_NAME
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    security_ids: tuple[str, ...] | None = None
    source: str = UNIVERSE_SOURCE_NAME
    # Knowledge cutoff: inputs known after the end of this date are not used.
    as_of_date: dt.date | None = None
    run_id: str | None = None
    # RX1 labeled reconstruction; None disables the variant.
    reconstructed_universe_id: str | None = RECONSTRUCTED_US_LISTED_UNIVERSE_ID
    reconstructed_name: str = "US-listed equity universe (reconstructed, unverified)"
    reconstructed_description: str = (
        "Labeled reconstruction for panel/coverage measurement and research: current "
        "directory attributes backcast per security by its latest symbol, current CIK "
        "mapping, availability modeled at bar close. Not verified historical membership "
        "and never eligible for certification."
    )
    max_bars_per_batch: int = DEFAULT_MAX_BARS_PER_BATCH


@dataclass
class UniverseBuildSummary:
    """What one :func:`build_universe_us_listed` run wrote and why rows were dropped."""

    rows_by_universe: dict[str, int] = field(default_factory=dict)
    intervals_with_cik: dict[str, int] = field(default_factory=dict)
    dispositions: dict[str, dict[str, int]] = field(default_factory=dict)
    late_decisions: dict[str, int] = field(default_factory=dict)
    batch_count: int = 0
    bar_rows: int = 0
    diagnostics: dict[str, object] = field(default_factory=dict)
    phase_seconds: dict[str, float] = field(default_factory=dict)

    def tick(self, phase: str, started: float) -> float:
        now = time.perf_counter()
        self.phase_seconds[phase] = round(self.phase_seconds.get(phase, 0.0) + now - started, 3)
        return now

    @property
    def total_rows(self) -> int:
        return int(sum(self.rows_by_universe.values()))


def _knowledge_bounds(options: UniverseUsListedOptions) -> tuple[dt.datetime, dt.date]:
    if options.as_of_date is None:
        return dt.datetime(9999, 12, 31, 23, 59, 59), dt.date(9999, 12, 31)
    return dt.datetime.combine(options.as_of_date, dt.time.max), options.as_of_date


def _rules(options: UniverseUsListedOptions, variant: str = STRICT_VARIANT) -> dict[str, object]:
    common: dict[str, object] = {
        "variant": variant,
        "lookback_days": options.lookback_days,
        "eligible_exchange_codes": list(ELIGIBLE_EXCHANGE_CODES),
        "market_source": options.market_source,
        "classification_basis": CLASSIFICATION_VERSION,
        "knowledge_cutoff_date": None if options.as_of_date is None else options.as_of_date.isoformat(),
        "decile_basis": (
            "market_daily_metrics.market_cap at valid_from, newest revision visible at the "
            "decision cutoff, ranked (ntile 10) among eligible common members of the "
            "session known by the same cutoff, tie-broken by security_id"
        ),
    }
    if variant == STRICT_VARIANT:
        return common | {
            "eligible_security_types": list(ELIGIBLE_SECURITY_TYPES),
            "evidence_status": "pit_snapshot",
            "identity_basis": "dated_identifier_history",
            "availability_basis": "max_selected_input_clock",
            "listing_basis": "nasdaq_directory_snapshot_carried_forward",
            "decision_cutoff": (
                f"trade_date {DECISION_CUTOFF_HOURS:02d}:00, or the first clock at which a "
                "directory snapshot dated on/before the session was known"
            ),
        }
    return common | {
        "eligible_security_types": list(RECONSTRUCTED_ELIGIBLE_SECURITY_TYPES),
        "evidence_status": "reconstructed",
        "identity_basis": "current_ticker_unverified",
        "availability_basis": "modeled_backcast",
        "listing_basis": "current_directory_backcast_by_latest_symbol",
        "backcast_anchor": (
            "the security traded under its latest symbol within lookback_days sessions "
            "of the current snapshot date; otherwise reconstructed_no_listing_evidence"
        ),
        "certification_eligible": False,
    }


def _empty_output() -> pd.DataFrame:
    return pd.DataFrame(columns=list(UNIVERSE_OUTPUT_COLUMNS))


def _sql_list(values: tuple[str, ...]) -> str:
    return ", ".join("'" + value.replace("'", "''") + "'" for value in values)


_PARAM_PATTERN = re.compile(r"\$([A-Za-z_][A-Za-z0-9_]*)")


def _run(con: duckdb.DuckDBPyConnection, sql: str, params: dict[str, object]) -> duckdb.DuckDBPyConnection:
    """Execute ``sql`` binding only the named parameters it references."""

    names = set(_PARAM_PATTERN.findall(sql))
    return con.execute(sql, {name: params[name] for name in names})


# ---------------------------------------------------------------------------------------
# Interval compression (gaps and islands), shared by the store build and the pure helper.
# ---------------------------------------------------------------------------------------


def build_universe_interval_sql(decisions_sql: str, sessions_table: str) -> str:
    """Gaps-and-islands SQL compressing per-session decisions into validity intervals.

    ``decisions_sql`` yields one row per (security_id, session) with ``trade_date,
    session_rank, symbol, available_at, security_type, exchange_code, has_cik, cik,
    reason, late_key, market_cap_decile``. A new interval opens when any of
    :data:`INTERVAL_STATE_COLUMNS` changes or when the session-rank gap to the previous
    decision exceeds ``$lookback_days`` (the spec's "at least one trade in the prior N
    trading days" rule on the grid). A closing interval is extended forward through
    ``last_rank + lookback_days - 1``, bounded at the session before the successor
    interval's first decision when one exists (so a state change inside the lookback
    window never drops membership days and a real gap still leaves a hole), otherwise
    at the archive end -- ``valid_to`` NULL (open) when the extension reaches the last
    known session. ``available_at`` and ``market_cap_decile`` are taken from the
    interval's first decision. Parameters: ``$lookback_days, $universe_id,
    $rules_json, $source, $run_id``.
    """

    change = " OR ".join(f"{column} IS DISTINCT FROM prev_{column}" for column in INTERVAL_STATE_COLUMNS)
    lags = ",\n               ".join(f"lag({column}) OVER w AS prev_{column}" for column in INTERVAL_STATE_COLUMNS)
    constants = ",\n               ".join(
        f"first({column} ORDER BY session_rank) AS {column}" for column in INTERVAL_STATE_COLUMNS
    )
    return f"""
        WITH decisions AS ({decisions_sql}),
        grid AS (SELECT max(session_rank) AS last_session_rank FROM {sessions_table}),
        ordered AS (
            SELECT decisions.*,
                   lag(session_rank) OVER w AS prev_rank,
                   {lags}
            FROM decisions
            WINDOW w AS (PARTITION BY security_id ORDER BY session_rank)
        ),
        numbered AS (
            SELECT *,
                   sum(CASE WHEN prev_rank IS NULL
                              OR session_rank - prev_rank > $lookback_days
                              OR {change}
                            THEN 1 ELSE 0 END)
                       OVER (PARTITION BY security_id ORDER BY session_rank
                             ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS interval_no
            FROM ordered
        ),
        intervals AS (
            SELECT security_id, interval_no,
                   min(session_rank) AS first_rank,
                   max(session_rank) AS last_rank,
                   count(*) AS decision_count,
                   min(trade_date) AS valid_from,
                   first(available_at ORDER BY session_rank) AS available_at,
                   first(market_cap_decile ORDER BY session_rank) AS market_cap_decile,
                   {constants}
            FROM numbered
            GROUP BY security_id, interval_no
        ),
        closed AS (
            SELECT i.*,
                   CASE
                       WHEN lead(i.first_rank) OVER nxt IS NOT NULL
                           THEN least(i.last_rank + $lookback_days - 1, lead(i.first_rank) OVER nxt - 1)
                       WHEN i.last_rank + $lookback_days - 1 >= g.last_session_rank THEN NULL
                       ELSE i.last_rank + $lookback_days - 1
                   END AS close_rank
            FROM intervals i CROSS JOIN grid g
            WINDOW nxt AS (PARTITION BY i.security_id ORDER BY i.interval_no)
        )
        SELECT
            sha256(concat_ws('|', CAST($universe_id AS VARCHAR), CAST($source AS VARCHAR),
                             c.security_id, CAST(c.valid_from AS VARCHAR), coalesce(c.symbol, ''),
                             c.security_type, c.exchange_code, CAST(c.has_cik AS VARCHAR),
                             coalesce(c.cik, ''), c.reason,
                             coalesce(CAST(c.late_key AS VARCHAR), ''))) AS membership_id,
            CAST($universe_id AS VARCHAR) AS universe_id,
            c.security_id,
            c.symbol,
            c.valid_from,
            s.trade_date AS valid_to,
            c.available_at,
            c.security_type,
            c.exchange_code,
            c.has_cik,
            c.cik,
            CAST(c.market_cap_decile AS INTEGER) AS market_cap_decile,
            c.reason,
            CAST($rules_json AS VARCHAR) AS rules_json,
            CAST(c.decision_count AS INTEGER) AS decision_count,
            c.valid_from AS as_of_date,
            CAST($source AS VARCHAR) AS source,
            CAST($run_id AS VARCHAR) AS run_id
        FROM closed c
        LEFT JOIN {sessions_table} s ON s.session_rank = c.close_rank
    """


def _normalize_decision_frame(decisions: pd.DataFrame) -> pd.DataFrame:
    frame = decisions.copy()
    if "trade_date" not in frame.columns:
        frame["trade_date"] = frame["as_of_date"]
    frame["trade_date"] = pd.to_datetime(frame["trade_date"], errors="coerce")
    frame["available_at"] = pd.to_datetime(frame["available_at"], errors="coerce")
    frame["has_cik"] = frame["has_cik"].astype(bool)
    if "cik" not in frame.columns:
        frame["cik"] = None
    frame["cik"] = [None if (value is None or pd.isna(value)) else str(value) for value in frame["cik"]]
    if "reason" not in frame.columns:
        frame["reason"] = [REASON_MEMBER if flag else REASON_MEMBER_NO_CIK for flag in frame["has_cik"]]
    if "late_key" not in frame.columns:
        cutoff = frame["trade_date"] + pd.Timedelta(hours=DECISION_CUTOFF_HOURS)
        frame["late_key"] = frame["available_at"].where(frame["available_at"] > cutoff)
    frame["late_key"] = pd.to_datetime(frame["late_key"], errors="coerce")
    if "market_cap_decile" not in frame.columns:
        frame["market_cap_decile"] = None
    frame["market_cap_decile"] = pd.to_numeric(frame["market_cap_decile"], errors="coerce")
    frame["symbol"] = [symbol_key(value) or None for value in frame.get("symbol", frame["security_id"])]
    frame["security_id"] = frame["security_id"].astype(str)
    frame["session_rank"] = frame["session_rank"].astype("int64")
    return frame[
        [
            "security_id",
            "trade_date",
            "session_rank",
            "symbol",
            "available_at",
            "security_type",
            "exchange_code",
            "has_cik",
            "cik",
            "reason",
            "late_key",
            "market_cap_decile",
        ]
    ]


def compute_universe_us_listed_intervals(
    decisions: pd.DataFrame,
    sessions: pd.DataFrame,
    options: UniverseUsListedOptions,
    *,
    variant: str = STRICT_VARIANT,
) -> pd.DataFrame:
    """Compress small in-memory decision frames with the production interval SQL.

    A convenience/test surface over :func:`build_universe_interval_sql` (the exact SQL
    the store build runs), for frames small enough to hold in memory. ``decisions``
    carries ``security_id, symbol, as_of_date (or trade_date), session_rank,
    available_at, security_type, exchange_code, has_cik, cik, market_cap_decile`` and
    optionally ``reason`` (default derived from ``has_cik``) and ``late_key`` (default:
    ``available_at`` when after the session's 22:00 cutoff). Deterministic and
    row-order independent; sorted by ``(security_id, valid_from)``.
    """

    if decisions is None or decisions.empty or sessions is None or sessions.empty:
        return _empty_output()
    frame = _normalize_decision_frame(decisions)
    grid = pd.DataFrame(
        {
            "trade_date": pd.to_datetime(sessions["trade_date"], errors="coerce"),
            "session_rank": sessions["session_rank"].astype("int64"),
        }
    )
    params = _interval_params(options, options.universe_id, variant)
    con = duckdb.connect(config={"threads": 1, "memory_limit": "256MB"})
    try:
        con.execute("SET TimeZone='UTC'")
        con.register("pure_decisions_frame", frame)
        con.register("pure_sessions_frame", grid)
        con.execute(
            """
            CREATE TEMP TABLE pure_sessions AS
            SELECT CAST(trade_date AS DATE) AS trade_date, CAST(session_rank AS BIGINT) AS session_rank
            FROM pure_sessions_frame
            """
        )
        decisions_sql = """
            SELECT CAST(security_id AS VARCHAR) AS security_id,
                   CAST(trade_date AS DATE) AS trade_date,
                   CAST(session_rank AS BIGINT) AS session_rank,
                   CAST(symbol AS VARCHAR) AS symbol,
                   CAST(available_at AS TIMESTAMP) AS available_at,
                   CAST(security_type AS VARCHAR) AS security_type,
                   CAST(exchange_code AS VARCHAR) AS exchange_code,
                   CAST(has_cik AS BOOLEAN) AS has_cik,
                   CAST(cik AS VARCHAR) AS cik,
                   CAST(reason AS VARCHAR) AS reason,
                   CAST(late_key AS TIMESTAMP) AS late_key,
                   CAST(market_cap_decile AS INTEGER) AS market_cap_decile
            FROM pure_decisions_frame
        """
        out = _run(
            con,
            build_universe_interval_sql(decisions_sql, "pure_sessions") + " ORDER BY security_id, valid_from",
            params,
        ).df()
    finally:
        con.close()
    if out.empty:
        return _empty_output()
    for column in ("valid_from", "valid_to", "as_of_date"):
        out[column] = [None if pd.isna(value) else pd.Timestamp(value).date() for value in out[column]]
    out["has_cik"] = out["has_cik"].astype(bool)
    return out[list(UNIVERSE_OUTPUT_COLUMNS)].reset_index(drop=True)


def _interval_params(options: UniverseUsListedOptions, universe_id: str, variant: str) -> dict[str, object]:
    return {
        "lookback_days": int(options.lookback_days),
        "universe_id": universe_id,
        "rules_json": json_dumps(_rules(options, variant)),
        "source": options.source,
        "run_id": options.run_id,
    }


# ---------------------------------------------------------------------------------------
# Decision SQL (store build).
# ---------------------------------------------------------------------------------------

_TEMP_TABLES: tuple[str, ...] = (
    "_uul_sessions",
    "_uul_dir_raw",
    "_uul_dir_class",
    "_uul_dir_rows",
    "_uul_dir_ontime",
    "_uul_dir_late",
    "_uul_dir_current",
    "_uul_cik",
    "_uul_cik_current",
    "_uul_b_bars",
    "_uul_b_strict",
    "_uul_b_recon_listing",
    "_uul_b_recon",
    "_uul_dec_strict",
    "_uul_dec_reconstructed",
    "_uul_decile_strict",
    "_uul_decile_reconstructed",
    "_uul_pop_strict",
    "_uul_pop_reconstructed",
    "_uul_intervals",
)

_DECISION_TABLE_COLUMNS = """
    security_id VARCHAR, trade_date DATE, session_rank BIGINT, symbol VARCHAR,
    available_at TIMESTAMP, security_type VARCHAR, exchange_code VARCHAR, has_cik BOOLEAN,
    cik VARCHAR, reason VARCHAR, late_key TIMESTAMP, market_cap DOUBLE
"""

_SESSION_CUTOFF = f"INTERVAL {DECISION_CUTOFF_HOURS} HOUR"


def _bar_filters(has_security_filter: bool) -> str:
    filters = [
        "b.security_id IS NOT NULL",
        "b.trade_date IS NOT NULL",
        "b.close > 0",
        "isfinite(b.close)",
        "coalesce(b.is_latest_revision, true)",
        "coalesce(b.available_at, b.source_loaded_at) <= $knowledge_cutoff",
        "b.trade_date <= $trade_date_bound",
    ]
    if has_security_filter:
        filters.append("b.security_id IN (SELECT security_id FROM universe_security_filter)")
    return " AND ".join(filters)


def build_universe_decision_sql(variant: str, *, has_security_filter: bool = False) -> list[str]:
    """Per-batch decision statements for one variant, in execution order.

    Each statement reads the batch's ``_uul_b_bars`` (one row per security-session:
    the earliest-known valid bar, its symbol and clock) plus the small prepared
    directory/CIK tables, materializes the candidate table used for disposition
    counts, and appends the kept decisions to ``_uul_dec_<variant>``. Parameters:
    ``$emit_start, $emit_end, $lookback_days, $market_source``.
    """

    del has_security_filter  # the security filter is applied once, in _uul_b_bars
    eligible_exchanges = _sql_list(ELIGIBLE_EXCHANGE_CODES)
    if variant == STRICT_VARIANT:
        eligible_types = _sql_list(ELIGIBLE_SECURITY_TYPES)
        candidates = f"""
            CREATE OR REPLACE TEMP TABLE _uul_b_strict AS
            WITH cand AS (
                SELECT b.security_id, b.trade_date, b.session_rank, b.symbol, b.bar_at,
                       b.trade_date + {_SESSION_CUTOFF} AS session_cutoff,
                       o.best_rank AS ontime_rank,
                       l.best_rank AS late_rank,
                       l.first_known_at
                FROM (
                    SELECT * FROM _uul_b_bars
                    WHERE trade_date >= $emit_start AND trade_date <= $emit_end
                ) b
                ASOF LEFT JOIN _uul_dir_ontime o
                  ON o.symbol = b.symbol AND b.trade_date >= o.usable_from
                ASOF LEFT JOIN _uul_dir_late l
                  ON l.symbol = b.symbol AND b.trade_date >= l.as_of_date
            ),
            sel AS (
                SELECT *,
                       coalesce(ontime_rank, late_rank) AS listing_rank,
                       CASE WHEN ontime_rank IS NOT NULL THEN session_cutoff ELSE first_known_at END
                           AS listing_cutoff
                FROM cand
            )
            SELECT s.security_id, s.trade_date, s.session_rank, s.symbol, s.bar_at, s.session_cutoff,
                   greatest(s.listing_cutoff, s.bar_at) AS input_cutoff,
                   r.clock AS listing_at,
                   r.security_type,
                   r.exchange_code,
                   CASE WHEN r.pref_rank IS NULL THEN 'no_listing_reference'
                        WHEN r.exchange_code IS NULL OR r.exchange_code NOT IN ({eligible_exchanges})
                            THEN 'not_eligible_exchange'
                        WHEN r.security_type NOT IN ({eligible_types}) THEN 'not_eligible_security_type'
                        ELSE 'eligible' END AS disposition
            FROM sel s
            LEFT JOIN _uul_dir_rows r ON r.symbol = s.symbol AND r.pref_rank = s.listing_rank
        """
        decide = f"""
            INSERT INTO _uul_dec_strict
            WITH eligible AS (SELECT * FROM _uul_b_strict WHERE disposition = 'eligible'),
            cik AS (
                SELECT e.security_id, e.trade_date,
                       count(DISTINCT h.cik) AS cik_count,
                       max(h.cik) AS cik,
                       bool_or(h.cik IS NULL) AS invalid_cik,
                       max(h.available_at) AS cik_at
                FROM eligible e
                JOIN _uul_cik h
                  ON h.security_id = e.security_id
                 AND h.available_at IS NOT NULL
                 AND h.available_at <= e.input_cutoff
                 AND h.valid_from <= e.trade_date
                 AND (h.valid_to IS NULL OR h.valid_to > e.trade_date)
                 AND h.as_of_date <= e.trade_date
                GROUP BY e.security_id, e.trade_date
            ),
            mcap AS (
                SELECT e.security_id, e.trade_date,
                       max({{'ts': m.available_at, 'loaded': m.source_loaded_at,
                             'id': m.market_daily_id, 'cap': m.market_cap}}) AS pick
                FROM eligible e
                JOIN market_daily_metrics m
                  ON m.security_id = e.security_id
                 AND m.trade_date = e.trade_date
                 AND m.source = $market_source
                 AND m.available_at <= e.input_cutoff
                WHERE e.security_type = 'common'
                GROUP BY e.security_id, e.trade_date
            ),
            decided AS (
                SELECT e.*,
                       coalesce(c.cik_count, 0) AS cik_count,
                       c.cik AS cik_value,
                       coalesce(c.invalid_cik, false) AS invalid_cik,
                       c.cik_at,
                       CASE WHEN isfinite(m.pick.cap) AND m.pick.cap > 0 THEN m.pick.cap END AS market_cap,
                       m.pick.ts AS mcap_at
                FROM eligible e
                LEFT JOIN cik c ON c.security_id = e.security_id AND c.trade_date = e.trade_date
                LEFT JOIN mcap m ON m.security_id = e.security_id AND m.trade_date = e.trade_date
            ),
            clocked AS (
                SELECT *,
                       (cik_count = 1 AND NOT invalid_cik) AS has_cik,
                       greatest(bar_at, listing_at, cik_at, mcap_at) AS decision_at
                FROM decided
            ),
            final AS (
                SELECT *,
                       CASE WHEN market_cap IS NOT NULL THEN greatest(decision_at, session_cutoff)
                            ELSE decision_at END AS decided_at
                FROM clocked
            )
            SELECT security_id, trade_date, session_rank, symbol, decided_at,
                   security_type, exchange_code, has_cik,
                   CASE WHEN has_cik THEN cik_value END,
                   CASE WHEN has_cik THEN '{REASON_MEMBER}'
                        WHEN cik_count = 0 AND NOT invalid_cik THEN '{REASON_MEMBER_NO_CIK}'
                        ELSE '{REASON_MEMBER_CONFLICTING_CIK}' END,
                   CASE WHEN decided_at > session_cutoff THEN decided_at END,
                   market_cap
            FROM final
        """
        return [candidates, decide]

    if variant != RECONSTRUCTED_VARIANT:
        raise ValueError(f"unknown universe variant: {variant!r}")
    recon_types = _sql_list(RECONSTRUCTED_ELIGIBLE_SECURITY_TYPES)
    listing = """
        CREATE OR REPLACE TEMP TABLE _uul_b_recon_listing AS
        WITH latest AS (
            SELECT security_id, first(symbol ORDER BY session_rank DESC) AS latest_symbol
            FROM _uul_b_bars
            GROUP BY security_id
        ),
        cur AS (
            SELECT l.security_id, l.latest_symbol, c.security_type, c.exchange_code,
                   c.snapshot_floor_rank, c.snapshot_ceil_rank
            FROM latest l
            LEFT JOIN _uul_dir_current c ON c.symbol = l.latest_symbol
        ),
        anchor AS (
            SELECT cur.security_id, bool_or(b.symbol = cur.latest_symbol) AS anchored
            FROM cur
            JOIN _uul_b_bars b
              ON b.security_id = cur.security_id
             AND b.session_rank >= cur.snapshot_floor_rank - $lookback_days + 1
             AND b.session_rank <= cur.snapshot_ceil_rank - 1 + $lookback_days
            GROUP BY cur.security_id
        )
        SELECT cur.security_id,
               coalesce(a.anchored, false) AS has_listing,
               CASE WHEN coalesce(a.anchored, false) THEN cur.security_type END AS security_type,
               CASE WHEN coalesce(a.anchored, false) THEN cur.exchange_code END AS exchange_code
        FROM cur
        LEFT JOIN anchor a ON a.security_id = cur.security_id
    """
    candidates = f"""
        CREATE OR REPLACE TEMP TABLE _uul_b_recon AS
        SELECT b.security_id, b.trade_date, b.session_rank, b.symbol, b.bar_at,
               b.trade_date + {_SESSION_CUTOFF} AS session_cutoff,
               rl.security_type, rl.exchange_code,
               CASE WHEN NOT rl.has_listing THEN 'no_listing_evidence'
                    WHEN rl.exchange_code IS NULL OR rl.exchange_code NOT IN ({eligible_exchanges})
                        THEN 'not_eligible_exchange'
                    WHEN rl.security_type NOT IN ({recon_types}) THEN 'not_eligible_security_type'
                    ELSE 'eligible' END AS disposition
        FROM _uul_b_bars b
        JOIN _uul_b_recon_listing rl ON rl.security_id = b.security_id
        WHERE b.trade_date >= $emit_start AND b.trade_date <= $emit_end
    """
    decide = f"""
        INSERT INTO _uul_dec_reconstructed
        WITH kept AS (
            SELECT * FROM _uul_b_recon WHERE disposition IN ('eligible', 'no_listing_evidence')
        ),
        mcap AS (
            SELECT k.security_id, k.trade_date,
                   max({{'ts': m.available_at, 'loaded': m.source_loaded_at,
                         'id': m.market_daily_id, 'cap': m.market_cap}}) AS pick
            FROM kept k
            JOIN market_daily_metrics m
              ON m.security_id = k.security_id
             AND m.trade_date = k.trade_date
             AND m.source = $market_source
             AND m.available_at <= greatest(k.bar_at, k.session_cutoff)
            WHERE k.disposition = 'eligible' AND k.security_type = 'common'
            GROUP BY k.security_id, k.trade_date
        ),
        decided AS (
            SELECT k.*,
                   coalesce(c.cik_count, 0) AS cik_count,
                   c.cik AS cik_value,
                   coalesce(c.invalid_cik, false) AS invalid_cik,
                   CASE WHEN isfinite(m.pick.cap) AND m.pick.cap > 0 THEN m.pick.cap END AS market_cap
            FROM kept k
            LEFT JOIN _uul_cik_current c ON c.security_id = k.security_id
            LEFT JOIN mcap m ON m.security_id = k.security_id AND m.trade_date = k.trade_date
        ),
        final AS (
            SELECT *,
                   (cik_count = 1 AND NOT invalid_cik) AS has_cik,
                   CASE WHEN market_cap IS NOT NULL THEN greatest(bar_at, session_cutoff)
                        ELSE bar_at END AS decided_at
            FROM decided
        )
        SELECT security_id, trade_date, session_rank, symbol, decided_at,
               CASE WHEN disposition = 'eligible' THEN security_type ELSE '{UNKNOWN_SECURITY_TYPE}' END,
               CASE WHEN disposition = 'eligible' THEN exchange_code ELSE '{UNKNOWN_EXCHANGE_CODE}' END,
               has_cik,
               CASE WHEN has_cik THEN cik_value END,
               CASE WHEN disposition <> 'eligible' THEN '{REASON_RECONSTRUCTED_NO_LISTING_EVIDENCE}'
                    WHEN has_cik THEN '{REASON_MEMBER}'
                    WHEN cik_count = 0 AND NOT invalid_cik THEN '{REASON_MEMBER_NO_CIK}'
                    ELSE '{REASON_MEMBER_CONFLICTING_CIK}' END,
               CASE WHEN decided_at > session_cutoff THEN decided_at END,
               market_cap
        FROM final
    """
    return [listing, candidates, decide]


_CANDIDATE_TABLE = {STRICT_VARIANT: "_uul_b_strict", RECONSTRUCTED_VARIANT: "_uul_b_recon"}


def _prepare_inputs(
    con: duckdb.DuckDBPyConnection,
    params: dict[str, object],
    *,
    has_security_filter: bool,
) -> dict[str, object]:
    """Build the small, bounded input tables every batch joins against."""

    _run(
        con,
        """
        CREATE OR REPLACE TEMP TABLE _uul_sessions AS
        SELECT trade_date, CAST(row_number() OVER (ORDER BY trade_date) AS BIGINT) AS session_rank
        FROM (
            SELECT DISTINCT trade_date FROM equity_daily_bars
            WHERE trade_date IS NOT NULL
              AND trade_date <= $trade_date_bound
              AND coalesce(available_at, source_loaded_at) <= $knowledge_cutoff
        )
        """,
        params,
    )
    # Directory rows visible at the knowledge cutoff. Superseded (is_latest_revision =
    # false) rows are dropped: that can only make a decision later-known or staler,
    # never earlier.
    _run(
        con,
        """
        CREATE OR REPLACE TEMP TABLE _uul_dir_raw AS
        SELECT upper(trim(d.symbol)) AS symbol,
               d.as_of_date,
               coalesce(d.available_at, d.source_loaded_at) AS clock,
               d.directory,
               d.security_name,
               upper(trim(d.exchange)) AS exchange,
               d.etf,
               d.test_issue,
               d.source_url
        FROM nasdaq_symbol_directory d
        WHERE coalesce(d.is_latest_revision, true)
          AND coalesce(d.available_at, d.source_loaded_at) <= $knowledge_cutoff
          AND d.as_of_date <= $trade_date_bound
          AND nullif(trim(d.symbol), '') IS NOT NULL
        """,
        params,
    )
    # Classify only the distinct (name, flags, venue) tuples in Python -- bounded by
    # the number of distinct listing lines, not by snapshots or bars.
    tuples = con.execute("SELECT DISTINCT security_name, etf, test_issue, exchange FROM _uul_dir_raw").fetchall()
    classification = pd.DataFrame(
        {
            "security_name": pd.Series([row[0] for row in tuples], dtype="object"),
            "etf": pd.Series([row[1] for row in tuples], dtype="boolean"),
            "test_issue": pd.Series([row[2] for row in tuples], dtype="boolean"),
            "exchange": pd.Series([row[3] for row in tuples], dtype="object"),
            "security_type": pd.Series(
                [classify_security_type(row[0], etf=row[1], test_issue=row[2]) for row in tuples],
                dtype="object",
            ),
            "exchange_code": pd.Series([exchange_code_for(row[3]) for row in tuples], dtype="object"),
        }
    )
    con.register("_uul_dir_class_frame", classification)
    try:
        con.execute(
            """
            CREATE OR REPLACE TEMP TABLE _uul_dir_class AS
            SELECT CAST(security_name AS VARCHAR) AS security_name,
                   CAST(etf AS BOOLEAN) AS etf,
                   CAST(test_issue AS BOOLEAN) AS test_issue,
                   CAST(exchange AS VARCHAR) AS exchange,
                   CAST(security_type AS VARCHAR) AS security_type,
                   CAST(exchange_code AS VARCHAR) AS exchange_code
            FROM _uul_dir_class_frame
            """
        )
    finally:
        con.unregister("_uul_dir_class_frame")
    # pref_rank totally orders a symbol's rows: newest snapshot date, then the
    # nasdaqlisted file over otherlisted, then the newest revision clock.
    con.execute(
        """
        CREATE OR REPLACE TEMP TABLE _uul_dir_rows AS
        SELECT r.symbol, r.as_of_date, r.clock, c.security_type, c.exchange_code,
               row_number() OVER (
                   PARTITION BY r.symbol
                   ORDER BY r.as_of_date DESC,
                            CASE r.directory WHEN 'nasdaqlisted' THEN 0 ELSE 1 END,
                            r.clock DESC, r.security_name, r.exchange, r.source_url
               ) AS pref_rank
        FROM _uul_dir_raw r
        JOIN _uul_dir_class c
          ON c.security_name IS NOT DISTINCT FROM r.security_name
         AND c.etf IS NOT DISTINCT FROM r.etf
         AND c.test_issue IS NOT DISTINCT FROM r.test_issue
         AND c.exchange IS NOT DISTINCT FROM r.exchange
        """
    )
    # On-time step function: row r is usable for session D iff as_of_date <= D and
    # clock <= D + 22:00, i.e. D >= usable_from. The best usable row at D is the
    # running minimum pref_rank over usable_from <= D (an ASOF lookup at D).
    con.execute(
        f"""
        CREATE OR REPLACE TEMP TABLE _uul_dir_ontime AS
        WITH usable AS (
            SELECT symbol, pref_rank,
                   greatest(
                       as_of_date,
                       CASE WHEN clock - {_SESSION_CUTOFF} = CAST(clock - {_SESSION_CUTOFF} AS DATE)
                            THEN CAST(clock - {_SESSION_CUTOFF} AS DATE)
                            ELSE CAST(clock - {_SESSION_CUTOFF} AS DATE) + 1 END
                   ) AS usable_from
            FROM _uul_dir_rows
        ),
        per_day AS (SELECT symbol, usable_from, min(pref_rank) AS day_best FROM usable GROUP BY ALL)
        SELECT symbol, usable_from,
               min(day_best) OVER (PARTITION BY symbol ORDER BY usable_from
                               ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS best_rank
        FROM per_day
        """
    )
    # Late-known step function: when nothing dated on/before D was known by D 22:00,
    # the decision cutoff is the earliest clock of any row dated on/before D, and the
    # selected row is the best-ranked row known at exactly that clock.
    con.execute(
        """
        CREATE OR REPLACE TEMP TABLE _uul_dir_late AS
        WITH per_day AS (
            SELECT symbol, as_of_date, min({'ts': clock, 'pref': pref_rank}) AS pick
            FROM _uul_dir_rows GROUP BY ALL
        ),
        running AS (
            SELECT symbol, as_of_date,
                   min(pick) OVER (PARTITION BY symbol ORDER BY as_of_date
                                   ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS pick
            FROM per_day
        )
        SELECT symbol, as_of_date, pick.ts AS first_known_at, pick.pref AS best_rank FROM running
        """
    )
    # Reconstruction: the current row per symbol as of the knowledge cutoff, with the
    # sessions bracketing its snapshot date (latest on/before, earliest on/after) that
    # the backcast anchor window is built around.
    con.execute(
        """
        CREATE OR REPLACE TEMP TABLE _uul_dir_current AS
        SELECT r.symbol, r.as_of_date, r.security_type, r.exchange_code,
               coalesce(f.session_rank, 0) AS snapshot_floor_rank,
               coalesce(c.session_rank, 9223372036854775807) AS snapshot_ceil_rank
        FROM (SELECT * FROM _uul_dir_rows WHERE pref_rank = 1) r
        ASOF LEFT JOIN _uul_sessions f ON r.as_of_date >= f.trade_date
        ASOF LEFT JOIN _uul_sessions c ON c.trade_date >= r.as_of_date
        """
    )
    security_filter = (
        "AND h.security_id IN (SELECT security_id FROM universe_security_filter)" if has_security_filter else ""
    )
    _run(
        con,
        f"""
        CREATE OR REPLACE TEMP TABLE _uul_cik AS
        SELECT h.security_id,
               CASE WHEN regexp_full_match(trim(h.id_value), '[0-9]{{1,10}}')
                    THEN lpad(trim(h.id_value), 10, '0') END AS cik,
               h.valid_from, h.valid_to, h.as_of_date, h.available_at,
               coalesce(h.is_latest_revision, true) AS is_latest
        FROM security_identifier_history h
        WHERE h.id_type = 'CIK'
          AND h.id_value IS NOT NULL
          AND coalesce(h.available_at, h.source_loaded_at) <= $knowledge_cutoff
          {security_filter}
        """,
        params,
    )
    con.execute(
        """
        CREATE OR REPLACE TEMP TABLE _uul_cik_current AS
        SELECT security_id, count(DISTINCT cik) AS cik_count, max(cik) AS cik,
               bool_or(cik IS NULL) AS invalid_cik
        FROM _uul_cik
        WHERE valid_to IS NULL AND is_latest
        GROUP BY security_id
        """
    )
    row = _run(
        con,
        """
        SELECT count(*),
               count(*) FILTER (WHERE coalesce(is_latest_revision, true)
                                  AND coalesce(available_at, source_loaded_at) > $knowledge_cutoff),
               count(*) FILTER (WHERE NOT coalesce(is_latest_revision, true)),
               min(as_of_date), max(as_of_date)
        FROM nasdaq_symbol_directory
        """,
        params,
    ).fetchone()
    visible = con.execute(
        "SELECT count(*), count(DISTINCT symbol), min(clock), max(clock) FROM _uul_dir_raw"
    ).fetchone()
    sessions = con.execute("SELECT count(*), min(trade_date), max(trade_date) FROM _uul_sessions").fetchone()
    return {
        "directory_rows": int(row[0]),
        "directory_rows_after_knowledge_cutoff": int(row[1]),
        "directory_rows_superseded": int(row[2]),
        "directory_min_as_of_date": None if row[3] is None else row[3].isoformat(),
        "directory_max_as_of_date": None if row[4] is None else row[4].isoformat(),
        "directory_rows_visible": int(visible[0]),
        "directory_symbols_visible": int(visible[1]),
        "directory_min_clock": None if visible[2] is None else visible[2].isoformat(),
        "directory_max_clock": None if visible[3] is None else visible[3].isoformat(),
        "distinct_listing_lines_classified": len(tuples),
        "sessions": int(sessions[0]),
        "first_session": None if sessions[1] is None else sessions[1].isoformat(),
        "last_session": None if sessions[2] is None else sessions[2].isoformat(),
    }


def _variants(options: UniverseUsListedOptions) -> list[tuple[str, str]]:
    variants = [(STRICT_VARIANT, options.universe_id)]
    if options.reconstructed_universe_id:
        if options.reconstructed_universe_id == options.universe_id:
            raise ValueError("reconstructed_universe_id must differ from universe_id")
        variants.append((RECONSTRUCTED_VARIANT, options.reconstructed_universe_id))
    return variants


def _build_params(options: UniverseUsListedOptions) -> dict[str, object]:
    knowledge_cutoff, trade_date_bound = _knowledge_bounds(options)
    return {
        "knowledge_cutoff": knowledge_cutoff,
        "trade_date_bound": trade_date_bound,
        "emit_start": options.start_date or dt.date(1, 1, 1),
        "emit_end": min(options.end_date or dt.date(9999, 12, 31), trade_date_bound),
        "lookback_days": int(options.lookback_days),
        "market_source": options.market_source,
    }


def _drop_temp_tables(con: duckdb.DuckDBPyConnection) -> None:
    for table in _TEMP_TABLES:
        con.execute(f"DROP TABLE IF EXISTS {table}")


def _write_variant(
    store: DuckDBStore,
    options: UniverseUsListedOptions,
    variant: str,
    universe_id: str,
) -> int:
    name = options.name if variant == STRICT_VARIANT else options.reconstructed_name
    description = options.description if variant == STRICT_VARIANT else options.reconstructed_description
    con = store.con
    con.execute("DELETE FROM universes WHERE universe_id = ?", [universe_id])
    con.execute(
        "INSERT INTO universes (universe_id, name, description, rules_json) VALUES (?, ?, ?, ?)",
        [
            universe_id,
            name,
            description,
            json_dumps(
                {
                    key: (value.isoformat() if isinstance(value, dt.date) else value)
                    for key, value in asdict(options).items()
                }
                | {"universe_id": universe_id, "rules": _rules(options, variant)}
            ),
        ],
    )
    predicates = ["universe_id = ?", "source = ?"]
    params: list[object] = [universe_id, options.source]
    if options.start_date is not None:
        predicates.append("coalesce(valid_to, valid_from) >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("valid_from <= ?")
        params.append(options.end_date)
    con.execute(f"DELETE FROM universe_us_listed_membership WHERE {' AND '.join(predicates)}", params)
    columns = ", ".join(UNIVERSE_OUTPUT_COLUMNS)
    con.execute(
        f"INSERT INTO universe_us_listed_membership ({columns}) "
        f"SELECT {columns} FROM _uul_intervals WHERE universe_id = ?",
        [universe_id],
    )
    return int(con.execute("SELECT count(*) FROM _uul_intervals WHERE universe_id = ?", [universe_id]).fetchone()[0])


def build_universe_us_listed(
    store: DuckDBStore,
    options: UniverseUsListedOptions | None = None,
) -> UniverseBuildSummary:
    """Rebuild the strict (and, unless disabled, reconstructed) universe intervals."""

    options = options or UniverseUsListedOptions()
    if options.lookback_days < 1:
        raise ValueError("lookback_days must be positive")
    if options.max_bars_per_batch < 1:
        raise ValueError("max_bars_per_batch must be positive")
    variants = _variants(options)
    store.initialize()
    con = store.con
    params = _build_params(options)
    summary = UniverseBuildSummary()
    has_security_filter = options.security_ids is not None
    if has_security_filter:
        ids = sorted({str(value) for value in (options.security_ids or ()) if value})
        con.register("universe_security_filter", pd.DataFrame({"security_id": pd.Series(ids, dtype="object")}))
    started = time.perf_counter()
    try:
        summary.diagnostics = _prepare_inputs(con, params, has_security_filter=has_security_filter)
        bar_filters = _bar_filters(has_security_filter)
        summary.bar_rows = int(
            _run(con, f"SELECT count(*) FROM equity_daily_bars b WHERE {bar_filters}", params).fetchone()[0]
        )
        started = summary.tick("prepare_inputs", started)
        summary.batch_count = max(1, math.ceil(summary.bar_rows / options.max_bars_per_batch))
        columns = ", ".join(UNIVERSE_OUTPUT_COLUMNS)
        con.execute(
            "CREATE OR REPLACE TEMP TABLE _uul_intervals AS "
            f"SELECT {columns} FROM universe_us_listed_membership WHERE false"
        )
        for variant, _universe_id in variants:
            # The only cross-security state kept across batches: the narrow decile
            # population (eligible common members with a visible market cap).
            con.execute(
                f"CREATE OR REPLACE TEMP TABLE _uul_pop_{variant} "
                "(trade_date DATE, security_id VARCHAR, market_cap DOUBLE, level TIMESTAMP)"
            )
            summary.late_decisions[variant] = 0
        statements = {variant: build_universe_decision_sql(variant) for variant, _ in variants}
        counts: dict[str, Counter[str]] = {variant: Counter() for variant, _ in variants}
        for batch in range(summary.batch_count):
            batch_params = params | {"batch": batch, "batch_count": summary.batch_count}
            _run(
                con,
                f"""
                CREATE OR REPLACE TEMP TABLE _uul_b_bars AS
                WITH picked AS (
                    SELECT b.security_id, b.trade_date,
                           min({{'known_at': coalesce(b.available_at, b.source_loaded_at),
                                 'source': b.source,
                                 'symbol': upper(trim(b.symbol))}}) AS pick
                    FROM equity_daily_bars b
                    WHERE {bar_filters}
                      AND hash(b.security_id) % $batch_count = $batch
                    GROUP BY b.security_id, b.trade_date
                )
                SELECT p.security_id, p.trade_date, s.session_rank,
                       nullif(p.pick.symbol, '') AS symbol, p.pick.known_at AS bar_at
                FROM picked p
                JOIN _uul_sessions s ON s.trade_date = p.trade_date
                """,
                batch_params,
            )
            started = summary.tick("batch_bars", started)
            for variant, universe_id in variants:
                con.execute(f"CREATE OR REPLACE TEMP TABLE _uul_dec_{variant} ({_DECISION_TABLE_COLUMNS})")
                for statement in statements[variant][:-1]:
                    _run(con, statement, batch_params)
                for disposition, security_type, count in con.execute(
                    f"""
                    SELECT disposition, coalesce(security_type, ''), count(*)
                    FROM {_CANDIDATE_TABLE[variant]} GROUP BY ALL
                    """
                ).fetchall():
                    counts[variant][disposition] += int(count)
                    if disposition == "not_eligible_security_type":
                        counts[variant][f"not_eligible_security_type:{security_type}"] += int(count)
                _run(con, statements[variant][-1], batch_params)
                summary.late_decisions[variant] += int(
                    con.execute(f"SELECT count(*) FROM _uul_dec_{variant} WHERE late_key IS NOT NULL").fetchone()[0]
                )
                con.execute(
                    f"INSERT INTO _uul_pop_{variant} SELECT trade_date, security_id, market_cap, available_at "
                    f"FROM _uul_dec_{variant} WHERE market_cap IS NOT NULL"
                )
                started = summary.tick(f"decisions_{variant}", started)
                # Interval boundaries never depend on the decile (it is not interval
                # state), so each batch is compressed immediately and its security-day
                # decisions are dropped; deciles are attached to interval starts below.
                decisions_sql = f"""
                    SELECT security_id, trade_date, session_rank, symbol, available_at,
                           security_type, exchange_code, has_cik, cik, reason, late_key,
                           CAST(NULL AS INTEGER) AS market_cap_decile
                    FROM _uul_dec_{variant}
                """
                _run(
                    con,
                    f"INSERT INTO _uul_intervals ({columns}) "
                    + build_universe_interval_sql(decisions_sql, "_uul_sessions"),
                    params | _interval_params(options, universe_id, variant),
                )
                con.execute(f"DROP TABLE _uul_dec_{variant}")
                started = summary.tick(f"intervals_{variant}", started)
        for variant, universe_id in variants:
            summary.dispositions[variant] = dict(sorted(counts[variant].items()))
            # Deciles at interval starts. The population at ranking level L is every
            # decile-eligible member of the session whose own decision clock is <= L, so
            # a decile never uses a member (or a market cap) not yet known at the ranked
            # row's clock. Only sessions/levels holding an interval start are ranked.
            _run(
                con,
                f"""
                CREATE OR REPLACE TEMP TABLE _uul_decile_{variant} AS
                WITH needed AS (
                    SELECT p.trade_date, p.security_id, p.level
                    FROM _uul_pop_{variant} p
                    JOIN _uul_intervals i
                      ON i.universe_id = $universe_id
                     AND i.security_id = p.security_id
                     AND i.valid_from = p.trade_date
                ),
                levels AS (SELECT DISTINCT trade_date, level FROM needed),
                ranked AS (
                    SELECT p.trade_date, p.security_id, p.level AS own_level, l.level,
                           ntile(10) OVER (PARTITION BY l.trade_date, l.level
                                           ORDER BY p.market_cap, p.security_id) AS decile
                    FROM levels l
                    JOIN _uul_pop_{variant} p ON p.trade_date = l.trade_date AND p.level <= l.level
                )
                SELECT r.trade_date, r.security_id, CAST(r.decile AS INTEGER) AS market_cap_decile
                FROM ranked r
                JOIN needed n
                  ON n.security_id = r.security_id AND n.trade_date = r.trade_date AND n.level = r.level
                WHERE r.own_level = r.level
                """,
                {"universe_id": universe_id},
            )
            _run(
                con,
                f"""
                UPDATE _uul_intervals i
                SET market_cap_decile = d.market_cap_decile
                FROM _uul_decile_{variant} d
                WHERE i.universe_id = $universe_id
                  AND d.security_id = i.security_id
                  AND d.trade_date = i.valid_from
                """,
                {"universe_id": universe_id},
            )
            started = summary.tick(f"deciles_{variant}", started)
        with store.transaction():
            for variant, universe_id in variants:
                summary.rows_by_universe[universe_id] = _write_variant(store, options, variant, universe_id)
                summary.intervals_with_cik[universe_id] = int(
                    con.execute(
                        "SELECT count(*) FROM _uul_intervals WHERE universe_id = ? AND has_cik", [universe_id]
                    ).fetchone()[0]
                )
        summary.tick("write", started)
    finally:
        _drop_temp_tables(con)
        if has_security_filter:
            con.unregister("universe_security_filter")

    for variant, universe_id in variants:
        rows = summary.rows_by_universe.get(universe_id, 0)
        with_cik = summary.intervals_with_cik.get(universe_id, 0)
        quality_check(
            store,
            dataset_id="universe_us_listed",
            table_name="universe_us_listed_membership",
            check_name="rows_loaded" if variant == STRICT_VARIANT else "rows_loaded_reconstructed",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={
                "universe_id": universe_id,
                "variant": variant,
                "intervals": rows,
                "intervals_with_cik": with_cik,
                "intervals_without_cik": rows - with_cik,
                "decision_dispositions": summary.dispositions.get(variant, {}),
                "late_known_decisions": summary.late_decisions.get(variant, 0),
                "bar_rows": summary.bar_rows,
                "batch_count": summary.batch_count,
                "phase_seconds": summary.phase_seconds,
                "inputs": summary.diagnostics,
                "rules": _rules(options, variant),
            },
        )
    return summary


def refresh_universe_us_listed(
    store: DuckDBStore,
    options: UniverseUsListedOptions | None = None,
) -> int:
    """Rebuild ``universe_us_listed_membership``; returns intervals written (all variants)."""

    return build_universe_us_listed(store, options).total_rows


def universe_us_listed(
    store: DuckDBStore,
    as_of_date: dt.date,
    *,
    universe_id: str = DEFAULT_US_LISTED_UNIVERSE_ID,
    require_cik: bool = False,
) -> pd.DataFrame:
    """The universe as it stood on ``as_of_date``.

    Only intervals whose ``available_at`` is at or before the end of ``as_of_date`` are
    visible, so the accessor is point-in-time by construction. ``require_cik=True``
    narrows to the fundamentals universe; the default returns the full market universe
    including the unresolved tail.
    """

    store.initialize()
    cutoff = dt.datetime.combine(as_of_date, dt.time(23, 59, 59))
    predicates = [
        "universe_id = ?",
        "valid_from <= ?",
        "(valid_to IS NULL OR valid_to >= ?)",
        "available_at <= ?",
    ]
    params: list[object] = [universe_id, as_of_date, as_of_date, cutoff]
    if require_cik:
        predicates.append("has_cik")
    return store.con.execute(
        f"""
        SELECT security_id, symbol, security_type, exchange_code, has_cik, cik,
               market_cap_decile, valid_from, available_at
        FROM universe_us_listed_membership
        WHERE {" AND ".join(predicates)}
        ORDER BY security_id
        """,
        params,
    ).df()


class UniverseUsListedDataset(Dataset):
    """Dataset wrapper so the universe builder is a first-class DAG node."""

    dataset_id = "universe_us_listed"
    source_name = UNIVERSE_SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: UniverseUsListedOptions) -> DatasetLoadResult:
        summary = build_universe_us_listed(store, options)
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=summary.total_rows,
            source=options.source,
            details={
                "universe_id": options.universe_id,
                "rows_by_universe": summary.rows_by_universe,
                "decision_dispositions": summary.dispositions,
                "rules": _rules(options),
            },
            run_id=options.run_id,
        )
