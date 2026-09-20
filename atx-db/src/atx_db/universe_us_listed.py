"""Point-in-time US-listed equity universe.

Implements the Tier-1 spec's ``universe_us_listed(as_of_date)``: securities with at
least one trade in the trailing ``lookback_days`` trading sessions, listed on NYSE,
NASDAQ, NYSE American, ARCA or BATS, whose security type is common stock, an ADR, a
REIT or an LP. ETFs, ETNs, closed-end funds, warrants, rights, units, preferreds and
notes are excluded. Securities with no resolved CIK stay in the universe (it is the
market universe) but carry ``has_cik = false`` so the fundamentals universe can subset
without losing the tail.

Everything here is deterministic: the trading-session grid is the archive's own
distinct ``equity_daily_bars.trade_date`` values, classification is an ordered regex
table, and the interval writer emits a stable-sorted frame.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import re
from dataclasses import asdict, dataclass

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .market_daily import MARKET_DAILY_SOURCE_NAME
from .warehouse import insert_frame, json_dumps, quality_check, symbol_key

UNIVERSE_SOURCE_NAME = "atx-db us-listed universe builder"
DEFAULT_US_LISTED_UNIVERSE_ID = "us_listed_v1"

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
ELIGIBLE_SECURITY_TYPES: tuple[str, ...] = ("ADR", "LP", "REIT", "common")

# Ordered classification table; first match wins, so the exclusions that can masquerade
# as an eligible type (a preferred ADS, a partnership *unit*) are tested first.
SECURITY_TYPE_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("ETN", re.compile(r"\bETNS?\b|EXCHANGE[- ]TRADED NOTE")),
    (
        "preferred",
        re.compile(
            r"\bPREFERRED\b|\bPREFERENCE\b|\bPFD\b|(?<!AMERICAN )DEPOSITARY (?:SHARE|SHS|SHARES)"
        ),
    ),
    ("warrant", re.compile(r"\bWARRANTS?\b|\bWTS?\b")),
    ("right", re.compile(r"\bRIGHTS?\b")),
    ("unit", re.compile(r"\bUNITS?\b")),
    ("note", re.compile(r"\bNOTES?\b|\bDEBENTURES?\b|\bBONDS?\b|\bSUBORDINATED\b")),
    ("ADR", re.compile(r"AMERICAN DEPOSITARY|AMERICAN DEPOSITORY|\bADR\b|\bADS\b")),
    ("REIT", re.compile(r"\bREIT\b|REAL ESTATE INVESTMENT TRUST")),
    ("LP", re.compile(r"\bL\.?P\.?\b|LIMITED PARTNERSHIP")),
    ("fund", re.compile(r"\bETFS?\b|\bFUND\b|CLOSED[- ]END|\bINDEX TRUST\b|\bPORTFOLIO\b")),
)


def _clean_name(value: object) -> str:
    if value is None:
        return ""
    try:
        if pd.isna(value):
            return ""
    except (TypeError, ValueError):
        pass
    return str(value).strip().upper()


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
    name, then :data:`SECURITY_TYPE_PATTERNS` is walked in order, then ``"common"``.
    Pure and total -- always returns a label, never raises.
    """

    if _flag(test_issue):
        return "test"
    if _flag(etf):
        return "ETF"
    name = _clean_name(security_name)
    if not name:
        return "unknown"
    for label, pattern in SECURITY_TYPE_PATTERNS:
        if pattern.search(name):
            return label
    return "common"


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

_INTERVAL_STATE_COLUMNS = ("security_type", "exchange_code", "has_cik", "reason")


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
    as_of_date: dt.date | None = None
    run_id: str | None = None


def _rules(options: UniverseUsListedOptions) -> dict[str, object]:
    return {
        "lookback_days": options.lookback_days,
        "eligible_exchange_codes": list(ELIGIBLE_EXCHANGE_CODES),
        "eligible_security_types": list(ELIGIBLE_SECURITY_TYPES),
        "market_source": options.market_source,
        "decile_basis": "market_daily_metrics.market_cap at valid_from",
    }


def _empty_output() -> pd.DataFrame:
    return pd.DataFrame(columns=list(UNIVERSE_OUTPUT_COLUMNS))


def _membership_id(*parts: object) -> str:
    payload = "|".join("" if part is None else str(part) for part in parts)
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def compute_universe_us_listed_intervals(
    decisions: pd.DataFrame,
    sessions: pd.DataFrame,
    options: UniverseUsListedOptions,
) -> pd.DataFrame:
    """Compress per-session universe decisions into deterministic validity intervals.

    ``decisions`` carries one row per (security_id, trading session the security
    traded on) with the attributes resolved as of that session:
    ``security_id, symbol, as_of_date, session_rank, available_at, security_type,
    exchange_code, has_cik, cik, market_cap_decile``. ``sessions`` is the archive
    trading-session grid (``trade_date``, ``session_rank``), ascending and gap-free in
    rank.

    A new interval opens when any of ``_INTERVAL_STATE_COLUMNS`` changes or when the
    session-rank gap to the previous decision exceeds ``lookback_days`` -- the latter is
    the spec's "at least one trade in the prior N trading days" rule expressed on the
    grid. An interval closes at ``min(last decision rank + lookback_days - 1, archive
    end)``; an interval whose extension reaches the last known session stays OPEN
    (``valid_to`` NULL) because the archive cannot prove the name stopped trading.
    ``market_cap_decile`` is the decile observed at ``valid_from`` only (deviation 3).

    Pure and stable-sorted: the same decisions in any row order yield a byte-identical
    frame.
    """

    if decisions is None or decisions.empty or sessions is None or sessions.empty:
        return _empty_output()

    grid = sessions.copy()
    grid["session_rank"] = grid["session_rank"].astype(int)
    grid = grid.sort_values("session_rank", kind="mergesort").reset_index(drop=True)
    rank_to_date = dict(zip(grid["session_rank"], grid["trade_date"], strict=True))
    last_rank = int(grid["session_rank"].iloc[-1])

    frame = decisions.copy()
    frame["security_id"] = frame["security_id"].astype("string")
    frame["session_rank"] = frame["session_rank"].astype(int)
    frame["has_cik"] = frame["has_cik"].astype(bool)
    frame["reason"] = ["member" if flag else "member_no_cik" for flag in frame["has_cik"]]
    frame = frame.sort_values(["security_id", "session_rank"], kind="mergesort").reset_index(drop=True)

    rules_json = json_dumps(_rules(options))
    rows: list[dict[str, object]] = []

    def close(current: dict[str, object], final_rank: int, *, extend: bool) -> None:
        # An interval closed by a real gap (a rank jump beyond lookback_days) or by
        # running off the end of the security's decisions is extended forward --
        # the archive cannot prove membership lapsed the instant the last known bar
        # printed, so it is held open through the lookback window (closed=None if
        # that reaches the archive end). An interval closed by an immediate state
        # change (the very next session, just under different attributes) is NOT
        # extended: the successor interval's valid_from already covers the next
        # session, so extending here would overlap it.
        if extend:
            extended = final_rank + options.lookback_days - 1
            current["valid_to"] = None if extended >= last_rank else rank_to_date[extended]
        else:
            current["valid_to"] = rank_to_date[final_rank]
        rows.append({key: value for key, value in current.items() if not key.startswith("_")})

    for security_id, group in frame.groupby("security_id", sort=True, dropna=False):
        current: dict[str, object] | None = None
        previous_rank: int | None = None
        for row in group.itertuples(index=False):
            state = tuple(getattr(row, column) for column in _INTERVAL_STATE_COLUMNS)
            rank = int(row.session_rank)
            gapped = previous_rank is not None and (rank - previous_rank) > options.lookback_days
            if current is None or current["_state"] != state or gapped:
                if current is not None:
                    close(current, int(previous_rank), extend=gapped)
                current = {
                    "_state": state,
                    "membership_id": _membership_id(options.universe_id, security_id, row.as_of_date, *state),
                    "universe_id": options.universe_id,
                    "security_id": str(security_id),
                    "symbol": symbol_key(getattr(row, "symbol", None)),
                    "valid_from": row.as_of_date,
                    "valid_to": None,
                    "available_at": pd.Timestamp(row.available_at).to_pydatetime(),
                    "security_type": str(row.security_type),
                    "exchange_code": str(row.exchange_code),
                    "has_cik": bool(row.has_cik),
                    "cik": getattr(row, "cik", None),
                    "market_cap_decile": None
                    if pd.isna(getattr(row, "market_cap_decile", None))
                    else int(row.market_cap_decile),
                    "reason": str(row.reason),
                    "rules_json": rules_json,
                    "decision_count": 0,
                    "as_of_date": row.as_of_date,
                    "source": options.source,
                    "run_id": options.run_id,
                }
            current["decision_count"] = int(current["decision_count"]) + 1
            previous_rank = rank
        if current is not None:
            close(current, int(previous_rank), extend=True)

    if not rows:
        return _empty_output()
    return (
        pd.DataFrame.from_records(rows, columns=list(UNIVERSE_OUTPUT_COLUMNS))
        .sort_values(["security_id", "valid_from"], kind="mergesort")
        .reset_index(drop=True)
    )


def build_universe_decision_sql(
    *,
    has_security_filter: bool,
    has_start_date: bool,
    has_end_date: bool,
) -> str:
    """SQL for the per-session universe decision grid.

    One row per (security_id, session the security traded on) with the newest listing
    snapshot visible on that session, the CIK resolution from ``securities.entity_id``
    and the market-cap decile from ``market_daily_metrics``. Placeholder order:
    ``[market_source]`` then, when present, ``[start_date]``, ``[end_date]``.
    """

    filters = [
        "b.security_id IS NOT NULL",
        "b.trade_date IS NOT NULL",
        "b.close IS NOT NULL",
        "b.close > 0",
    ]
    if has_security_filter:
        filters.append("b.security_id IN (SELECT security_id FROM universe_security_filter)")
    bar_window = []
    if has_start_date:
        bar_window.append("s.trade_date >= ?")
    if has_end_date:
        bar_window.append("s.trade_date <= ?")
    emit = f"WHERE {' AND '.join(bar_window)}" if bar_window else ""
    return f"""
        WITH sessions AS (
            SELECT trade_date,
                   row_number() OVER (ORDER BY trade_date) AS session_rank
            FROM (SELECT DISTINCT trade_date FROM equity_daily_bars WHERE trade_date IS NOT NULL)
        ),
        bars AS (
            SELECT
                b.security_id,
                b.trade_date,
                any_value(b.symbol ORDER BY b.available_at DESC, b.source DESC) AS symbol,
                min(b.available_at) AS available_at
            FROM equity_daily_bars b
            WHERE {" AND ".join(filters)}
            GROUP BY b.security_id, b.trade_date
        ),
        directory AS (
            SELECT
                d.symbol,
                d.as_of_date,
                d.security_name,
                d.exchange,
                d.etf,
                d.test_issue,
                row_number() OVER (
                    PARTITION BY d.symbol, d.as_of_date
                    ORDER BY d.directory, d.source_loaded_at DESC
                ) AS rn
            FROM nasdaq_symbol_directory d
        ),
        deciles AS (
            SELECT
                m.security_id,
                m.trade_date,
                ntile(10) OVER (PARTITION BY m.trade_date ORDER BY m.market_cap) AS market_cap_decile
            FROM market_daily_metrics m
            WHERE m.source = ? AND m.market_cap IS NOT NULL AND m.is_latest_revision
        ),
        listing AS (
            SELECT
                bars.security_id,
                bars.trade_date,
                dir.security_name,
                dir.exchange,
                dir.etf,
                dir.test_issue,
                row_number() OVER (
                    PARTITION BY bars.security_id, bars.trade_date
                    ORDER BY dir.as_of_date DESC
                ) AS rn
            FROM bars
            JOIN directory dir
              ON dir.rn = 1
             AND dir.symbol = bars.symbol
             AND dir.as_of_date <= bars.trade_date
        )
        SELECT
            bars.security_id,
            bars.symbol,
            s.trade_date AS as_of_date,
            s.session_rank,
            bars.available_at,
            listing.security_name,
            listing.exchange,
            listing.etf,
            listing.test_issue,
            coalesce(sec.entity_id LIKE 'CIK-%', false) AS has_cik,
            CASE WHEN sec.entity_id LIKE 'CIK-%' THEN substr(sec.entity_id, 5) END AS cik,
            deciles.market_cap_decile
        FROM bars
        JOIN sessions s ON s.trade_date = bars.trade_date
        LEFT JOIN listing
          ON listing.security_id = bars.security_id
         AND listing.trade_date = bars.trade_date
         AND listing.rn = 1
        LEFT JOIN securities sec ON sec.security_id = bars.security_id
        LEFT JOIN deciles
          ON deciles.security_id = bars.security_id
         AND deciles.trade_date = bars.trade_date
        {emit}
        ORDER BY bars.security_id, s.session_rank
    """


def load_universe_decisions(
    store: DuckDBStore,
    options: UniverseUsListedOptions,
) -> tuple[pd.DataFrame, pd.DataFrame, dict[str, int]]:
    """Run the decision SQL and apply the pure classification/eligibility filters.

    Returns ``(eligible_decisions, sessions, exclusion_counts)``. ``exclusion_counts``
    carries one entry per rejection reason so the excluded tail is reported rather than
    discarded silently.
    """

    store.initialize()
    registered = False
    if options.security_ids is not None:
        ids = sorted({str(value) for value in options.security_ids if value})
        store.con.register("universe_security_filter", pd.DataFrame({"security_id": ids}))
        registered = True
    params: list[object] = [options.market_source]
    if options.start_date is not None:
        params.append(options.start_date)
    if options.end_date is not None:
        params.append(options.end_date)
    sql = build_universe_decision_sql(
        has_security_filter=options.security_ids is not None,
        has_start_date=options.start_date is not None,
        has_end_date=options.end_date is not None,
    )
    try:
        frame = store.con.execute(sql, params).df()
        sessions = store.con.execute(
            """
            SELECT trade_date, row_number() OVER (ORDER BY trade_date) AS session_rank
            FROM (SELECT DISTINCT trade_date FROM equity_daily_bars WHERE trade_date IS NOT NULL)
            ORDER BY trade_date
            """
        ).df()
    finally:
        if registered:
            store.con.unregister("universe_security_filter")

    if frame.empty:
        return frame, sessions, {"no_bars": 0}

    frame["as_of_date"] = pd.to_datetime(frame["as_of_date"], errors="coerce").dt.date
    sessions["trade_date"] = pd.to_datetime(sessions["trade_date"], errors="coerce").dt.date
    frame["exchange_code"] = [exchange_code_for(value) for value in frame["exchange"]]
    frame["security_type"] = [
        classify_security_type(name, etf=etf, test_issue=test)
        for name, etf, test in zip(frame["security_name"], frame["etf"], frame["test_issue"], strict=True)
    ]
    no_listing = frame["exchange_code"].isna()
    bad_exchange = ~no_listing & ~frame["exchange_code"].isin(ELIGIBLE_EXCHANGE_CODES)
    bad_type = ~no_listing & ~bad_exchange & ~frame["security_type"].isin(ELIGIBLE_SECURITY_TYPES)
    exclusions = {
        "no_listing_reference": int(no_listing.sum()),
        "not_eligible_exchange": int(bad_exchange.sum()),
        "not_eligible_security_type": int(bad_type.sum()),
    }
    eligible = frame[~(no_listing | bad_exchange | bad_type)].reset_index(drop=True)
    return eligible, sessions, exclusions


def refresh_universe_us_listed(
    store: DuckDBStore,
    options: UniverseUsListedOptions | None = None,
) -> int:
    """Rebuild ``universe_us_listed_membership`` for one universe id."""

    options = options or UniverseUsListedOptions()
    if options.lookback_days < 1:
        raise ValueError("lookback_days must be positive")
    decisions, sessions, exclusions = load_universe_decisions(store, options)
    intervals = compute_universe_us_listed_intervals(decisions, sessions, options)

    with store.transaction():
        store.con.execute("DELETE FROM universes WHERE universe_id = ?", [options.universe_id])
        store.con.execute(
            "INSERT INTO universes (universe_id, name, description, rules_json) VALUES (?, ?, ?, ?)",
            [
                options.universe_id,
                options.name,
                options.description,
                json_dumps(
                    {
                        key: (value.isoformat() if isinstance(value, dt.date) else value)
                        for key, value in asdict(options).items()
                    }
                    | {"rules": _rules(options)}
                ),
            ],
        )
        predicates = ["universe_id = ?", "source = ?"]
        params: list[object] = [options.universe_id, options.source]
        if options.start_date is not None:
            predicates.append("coalesce(valid_to, valid_from) >= ?")
            params.append(options.start_date)
        if options.end_date is not None:
            predicates.append("valid_from <= ?")
            params.append(options.end_date)
        store.con.execute(
            f"DELETE FROM universe_us_listed_membership WHERE {' AND '.join(predicates)}",
            params,
        )
        rows = 0
        if not intervals.empty:
            rows = insert_frame(
                store,
                intervals,
                "universe_us_listed_membership",
                "universe_us_listed_membership_insert",
            )

    with_cik = int(intervals["has_cik"].sum()) if not intervals.empty else 0
    quality_check(
        store,
        dataset_id="universe_us_listed",
        table_name="universe_us_listed_membership",
        check_name="rows_loaded",
        status="passed" if rows > 0 else "warning",
        observed_value=float(rows),
        threshold_value=1.0,
        details={
            "universe_id": options.universe_id,
            "intervals": rows,
            "intervals_with_cik": with_cik,
            "intervals_without_cik": rows - with_cik,
            "excluded_decisions": exclusions,
            "rules": _rules(options),
        },
    )
    return rows


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
        rows = refresh_universe_us_listed(store, options)
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={"universe_id": options.universe_id, "rules": _rules(options)},
            run_id=options.run_id,
        )
