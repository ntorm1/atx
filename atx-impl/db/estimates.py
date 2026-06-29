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
est_consensus: available_at carried from provider/source row if supplied; injectable
               files fall back to snapshot_date/statpers end-of-day; callables fall
               back to now_utc_naive() for backward compatibility.
est_guidance : available_at carried from provider row if supplied; else now_utc_naive().
est_recommendation: available_at carried from provider/source row if supplied; injectable
                    files fall back to activation/anndats/rating_date timestamps;
                    callables fall back to now_utc_naive() for compatibility.

Revenue concept preference
--------------------------
For measure_code=REVENUE we prefer concept "Revenues".  If a security/fy/fp/accession has
no "Revenues" value we fall back to "RevenueFromContractWithCustomerExcludingAssessedTax".
Implemented via a SQL window function that ranks concepts per PK group.
"""
from __future__ import annotations

import datetime as dt
import hashlib
import json
import math
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Iterable

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import (
    file_sha256,
    insert_frame,
    json_dumps,
    now_utc_naive,
    quality_check,
    record_source_file,
    security_id_for_symbol,
    snake_case,
    symbol_key,
)


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

ESTIMATE_DETAIL_COLUMNS = [
    "est_detail_id",
    "security_id",
    "symbol",
    "vendor_security_id",
    "vendor_security_id_type",
    "measure_code",
    "fiscal_year",
    "fiscal_period",
    "period_end",
    "broker_id",
    "analyst_id",
    "value",
    "estimate_date",
    "as_of_date",
    "available_at",
    "provider",
    "source_vendor_table",
    "vendor_broker_id",
    "vendor_analyst_id",
    "broker_mask_code",
    "analyst_mask_code",
    "broker_name",
    "analyst_name",
    "fpi",
    "period_type",
    "expected_report_date",
    "announce_date",
    "announce_time",
    "activation_date",
    "activation_time",
    "revision_date",
    "revision_time",
    "stop_date",
    "pdf",
    "basis",
    "is_gaap",
    "estimate_type",
    "currency",
    "unit",
    "source_file",
    "source_file_sha256",
    "raw_payload_json",
    "run_id",
    "source",
]

ESTIMATE_BROKER_COLUMNS = [
    "broker_id",
    "broker_name",
    "source",
    "provider",
    "vendor_broker_id",
    "broker_mask_code",
    "valid_from",
    "valid_to",
    "available_at",
    "run_id",
    "source_file_sha256",
]

ESTIMATE_ANALYST_COLUMNS = [
    "analyst_id",
    "analyst_name",
    "broker_id",
    "source",
    "provider",
    "vendor_analyst_id",
    "analyst_mask_code",
    "valid_from",
    "valid_to",
    "available_at",
    "run_id",
    "source_file_sha256",
]

ESTIMATE_PERIOD_COLUMNS = [
    "est_period_id",
    "provider",
    "measure_code",
    "fiscal_year",
    "fiscal_period",
    "period_end",
    "fpi",
    "period_type",
    "expected_report_date",
    "valid_from",
    "valid_to",
    "as_of_date",
    "available_at",
    "source",
    "run_id",
]

ESTIMATE_BROKER_ALIAS_COLUMNS = [
    "broker_alias_id",
    "broker_id",
    "provider",
    "alias_type",
    "alias_value",
    "valid_from",
    "valid_to",
    "available_at",
    "source",
    "run_id",
]

ESTIMATE_ANALYST_ALIAS_COLUMNS = [
    "analyst_alias_id",
    "analyst_id",
    "provider",
    "alias_type",
    "alias_value",
    "valid_from",
    "valid_to",
    "available_at",
    "source",
    "run_id",
]

ESTIMATE_CONSENSUS_COLUMNS = [
    "est_consensus_id",
    "security_id",
    "symbol",
    "vendor_security_id",
    "vendor_security_id_type",
    "provider",
    "source_vendor_table",
    "measure_code",
    "fiscal_year",
    "fiscal_period",
    "period_end",
    "fpi",
    "period_type",
    "expected_report_date",
    "consensus_date",
    "mean",
    "median",
    "high",
    "low",
    "stdev",
    "num_estimates",
    "num_up",
    "num_down",
    "currency",
    "pdf",
    "basis",
    "is_gaap",
    "unit",
    "stale_after_date",
    "as_of_date",
    "available_at",
    "source_file",
    "source_file_sha256",
    "raw_payload_json",
    "run_id",
    "source",
]

ESTIMATE_RECOMMENDATION_COLUMNS = [
    "est_recommendation_id",
    "security_id",
    "symbol",
    "vendor_security_id",
    "vendor_security_id_type",
    "cusip",
    "provider",
    "source_vendor_table",
    "broker_id",
    "analyst_id",
    "vendor_broker_id",
    "vendor_analyst_id",
    "broker_mask_code",
    "analyst_mask_code",
    "broker_name",
    "analyst_name",
    "rating",
    "rating_standardized",
    "recommendation_code",
    "recommendation_label",
    "prior_rating",
    "prior_recommendation_code",
    "prior_recommendation_label",
    "rating_scale",
    "action",
    "event_type",
    "price_target",
    "target_currency",
    "target_horizon_months",
    "industry_code",
    "is_industry_recommendation",
    "usfirm",
    "rating_date",
    "announce_date",
    "announce_time",
    "activation_date",
    "activation_time",
    "revision_date",
    "revision_time",
    "stop_date",
    "as_of_date",
    "available_at",
    "source_file",
    "source_file_sha256",
    "raw_payload_json",
    "run_id",
    "source",
]

DETAIL_COLUMN_ALIASES = {
    "actdats": "activation_date",
    "acttims": "activation_time",
    "actual_activation_date": "activation_date",
    "amaskcd": "analyst_mask_code",
    "analys": "vendor_analyst_id",
    "analyst": "analyst_name",
    "analyst_code": "vendor_analyst_id",
    "analystid": "analyst_id",
    "anntims": "announce_time",
    "anndats": "announce_date",
    "broker": "broker_name",
    "broker_code": "vendor_broker_id",
    "brokerid": "broker_id",
    "curr": "currency",
    "emaskcd": "broker_mask_code",
    "estimate": "value",
    "estimator": "vendor_broker_id",
    "est_value": "value",
    "fpedats": "period_end",
    "fiscal_period_end": "period_end",
    "ibes_ticker": "vendor_security_id",
    "measure": "measure_code",
    "oftic": "symbol",
    "official_ticker": "symbol",
    "pends": "period_end",
    "period_end_date": "period_end",
    "revdats": "revision_date",
    "revtims": "revision_time",
    "source_table": "source_vendor_table",
    "stop": "stop_date",
    "stopdate": "stop_date",
    "ticker": "vendor_security_id",
    "tic": "symbol",
    "value": "value",
}

CONSENSUS_COLUMN_ALIASES = {
    "asof": "as_of_date",
    "availabledate": "available_date",
    "available_date": "available_date",
    "availability_date": "available_date",
    "availabilitydate": "available_date",
    "availability_time": "available_time",
    "availabilitytime": "available_time",
    "currency_code": "currency",
    "curr": "currency",
    "estcur": "currency",
    "fiscal_period_end": "period_end",
    "fpedats": "period_end",
    "highest": "high",
    "high_est": "high",
    "ibes_ticker": "vendor_security_id",
    "low_est": "low",
    "lowest": "low",
    "mean_est": "mean",
    "meanest": "mean",
    "measure": "measure_code",
    "medest": "median",
    "median_est": "median",
    "num_down": "num_down",
    "num_down_30d": "num_down",
    "num_down_revisions_30d": "num_down",
    "num_est": "num_estimates",
    "num_up": "num_up",
    "num_up_30d": "num_up",
    "num_up_revisions_30d": "num_up",
    "numdown": "num_down",
    "numest": "num_estimates",
    "numup": "num_up",
    "oftic": "symbol",
    "official_ticker": "symbol",
    "pends": "period_end",
    "period_end_date": "period_end",
    "snapshot_date": "consensus_date",
    "source_table": "source_vendor_table",
    "statpers": "consensus_date",
    "stdev_est": "stdev",
    "stddev": "stdev",
    "ticker": "vendor_security_id",
    "tic": "symbol",
}

RECOMMENDATION_COLUMN_ALIASES = {
    "actdats": "activation_date",
    "acttims": "activation_time",
    "actual_activation_date": "activation_date",
    "amaskcd": "analyst_mask_code",
    "analys": "vendor_analyst_id",
    "analyst": "analyst_name",
    "analyst_code": "vendor_analyst_id",
    "analystid": "analyst_id",
    "anntims": "announce_time",
    "anndats": "announce_date",
    "asof": "as_of_date",
    "broker": "broker_name",
    "broker_code": "vendor_broker_id",
    "brokerid": "broker_id",
    "curr": "target_currency",
    "emaskcd": "broker_mask_code",
    "estcur": "target_currency",
    "estimator": "vendor_broker_id",
    "horizon": "target_horizon_months",
    "ibes_ticker": "vendor_security_id",
    "ind_idx": "industry_code",
    "industry": "industry_code",
    "ireccd": "recommendation_code",
    "itext": "rating",
    "oftic": "symbol",
    "official_ticker": "symbol",
    "prior_ireccd": "prior_recommendation_code",
    "prior_rating_code": "prior_recommendation_code",
    "ptg": "price_target",
    "rating_code": "recommendation_code",
    "recd": "recommendation_code",
    "recommendation": "recommendation_code",
    "recommendation_text": "rating",
    "rec_code": "recommendation_code",
    "revdats": "revision_date",
    "revtims": "revision_time",
    "source_table": "source_vendor_table",
    "stop": "stop_date",
    "stopdate": "stop_date",
    "target": "price_target",
    "target_currency": "target_currency",
    "target_horizon_mo": "target_horizon_months",
    "target_price": "price_target",
    "ticker": "vendor_security_id",
    "tic": "symbol",
    "value": "price_target",
}

RECOMMENDATION_LABELS = {
    1: "Strong Buy",
    2: "Buy",
    3: "Hold",
    4: "Underperform",
    5: "Sell",
}

RECOMMENDATION_TEXT_MAP = {
    "ACCUMULATE": 2,
    "ADD": 2,
    "BUY": 2,
    "EQUAL-WEIGHT": 3,
    "EQUAL WEIGHT": 3,
    "HOLD": 3,
    "MARKET PERFORM": 3,
    "MARKET-PERFORM": 3,
    "NEUTRAL": 3,
    "OUTPERFORM": 2,
    "OUTPERFORMER": 2,
    "OVERWEIGHT": 2,
    "REDUCE": 4,
    "SECTOR PERFORM": 3,
    "SECTOR-PERFORM": 3,
    "SELL": 5,
    "STRONG BUY": 1,
    "STRONG-BUY": 1,
    "STRONG SELL": 5,
    "STRONG-SELL": 5,
    "UNDERPERFORM": 4,
    "UNDERPERFORMER": 4,
    "UNDERWEIGHT": 4,
}

IBES_MEASURE_MAP = {
    "SAL": "REVENUE",
    "SALES": "REVENUE",
    "REV": "REVENUE",
    "NET": "NET_INCOME",
    "INC": "NET_INCOME",
    "NI": "NET_INCOME",
    "OPR": "OPERATING_INCOME",
}


@dataclass(frozen=True)
class EstimateDetailOptions:
    source_file: Path | None = None
    source: str = "est_detail_injected_v1"
    provider: str = "INJECTED"
    vendor_security_id_type: str = "IBES_TICKER"
    replace_source_file: bool = True
    run_id: str | None = None


def _empty_estimate_detail_frame() -> pd.DataFrame:
    return pd.DataFrame(columns=ESTIMATE_DETAIL_COLUMNS)


def _normalize_detail_columns(frame: pd.DataFrame) -> pd.DataFrame:
    renamed: dict[str, str] = {}
    for column in frame.columns:
        normalized = snake_case(str(column)).lower()
        compact = normalized.replace("_", "")
        renamed[column] = DETAIL_COLUMN_ALIASES.get(
            normalized,
            DETAIL_COLUMN_ALIASES.get(compact, normalized),
        )
    return frame.rename(columns=renamed)


def _string_series(frame: pd.DataFrame, column: str) -> pd.Series:
    if column not in frame.columns:
        return pd.Series([pd.NA] * len(frame), index=frame.index, dtype="string")
    return frame[column].replace("", pd.NA).astype("string")


def _numeric_series(frame: pd.DataFrame, column: str) -> pd.Series:
    if column not in frame.columns:
        return pd.Series([pd.NA] * len(frame), index=frame.index, dtype="Float64")
    return pd.to_numeric(frame[column].replace("", pd.NA), errors="coerce")


def _integer_series(frame: pd.DataFrame, column: str) -> pd.Series:
    numeric = _numeric_series(frame, column)
    return numeric.round().astype("Int64")


def _date_series(
    frame: pd.DataFrame,
    column: str,
    fallback: pd.Series | dt.date | None = None,
) -> pd.Series:
    if column in frame.columns:
        parsed = pd.to_datetime(frame[column].replace("", pd.NA), errors="coerce").dt.date
    else:
        parsed = pd.Series([pd.NA] * len(frame), index=frame.index, dtype="object")
    if fallback is None:
        return parsed
    fallback_series = (
        fallback
        if isinstance(fallback, pd.Series)
        else pd.Series([fallback] * len(frame), index=frame.index, dtype="object")
    )
    return parsed.where(pd.notna(parsed), fallback_series)


def _time_series(frame: pd.DataFrame, column: str) -> pd.Series:
    if column not in frame.columns:
        return pd.Series([pd.NA] * len(frame), index=frame.index, dtype="object")
    values: list[dt.time | Any] = []
    for value in frame[column].replace("", pd.NA):
        if pd.isna(value):
            values.append(pd.NA)
            continue
        text = str(value).strip()
        try:
            values.append(dt.time.fromisoformat(text))
            continue
        except ValueError:
            pass
        try:
            values.append(dt.datetime.fromisoformat(text).time())
        except ValueError:
            values.append(pd.NA)
    return pd.Series(values, index=frame.index, dtype="object")


def _timestamp_series(frame: pd.DataFrame, column: str) -> pd.Series:
    if column not in frame.columns:
        return pd.Series([pd.NaT] * len(frame), index=frame.index, dtype="datetime64[ns]")
    return pd.to_datetime(frame[column].replace("", pd.NA), errors="coerce")


def _timestamp_from_date_time(
    dates: pd.Series,
    times: pd.Series,
    fallback: dt.datetime,
) -> pd.Series:
    values: list[dt.datetime] = []
    for date_value, time_value in zip(dates, times):
        if pd.isna(date_value):
            values.append(fallback)
            continue
        parsed_time = dt.time()
        if not pd.isna(time_value):
            if isinstance(time_value, dt.time):
                parsed_time = time_value
            else:
                try:
                    parsed_time = dt.time.fromisoformat(str(time_value))
                except ValueError:
                    parsed_time = dt.time()
        values.append(dt.datetime.combine(date_value, parsed_time))
    return pd.Series(values, index=dates.index, dtype="datetime64[ns]")


def _timestamp_from_dates_end_of_day(
    dates: pd.Series,
    fallback: dt.datetime,
) -> pd.Series:
    values: list[dt.datetime] = []
    for date_value in dates:
        if pd.isna(date_value):
            values.append(fallback)
        else:
            values.append(dt.datetime.combine(date_value, dt.time(23, 59, 59)))
    return pd.Series(values, index=dates.index, dtype="datetime64[ns]")


def _bool_series(frame: pd.DataFrame, column: str) -> pd.Series:
    if column not in frame.columns:
        return pd.Series([pd.NA] * len(frame), index=frame.index, dtype="boolean")

    def _parse(value: Any) -> bool | pd._libs.missing.NAType:
        if pd.isna(value) or str(value).strip() == "":
            return pd.NA
        normalized = str(value).strip().lower()
        if normalized in {"1", "true", "t", "yes", "y", "gaap", "g"}:
            return True
        if normalized in {"0", "false", "f", "no", "n", "non-gaap", "nongaap", "adjusted"}:
            return False
        return pd.NA

    return frame[column].map(_parse).astype("boolean")


def _series_clean(value: Any) -> str:
    if pd.isna(value):
        return ""
    return str(value).strip()


def _hash_id(prefix: str, *parts: Any) -> str:
    payload = "|".join(_series_clean(part) for part in parts)
    digest = hashlib.sha256(payload.encode("utf-8")).hexdigest()[:16]
    return f"{prefix}-{digest}"


def _safe_provider(value: str) -> str:
    cleaned = re.sub(r"[^0-9A-Z]+", "-", str(value).strip().upper()).strip("-")
    return cleaned or "INJECTED"


def _period_type_from_fpi(value: Any) -> str | None:
    if pd.isna(value) or str(value).strip() == "":
        return None
    code = str(value).strip().upper()
    if code == "0":
        return "LTG"
    if code in {"1", "2", "3", "4", "5"}:
        return "FY"
    if code in {"6", "7", "8", "9"}:
        return "FQ"
    if code in {"A", "B"}:
        return "SEMI"
    if code == "Y":
        return "YTD"
    return None


def _quarter_label(value: Any) -> str | None:
    if pd.isna(value):
        return None
    month = int(value.month)
    if month <= 3:
        return "Q1"
    if month <= 6:
        return "Q2"
    if month <= 9:
        return "Q3"
    return "Q4"


def _canonical_measure(measure: Any, pdf: Any = None) -> str | None:
    if pd.isna(measure) or str(measure).strip() == "":
        return None
    code = str(measure).strip().upper()
    if code == "EPS":
        pdf_code = "" if pd.isna(pdf) else str(pdf).strip().upper()
        return "EPS_BASIC" if pdf_code == "P" else "EPS_DILUTED"
    return IBES_MEASURE_MAP.get(code, code)


def _derive_party_id(
    prefix: str,
    canonical: Any,
    provider: str,
    vendor_id: Any,
    mask_code: Any,
    name: Any,
    valid_from: Any,
) -> str | None:
    if not pd.isna(canonical) and str(canonical).strip():
        return str(canonical).strip()
    basis = vendor_id if not pd.isna(vendor_id) and str(vendor_id).strip() else mask_code
    if pd.isna(basis) or not str(basis).strip():
        basis = name
    if pd.isna(basis) or not str(basis).strip():
        return None
    return _hash_id(prefix, provider, basis, valid_from)


def _raw_payloads(frame: pd.DataFrame) -> pd.Series:
    return frame.apply(lambda row: json_dumps(row.dropna().to_dict()), axis=1)


def normalize_estimate_detail_rows(
    frame: pd.DataFrame,
    *,
    options: EstimateDetailOptions,
    source_file_sha256: str | None = None,
    source_file: Path | None = None,
) -> pd.DataFrame:
    if frame.empty:
        return _empty_estimate_detail_frame()

    raw = _normalize_detail_columns(frame.copy())
    if "measure_code" not in raw.columns:
        raise ValueError("Estimate detail rows require measure/measure_code")
    if "value" not in raw.columns:
        raise ValueError("Estimate detail rows require value/estimate")
    if "period_end" not in raw.columns:
        raise ValueError("Estimate detail rows require period_end/FPEDATS")

    now = now_utc_naive()
    provider = _safe_provider(options.provider)
    period_end = _date_series(raw, "period_end")
    announce_date = _date_series(raw, "announce_date")
    activation_date = _date_series(raw, "activation_date")
    revision_date = _date_series(raw, "revision_date")
    stop_date = _date_series(raw, "stop_date")
    estimate_date = _date_series(raw, "estimate_date", announce_date)
    estimate_date = estimate_date.where(pd.notna(estimate_date), activation_date)
    as_of_date = _date_series(raw, "as_of_date", estimate_date)
    as_of_date = as_of_date.where(pd.notna(as_of_date), period_end)
    available_at = _timestamp_series(raw, "available_at")
    activation_ts = _timestamp_from_date_time(
        activation_date,
        _time_series(raw, "activation_time"),
        now,
    )
    available_at = available_at.fillna(activation_ts).fillna(pd.Timestamp(now))

    normalized = pd.DataFrame(index=raw.index)
    normalized["security_id"] = _string_series(raw, "security_id")
    normalized["symbol"] = _string_series(raw, "symbol").map(
        lambda value: symbol_key(None if pd.isna(value) else str(value))
    )
    normalized["vendor_security_id"] = _string_series(raw, "vendor_security_id")
    non_ibes_symbol = (
        normalized["symbol"].isna()
        & normalized["vendor_security_id"].notna()
        & (provider not in {"IBES", "LSEG-IBES", "I-B-E-S"})
    )
    normalized.loc[non_ibes_symbol, "symbol"] = normalized.loc[non_ibes_symbol, "vendor_security_id"].map(symbol_key)
    missing_security = normalized["security_id"].isna() & normalized["symbol"].notna()
    normalized.loc[missing_security, "security_id"] = normalized.loc[missing_security, "symbol"].map(security_id_for_symbol)
    normalized["vendor_security_id_type"] = _string_series(raw, "vendor_security_id_type").fillna(
        options.vendor_security_id_type
    ).str.upper()
    pdf = _string_series(raw, "pdf").str.upper()
    normalized["measure_code"] = [
        _canonical_measure(measure, pdf_value)
        for measure, pdf_value in zip(_string_series(raw, "measure_code"), pdf)
    ]
    normalized["fiscal_year"] = pd.to_numeric(
        _string_series(raw, "fiscal_year").replace(pd.NA, None),
        errors="coerce",
    ).astype("Int64")
    missing_fy = normalized["fiscal_year"].isna() & pd.notna(period_end)
    normalized.loc[missing_fy, "fiscal_year"] = [
        int(value.year) if pd.notna(value) else pd.NA
        for value in period_end[missing_fy]
    ]
    normalized["fiscal_period"] = _string_series(raw, "fiscal_period").str.upper()
    fpi = _string_series(raw, "fpi").str.upper()
    period_type = fpi.map(_period_type_from_fpi).astype("string")
    missing_fp = normalized["fiscal_period"].isna() & pd.notna(period_end)
    derived_fp = []
    for pt, pe in zip(period_type[missing_fp], period_end[missing_fp]):
        if pt == "FY":
            derived_fp.append("FY")
        elif pt == "FQ":
            derived_fp.append(_quarter_label(pe))
        else:
            derived_fp.append(_quarter_label(pe))
    if derived_fp:
        normalized.loc[missing_fp, "fiscal_period"] = derived_fp
    normalized["period_end"] = period_end
    normalized["value"] = _numeric_series(raw, "value")
    normalized["estimate_date"] = estimate_date
    normalized["as_of_date"] = as_of_date
    normalized["available_at"] = available_at
    normalized["provider"] = provider
    normalized["source_vendor_table"] = _string_series(raw, "source_vendor_table")
    normalized["vendor_broker_id"] = _string_series(raw, "vendor_broker_id")
    normalized["vendor_analyst_id"] = _string_series(raw, "vendor_analyst_id")
    normalized["broker_mask_code"] = _string_series(raw, "broker_mask_code")
    normalized["analyst_mask_code"] = _string_series(raw, "analyst_mask_code")
    normalized["broker_name"] = _string_series(raw, "broker_name")
    normalized["analyst_name"] = _string_series(raw, "analyst_name")
    normalized["broker_id"] = [
        _derive_party_id(
            "EST-BROKER",
            canonical,
            provider,
            vendor_id,
            mask_code,
            name,
            valid_from,
        )
        for canonical, vendor_id, mask_code, name, valid_from in zip(
            _string_series(raw, "broker_id"),
            normalized["vendor_broker_id"],
            normalized["broker_mask_code"],
            normalized["broker_name"],
            estimate_date,
        )
    ]
    normalized["analyst_id"] = [
        _derive_party_id(
            "EST-ANALYST",
            canonical,
            provider,
            vendor_id,
            mask_code,
            name,
            valid_from,
        )
        for canonical, vendor_id, mask_code, name, valid_from in zip(
            _string_series(raw, "analyst_id"),
            normalized["vendor_analyst_id"],
            normalized["analyst_mask_code"],
            normalized["analyst_name"],
            estimate_date,
        )
    ]
    normalized["fpi"] = fpi
    normalized["period_type"] = period_type
    normalized["expected_report_date"] = _date_series(raw, "expected_report_date")
    normalized["announce_date"] = announce_date
    normalized["announce_time"] = _time_series(raw, "announce_time")
    normalized["activation_date"] = activation_date
    normalized["activation_time"] = _time_series(raw, "activation_time")
    normalized["revision_date"] = revision_date
    normalized["revision_time"] = _time_series(raw, "revision_time")
    normalized["stop_date"] = stop_date
    normalized["pdf"] = pdf
    normalized["basis"] = _string_series(raw, "basis")
    normalized["is_gaap"] = _bool_series(raw, "is_gaap")
    normalized["estimate_type"] = _string_series(raw, "estimate_type").str.upper()
    normalized["currency"] = _string_series(raw, "currency").str.upper()
    normalized["unit"] = _string_series(raw, "unit")
    normalized["source_file"] = str(source_file) if source_file else pd.NA
    normalized["source_file_sha256"] = source_file_sha256
    normalized["raw_payload_json"] = _raw_payloads(raw)
    normalized["run_id"] = options.run_id
    normalized["source"] = options.source

    normalized = normalized[
        normalized["period_end"].notna()
        & normalized["as_of_date"].notna()
        & normalized["available_at"].notna()
        & normalized["measure_code"].notna()
        & normalized["value"].notna()
        & (
            normalized["security_id"].notna()
            | normalized["symbol"].notna()
            | normalized["vendor_security_id"].notna()
        )
    ].copy()
    if normalized.empty:
        return _empty_estimate_detail_frame()
    normalized["est_detail_id"] = normalized.apply(
        lambda row: _hash_id(
            "EST-DETAIL",
            row.get("source"),
            row.get("provider"),
            row.get("security_id"),
            row.get("symbol"),
            row.get("vendor_security_id_type"),
            row.get("vendor_security_id"),
            row.get("measure_code"),
            row.get("period_end"),
            row.get("broker_id"),
            row.get("analyst_id"),
            row.get("announce_date"),
            row.get("activation_date"),
            row.get("revision_date"),
            row.get("value"),
            row.get("source_file_sha256"),
        ),
        axis=1,
    )
    normalized = normalized.drop_duplicates(subset=["est_detail_id"])
    return normalized[ESTIMATE_DETAIL_COLUMNS]


def _insert_or_replace_frame(
    store: DuckDBStore,
    frame: pd.DataFrame,
    table: str,
    relation_name: str,
) -> int:
    if frame.empty:
        return 0
    store.con.register(relation_name, frame)
    try:
        columns = ", ".join(frame.columns)
        store.con.execute(f"INSERT OR REPLACE INTO {table} ({columns}) SELECT {columns} FROM {relation_name}")
    finally:
        store.con.unregister(relation_name)
    return int(len(frame))


def _estimate_detail_dimensions(detail: pd.DataFrame) -> tuple[pd.DataFrame, pd.DataFrame, pd.DataFrame, pd.DataFrame, pd.DataFrame]:
    valid_from = detail["estimate_date"].where(detail["estimate_date"].notna(), detail["as_of_date"])
    valid_to = detail["stop_date"].where(detail["stop_date"].notna(), detail["revision_date"])
    brokers = pd.DataFrame(
        {
            "broker_id": detail["broker_id"],
            "broker_name": detail["broker_name"],
            "source": detail["source"],
            "provider": detail["provider"],
            "vendor_broker_id": detail["vendor_broker_id"],
            "broker_mask_code": detail["broker_mask_code"],
            "valid_from": valid_from,
            "valid_to": valid_to,
            "available_at": detail["available_at"],
            "run_id": detail["run_id"],
            "source_file_sha256": detail["source_file_sha256"],
        }
    ).dropna(subset=["broker_id"]).drop_duplicates(subset=["broker_id"])
    analysts = pd.DataFrame(
        {
            "analyst_id": detail["analyst_id"],
            "analyst_name": detail["analyst_name"],
            "broker_id": detail["broker_id"],
            "source": detail["source"],
            "provider": detail["provider"],
            "vendor_analyst_id": detail["vendor_analyst_id"],
            "analyst_mask_code": detail["analyst_mask_code"],
            "valid_from": valid_from,
            "valid_to": valid_to,
            "available_at": detail["available_at"],
            "run_id": detail["run_id"],
            "source_file_sha256": detail["source_file_sha256"],
        }
    ).dropna(subset=["analyst_id"]).drop_duplicates(subset=["analyst_id"])
    periods = pd.DataFrame(
        {
            "provider": detail["provider"],
            "measure_code": detail["measure_code"],
            "fiscal_year": detail["fiscal_year"],
            "fiscal_period": detail["fiscal_period"],
            "period_end": detail["period_end"],
            "fpi": detail["fpi"],
            "period_type": detail["period_type"],
            "expected_report_date": detail["expected_report_date"],
            "valid_from": valid_from,
            "valid_to": pd.Series([pd.NA] * len(detail), index=detail.index, dtype="object"),
            "as_of_date": detail["as_of_date"],
            "available_at": detail["available_at"],
            "source": detail["source"],
            "run_id": detail["run_id"],
        }
    ).drop_duplicates()
    periods["est_period_id"] = periods.apply(
        lambda row: _hash_id(
            "EST-PERIOD",
            row.get("provider"),
            row.get("measure_code"),
            row.get("period_end"),
            row.get("fpi"),
            row.get("fiscal_year"),
            row.get("fiscal_period"),
        ),
        axis=1,
    )
    periods = periods[ESTIMATE_PERIOD_COLUMNS].drop_duplicates(subset=["est_period_id"])

    broker_alias_rows = []
    for _, row in detail.dropna(subset=["broker_id"]).iterrows():
        for alias_type, alias_value in (
            ("VENDOR_BROKER_ID", row.get("vendor_broker_id")),
            ("BROKER_MASK_CODE", row.get("broker_mask_code")),
            ("BROKER_NAME", row.get("broker_name")),
        ):
            if pd.isna(alias_value) or not str(alias_value).strip():
                continue
            broker_alias_rows.append(
                {
                    "broker_alias_id": _hash_id(
                        "EST-BROKER-ALIAS",
                        row.get("provider"),
                        row.get("broker_id"),
                        alias_type,
                        alias_value,
                        row.get("estimate_date"),
                    ),
                    "broker_id": row.get("broker_id"),
                    "provider": row.get("provider"),
                    "alias_type": alias_type,
                    "alias_value": str(alias_value).strip(),
                    "valid_from": row.get("estimate_date"),
                    "valid_to": row.get("stop_date") if pd.notna(row.get("stop_date")) else row.get("revision_date"),
                    "available_at": row.get("available_at"),
                    "source": row.get("source"),
                    "run_id": row.get("run_id"),
                }
            )
    analyst_alias_rows = []
    for _, row in detail.dropna(subset=["analyst_id"]).iterrows():
        for alias_type, alias_value in (
            ("VENDOR_ANALYST_ID", row.get("vendor_analyst_id")),
            ("ANALYST_MASK_CODE", row.get("analyst_mask_code")),
            ("ANALYST_NAME", row.get("analyst_name")),
        ):
            if pd.isna(alias_value) or not str(alias_value).strip():
                continue
            analyst_alias_rows.append(
                {
                    "analyst_alias_id": _hash_id(
                        "EST-ANALYST-ALIAS",
                        row.get("provider"),
                        row.get("analyst_id"),
                        alias_type,
                        alias_value,
                        row.get("estimate_date"),
                    ),
                    "analyst_id": row.get("analyst_id"),
                    "provider": row.get("provider"),
                    "alias_type": alias_type,
                    "alias_value": str(alias_value).strip(),
                    "valid_from": row.get("estimate_date"),
                    "valid_to": row.get("stop_date") if pd.notna(row.get("stop_date")) else row.get("revision_date"),
                    "available_at": row.get("available_at"),
                    "source": row.get("source"),
                    "run_id": row.get("run_id"),
                }
            )
    broker_aliases = pd.DataFrame(broker_alias_rows, columns=ESTIMATE_BROKER_ALIAS_COLUMNS)
    analyst_aliases = pd.DataFrame(analyst_alias_rows, columns=ESTIMATE_ANALYST_ALIAS_COLUMNS)
    return brokers, analysts, periods, broker_aliases, analyst_aliases


def load_estimate_detail_rows(
    store: DuckDBStore,
    options: EstimateDetailOptions,
) -> int:
    store.initialize()
    if options.source_file is None:
        return 0
    source_file = Path(options.source_file)
    frame = pd.read_csv(source_file, dtype=str, keep_default_na=False)
    source_hash = file_sha256(source_file)
    detail = normalize_estimate_detail_rows(
        frame,
        options=options,
        source_file_sha256=source_hash,
        source_file=source_file,
    )
    record_source_file(
        store,
        dataset_id="est_detail",
        source_url=str(source_file),
        cache_path=source_file,
        sha256=source_hash,
        metadata={"provider": options.provider, "rows": int(len(frame))},
    )
    if detail.empty:
        return 0
    brokers, analysts, periods, broker_aliases, analyst_aliases = _estimate_detail_dimensions(detail)
    with store.transaction():
        if options.replace_source_file:
            store.con.execute(
                """
                DELETE FROM est_detail
                WHERE source = ?
                  AND provider = ?
                  AND source_file_sha256 = ?
                """,
                [options.source, _safe_provider(options.provider), source_hash],
            )
        insert_frame(store, detail, "est_detail", "est_detail_insert")
        _insert_or_replace_frame(store, brokers, "est_broker", "est_broker_insert")
        _insert_or_replace_frame(store, analysts, "est_analyst", "est_analyst_insert")
        _insert_or_replace_frame(store, periods, "est_period_dim", "est_period_insert")
        _insert_or_replace_frame(store, broker_aliases, "est_broker_alias", "est_broker_alias_insert")
        _insert_or_replace_frame(store, analyst_aliases, "est_analyst_alias", "est_analyst_alias_insert")
    return int(len(detail))


class EstimateDetailDataset(Dataset):
    dataset_id = "est_detail"
    source_name = "est_detail_injectable"

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: EstimateDetailOptions) -> DatasetLoadResult:
        rows_loaded = load_estimate_detail_rows(store, options)
        if rows_loaded:
            quality_check(
                store,
                dataset_id=self.dataset_id,
                table_name="est_detail",
                check_name="est_detail_loaded",
                status="passed",
                observed_value=float(rows_loaded),
                threshold_value=0.0,
                details={"provider": options.provider, "source_file": str(options.source_file)},
            )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows_loaded,
            source=self.source_name,
            details={
                "provider": options.provider,
                "source_file": None if options.source_file is None else str(options.source_file),
            },
        )


@dataclass(frozen=True)
class EstimateConsensusOptions:
    provider: Callable[[], Iterable[dict]] | None = None
    source_file: Path | None = None
    source: str = "est_consensus_injected_v1"
    provider_name: str = "INJECTED"
    vendor_security_id_type: str = "IBES_TICKER"
    replace_source_file: bool = True
    stale_after_days: int = 105
    run_id: str | None = None


def _empty_estimate_consensus_frame() -> pd.DataFrame:
    return pd.DataFrame(columns=ESTIMATE_CONSENSUS_COLUMNS)


def _normalize_consensus_columns(frame: pd.DataFrame) -> pd.DataFrame:
    renamed: dict[str, str] = {}
    for column in frame.columns:
        normalized = snake_case(str(column)).lower()
        compact = normalized.replace("_", "")
        renamed[column] = CONSENSUS_COLUMN_ALIASES.get(
            normalized,
            CONSENSUS_COLUMN_ALIASES.get(compact, normalized),
        )
    return frame.rename(columns=renamed)


def _provider_name_series(raw: pd.DataFrame, default_provider: str) -> pd.Series:
    base = _string_series(raw, "provider")
    fallback = _safe_provider(default_provider)
    values = [
        fallback if pd.isna(value) or not str(value).strip() else _safe_provider(str(value))
        for value in base
    ]
    return pd.Series(values, index=raw.index, dtype="string")


def _source_series(raw: pd.DataFrame, default_source: str) -> pd.Series:
    base = _string_series(raw, "source")
    return base.where(base.notna(), default_source)


def _derive_consensus_fiscal_period(existing: Any, period_type: Any, period_end: Any) -> str | None:
    if not pd.isna(existing) and str(existing).strip():
        return str(existing).strip().upper()
    if not pd.isna(period_type):
        period_type_text = str(period_type).strip().upper()
        if period_type_text == "FY":
            return "FY"
        if period_type_text == "FQ":
            return _quarter_label(period_end)
    return None


def _stale_after_series(consensus_date: pd.Series, stale_after_days: int) -> pd.Series:
    delta = dt.timedelta(days=max(int(stale_after_days), 0))
    return consensus_date.map(lambda value: pd.NA if pd.isna(value) else value + delta)


def normalize_estimate_consensus_rows(
    frame: pd.DataFrame,
    *,
    options: EstimateConsensusOptions,
    source_file_sha256: str | None = None,
    source_file: Path | None = None,
) -> pd.DataFrame:
    if frame.empty:
        return _empty_estimate_consensus_frame()

    raw = _normalize_consensus_columns(frame.copy())
    if "measure_code" not in raw.columns:
        raise ValueError("Estimate consensus rows require measure/measure_code")
    if "period_end" not in raw.columns:
        raise ValueError("Estimate consensus rows require period_end/FPEDATS")

    now = now_utc_naive()
    provider = _provider_name_series(raw, options.provider_name)
    source = _source_series(raw, options.source)
    pdf = _string_series(raw, "pdf").str.upper()
    measure_code = pd.Series(
        [_canonical_measure(measure, pdf_value) for measure, pdf_value in zip(_string_series(raw, "measure_code"), pdf)],
        index=raw.index,
        dtype="string",
    )
    symbol = _string_series(raw, "symbol").map(
        lambda value: symbol_key(value) if not pd.isna(value) and str(value).strip() else pd.NA
    ).astype("string")
    vendor_security_id = _string_series(raw, "vendor_security_id").str.upper()
    vendor_security_id_type = _string_series(raw, "vendor_security_id_type").where(
        _string_series(raw, "vendor_security_id_type").notna(),
        options.vendor_security_id_type,
    ).str.upper()
    security_raw = _string_series(raw, "security_id")
    security_id = pd.Series(
        [
            str(existing).strip()
            if not pd.isna(existing) and str(existing).strip()
            else (security_id_for_symbol(sym) if not pd.isna(sym) and str(sym).strip() else pd.NA)
            for existing, sym in zip(security_raw, symbol)
        ],
        index=raw.index,
        dtype="string",
    )

    period_end = _date_series(raw, "period_end")
    consensus_date = _date_series(raw, "consensus_date", fallback=_date_series(raw, "as_of_date"))
    consensus_date = consensus_date.where(pd.notna(consensus_date), period_end)
    as_of_date = _date_series(raw, "as_of_date", fallback=consensus_date)
    as_of_date = as_of_date.where(pd.notna(as_of_date), period_end)
    available_at = _timestamp_series(raw, "available_at")
    available_date = _date_series(raw, "available_date")
    available_time = _time_series(raw, "available_time")
    available_from_parts = _timestamp_from_date_time(available_date, available_time, now)
    available_at = available_at.where(available_at.notna(), available_from_parts.where(pd.notna(available_date), pd.NaT))
    availability_anchor = consensus_date.where(pd.notna(consensus_date), as_of_date)
    available_at = available_at.where(
        available_at.notna(),
        _timestamp_from_dates_end_of_day(availability_anchor, now),
    )

    fiscal_year = _integer_series(raw, "fiscal_year")
    fiscal_year = fiscal_year.where(
        fiscal_year.notna(),
        period_end.map(lambda value: pd.NA if pd.isna(value) else value.year).astype("Int64"),
    )
    fpi = _string_series(raw, "fpi").str.upper()
    period_type_existing = _string_series(raw, "period_type").str.upper()
    period_type = pd.Series(
        [
            str(existing).strip().upper()
            if not pd.isna(existing) and str(existing).strip()
            else _period_type_from_fpi(code)
            for existing, code in zip(period_type_existing, fpi)
        ],
        index=raw.index,
        dtype="string",
    )
    fiscal_period_raw = _string_series(raw, "fiscal_period")
    fiscal_period = pd.Series(
        [
            _derive_consensus_fiscal_period(existing, ptype, pend)
            for existing, ptype, pend in zip(fiscal_period_raw, period_type, period_end)
        ],
        index=raw.index,
        dtype="string",
    )

    stale_after_date = _date_series(raw, "stale_after_date")
    stale_after_date = stale_after_date.where(
        pd.notna(stale_after_date),
        _stale_after_series(consensus_date, options.stale_after_days),
    )

    normalized = pd.DataFrame(index=raw.index)
    normalized["security_id"] = security_id
    normalized["symbol"] = symbol
    normalized["vendor_security_id"] = vendor_security_id
    normalized["vendor_security_id_type"] = vendor_security_id_type
    normalized["provider"] = provider
    normalized["source_vendor_table"] = _string_series(raw, "source_vendor_table")
    normalized["measure_code"] = measure_code
    normalized["fiscal_year"] = fiscal_year
    normalized["fiscal_period"] = fiscal_period
    normalized["period_end"] = period_end
    normalized["fpi"] = fpi
    normalized["period_type"] = period_type
    normalized["expected_report_date"] = _date_series(raw, "expected_report_date")
    normalized["consensus_date"] = consensus_date
    normalized["mean"] = _numeric_series(raw, "mean")
    normalized["median"] = _numeric_series(raw, "median")
    normalized["high"] = _numeric_series(raw, "high")
    normalized["low"] = _numeric_series(raw, "low")
    normalized["stdev"] = _numeric_series(raw, "stdev")
    normalized["num_estimates"] = _integer_series(raw, "num_estimates")
    normalized["num_up"] = _integer_series(raw, "num_up")
    normalized["num_down"] = _integer_series(raw, "num_down")
    normalized["currency"] = _string_series(raw, "currency").str.upper()
    normalized["pdf"] = pdf
    normalized["basis"] = _string_series(raw, "basis")
    normalized["is_gaap"] = _bool_series(raw, "is_gaap")
    normalized["unit"] = _string_series(raw, "unit")
    normalized["stale_after_date"] = stale_after_date
    normalized["as_of_date"] = as_of_date
    normalized["available_at"] = available_at
    normalized["source_file"] = str(source_file) if source_file else pd.NA
    normalized["source_file_sha256"] = source_file_sha256
    normalized["raw_payload_json"] = _raw_payloads(raw)
    normalized["run_id"] = _string_series(raw, "run_id").where(_string_series(raw, "run_id").notna(), options.run_id)
    normalized["source"] = source

    statistic_columns = ["mean", "median", "high", "low", "stdev", "num_estimates"]
    has_statistic = normalized[statistic_columns].notna().any(axis=1)
    normalized = normalized[
        normalized["period_end"].notna()
        & normalized["consensus_date"].notna()
        & normalized["available_at"].notna()
        & normalized["measure_code"].notna()
        & has_statistic
        & (
            normalized["security_id"].notna()
            | normalized["symbol"].notna()
            | normalized["vendor_security_id"].notna()
        )
    ].copy()
    if normalized.empty:
        return _empty_estimate_consensus_frame()

    normalized["est_consensus_id"] = normalized.apply(
        lambda row: _hash_id(
            "EST-CONSENSUS",
            row.get("source"),
            row.get("provider"),
            row.get("security_id"),
            row.get("symbol"),
            row.get("vendor_security_id_type"),
            row.get("vendor_security_id"),
            row.get("measure_code"),
            row.get("period_end"),
            row.get("consensus_date"),
            row.get("fpi"),
            row.get("source_file_sha256"),
        ),
        axis=1,
    )
    normalized = normalized.drop_duplicates(subset=["est_consensus_id"])
    return normalized[ESTIMATE_CONSENSUS_COLUMNS]


def _estimate_consensus_periods(consensus: pd.DataFrame) -> pd.DataFrame:
    if consensus.empty:
        return pd.DataFrame(columns=ESTIMATE_PERIOD_COLUMNS)
    periods = pd.DataFrame(
        {
            "provider": consensus["provider"],
            "measure_code": consensus["measure_code"],
            "fiscal_year": consensus["fiscal_year"],
            "fiscal_period": consensus["fiscal_period"],
            "period_end": consensus["period_end"],
            "fpi": consensus["fpi"],
            "period_type": consensus["period_type"],
            "expected_report_date": consensus["expected_report_date"],
            "valid_from": consensus["consensus_date"],
            "valid_to": consensus["stale_after_date"],
            "as_of_date": consensus["as_of_date"],
            "available_at": consensus["available_at"],
            "source": consensus["source"],
            "run_id": consensus["run_id"],
        }
    ).drop_duplicates()
    periods["est_period_id"] = periods.apply(
        lambda row: _hash_id(
            "EST-PERIOD",
            row.get("provider"),
            row.get("measure_code"),
            row.get("period_end"),
            row.get("fpi"),
            row.get("fiscal_year"),
            row.get("fiscal_period"),
        ),
        axis=1,
    )
    return periods[ESTIMATE_PERIOD_COLUMNS].drop_duplicates(subset=["est_period_id"])


def _write_estimate_consensus_frame(
    store: DuckDBStore,
    consensus: pd.DataFrame,
    *,
    options: EstimateConsensusOptions,
    source_file_sha256: str | None = None,
) -> int:
    if consensus.empty:
        return 0
    periods = _estimate_consensus_periods(consensus)
    with store.transaction():
        if source_file_sha256 and options.replace_source_file:
            store.con.execute(
                """
                DELETE FROM est_consensus
                WHERE source = ?
                  AND source_file_sha256 = ?
                """,
                [options.source, source_file_sha256],
            )
        insert_frame(store, consensus, "est_consensus", "est_consensus_insert")
        _insert_or_replace_frame(store, periods, "est_period_dim", "est_consensus_period_insert")
    return int(len(consensus))


def load_estimate_consensus_rows(
    store: DuckDBStore,
    options: EstimateConsensusOptions,
) -> int:
    store.initialize()
    if options.source_file is None:
        return 0
    source_file = Path(options.source_file)
    frame = pd.read_csv(source_file, dtype=str, keep_default_na=False)
    source_hash = file_sha256(source_file)
    consensus = normalize_estimate_consensus_rows(
        frame,
        options=options,
        source_file_sha256=source_hash,
        source_file=source_file,
    )
    record_source_file(
        store,
        dataset_id="est_consensus",
        source_url=str(source_file),
        cache_path=source_file,
        sha256=source_hash,
        metadata={"provider": options.provider_name, "rows": int(len(frame))},
    )
    return _write_estimate_consensus_frame(
        store,
        consensus,
        options=options,
        source_file_sha256=source_hash,
    )


class EstimateConsensusDataset(Dataset):
    """Consensus estimates loader.

    Default-empty (licensed: IBES, FactSet Estimates, Zacks).
    Pass `source_file` for CSV/IBES-style summary snapshots or an injectable
    `provider: Callable[[], Iterable[dict]]` for test/adapter rows.

    File loads are idempotent per `(source, source_file_sha256)` and keep a stable
    `est_consensus_id`. Callable providers preserve the old append semantics.
    """
    dataset_id = "est_consensus"
    source_name = "est_consensus_injectable"

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: EstimateConsensusOptions) -> DatasetLoadResult:
        if options.source_file is not None:
            rows_loaded = load_estimate_consensus_rows(store, options)
            if rows_loaded:
                quality_check(
                    store,
                    dataset_id=self.dataset_id,
                    table_name="est_consensus",
                    check_name="est_consensus_loaded",
                    status="passed",
                    observed_value=float(rows_loaded),
                    threshold_value=0.0,
                    details={"provider": options.provider_name, "source_file": str(options.source_file)},
                )
            return DatasetLoadResult(
                dataset_id=self.dataset_id,
                rows_loaded=rows_loaded,
                source=self.source_name,
                details={
                    "provider": options.provider_name,
                    "source_file": str(options.source_file),
                },
            )

        if options.provider is None:
            return DatasetLoadResult(
                dataset_id=self.dataset_id,
                rows_loaded=0,
                source=self.source_name,
                details={"reason": "no provider/source_file supplied; table remains empty"},
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
        # Preserve historical behavior for callables: omit available_at -> now.
        if "available_at" not in df.columns:
            df["available_at"] = now_utc_naive()
        else:
            df["available_at"] = df["available_at"].fillna(now_utc_naive())
        consensus = normalize_estimate_consensus_rows(df, options=options)
        rows_loaded = _write_estimate_consensus_frame(store, consensus, options=options)

        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows_loaded,
            source=self.source_name,
            details={"provider": options.provider_name},
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
    source_file: Path | None = None
    source: str = "est_recommendation_injected_v1"
    provider_name: str = "INJECTED"
    vendor_security_id_type: str = "IBES_TICKER"
    source_vendor_table: str | None = None
    replace_source_file: bool = True
    run_id: str | None = None


def _empty_estimate_recommendation_frame() -> pd.DataFrame:
    return pd.DataFrame(columns=ESTIMATE_RECOMMENDATION_COLUMNS)


def _normalize_recommendation_columns(frame: pd.DataFrame) -> pd.DataFrame:
    renamed: dict[str, str] = {}
    for column in frame.columns:
        normalized = snake_case(str(column)).lower()
        compact = normalized.replace("_", "")
        renamed[column] = RECOMMENDATION_COLUMN_ALIASES.get(
            normalized,
            RECOMMENDATION_COLUMN_ALIASES.get(compact, normalized),
        )
    return frame.rename(columns=renamed)


def _recommendation_code(value: Any, rating_text: Any = None) -> int | None:
    if not pd.isna(value) and str(value).strip():
        try:
            parsed = int(float(str(value).strip()))
        except ValueError:
            parsed = RECOMMENDATION_TEXT_MAP.get(str(value).strip().upper())
        if parsed in RECOMMENDATION_LABELS:
            return parsed
    if not pd.isna(rating_text) and str(rating_text).strip():
        text = re.sub(r"\s+", " ", str(rating_text).strip().upper())
        if text in RECOMMENDATION_TEXT_MAP:
            return RECOMMENDATION_TEXT_MAP[text]
        compact = text.replace("_", " ").replace("-", " ")
        return RECOMMENDATION_TEXT_MAP.get(compact)
    return None


def _recommendation_label(code: Any, rating_text: Any = None) -> str | None:
    if not pd.isna(code):
        try:
            parsed = int(code)
            if parsed in RECOMMENDATION_LABELS:
                return RECOMMENDATION_LABELS[parsed]
        except (TypeError, ValueError):
            pass
    if not pd.isna(rating_text) and str(rating_text).strip():
        return str(rating_text).strip()
    return None


def _standardized_rating(label: Any) -> str | None:
    if pd.isna(label) or not str(label).strip():
        return None
    return re.sub(r"[^0-9A-Z]+", "_", str(label).strip().upper()).strip("_") or None


def _recommendation_action(new_code: Any, prior_code: Any, explicit_action: Any = None) -> str | None:
    if not pd.isna(explicit_action) and str(explicit_action).strip():
        return str(explicit_action).strip().upper()
    if pd.isna(new_code) or pd.isna(prior_code):
        return None
    try:
        new_int = int(new_code)
        prior_int = int(prior_code)
    except (TypeError, ValueError):
        return None
    if new_int < prior_int:
        return "UPGRADE"
    if new_int > prior_int:
        return "DOWNGRADE"
    return "REITERATE"


def normalize_estimate_recommendation_rows(
    frame: pd.DataFrame,
    *,
    options: EstimateRecommendationOptions,
    source_file_sha256: str | None = None,
    source_file: Path | None = None,
) -> pd.DataFrame:
    if frame.empty:
        return _empty_estimate_recommendation_frame()

    raw = _normalize_recommendation_columns(frame.copy())
    if "rating" not in raw.columns and "recommendation_code" not in raw.columns and "price_target" not in raw.columns:
        raise ValueError("Estimate recommendation rows require rating/recommendation_code or price_target")

    now = now_utc_naive()
    provider = _provider_name_series(raw, options.provider_name)
    source = _source_series(raw, options.source)
    symbol = _string_series(raw, "symbol").map(
        lambda value: symbol_key(value) if not pd.isna(value) and str(value).strip() else pd.NA
    ).astype("string")
    vendor_security_id = _string_series(raw, "vendor_security_id").str.upper()
    vendor_security_id_type = _string_series(raw, "vendor_security_id_type").where(
        _string_series(raw, "vendor_security_id_type").notna(),
        options.vendor_security_id_type,
    ).str.upper()
    security_raw = _string_series(raw, "security_id")
    security_id = pd.Series(
        [
            str(existing).strip()
            if not pd.isna(existing) and str(existing).strip()
            else (security_id_for_symbol(sym) if not pd.isna(sym) and str(sym).strip() else pd.NA)
            for existing, sym in zip(security_raw, symbol)
        ],
        index=raw.index,
        dtype="string",
    )

    rating_text = _string_series(raw, "rating")
    recommendation_code = pd.Series(
        [
            _recommendation_code(code, text)
            for code, text in zip(_string_series(raw, "recommendation_code"), rating_text)
        ],
        index=raw.index,
        dtype="Int64",
    )
    prior_rating = _string_series(raw, "prior_rating")
    prior_recommendation_code = pd.Series(
        [
            _recommendation_code(code, text)
            for code, text in zip(_string_series(raw, "prior_recommendation_code"), prior_rating)
        ],
        index=raw.index,
        dtype="Int64",
    )
    recommendation_label = pd.Series(
        [_recommendation_label(code, text) for code, text in zip(recommendation_code, rating_text)],
        index=raw.index,
        dtype="string",
    )
    prior_recommendation_label = pd.Series(
        [_recommendation_label(code, text) for code, text in zip(prior_recommendation_code, prior_rating)],
        index=raw.index,
        dtype="string",
    )
    rating_standardized_raw = _string_series(raw, "rating_standardized")
    rating_standardized = rating_standardized_raw.where(
        rating_standardized_raw.notna(),
        recommendation_label.map(_standardized_rating).astype("string"),
    )

    announce_date = _date_series(raw, "announce_date", fallback=_date_series(raw, "rating_date"))
    rating_date = _date_series(raw, "rating_date", fallback=announce_date)
    activation_date = _date_series(raw, "activation_date", fallback=announce_date)
    as_of_date = _date_series(raw, "as_of_date", fallback=rating_date)
    available_at = _timestamp_series(raw, "available_at")
    activation_ts = _timestamp_from_date_time(activation_date, _time_series(raw, "activation_time"), now)
    announce_ts = _timestamp_from_date_time(announce_date, _time_series(raw, "announce_time"), now)
    available_at = available_at.where(available_at.notna(), activation_ts.where(pd.notna(activation_date), pd.NaT))
    available_at = available_at.where(available_at.notna(), announce_ts.where(pd.notna(announce_date), pd.NaT))
    available_at = available_at.where(available_at.notna(), _timestamp_from_dates_end_of_day(rating_date, now))

    price_target = _numeric_series(raw, "price_target")
    target_horizon_months = _integer_series(raw, "target_horizon_months")
    target_horizon_months = target_horizon_months.where(
        target_horizon_months.notna() | price_target.isna(),
        12,
    )
    source_vendor_table = _string_series(raw, "source_vendor_table")
    if options.source_vendor_table:
        source_vendor_table = source_vendor_table.where(source_vendor_table.notna(), options.source_vendor_table)
    event_type = pd.Series(
        [
            "RECOMMENDATION_PRICE_TARGET"
            if not pd.isna(code) and not pd.isna(target)
            else ("PRICE_TARGET" if not pd.isna(target) else "RECOMMENDATION")
            for code, target in zip(recommendation_code, price_target)
        ],
        index=raw.index,
        dtype="string",
    )
    rating_scale_raw = _string_series(raw, "rating_scale")
    rating_scale = rating_scale_raw.where(rating_scale_raw.notna(), "IBES_1_STRONG_BUY_5_SELL")
    industry_code = _string_series(raw, "industry_code")
    is_industry = _bool_series(raw, "is_industry_recommendation")
    is_industry = is_industry.where(is_industry.notna(), industry_code.notna())

    normalized = pd.DataFrame(index=raw.index)
    normalized["security_id"] = security_id
    normalized["symbol"] = symbol
    normalized["vendor_security_id"] = vendor_security_id
    normalized["vendor_security_id_type"] = vendor_security_id_type
    normalized["cusip"] = _string_series(raw, "cusip").str.upper()
    normalized["provider"] = provider
    normalized["source_vendor_table"] = source_vendor_table
    normalized["vendor_broker_id"] = _string_series(raw, "vendor_broker_id")
    normalized["vendor_analyst_id"] = _string_series(raw, "vendor_analyst_id")
    normalized["broker_mask_code"] = _string_series(raw, "broker_mask_code")
    normalized["analyst_mask_code"] = _string_series(raw, "analyst_mask_code")
    normalized["broker_name"] = _string_series(raw, "broker_name")
    normalized["analyst_name"] = _string_series(raw, "analyst_name")
    normalized["broker_id"] = [
        _derive_party_id("EST-BROKER", canonical, prov, vendor_id, mask_code, name, valid_from)
        for canonical, prov, vendor_id, mask_code, name, valid_from in zip(
            _string_series(raw, "broker_id"),
            provider,
            normalized["vendor_broker_id"],
            normalized["broker_mask_code"],
            normalized["broker_name"],
            rating_date,
        )
    ]
    normalized["analyst_id"] = [
        _derive_party_id("EST-ANALYST", canonical, prov, vendor_id, mask_code, name, valid_from)
        for canonical, prov, vendor_id, mask_code, name, valid_from in zip(
            _string_series(raw, "analyst_id"),
            provider,
            normalized["vendor_analyst_id"],
            normalized["analyst_mask_code"],
            normalized["analyst_name"],
            rating_date,
        )
    ]
    normalized["rating"] = rating_text.where(rating_text.notna(), recommendation_label)
    normalized["rating_standardized"] = rating_standardized
    normalized["recommendation_code"] = recommendation_code
    normalized["recommendation_label"] = recommendation_label
    normalized["prior_rating"] = prior_rating.where(prior_rating.notna(), prior_recommendation_label)
    normalized["prior_recommendation_code"] = prior_recommendation_code
    normalized["prior_recommendation_label"] = prior_recommendation_label
    normalized["rating_scale"] = rating_scale
    normalized["action"] = [
        _recommendation_action(new, prior, action)
        for new, prior, action in zip(recommendation_code, prior_recommendation_code, _string_series(raw, "action"))
    ]
    normalized["event_type"] = event_type
    normalized["price_target"] = price_target
    normalized["target_currency"] = _string_series(raw, "target_currency").str.upper()
    normalized["target_horizon_months"] = target_horizon_months
    normalized["industry_code"] = industry_code
    normalized["is_industry_recommendation"] = is_industry
    normalized["usfirm"] = _string_series(raw, "usfirm")
    normalized["rating_date"] = rating_date
    normalized["announce_date"] = announce_date
    normalized["announce_time"] = _time_series(raw, "announce_time")
    normalized["activation_date"] = activation_date
    normalized["activation_time"] = _time_series(raw, "activation_time")
    normalized["revision_date"] = _date_series(raw, "revision_date")
    normalized["revision_time"] = _time_series(raw, "revision_time")
    normalized["stop_date"] = _date_series(raw, "stop_date")
    normalized["as_of_date"] = as_of_date
    normalized["available_at"] = available_at
    normalized["source_file"] = str(source_file) if source_file else pd.NA
    normalized["source_file_sha256"] = source_file_sha256
    normalized["raw_payload_json"] = _raw_payloads(raw)
    normalized["run_id"] = _string_series(raw, "run_id").where(_string_series(raw, "run_id").notna(), options.run_id)
    normalized["source"] = source

    normalized = normalized[
        normalized["rating_date"].notna()
        & normalized["available_at"].notna()
        & (
            normalized["recommendation_code"].notna()
            | normalized["rating"].notna()
            | normalized["price_target"].notna()
        )
        & (
            normalized["security_id"].notna()
            | normalized["symbol"].notna()
            | normalized["vendor_security_id"].notna()
            | normalized["cusip"].notna()
        )
    ].copy()
    if normalized.empty:
        return _empty_estimate_recommendation_frame()

    normalized["est_recommendation_id"] = normalized.apply(
        lambda row: _hash_id(
            "EST-REC",
            row.get("source"),
            row.get("provider"),
            row.get("security_id"),
            row.get("symbol"),
            row.get("vendor_security_id_type"),
            row.get("vendor_security_id"),
            row.get("broker_id"),
            row.get("analyst_id"),
            row.get("event_type"),
            row.get("rating_date"),
            row.get("recommendation_code"),
            row.get("price_target"),
            row.get("source_file_sha256"),
        ),
        axis=1,
    )
    normalized = normalized.drop_duplicates(subset=["est_recommendation_id"])
    return normalized[ESTIMATE_RECOMMENDATION_COLUMNS]


def _estimate_recommendation_dimensions(
    recommendation: pd.DataFrame,
) -> tuple[pd.DataFrame, pd.DataFrame, pd.DataFrame, pd.DataFrame]:
    valid_from = recommendation["rating_date"].where(recommendation["rating_date"].notna(), recommendation["as_of_date"])
    valid_to = recommendation["stop_date"].where(recommendation["stop_date"].notna(), recommendation["revision_date"])
    brokers = pd.DataFrame(
        {
            "broker_id": recommendation["broker_id"],
            "broker_name": recommendation["broker_name"],
            "source": recommendation["source"],
            "provider": recommendation["provider"],
            "vendor_broker_id": recommendation["vendor_broker_id"],
            "broker_mask_code": recommendation["broker_mask_code"],
            "valid_from": valid_from,
            "valid_to": valid_to,
            "available_at": recommendation["available_at"],
            "run_id": recommendation["run_id"],
            "source_file_sha256": recommendation["source_file_sha256"],
        }
    ).dropna(subset=["broker_id"]).drop_duplicates(subset=["broker_id"])
    analysts = pd.DataFrame(
        {
            "analyst_id": recommendation["analyst_id"],
            "analyst_name": recommendation["analyst_name"],
            "broker_id": recommendation["broker_id"],
            "source": recommendation["source"],
            "provider": recommendation["provider"],
            "vendor_analyst_id": recommendation["vendor_analyst_id"],
            "analyst_mask_code": recommendation["analyst_mask_code"],
            "valid_from": valid_from,
            "valid_to": valid_to,
            "available_at": recommendation["available_at"],
            "run_id": recommendation["run_id"],
            "source_file_sha256": recommendation["source_file_sha256"],
        }
    ).dropna(subset=["analyst_id"]).drop_duplicates(subset=["analyst_id"])
    broker_alias_rows = []
    for _, row in recommendation.dropna(subset=["broker_id"]).iterrows():
        for alias_type, alias_value in (
            ("VENDOR_BROKER_ID", row.get("vendor_broker_id")),
            ("BROKER_MASK_CODE", row.get("broker_mask_code")),
            ("BROKER_NAME", row.get("broker_name")),
        ):
            if pd.isna(alias_value) or not str(alias_value).strip():
                continue
            broker_alias_rows.append(
                {
                    "broker_alias_id": _hash_id(
                        "EST-BROKER-ALIAS",
                        row.get("provider"),
                        row.get("broker_id"),
                        alias_type,
                        alias_value,
                        row.get("rating_date"),
                    ),
                    "broker_id": row.get("broker_id"),
                    "provider": row.get("provider"),
                    "alias_type": alias_type,
                    "alias_value": str(alias_value).strip(),
                    "valid_from": row.get("rating_date"),
                    "valid_to": row.get("stop_date") if pd.notna(row.get("stop_date")) else row.get("revision_date"),
                    "available_at": row.get("available_at"),
                    "source": row.get("source"),
                    "run_id": row.get("run_id"),
                }
            )
    analyst_alias_rows = []
    for _, row in recommendation.dropna(subset=["analyst_id"]).iterrows():
        for alias_type, alias_value in (
            ("VENDOR_ANALYST_ID", row.get("vendor_analyst_id")),
            ("ANALYST_MASK_CODE", row.get("analyst_mask_code")),
            ("ANALYST_NAME", row.get("analyst_name")),
        ):
            if pd.isna(alias_value) or not str(alias_value).strip():
                continue
            analyst_alias_rows.append(
                {
                    "analyst_alias_id": _hash_id(
                        "EST-ANALYST-ALIAS",
                        row.get("provider"),
                        row.get("analyst_id"),
                        alias_type,
                        alias_value,
                        row.get("rating_date"),
                    ),
                    "analyst_id": row.get("analyst_id"),
                    "provider": row.get("provider"),
                    "alias_type": alias_type,
                    "alias_value": str(alias_value).strip(),
                    "valid_from": row.get("rating_date"),
                    "valid_to": row.get("stop_date") if pd.notna(row.get("stop_date")) else row.get("revision_date"),
                    "available_at": row.get("available_at"),
                    "source": row.get("source"),
                    "run_id": row.get("run_id"),
                }
            )
    return (
        brokers,
        analysts,
        pd.DataFrame(broker_alias_rows, columns=ESTIMATE_BROKER_ALIAS_COLUMNS),
        pd.DataFrame(analyst_alias_rows, columns=ESTIMATE_ANALYST_ALIAS_COLUMNS),
    )


def _write_estimate_recommendation_frame(
    store: DuckDBStore,
    recommendation: pd.DataFrame,
    *,
    options: EstimateRecommendationOptions,
    source_file_sha256: str | None = None,
) -> int:
    if recommendation.empty:
        return 0
    brokers, analysts, broker_aliases, analyst_aliases = _estimate_recommendation_dimensions(recommendation)
    with store.transaction():
        if source_file_sha256 and options.replace_source_file:
            store.con.execute(
                """
                DELETE FROM est_recommendation
                WHERE source = ?
                  AND source_file_sha256 = ?
                """,
                [options.source, source_file_sha256],
            )
        insert_frame(store, recommendation, "est_recommendation", "est_recommendation_insert")
        _insert_or_replace_frame(store, brokers, "est_broker", "est_rec_broker_insert")
        _insert_or_replace_frame(store, analysts, "est_analyst", "est_rec_analyst_insert")
        _insert_or_replace_frame(store, broker_aliases, "est_broker_alias", "est_rec_broker_alias_insert")
        _insert_or_replace_frame(store, analyst_aliases, "est_analyst_alias", "est_rec_analyst_alias_insert")
    return int(len(recommendation))


def load_estimate_recommendation_rows(
    store: DuckDBStore,
    options: EstimateRecommendationOptions,
) -> int:
    store.initialize()
    if options.source_file is None:
        return 0
    source_file = Path(options.source_file)
    frame = pd.read_csv(source_file, dtype=str, keep_default_na=False)
    source_hash = file_sha256(source_file)
    recommendation = normalize_estimate_recommendation_rows(
        frame,
        options=options,
        source_file_sha256=source_hash,
        source_file=source_file,
    )
    record_source_file(
        store,
        dataset_id="est_recommendation",
        source_url=str(source_file),
        cache_path=source_file,
        sha256=source_hash,
        metadata={"provider": options.provider_name, "rows": int(len(frame))},
    )
    return _write_estimate_recommendation_frame(
        store,
        recommendation,
        options=options,
        source_file_sha256=source_hash,
    )


class EstimateRecommendationDataset(Dataset):
    """Broker recommendation and price-target loader.

    Default-empty (licensed vendor data — IBES, FactSet, etc.).
    Pass a source CSV or injectable `provider: Callable[[], Iterable[dict]]` to populate.

    CSV loads are idempotent per source-file hash and normalize IBES-style
    recddet/ptgdet rows into deterministic event ids. Callable providers remain
    supported for tests or licensed adapters and are normalized before insert.
    """
    dataset_id = "est_recommendation"
    source_name = "est_recommendation_injectable"

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(
        self, store: DuckDBStore, options: EstimateRecommendationOptions
    ) -> DatasetLoadResult:
        if options.source_file is not None:
            rows_loaded = load_estimate_recommendation_rows(store, options)
            if rows_loaded:
                quality_check(
                    store,
                    dataset_id=self.dataset_id,
                    table_name="est_recommendation",
                    check_name="est_recommendation_loaded",
                    status="passed",
                    observed_value=float(rows_loaded),
                    threshold_value=0.0,
                    details={"provider": options.provider_name, "source_file": str(options.source_file)},
                )
            return DatasetLoadResult(
                dataset_id=self.dataset_id,
                rows_loaded=rows_loaded,
                source=self.source_name,
                details={
                    "provider": options.provider_name,
                    "source_file": str(options.source_file),
                },
            )

        if options.provider is None:
            return DatasetLoadResult(
                dataset_id=self.dataset_id,
                rows_loaded=0,
                source=self.source_name,
                details={"reason": "no provider/source_file supplied; table remains empty"},
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
        source_from_rows = "source" in df.columns
        if "available_at" not in df.columns:
            df["available_at"] = now_utc_naive()
        else:
            df["available_at"] = df["available_at"].fillna(now_utc_naive())
        if not source_from_rows:
            df["source"] = self.source_name
        recommendation = normalize_estimate_recommendation_rows(df, options=options)
        rows_loaded = _write_estimate_recommendation_frame(store, recommendation, options=options)

        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows_loaded,
            source=self.source_name,
            details={"provider": options.provider_name},
        )
