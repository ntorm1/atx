"""Estimates store datasets for the atx-impl DuckDB warehouse.

Provides:
- EstimateMeasureSeedDataset  — static dimension seed (5 measures)
- EstimateActualsDataset       — REAL: reads sec_company_facts, maps XBRL→measure
- EstimateSurpriseDataset      — REAL derived: Standardized Unexpected Earnings (SUE)
                                  Foster-Olsen-Shevlin (1984) seasonal-random-walk-with-drift.
- EstimateConsensusDataset     — injectable; default-empty (licensed: IBES/FactSet/Zacks)
- EstimateGuidanceDataset      — injectable; default-empty (SEC 8-K Item 2.02/7.01 NER TODO)
- EstimateRecommendationDataset — injectable; default-empty (licensed vendor)

PIT contract
------------
est_actual   : available_at CARRIED from sec_company_facts (filing availability, UTC-naive).
               Do NOT restamp to now().
est_surprise : available_at CARRIED from the originally-reported actual_t (SUE is knowable
               when period t is filed).  Prior-period restated values do NOT alter earlier SUE rows.
est_consensus: available_at carried from provider row if supplied; else now_utc_naive().
est_guidance : available_at carried from provider row if supplied; else now_utc_naive().
est_recommendation: available_at carried from provider row if supplied; else now_utc_naive().

Revenue concept preference
--------------------------
For measure_code=REVENUE we prefer concept "Revenues".  If a security/fy/fp/accession has
no "Revenues" value we fall back to "RevenueFromContractWithCustomerExcludingAssessedTax".
Implemented via a SQL window function that ranks concepts per PK group.
"""
from __future__ import annotations

import json
import math
from dataclasses import dataclass
from typing import Any, Callable, Iterable

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import now_utc_naive, quality_check


# ──────────────────────────────────────────────────────────────────────────────
# §1 EstimateMeasureSeedDataset
# ──────────────────────────────────────────────────────────────────────────────

_MEASURE_ROWS: list[dict[str, Any]] = [
    {
        "measure_code": "EPS_DILUTED",
        "label": "Diluted EPS",
        "statement": "income",
        "unit_type": "per_share",
        "is_per_share": True,
        "higher_is_better": True,
        "us_gaap_concepts": json.dumps(["EarningsPerShareDiluted"]),
        "source": "est_measure_seed",
    },
    {
        "measure_code": "EPS_BASIC",
        "label": "Basic EPS",
        "statement": "income",
        "unit_type": "per_share",
        "is_per_share": True,
        "higher_is_better": True,
        "us_gaap_concepts": json.dumps(["EarningsPerShareBasic"]),
        "source": "est_measure_seed",
    },
    {
        "measure_code": "REVENUE",
        "label": "Revenue",
        "statement": "income",
        "unit_type": "currency",
        "is_per_share": False,
        "higher_is_better": True,
        "us_gaap_concepts": json.dumps(
            ["Revenues", "RevenueFromContractWithCustomerExcludingAssessedTax"]
        ),
        "source": "est_measure_seed",
    },
    {
        "measure_code": "NET_INCOME",
        "label": "Net income",
        "statement": "income",
        "unit_type": "currency",
        "is_per_share": False,
        "higher_is_better": True,
        "us_gaap_concepts": json.dumps(["NetIncomeLoss"]),
        "source": "est_measure_seed",
    },
    {
        "measure_code": "OPERATING_INCOME",
        "label": "Operating income",
        "statement": "income",
        "unit_type": "currency",
        "is_per_share": False,
        "higher_is_better": True,
        "us_gaap_concepts": json.dumps(["OperatingIncomeLoss"]),
        "source": "est_measure_seed",
    },
]


@dataclass(frozen=True)
class EstimateMeasureSeedOptions:
    run_id: str | None = None


class EstimateMeasureSeedDataset(Dataset):
    dataset_id = "est_measure"
    source_name = "est_measure_seed"

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: EstimateMeasureSeedOptions) -> DatasetLoadResult:
        df = pd.DataFrame(_MEASURE_ROWS)
        store.con.register("_est_measure_seed", df)
        try:
            store.con.execute(
                """
                INSERT OR REPLACE INTO est_measure (
                    measure_code, label, statement, unit_type,
                    is_per_share, higher_is_better, us_gaap_concepts, source
                )
                SELECT
                    measure_code, label, statement, unit_type,
                    is_per_share, higher_is_better, us_gaap_concepts, source
                FROM _est_measure_seed
                """
            )
        finally:
            store.con.unregister("_est_measure_seed")

        rows_loaded = len(_MEASURE_ROWS)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="est_measure",
            check_name="est_measure_row_count",
            status="passed",
            observed_value=float(rows_loaded),
            threshold_value=5.0,
            details={"source": self.source_name},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows_loaded,
            source=self.source_name,
            details={"measures": [r["measure_code"] for r in _MEASURE_ROWS]},
        )


# ──────────────────────────────────────────────────────────────────────────────
# §2 EstimateActualsDataset
# ──────────────────────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class EstimateActualsOptions:
    source: str = "sec_company_facts"
    measure_codes: tuple[str, ...] | None = None
    security_ids: tuple[str, ...] | None = None
    run_id: str | None = None


def _build_concept_map(store: DuckDBStore) -> dict[str, str]:
    """Read est_measure.us_gaap_concepts and build concept -> measure_code mapping.

    For REVENUE, both Revenues and RevenueFromContractWithCustomerExcludingAssessedTax
    map to REVENUE; the coalesce preference is handled in the SQL query.
    """
    rows = store.con.execute(
        "SELECT measure_code, us_gaap_concepts FROM est_measure WHERE us_gaap_concepts IS NOT NULL"
    ).fetchall()
    concept_map: dict[str, str] = {}
    for measure_code, concepts_json in rows:
        try:
            concepts = json.loads(concepts_json)
        except (json.JSONDecodeError, TypeError):
            continue
        for concept in concepts:
            concept_map[concept] = measure_code
    return concept_map


class EstimateActualsDataset(Dataset):
    dataset_id = "est_actual"
    source_name = "sec_company_facts"

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: EstimateActualsOptions) -> DatasetLoadResult:
        # Ensure measure seed is loaded so concept_map works
        concept_map = _build_concept_map(store)
        if not concept_map:
            # If no measures seeded yet, seed them inline
            EstimateMeasureSeedDataset().load(store, EstimateMeasureSeedOptions())
            concept_map = _build_concept_map(store)

        # Build a concept-filter list: all known concepts
        all_concepts = list(concept_map.keys())

        # Apply measure_codes filter
        if options.measure_codes:
            wanted_measures = set(options.measure_codes)
            all_concepts = [c for c, m in concept_map.items() if m in wanted_measures]

        if not all_concepts:
            return DatasetLoadResult(
                dataset_id=self.dataset_id,
                rows_loaded=0,
                source=self.source_name,
                details={"reason": "no concepts matched options.measure_codes"},
            )

        # Build concept→measure mapping as a temporary table (avoids DuckDB registered-
        # DataFrame column-binding bugs when combined with window functions).
        store.con.execute("DROP TABLE IF EXISTS _tmp_est_concept_map")
        store.con.execute(
            "CREATE TEMP TABLE _tmp_est_concept_map (concept VARCHAR, measure_code VARCHAR)"
        )
        map_rows = [(c, m) for c, m in concept_map.items() if c in all_concepts]
        for concept_val, measure_val in map_rows:
            store.con.execute(
                "INSERT INTO _tmp_est_concept_map VALUES (?, ?)", [concept_val, measure_val]
            )

        # Optional security_ids filter as temp table
        security_filter_join = ""
        if options.security_ids:
            store.con.execute("DROP TABLE IF EXISTS _tmp_est_security_filter")
            store.con.execute(
                "CREATE TEMP TABLE _tmp_est_security_filter (security_id VARCHAR)"
            )
            for sid in options.security_ids:
                store.con.execute(
                    "INSERT INTO _tmp_est_security_filter VALUES (?)", [sid]
                )
            security_filter_join = (
                "JOIN _tmp_est_security_filter sf ON sf.security_id = f.security_id"
            )

        try:
            # For REVENUE: if a security/fy/fp/accession has BOTH concepts, prefer "Revenues".
            # Use row_number() to pick the preferred concept per PK group.
            sql = f"""
            SELECT
                f.security_id,
                m.measure_code,
                f.fiscal_year,
                f.fiscal_period,
                f.period_end,
                f.value,
                f.unit,
                f.form,
                f.accession_number,
                f.filed_date        AS announce_date,
                f.period_end        AS as_of_date,
                f.available_at
            FROM sec_company_facts f
            JOIN _tmp_est_concept_map m ON m.concept = f.concept
            {security_filter_join}
            WHERE f.fiscal_period IN ('Q1','Q2','Q3','Q4','FY')
              AND f.period_end IS NOT NULL
              AND f.value IS NOT NULL
              AND f.form IN ('10-Q','10-K','8-K','10-K/A','10-Q/A')
              -- For REVENUE: if a preferred concept "Revenues" exists for the same
              -- (security, fiscal_year, fiscal_period, accession, period_end),
              -- skip the fallback concept row so we don't double-count.
              AND NOT (
                  m.measure_code = 'REVENUE'
                  AND f.concept != 'Revenues'
                  AND EXISTS (
                      SELECT 1
                      FROM sec_company_facts f2
                      WHERE f2.security_id      = f.security_id
                        AND f2.concept          = 'Revenues'
                        AND f2.fiscal_year      = f.fiscal_year
                        AND f2.fiscal_period    = f.fiscal_period
                        AND f2.accession_number = f.accession_number
                        AND f2.period_end       = f.period_end
                  )
              )
            """
            result_df = store.con.execute(sql).df()
        finally:
            store.con.execute("DROP TABLE IF EXISTS _tmp_est_concept_map")
            if options.security_ids:
                store.con.execute("DROP TABLE IF EXISTS _tmp_est_security_filter")

        if result_df.empty:
            return DatasetLoadResult(
                dataset_id=self.dataset_id,
                rows_loaded=0,
                source=self.source_name,
                details={"reason": "no sec_company_facts rows matched"},
            )

        result_df["source"] = options.source
        result_df["run_id"] = options.run_id

        store.con.register("_est_actual_batch", result_df)
        try:
            store.con.execute(
                """
                INSERT OR REPLACE INTO est_actual (
                    security_id, measure_code, fiscal_year, fiscal_period,
                    period_end, value, unit, form, accession_number,
                    announce_date, as_of_date, available_at, run_id, source
                )
                SELECT
                    security_id, measure_code, fiscal_year, fiscal_period,
                    period_end, value, unit, form, accession_number,
                    announce_date, as_of_date, available_at, run_id, source
                FROM _est_actual_batch
                """
            )
        finally:
            store.con.unregister("_est_actual_batch")

        rows_loaded = len(result_df)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="est_actual",
            check_name="est_actual_loaded",
            status="passed",
            observed_value=float(rows_loaded),
            threshold_value=0.0,
            details={"source": self.source_name},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows_loaded,
            source=self.source_name,
            details={"concepts_mapped": len(concept_map)},
        )


# ──────────────────────────────────────────────────────────────────────────────
# §3 EstimateSurpriseDataset  (SUE, Foster-Olsen-Shevlin 1984)
# ──────────────────────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class EstimateSurpriseOptions:
    measure_codes: tuple[str, ...] | None = None
    window: int = 8
    min_obs: int = 4
    model: str = "srw_drift"
    run_id: str | None = None


def _compute_sue_series(
    df: pd.DataFrame,
    *,
    window: int,
    min_obs: int,
    model: str,
) -> list[dict[str, Any]]:
    """Compute SUE for one (security_id, measure_code, series_type) group.

    df must have columns: fiscal_year, fiscal_period, period_end, actual, available_at
    sorted by period_end ascending.

    seasonal-random-walk-with-drift:
    - Quarterly series: seasonal prior = same fp from fy-1
    - Annual series:    seasonal prior = FY from fy-1
    - For each period t with a prior:
        Δ_t = actual_t − actual_{prior(t)}
      Using only Δ values from periods STRICTLY BEFORE t (up to `window` most recent):
        drift  = mean(trailing Δs)
        sigma  = stdev(trailing Δs, ddof=1)
        expected_t = actual_{prior(t)} + drift
        surprise_t = actual_t − expected_t
        sue_t      = surprise_t / sigma
      NULL if fewer than min_obs prior Δ, or sigma == 0/NaN.
    """
    # Build a dict: (fy, fp) -> row for fast seasonal lookup
    # Use EARLIEST available_at per (fy, fp) — no lookahead. A NaT/None available_at
    # sorts as "latest" (never preferred as originally-reported) so a real filing
    # timestamp always wins; a group of all-NaT keeps the first row seen.
    earliest: dict[tuple[int, str], dict[str, Any]] = {}
    for _, row in df.iterrows():
        key = (int(row["fiscal_year"]), str(row["fiscal_period"]))
        cur = row["available_at"]
        if key not in earliest:
            earliest[key] = row.to_dict()
            continue
        prev = earliest[key]["available_at"]
        if pd.notna(cur) and (pd.isna(prev) or cur < prev):
            earliest[key] = row.to_dict()

    # Sort periods chronologically by period_end
    sorted_keys = sorted(earliest.keys(), key=lambda k: earliest[k]["period_end"])

    # Build ordered list of (key, row) — this is the no-lookahead series
    series = [(k, earliest[k]) for k in sorted_keys]

    # Compute seasonal diffs in order; Δ_t is appended AFTER processing period t
    # so Δ_t is never used in computing sue_t itself (strictly before t).
    deltas: list[float] = []  # ordered list of seasonal diffs Δ chronologically

    results: list[dict[str, Any]] = []

    for idx, (key, row) in enumerate(series):
        fy, fp = key
        actual_t = row["actual"]
        avail_t = row["available_at"]
        period_end_t = row["period_end"]

        # Find seasonal prior: same fp from fy-1
        prior_key = (fy - 1, fp)
        if prior_key not in earliest:
            # No prior → record actual but no SUE; no Δ to append
            results.append({
                "fiscal_year": fy,
                "fiscal_period": fp,
                "period_end": period_end_t,
                "actual": actual_t,
                "expected": None,
                "surprise": None,
                "sue": None,
                "as_of_date": period_end_t,
                "available_at": avail_t,
            })
            continue

        prior_row = earliest[prior_key]
        actual_prior = prior_row["actual"]
        delta_t = actual_t - actual_prior

        # Trailing window of Δs STRICTLY BEFORE period t (already in deltas list)
        trailing = deltas[-window:] if len(deltas) >= window else deltas[:]

        expected = None
        surprise = None
        sue = None

        if len(trailing) >= min_obs:
            n = len(trailing)
            mean_delta = sum(trailing) / n
            # sample stdev ddof=1
            variance = sum((x - mean_delta) ** 2 for x in trailing) / (n - 1)
            sigma = math.sqrt(variance)
            if sigma > 0 and not math.isnan(sigma):
                expected = actual_prior + mean_delta
                surprise = actual_t - expected
                sue = surprise / sigma

        results.append({
            "fiscal_year": fy,
            "fiscal_period": fp,
            "period_end": period_end_t,
            "actual": actual_t,
            "expected": expected,
            "surprise": surprise,
            "sue": sue,
            "as_of_date": period_end_t,
            "available_at": avail_t,
        })

        # NOW append Δ_t so it's available for FUTURE periods
        deltas.append(delta_t)

    return results


class EstimateSurpriseDataset(Dataset):
    dataset_id = "est_surprise"
    source_name = "est_surprise_srw_drift"

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: EstimateSurpriseOptions) -> DatasetLoadResult:
        # Pull all originally-reported actuals from est_actual
        # "originally reported" = earliest available_at per (security_id, measure_code, fy, fp)
        measure_filter = ""
        if options.measure_codes:
            placeholders = ",".join("?" * len(options.measure_codes))
            measure_filter = f"AND measure_code IN ({placeholders})"

        sql = f"""
        WITH ranked AS (
            SELECT
                security_id,
                measure_code,
                fiscal_year,
                fiscal_period,
                period_end,
                value          AS actual,
                available_at,
                row_number() OVER (
                    PARTITION BY security_id, measure_code, fiscal_year, fiscal_period
                    ORDER BY available_at ASC NULLS LAST
                ) AS rn
            FROM est_actual
            WHERE value IS NOT NULL
              AND period_end IS NOT NULL
              {measure_filter}
        )
        SELECT security_id, measure_code, fiscal_year, fiscal_period,
               period_end, actual, available_at
        FROM ranked
        WHERE rn = 1
        -- Per-series chronological order is re-established in Python (sort_values +
        -- _compute_sue_series sorted()); this ORDER BY is only for deterministic output.
        ORDER BY security_id, measure_code, period_end
        """
        params = list(options.measure_codes) if options.measure_codes else []
        actuals_df = store.con.execute(sql, params).df()

        if actuals_df.empty:
            return DatasetLoadResult(
                dataset_id=self.dataset_id,
                rows_loaded=0,
                source=self.source_name,
                details={"reason": "no actuals in est_actual"},
            )

        # Fetch consensus for surprise_pct enrichment (best-effort; no consensus = NULL)
        try:
            consensus_df = store.con.execute(
                """
                SELECT security_id, measure_code, period_end, mean AS consensus_mean,
                       available_at AS consensus_available_at
                FROM est_consensus
                WHERE mean IS NOT NULL
                """
            ).df()
            has_consensus = not consensus_df.empty
        except Exception:
            has_consensus = False
            consensus_df = pd.DataFrame()

        # Group by (security_id, measure_code, series_type) where series_type = Q or FY
        all_rows: list[dict[str, Any]] = []

        grouped = actuals_df.groupby(["security_id", "measure_code"])
        for (security_id, measure_code), grp in grouped:
            # Split quarterly vs annual
            q_mask = grp["fiscal_period"].isin(["Q1", "Q2", "Q3", "Q4"])
            fy_mask = grp["fiscal_period"] == "FY"

            for sub_df in (grp[q_mask], grp[fy_mask]):
                if sub_df.empty:
                    continue
                sub_df = sub_df.sort_values("period_end").reset_index(drop=True)
                rows = _compute_sue_series(
                    sub_df,
                    window=options.window,
                    min_obs=options.min_obs,
                    model=options.model,
                )
                for r in rows:
                    r["security_id"] = security_id
                    r["measure_code"] = measure_code
                    r["model"] = options.model
                    r["source"] = self.source_name
                    r["run_id"] = options.run_id
                    r["consensus_mean"] = None
                    r["surprise_pct"] = None
                    all_rows.append(r)

        if not all_rows:
            return DatasetLoadResult(
                dataset_id=self.dataset_id,
                rows_loaded=0,
                source=self.source_name,
                details={"reason": "no surprise rows computed"},
            )

        out_df = pd.DataFrame(all_rows)

        # Enrich with consensus if available
        if has_consensus and not consensus_df.empty:
            # For each surprise row, find consensus where consensus_available_at <= actual.available_at
            # We do this with a merge then filter
            merged = out_df.merge(
                consensus_df,
                on=["security_id", "measure_code", "period_end"],
                how="left",
            )
            mask = (
                merged["consensus_available_at"].notna()
                & merged["available_at"].notna()
                & (merged["consensus_available_at"] <= merged["available_at"])
            )
            merged.loc[mask, "consensus_mean"] = merged.loc[mask, "consensus_mean_y"]
            # surprise_pct = (actual - consensus_mean) / abs(consensus_mean)
            valid_pct = mask & merged["consensus_mean"].notna() & (merged["consensus_mean"] != 0)
            merged.loc[valid_pct, "surprise_pct"] = (
                (merged.loc[valid_pct, "actual"] - merged.loc[valid_pct, "consensus_mean"])
                / merged.loc[valid_pct, "consensus_mean"].abs()
            )
            # Keep only the columns we need
            cols = [
                "security_id", "measure_code", "fiscal_year", "fiscal_period",
                "period_end", "actual", "expected", "surprise", "sue",
                "consensus_mean", "surprise_pct", "model",
                "as_of_date", "available_at", "run_id", "source",
            ]
            out_df = merged[cols].copy()
        else:
            out_df = out_df[[
                "security_id", "measure_code", "fiscal_year", "fiscal_period",
                "period_end", "actual", "expected", "surprise", "sue",
                "consensus_mean", "surprise_pct", "model",
                "as_of_date", "available_at", "run_id", "source",
            ]]

        store.con.register("_est_surprise_batch", out_df)
        try:
            store.con.execute(
                """
                INSERT OR REPLACE INTO est_surprise (
                    security_id, measure_code, fiscal_year, fiscal_period,
                    period_end, actual, expected, surprise, sue,
                    consensus_mean, surprise_pct, model,
                    as_of_date, available_at, run_id, source
                )
                SELECT
                    security_id, measure_code, fiscal_year, fiscal_period,
                    period_end, actual, expected, surprise, sue,
                    consensus_mean, surprise_pct, model,
                    as_of_date, available_at, run_id, source
                FROM _est_surprise_batch
                """
            )
        finally:
            store.con.unregister("_est_surprise_batch")

        rows_loaded = len(out_df)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="est_surprise",
            check_name="est_surprise_loaded",
            status="passed",
            observed_value=float(rows_loaded),
            threshold_value=0.0,
            details={"model": options.model},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows_loaded,
            source=self.source_name,
            details={"model": options.model, "window": options.window, "min_obs": options.min_obs},
        )


# ──────────────────────────────────────────────────────────────────────────────
# §4 Injectable schema loaders
# ──────────────────────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class EstimateConsensusOptions:
    provider: Callable[[], Iterable[dict]] | None = None
    run_id: str | None = None


class EstimateConsensusDataset(Dataset):
    """Consensus estimates loader.

    Default-empty (licensed: IBES, FactSet Estimates, Zacks).
    Pass an injectable `provider: Callable[[], Iterable[dict]]` to populate.

    Append/snapshot semantics: est_consensus has no primary key and rows are INSERTed
    (not INSERT OR REPLACE). Each call appends the provider's rows verbatim, so a
    provider that re-emits the same consensus snapshot will create duplicate rows.
    Providers are responsible for emitting each (security, measure, period, consensus_date)
    snapshot at most once (or the caller should truncate before a full reload).
    """
    dataset_id = "est_consensus"
    source_name = "est_consensus_injectable"

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: EstimateConsensusOptions) -> DatasetLoadResult:
        if options.provider is None:
            return DatasetLoadResult(
                dataset_id=self.dataset_id,
                rows_loaded=0,
                source=self.source_name,
                details={"reason": "no provider supplied; table remains empty"},
            )

        rows = list(options.provider())
        if not rows:
            return DatasetLoadResult(
                dataset_id=self.dataset_id,
                rows_loaded=0,
                source=self.source_name,
                details={"reason": "provider returned no rows"},
            )

        df = pd.DataFrame(rows)
        # Stamp available_at if provider omits it
        if "available_at" not in df.columns:
            df["available_at"] = now_utc_naive()
        else:
            df["available_at"] = df["available_at"].fillna(now_utc_naive())
        df["run_id"] = df.get("run_id", options.run_id)
        df["source"] = df.get("source", self.source_name)
        # Fill optional columns not supplied by provider with None
        for col in ("consensus_date", "median", "high", "low", "stdev",
                    "num_estimates", "num_up", "num_down", "currency"):
            if col not in df.columns:
                df[col] = None

        store.con.register("_est_consensus_batch", df)
        try:
            store.con.execute(
                """
                INSERT INTO est_consensus (
                    security_id, measure_code, fiscal_year, fiscal_period,
                    period_end, consensus_date, mean, median, high, low,
                    stdev, num_estimates, num_up, num_down, currency,
                    as_of_date, available_at, run_id, source
                )
                SELECT
                    security_id, measure_code, fiscal_year, fiscal_period,
                    period_end, consensus_date, mean, median, high, low,
                    stdev, num_estimates, num_up, num_down, currency,
                    as_of_date, available_at, run_id, source
                FROM _est_consensus_batch
                """
            )
        finally:
            store.con.unregister("_est_consensus_batch")

        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=len(df),
            source=self.source_name,
            details={},
        )


@dataclass(frozen=True)
class EstimateGuidanceOptions:
    """Injectable guidance loader.

    Real source: SEC 8-K Item 2.02/7.01 free-text NER (TODO — not implemented).
    Supply both `fetch` and `parse` callables to populate; default-empty otherwise.
    """
    fetch: Callable[[], Iterable[Any]] | None = None
    parse: Callable[[Any], Iterable[dict]] | None = None
    run_id: str | None = None


class EstimateGuidanceDataset(Dataset):
    """Management guidance loader.

    Default-empty.  Real extraction requires SEC 8-K Item 2.02/7.01 free-text NER
    (documented as a TODO for a future sprint).  Provide both `fetch` and `parse`
    callables in options to populate.

    Append/snapshot semantics: est_guidance has no primary key and rows are INSERTed
    (not INSERT OR REPLACE). Re-running the same fetch/parse appends duplicate rows;
    the fetch/parse pair is responsible for not re-emitting already-loaded guidance
    (or the caller should truncate before a full reload).
    """
    dataset_id = "est_guidance"
    source_name = "est_guidance_injectable"

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: EstimateGuidanceOptions) -> DatasetLoadResult:
        if options.fetch is None or options.parse is None:
            return DatasetLoadResult(
                dataset_id=self.dataset_id,
                rows_loaded=0,
                source=self.source_name,
                details={"reason": "fetch/parse not supplied; table remains empty"},
            )

        parsed_rows: list[dict] = []
        for raw in options.fetch():
            parsed_rows.extend(options.parse(raw))

        if not parsed_rows:
            return DatasetLoadResult(
                dataset_id=self.dataset_id,
                rows_loaded=0,
                source=self.source_name,
                details={"reason": "provider returned no rows"},
            )

        df = pd.DataFrame(parsed_rows)
        if "available_at" not in df.columns:
            df["available_at"] = now_utc_naive()
        else:
            df["available_at"] = df["available_at"].fillna(now_utc_naive())
        df["run_id"] = options.run_id
        df["source"] = df.get("source", self.source_name)
        # Fill optional columns not supplied by provider with None
        for col in ("low", "high", "mid", "basis", "guidance_date", "form", "accession_number"):
            if col not in df.columns:
                df[col] = None

        store.con.register("_est_guidance_batch", df)
        try:
            store.con.execute(
                """
                INSERT INTO est_guidance (
                    security_id, measure_code, fiscal_year, fiscal_period,
                    period_end, low, high, mid, basis, guidance_date,
                    form, accession_number, as_of_date, available_at, run_id, source
                )
                SELECT
                    security_id, measure_code, fiscal_year, fiscal_period,
                    period_end, low, high, mid, basis, guidance_date,
                    form, accession_number, as_of_date, available_at, run_id, source
                FROM _est_guidance_batch
                """
            )
        finally:
            store.con.unregister("_est_guidance_batch")

        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=len(df),
            source=self.source_name,
            details={},
        )


@dataclass(frozen=True)
class EstimateRecommendationOptions:
    provider: Callable[[], Iterable[dict]] | None = None
    run_id: str | None = None


class EstimateRecommendationDataset(Dataset):
    """Broker recommendation loader.

    Default-empty (licensed vendor data — IBES, FactSet, etc.).
    Pass an injectable `provider: Callable[[], Iterable[dict]]` to populate.

    Append/snapshot semantics: est_recommendation has no primary key and rows are
    INSERTed (not INSERT OR REPLACE). Each call appends; a provider that re-emits the
    same rating events will create duplicates. The provider is responsible for emitting
    each rating action at most once (or the caller should truncate before a full reload).
    """
    dataset_id = "est_recommendation"
    source_name = "est_recommendation_injectable"

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(
        self, store: DuckDBStore, options: EstimateRecommendationOptions
    ) -> DatasetLoadResult:
        if options.provider is None:
            return DatasetLoadResult(
                dataset_id=self.dataset_id,
                rows_loaded=0,
                source=self.source_name,
                details={"reason": "no provider supplied; table remains empty"},
            )

        rows = list(options.provider())
        if not rows:
            return DatasetLoadResult(
                dataset_id=self.dataset_id,
                rows_loaded=0,
                source=self.source_name,
                details={"reason": "provider returned no rows"},
            )

        df = pd.DataFrame(rows)
        if "available_at" not in df.columns:
            df["available_at"] = now_utc_naive()
        else:
            df["available_at"] = df["available_at"].fillna(now_utc_naive())
        df["run_id"] = options.run_id
        df["source"] = df.get("source", self.source_name)
        # Fill optional columns not supplied by provider with None
        for col in ("broker_id", "analyst_id", "rating_standardized",
                    "prior_rating", "action", "as_of_date"):
            if col not in df.columns:
                df[col] = None

        store.con.register("_est_recommendation_batch", df)
        try:
            store.con.execute(
                """
                INSERT INTO est_recommendation (
                    security_id, broker_id, analyst_id, rating, rating_standardized,
                    prior_rating, action, rating_date, as_of_date, available_at, run_id, source
                )
                SELECT
                    security_id, broker_id, analyst_id, rating, rating_standardized,
                    prior_rating, action, rating_date, as_of_date, available_at, run_id, source
                FROM _est_recommendation_batch
                """
            )
        finally:
            store.con.unregister("_est_recommendation_batch")

        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=len(df),
            source=self.source_name,
            details={},
        )
