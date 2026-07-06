"""PF4-S1 signal-evaluation surface: per-factor rank-IC + IC-decay over the exported panel.

Scores every factor in the PIT-safe exported panel (``v_factor_panel``, owned by pf3-S10)
for SIGNAL: rank information coefficient (rank-IC), IC information ratio, t-stat, and
sign-consistency over the horizon ladder, plus the IC-decay profile across that ladder.

Forward returns are used ONLY as a scoring *target* (t+1..t+h). They are never fed back
into any factor value, and this module never rewrites the panel it reads.

This is task PF4-S1-0 (migration 0176): the IC surface only. Later tasks in this sprint
(quantile/decile spread, turnover, factor-to-factor correlation/crowding, breadth, and the
gated leakage/coverage DQC checks) extend ``evaluate_panel`` without reworking this module.
"""

from __future__ import annotations

import hashlib
import math
from dataclasses import dataclass
from typing import Any, Iterable

import numpy as np
import pandas as pd

from .connection import DEFAULT_DB_PATH, connect  # noqa: F401  (re-exported per interfaces contract)
from .warehouse import insert_frame, json_dumps, quality_check  # noqa: F401  (quality_check reused by later tasks)


IC_HORIZONS: tuple[int, ...] = (1, 5, 10, 21, 63)
DEFAULT_N_QUANTILES: int = 10
SOURCE_NAME: str = "atx-impl signal evaluation engine"
DEFAULT_UNIVERSE_ID: str = "us_common_equity_liquid_v1"

# clause-G gated DQC check names (registered in quality_check_registry by migration 0179)
LEAKAGE_DQC_CHECK_NAME: str = "factor_leakage_tplus0"
COVERAGE_DQC_CHECK_NAME: str = "factor_coverage_asof_universe"
DEFAULT_LEAKAGE_ABS_CORR_THRESHOLD: float = 0.10  # |corr(factor, t+0 return)| above this -> leaky
DEFAULT_COVERAGE_MIN_FRACTION: float = 0.50  # factor must cover >= 50% of the as-of universe


@dataclass(frozen=True)
class IcResult:
    ic: pd.DataFrame  # factor_id, horizon, mean_rank_ic, ic_std, ic_information_ratio,
    #   ic_tstat, sign_consistency, n_dates, mean_names
    ic_decay: pd.DataFrame  # factor_id, horizon, ladder_position, mean_rank_ic, decay_ratio
    per_date: pd.DataFrame  # factor_id, as_of_date, horizon, rank_ic, n_names   (intermediate; not persisted)


_PER_DATE_COLUMNS = ["factor_id", "as_of_date", "horizon", "rank_ic", "n_names"]
_IC_COLUMNS = [
    "factor_id",
    "horizon",
    "mean_rank_ic",
    "ic_std",
    "ic_information_ratio",
    "ic_tstat",
    "sign_consistency",
    "n_dates",
    "mean_names",
]
_IC_DECAY_COLUMNS = ["factor_id", "horizon", "ladder_position", "mean_rank_ic", "decay_ratio"]

_FACTOR_EVAL_MANIFEST_COLUMNS = [
    "eval_id",
    "factor_id",
    "eval_kind",
    "universe_id",
    "start_date",
    "end_date",
    "horizon_days",
    "n_quantiles",
    "evaluation_days",
    "factor_row_count",
    "params_json",
    "source",
    "run_id",
]
_FACTOR_IC_TABLE_COLUMNS = [
    "eval_id",
    "factor_id",
    "horizon",
    "mean_rank_ic",
    "ic_std",
    "ic_information_ratio",
    "ic_tstat",
    "sign_consistency",
    "n_dates",
    "mean_names",
    "universe_id",
    "start_date",
    "end_date",
    "source",
    "run_id",
]
_FACTOR_IC_DECAY_TABLE_COLUMNS = [
    "eval_id",
    "factor_id",
    "horizon",
    "ladder_position",
    "mean_rank_ic",
    "decay_ratio",
    "universe_id",
    "source",
    "run_id",
]


# ---------------------------------------------------------------------------
# pure transforms (pandas in -> long DataFrame out; no DuckDB)
# ---------------------------------------------------------------------------


def compute_forward_returns(prices: pd.DataFrame, horizons: Iterable[int] = IC_HORIZONS) -> pd.DataFrame:
    """Derive long forward returns from a price panel.

    ``prices``: security_id, as_of_date, close  ->  long: security_id, as_of_date, horizon, forward_return.
    ``forward_return`` at ``as_of_date`` is ``close[t+h] / close[t] - 1``; rows with no t+h close
    (end of the series) are dropped rather than emitted as NaN.
    """

    horizons = tuple(horizons)
    columns = ["security_id", "as_of_date", "horizon", "forward_return"]
    if prices.empty:
        return pd.DataFrame(columns=columns)

    frame = prices.loc[:, ["security_id", "as_of_date", "close"]].copy()
    frame["as_of_date"] = pd.to_datetime(frame["as_of_date"])
    frame = frame.sort_values(["security_id", "as_of_date"], kind="stable").reset_index(drop=True)
    grouped_close = frame.groupby("security_id", sort=True)["close"]

    parts: list[pd.DataFrame] = []
    for horizon in horizons:
        future_close = grouped_close.shift(-horizon)
        forward_return = future_close / frame["close"] - 1.0
        parts.append(
            pd.DataFrame(
                {
                    "security_id": frame["security_id"],
                    "as_of_date": frame["as_of_date"].dt.date,
                    "horizon": int(horizon),
                    "forward_return": forward_return,
                }
            )
        )
    result = pd.concat(parts, ignore_index=True) if parts else pd.DataFrame(columns=columns)
    result = result.dropna(subset=["forward_return"])
    result = result.sort_values(["security_id", "as_of_date", "horizon"], kind="stable").reset_index(drop=True)
    return result.loc[:, columns]


def _normalize_asof(frame: pd.DataFrame) -> pd.DataFrame:
    """Coerce ``as_of_date`` to one canonical dtype (midnight-normalized datetime64).

    A panel read from DuckDB via ``.df()`` carries ``as_of_date`` as ``datetime64``, while
    ``compute_forward_returns`` emits it as ``datetime.date`` objects (object dtype). Merging
    the two on ``as_of_date`` otherwise raises ``ValueError: You are trying to merge on
    datetime64[...] and object columns``. Normalizing BOTH sides to the same representation
    before every panel x forward_returns merge closes that gap. Deterministic and row-order
    preserving (no sort side effects); returns a copy, never mutating the caller's frame.
    """

    if "as_of_date" not in frame.columns:
        return frame
    out = frame.copy()
    out["as_of_date"] = pd.to_datetime(out["as_of_date"]).dt.normalize()
    return out


def _rank_corr(values: pd.Series, targets: pd.Series) -> tuple[float, int]:
    """Spearman rank correlation of ``values``/``targets`` via Pearson-of-ranks (scipy-free)."""

    valid = values.notna() & targets.notna()
    n_names = int(valid.sum())
    if n_names < 3:
        return float("nan"), n_names
    ranked_values = values[valid].rank()
    ranked_targets = targets[valid].rank()
    if ranked_values.nunique() < 2 or ranked_targets.nunique() < 2:
        return float("nan"), n_names
    return float(ranked_values.corr(ranked_targets)), n_names


def _empty_ic_result() -> IcResult:
    return IcResult(
        ic=pd.DataFrame(columns=_IC_COLUMNS),
        ic_decay=pd.DataFrame(columns=_IC_DECAY_COLUMNS),
        per_date=pd.DataFrame(columns=_PER_DATE_COLUMNS),
    )


def _aggregate_ic(per_date: pd.DataFrame) -> pd.DataFrame:
    if per_date.empty:
        return pd.DataFrame(columns=_IC_COLUMNS)

    rows: list[dict[str, Any]] = []
    for (factor_id, horizon), group in per_date.groupby(["factor_id", "horizon"], sort=True):
        valid = group.dropna(subset=["rank_ic"])
        n_dates = int(len(valid))
        if n_dates == 0:
            rows.append(
                {
                    "factor_id": factor_id,
                    "horizon": int(horizon),
                    "mean_rank_ic": float("nan"),
                    "ic_std": float("nan"),
                    "ic_information_ratio": float("nan"),
                    "ic_tstat": float("nan"),
                    "sign_consistency": float("nan"),
                    "n_dates": 0,
                    "mean_names": float("nan"),
                }
            )
            continue
        mean_rank_ic = float(valid["rank_ic"].mean())
        ic_std = float(valid["rank_ic"].std(ddof=1)) if n_dates > 1 else float("nan")
        if math.isnan(ic_std) or ic_std == 0.0:
            ic_information_ratio = float("nan")
        else:
            ic_information_ratio = mean_rank_ic / ic_std
        ic_tstat = (
            ic_information_ratio * math.sqrt(n_dates)
            if not math.isnan(ic_information_ratio)
            else float("nan")
        )
        sign_target = np.sign(mean_rank_ic)
        sign_consistency = float((np.sign(valid["rank_ic"]) == sign_target).mean())
        mean_names = float(valid["n_names"].mean())
        rows.append(
            {
                "factor_id": factor_id,
                "horizon": int(horizon),
                "mean_rank_ic": mean_rank_ic,
                "ic_std": ic_std,
                "ic_information_ratio": ic_information_ratio,
                "ic_tstat": ic_tstat,
                "sign_consistency": sign_consistency,
                "n_dates": n_dates,
                "mean_names": mean_names,
            }
        )
    ic = pd.DataFrame(rows, columns=_IC_COLUMNS)
    return ic.sort_values(["factor_id", "horizon"], kind="stable").reset_index(drop=True)


def _compute_ic_decay(ic: pd.DataFrame, horizons: tuple[int, ...]) -> pd.DataFrame:
    if ic.empty:
        return pd.DataFrame(columns=_IC_DECAY_COLUMNS)

    ordered = sorted(set(horizons))
    ladder_position = {horizon: position + 1 for position, horizon in enumerate(ordered)}
    shortest = ordered[0]

    rows: list[dict[str, Any]] = []
    for factor_id, group in ic.groupby("factor_id", sort=True):
        by_horizon = dict(zip(group["horizon"], group["mean_rank_ic"]))
        base = by_horizon.get(shortest, float("nan"))
        for horizon in ordered:
            if horizon not in by_horizon:
                continue
            mean_rank_ic = by_horizon[horizon]
            if horizon == shortest:
                decay_ratio = 1.0
            elif math.isnan(base) or base == 0.0 or (isinstance(mean_rank_ic, float) and math.isnan(mean_rank_ic)):
                decay_ratio = float("nan")
            else:
                decay_ratio = float(mean_rank_ic / base)
            rows.append(
                {
                    "factor_id": factor_id,
                    "horizon": int(horizon),
                    "ladder_position": ladder_position[horizon],
                    "mean_rank_ic": mean_rank_ic,
                    "decay_ratio": decay_ratio,
                }
            )
    decay = pd.DataFrame(rows, columns=_IC_DECAY_COLUMNS)
    return decay.sort_values(["factor_id", "horizon"], kind="stable").reset_index(drop=True)


def compute_information_coefficient(
    panel: pd.DataFrame,
    forward_returns: pd.DataFrame,
    *,
    horizons: Iterable[int] = IC_HORIZONS,
) -> IcResult:
    """Per-(factor, as_of_date, horizon) Spearman rank-IC, aggregated across dates.

    ``panel``: security_id, as_of_date, factor_id, value.
    ``forward_returns``: security_id, as_of_date, horizon, forward_return.

    Cross-sections with fewer than 3 non-null (value, forward_return) pairs produce a NaN
    rank_ic and are excluded from the date-level aggregate. All outputs are stable-sorted
    by ["factor_id", "horizon"] with a reset index so identical inputs (any row order)
    reproduce byte-identical rows.
    """

    horizons = tuple(horizons)
    if panel is None or forward_returns is None or panel.empty or forward_returns.empty:
        return _empty_ic_result()

    # Normalize as_of_date on BOTH frames to one dtype before merging: the panel (from
    # DuckDB) is datetime64 while forward_returns (from compute_forward_returns) is
    # datetime.date objects, and pandas refuses to merge across that dtype mismatch.
    panel_norm = _normalize_asof(panel.loc[:, ["security_id", "as_of_date", "factor_id", "value"]])
    forward_norm = _normalize_asof(
        forward_returns.loc[:, ["security_id", "as_of_date", "horizon", "forward_return"]]
    )
    merged = panel_norm.merge(
        forward_norm,
        on=["security_id", "as_of_date"],
        how="inner",
    )
    merged = merged[merged["horizon"].isin(horizons)]
    if merged.empty:
        return _empty_ic_result()

    per_date_rows: list[dict[str, Any]] = []
    for (factor_id, as_of_date, horizon), group in merged.groupby(
        ["factor_id", "as_of_date", "horizon"], sort=True
    ):
        rank_ic, n_names = _rank_corr(group["value"], group["forward_return"])
        per_date_rows.append(
            {
                "factor_id": factor_id,
                "as_of_date": as_of_date,
                "horizon": int(horizon),
                "rank_ic": rank_ic,
                "n_names": n_names,
            }
        )
    per_date = pd.DataFrame(per_date_rows, columns=_PER_DATE_COLUMNS)
    per_date = per_date.sort_values(["factor_id", "horizon", "as_of_date"], kind="stable").reset_index(drop=True)

    ic = _aggregate_ic(per_date)
    ic_decay = _compute_ic_decay(ic, horizons)
    return IcResult(ic=ic, ic_decay=ic_decay, per_date=per_date)


# ---------------------------------------------------------------------------
# panel read (read-only) + persistence + orchestration (DuckDB)
# ---------------------------------------------------------------------------


def load_panel_for_eval(
    store,
    *,
    start_date=None,
    end_date=None,
    factor_ids: Iterable[str] | None = None,
) -> pd.DataFrame:
    """Read-only SELECT over ``v_factor_panel`` (the pf3-S10 exported panel).

    Never writes to the panel; only filters by an optional date window and/or factor_id set.
    """

    filters: list[str] = []
    params: list[Any] = []
    if start_date is not None:
        filters.append("p.as_of_date >= ?")
        params.append(start_date)
    if end_date is not None:
        filters.append("p.as_of_date <= ?")
        params.append(end_date)

    factor_list = [str(factor_id) for factor_id in factor_ids] if factor_ids else []
    join_sql = ""
    registered = False
    if factor_list:
        store.con.register("signal_eval_factor_filter", pd.DataFrame({"factor_id": factor_list}))
        registered = True
        join_sql = "JOIN signal_eval_factor_filter ff ON ff.factor_id = p.factor_id"

    where_sql = f"WHERE {' AND '.join(filters)}" if filters else ""
    try:
        return store.con.execute(
            f"""
            SELECT
                p.security_id,
                p.as_of_date,
                p.factor_id,
                p.value,
                p.available_at
            FROM v_factor_panel p
            {join_sql}
            {where_sql}
            ORDER BY p.factor_id, p.as_of_date, p.security_id
            """,
            params,
        ).df()
    finally:
        if registered:
            store.con.unregister("signal_eval_factor_filter")


def _hash_eval_id(prefix: str, *parts: object) -> str:
    """Deterministic id hash, mirroring ``db.alpha_research._hash_id``."""

    payload = "|".join("" if part is None else str(part) for part in parts)
    return hashlib.sha256(f"{prefix}|{payload}".encode("utf-8")).hexdigest()


def _build_ic_manifest(
    panel: pd.DataFrame,
    ic: pd.DataFrame,
    *,
    universe_id: str,
    horizons: tuple[int, ...],
    n_quantiles: int,
    source: str,
    run_id: str | None,
) -> pd.DataFrame:
    """One ``factor_eval_manifest`` row per factor scored by ``compute_information_coefficient``.

    ``eval_id`` is deterministic over (factor_id, eval_kind, universe_id, date window,
    params) so re-evaluating the same factor/window/params replaces its manifest row
    regardless of ``run_id`` -- mirroring ``alpha_research.py``'s ``backtest_id`` idiom.
    """

    if ic.empty:
        return pd.DataFrame(columns=_FACTOR_EVAL_MANIFEST_COLUMNS)

    params_json = json_dumps({"horizons": list(horizons), "n_quantiles": n_quantiles})

    if panel.empty:
        panel_stats = pd.DataFrame(
            columns=["start_date", "end_date", "evaluation_days", "factor_row_count"]
        )
    else:
        panel_stats = panel.groupby("factor_id").agg(
            start_date=("as_of_date", "min"),
            end_date=("as_of_date", "max"),
            evaluation_days=("as_of_date", "nunique"),
            factor_row_count=("value", "size"),
        )

    rows: list[dict[str, Any]] = []
    for factor_id in sorted(ic["factor_id"].unique()):
        if factor_id in panel_stats.index:
            stats = panel_stats.loc[factor_id]
            start_date = stats["start_date"]
            end_date = stats["end_date"]
            evaluation_days = int(stats["evaluation_days"])
            factor_row_count = int(stats["factor_row_count"])
        else:
            start_date = None
            end_date = None
            evaluation_days = None
            factor_row_count = None

        eval_id = _hash_eval_id(
            "factor_eval_manifest", factor_id, "ic", universe_id, start_date, end_date, params_json
        )
        rows.append(
            {
                "eval_id": eval_id,
                "factor_id": factor_id,
                "eval_kind": "ic",
                "universe_id": universe_id,
                "start_date": start_date,
                "end_date": end_date,
                "horizon_days": None,
                "n_quantiles": None,
                "evaluation_days": evaluation_days,
                "factor_row_count": factor_row_count,
                "params_json": params_json,
                "source": source,
                "run_id": run_id,
            }
        )
    manifest = pd.DataFrame(rows, columns=_FACTOR_EVAL_MANIFEST_COLUMNS)
    return manifest.sort_values(["factor_id"], kind="stable").reset_index(drop=True)


def _attach_manifest_lineage(
    frame: pd.DataFrame,
    manifest: pd.DataFrame,
    *,
    universe_id: str,
    run_id: str | None,
) -> pd.DataFrame:
    lineage_columns = ["factor_id", "eval_id", "start_date", "end_date", "source"]
    lineage = (
        manifest.loc[:, lineage_columns] if not manifest.empty else pd.DataFrame(columns=lineage_columns)
    )
    out = frame.merge(lineage, on="factor_id", how="left")
    out["universe_id"] = universe_id
    out["run_id"] = run_id
    return out


def _replace_rows(
    store,
    frame: pd.DataFrame,
    *,
    table: str,
    relation_name: str,
    key_columns: tuple[str, ...],
) -> int:
    """Idempotent DELETE-then-``insert_frame`` replace, keyed by ``key_columns``.

    Mirrors ``db.alpha_research``'s manifest discipline. This is a deliberate null-safe
    variant of ``db.warehouse.replace_by_relation``: that sibling helper keys its DELETE on
    ``coalesce(cast(col AS VARCHAR), '')``, which conflates SQL NULL with the empty string,
    whereas here we use ``IS NOT DISTINCT FROM`` so a nullable natural-key component
    (``run_id``) matches NULL-to-NULL without collapsing NULL and '' -- required for
    ``factor_ic``/``factor_ic_decay`` idempotency when ``run_id`` is not supplied.
    """

    if frame.empty:
        return 0
    store.con.register(relation_name, frame)
    try:
        predicate = " AND ".join(f"dst.{column} IS NOT DISTINCT FROM src.{column}" for column in key_columns)
        store.con.execute(
            f"""
            DELETE FROM {table} AS dst
            USING {relation_name} AS src
            WHERE {predicate}
            """
        )
        return insert_frame(store, frame, table, f"{relation_name}_insert")
    finally:
        store.con.unregister(relation_name)


def persist_factor_ic(
    store,
    *,
    manifest: pd.DataFrame,
    ic: pd.DataFrame,
    ic_decay: pd.DataFrame,
    universe_id: str,
    run_id: str | None,
) -> dict[str, int]:
    """Attach manifest lineage to the IC / IC-decay rows and persist all three tables."""

    ic_persist = _attach_manifest_lineage(ic, manifest, universe_id=universe_id, run_id=run_id)
    ic_decay_persist = _attach_manifest_lineage(ic_decay, manifest, universe_id=universe_id, run_id=run_id)

    with store.transaction():
        manifest_count = _replace_rows(
            store,
            manifest,
            table="factor_eval_manifest",
            relation_name="factor_eval_manifest_load",
            key_columns=("eval_id",),
        )
        ic_count = _replace_rows(
            store,
            ic_persist.loc[:, _FACTOR_IC_TABLE_COLUMNS],
            table="factor_ic",
            relation_name="factor_ic_load",
            key_columns=("factor_id", "horizon", "universe_id", "run_id"),
        )
        ic_decay_count = _replace_rows(
            store,
            ic_decay_persist.loc[:, _FACTOR_IC_DECAY_TABLE_COLUMNS],
            table="factor_ic_decay",
            relation_name="factor_ic_decay_load",
            key_columns=("factor_id", "horizon", "universe_id", "run_id"),
        )

    return {
        "factor_eval_manifest": manifest_count,
        "factor_ic": ic_count,
        "factor_ic_decay": ic_decay_count,
    }


def _derive_forward_returns_from_prices(store, *, horizons: tuple[int, ...]) -> pd.DataFrame:
    prices = store.con.execute(
        """
        SELECT security_id, trade_date AS as_of_date, close
        FROM equity_daily_bars
        WHERE close IS NOT NULL
        ORDER BY security_id, trade_date
        """
    ).df()
    return compute_forward_returns(prices, horizons=horizons)


def evaluate_panel(
    store,
    *,
    forward_returns: pd.DataFrame | None = None,
    n_quantiles: int = DEFAULT_N_QUANTILES,
    horizons: Iterable[int] = IC_HORIZONS,
    universe_id: str = DEFAULT_UNIVERSE_ID,
    run_id: str | None = None,
) -> dict[str, int]:
    """Orchestrate the IC surface: read the panel, score IC/IC-decay, persist.

    Reads ``v_factor_panel`` read-only via ``load_panel_for_eval``. If ``forward_returns``
    is not supplied, it is derived from ``equity_daily_bars`` (never re-derived from the
    panel itself, keeping the scoring target strictly separate from factor inputs).

    This is the IC-only orchestrator for task PF4-S1-0; later tasks in this sprint extend
    the returned counts dict with quantile/turnover/correlation/crowding/breadth/DQC keys
    without reworking this function's shape.
    """

    horizons = tuple(horizons)
    panel = load_panel_for_eval(store)
    if forward_returns is None:
        forward_returns = _derive_forward_returns_from_prices(store, horizons=horizons)

    ic_result = compute_information_coefficient(panel, forward_returns, horizons=horizons)
    manifest = _build_ic_manifest(
        panel,
        ic_result.ic,
        universe_id=universe_id,
        horizons=horizons,
        n_quantiles=n_quantiles,
        source=SOURCE_NAME,
        run_id=run_id,
    )

    return persist_factor_ic(
        store,
        manifest=manifest,
        ic=ic_result.ic,
        ic_decay=ic_result.ic_decay,
        universe_id=universe_id,
        run_id=run_id,
    )
