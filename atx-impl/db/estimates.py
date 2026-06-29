"""Estimates store datasets for the atx-impl DuckDB warehouse.

Provides:
- EstimateMeasureSeedDataset  — static dimension seed (5 measures)
- EstimateActualsDataset       — REAL: reads sec_company_facts, maps XBRL→measure
- EstimateSurpriseDataset      — REAL derived: Standardized Unexpected Earnings (SUE)
                                  Foster-Olsen-Shevlin (1984) seasonal-random-walk-with-drift.
- EstimateConsensusDataset     — injectable; default-empty (licensed: IBES/FactSet/Zacks)
- EstimateGuidanceDataset      — injectable plus local SEC 8-K guidance text extraction
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
est_guidance : available_at carried from provider/source row if supplied; local
               SEC 8-K text extraction falls back to acceptance_datetime or
               guidance_date end-of-day. Callable rows fall back to now_utc_naive().
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

ESTIMATE_GUIDANCE_COLUMNS = [
    "est_guidance_id",
    "security_id",
    "measure_code",
    "fiscal_year",
    "fiscal_period",
    "period_end",
    "low",
    "high",
    "mid",
    "guidance_type",
    "basis",
    "currency",
    "unit",
    "units_scale",
    "source_item",
    "guidance_date",
    "form",
    "accession_number",
    "as_of_date",
    "available_at",
    "extraction_confidence",
    "evidence_text",
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

ESTIMATE_RECOMMENDATION_SUMMARY_COLUMNS = [
    "est_recommendation_summary_id",
    "security_id",
    "symbol",
    "vendor_security_id",
    "vendor_security_id_type",
    "cusip",
    "provider",
    "source_vendor_table",
    "snapshot_date",
    "as_of_date",
    "available_at",
    "mean_recommendation",
    "median_recommendation",
    "rating_scale",
    "scale_direction",
    "strong_buy_count",
    "buy_count",
    "hold_count",
    "underperform_count",
    "sell_count",
    "buy_equivalent_count",
    "sell_equivalent_count",
    "total_recommendations",
    "mean_price_target",
    "median_price_target",
    "high_price_target",
    "low_price_target",
    "price_target_count",
    "target_currency",
    "target_horizon_months",
    "provider_scale_notes",
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

RECOMMENDATION_SUMMARY_COLUMN_ALIASES = {
    "asof": "as_of_date",
    "available_date": "available_date",
    "best_analyst_rating": "mean_recommendation",
    "best_analyst_recs": "total_recommendations",
    "best_recs_buys": "buy_equivalent_count",
    "best_recs_holds": "hold_count",
    "best_recs_sells": "sell_equivalent_count",
    "best_target_price": "mean_price_target",
    "buy": "buy_count",
    "buy_count": "buy_count",
    "buy_equivalent": "buy_equivalent_count",
    "buy_equivalent_count": "buy_equivalent_count",
    "buys": "buy_count",
    "consensus_date": "snapshot_date",
    "curr": "target_currency",
    "estcur": "target_currency",
    "highptg": "high_price_target",
    "hold": "hold_count",
    "hold_count": "hold_count",
    "holds": "hold_count",
    "horizon": "target_horizon_months",
    "ibes_ticker": "vendor_security_id",
    "lowptg": "low_price_target",
    "mean_ptg": "mean_price_target",
    "mean_rating": "mean_recommendation",
    "mean_recommendation": "mean_recommendation",
    "meanptg": "mean_price_target",
    "meanrec": "mean_recommendation",
    "med_ptg": "median_price_target",
    "median_rating": "median_recommendation",
    "median_recommendation": "median_recommendation",
    "medptg": "median_price_target",
    "medrec": "median_recommendation",
    "num_buy": "buy_count",
    "num_buys": "buy_count",
    "num_hold": "hold_count",
    "num_holds": "hold_count",
    "num_ptg": "price_target_count",
    "num_rec": "total_recommendations",
    "num_recommendations": "total_recommendations",
    "num_sell": "sell_count",
    "num_sells": "sell_count",
    "num_strong_buy": "strong_buy_count",
    "num_strong_buys": "strong_buy_count",
    "num_underperform": "underperform_count",
    "num_underperforms": "underperform_count",
    "numptg": "price_target_count",
    "numrec": "total_recommendations",
    "oftic": "symbol",
    "official_ticker": "symbol",
    "ptg_count": "price_target_count",
    "rating_count": "total_recommendations",
    "recs_buys": "buy_equivalent_count",
    "recs_holds": "hold_count",
    "recs_sells": "sell_equivalent_count",
    "sell": "sell_count",
    "sell_count": "sell_count",
    "sell_equivalent": "sell_equivalent_count",
    "sell_equivalent_count": "sell_equivalent_count",
    "sells": "sell_count",
    "snapshot_date": "snapshot_date",
    "source_table": "source_vendor_table",
    "statpers": "snapshot_date",
    "strong_buy": "strong_buy_count",
    "strong_buy_count": "strong_buy_count",
    "strong_buys": "strong_buy_count",
    "target": "mean_price_target",
    "target_currency": "target_currency",
    "target_horizon_mo": "target_horizon_months",
    "target_price": "mean_price_target",
    "ticker": "vendor_security_id",
    "tic": "symbol",
    "underperform": "underperform_count",
    "underperform_count": "underperform_count",
    "underperforms": "underperform_count",
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
    """Guidance loader for injectable rows or local SEC 8-K text files.

    Pass `source_file` for local text extraction or `fetch`/`parse` for callables.
    """
    source_file: Path | None = None
    source: str = "est_guidance_injectable"
    fetch: Callable[[], Iterable[Any]] | None = None
    parse: Callable[[Any], Iterable[dict]] | None = None
    replace_source_file: bool = True
    min_confidence: float = 0.70
    run_id: str | None = None


GUIDANCE_COLUMN_ALIASES = {
    "acceptancedatetime": "acceptance_datetime",
    "accession": "accession_number",
    "accessionnumber": "accession_number",
    "confidence": "extraction_confidence",
    "content": "document_text",
    "document": "document_text",
    "documenttext": "document_text",
    "exhibittext": "document_text",
    "extractionconfidence": "extraction_confidence",
    "filingdate": "filing_date",
    "fiscalperiod": "fiscal_period",
    "fiscalyear": "fiscal_year",
    "guidancecurrency": "currency",
    "guidancedate": "guidance_date",
    "guidancehigh": "high",
    "guidancelow": "low",
    "guidancemid": "mid",
    "guidancepoint": "mid",
    "guidancetype": "guidance_type",
    "item": "source_item",
    "measure": "measure_code",
    "measurecode": "measure_code",
    "periodend": "period_end",
    "rawtext": "document_text",
    "reportdate": "report_date",
    "securityid": "security_id",
    "sourceitem": "source_item",
    "sourceurl": "source_url",
    "ticker": "symbol",
    "unitsscale": "units_scale",
}

GUIDANCE_TEXT_COLUMNS = ("document_text", "text", "raw_text", "content", "exhibit_text")

GUIDANCE_SOURCE_CUE_RE = re.compile(
    r"\b(expects?|anticipates?|projects?|forecasts?|outlook|guidance|"
    r"raises?|reaffirms?|targets?|sees)\b",
    flags=re.IGNORECASE,
)
GUIDANCE_VALUE_RE = re.compile(
    r"(?P<currency>\$)?\s*(?P<number>\d[\d,]*(?:\.\d+)?)\s*"
    r"(?P<unit>billion|bn|million|mm|m|thousand|k)?\b",
    flags=re.IGNORECASE,
)
GUIDANCE_MEASURE_PATTERNS = (
    (
        "EPS_DILUTED",
        re.compile(
            r"\b(?:non-gaap\s+|adjusted\s+|gaap\s+)?(?:diluted\s+)?"
            r"(?:eps|earnings\s+per\s+share)\b",
            flags=re.IGNORECASE,
        ),
    ),
    ("OPERATING_INCOME", re.compile(r"\boperating\s+income\b", flags=re.IGNORECASE)),
    ("NET_INCOME", re.compile(r"\bnet\s+income\b", flags=re.IGNORECASE)),
    ("REVENUE", re.compile(r"\b(?:revenue|revenues|sales)\b", flags=re.IGNORECASE)),
)


def _normalize_guidance_columns(frame: pd.DataFrame) -> pd.DataFrame:
    renamed: dict[str, str] = {}
    for column in frame.columns:
        normalized = snake_case(str(column)).lower()
        compact = normalized.replace("_", "")
        renamed[column] = GUIDANCE_COLUMN_ALIASES.get(
            normalized,
            GUIDANCE_COLUMN_ALIASES.get(compact, normalized),
        )
    return frame.rename(columns=renamed)


def _read_guidance_source_file(source_file: Path) -> pd.DataFrame:
    suffix = source_file.suffix.lower()
    if suffix == ".jsonl":
        rows = []
        with source_file.open("r", encoding="utf-8") as handle:
            for line in handle:
                text = line.strip()
                if text:
                    rows.append(json.loads(text))
        return pd.DataFrame(rows)
    if suffix == ".json":
        with source_file.open("r", encoding="utf-8") as handle:
            payload = json.load(handle)
        rows = payload.get("rows", payload) if isinstance(payload, dict) else payload
        return pd.DataFrame(rows)
    return pd.read_csv(source_file, dtype=str, keep_default_na=False)


def _clean_guidance_text(value: Any) -> str | None:
    if pd.isna(value):
        return None
    text = re.sub(r"\s+", " ", str(value)).strip()
    return text or None


def _guidance_record_value(record: dict[str, Any], *names: str) -> Any:
    for name in names:
        value = record.get(name)
        if value is not None and not pd.isna(value) and str(value).strip():
            return value
    return None


def _guidance_date_value(value: Any) -> dt.date | None:
    if value is None or pd.isna(value):
        return None
    parsed = pd.to_datetime(value, errors="coerce")
    if pd.isna(parsed):
        return None
    return parsed.date()


def _guidance_ts_value(value: Any) -> dt.datetime | None:
    if value is None or pd.isna(value):
        return None
    parsed = pd.to_datetime(value, errors="coerce")
    if pd.isna(parsed):
        return None
    return parsed.to_pydatetime().replace(tzinfo=None)


def _guidance_source_item(record: dict[str, Any], text: str) -> str:
    explicit = _guidance_record_value(record, "source_item")
    if explicit is not None:
        return str(explicit).strip()
    combined = f"{record.get('items', '')} {text[:1000]}".upper()
    has_202 = "2.02" in combined or "ITEM 2.02" in combined
    has_701 = "7.01" in combined or "ITEM 7.01" in combined
    if has_202 and has_701:
        return "8-K_2.02_7.01"
    if has_202:
        return "8-K_2.02"
    if has_701:
        return "8-K_7.01"
    return "8-K"


def _quarter_end(year: int, quarter: int) -> dt.date:
    month = quarter * 3
    if month == 12:
        return dt.date(year, 12, 31)
    return dt.date(year, month + 1, 1) - dt.timedelta(days=1)


def _guidance_period_from_text(sentence: str, record: dict[str, Any]) -> tuple[int | None, str | None, dt.date | None]:
    fiscal_year = _guidance_record_value(record, "fiscal_year")
    fiscal_period = _guidance_record_value(record, "fiscal_period")
    period_end = _guidance_date_value(_guidance_record_value(record, "period_end"))
    if fiscal_year is not None and fiscal_period is not None and period_end is not None:
        return int(float(fiscal_year)), str(fiscal_period).strip().upper(), period_end
    fy_match = re.search(r"\b(?:full\s+year|fiscal\s+year|fiscal|fy)\s*(20\d{2})\b", sentence, re.IGNORECASE)
    if fy_match:
        year = int(fy_match.group(1))
        return year, "FY", dt.date(year, 12, 31)
    q_match = re.search(
        r"\b(?:(first|second|third|fourth|1st|2nd|3rd|4th)\s+quarter|q([1-4]))\s*(?:of\s*)?(20\d{2})\b",
        sentence,
        re.IGNORECASE,
    )
    if q_match:
        quarter_text = (q_match.group(1) or q_match.group(2) or "").lower()
        year = int(q_match.group(3))
        quarter_map = {
            "first": 1, "1st": 1, "1": 1,
            "second": 2, "2nd": 2, "2": 2,
            "third": 3, "3rd": 3, "3": 3,
            "fourth": 4, "4th": 4, "4": 4,
        }
        quarter = quarter_map.get(quarter_text, int(quarter_text) if quarter_text.isdigit() else None)
        if quarter is not None:
            return year, f"Q{quarter}", _quarter_end(year, quarter)
    if period_end is not None:
        year = int(fiscal_year) if fiscal_year is not None else period_end.year
        period = str(fiscal_period).strip().upper() if fiscal_period is not None else _quarter_label(period_end)
        return year, period, period_end
    return None, None, None


def _guidance_basis(sentence: str) -> str | None:
    text = sentence.lower()
    if "non-gaap" in text or "adjusted" in text:
        return "NON_GAAP"
    if "gaap" in text:
        return "GAAP"
    return None


def _guidance_scale(unit: str | None) -> int:
    if not unit:
        return 1
    normalized = unit.strip().lower()
    if normalized in {"billion", "bn"}:
        return 1_000_000_000
    if normalized in {"million", "mm", "m"}:
        return 1_000_000
    if normalized in {"thousand", "k"}:
        return 1_000
    return 1


def _guidance_values_after(sentence: str, start: int, *, measure_code: str) -> tuple[float | None, float | None, float | None, int, str, str | None]:
    segment = sentence[start : start + 180]
    values: list[tuple[float, str | None, bool]] = []
    for match in GUIDANCE_VALUE_RE.finditer(segment):
        number = float(match.group("number").replace(",", ""))
        unit = match.group("unit")
        has_currency = bool(match.group("currency"))
        if not has_currency and unit is None and 1900 <= number <= 2100 and number.is_integer():
            continue
        values.append((number, unit, has_currency))
        if len(values) >= 2:
            break
    if not values:
        return None, None, None, 1, "PER_SHARE" if measure_code.startswith("EPS") else "VALUE", None
    first, second = values[0], values[1] if len(values) > 1 else None
    unit = first[1] or (second[1] if second else None)
    if measure_code.startswith("EPS"):
        scale = 1
        value_unit = "USD_PER_SHARE" if first[2] else "PER_SHARE"
        currency = "USD" if first[2] else None
    else:
        scale = _guidance_scale(unit)
        value_unit = "USD" if first[2] else "VALUE"
        currency = "USD" if first[2] else None
    if second is not None:
        low = min(first[0], second[0])
        high = max(first[0], second[0])
        return low, high, (low + high) / 2.0, scale, value_unit, currency
    return None, None, first[0], scale, value_unit, currency


def _guidance_confidence(*, has_range: bool, has_period: bool, source_item: str) -> float:
    score = 0.82 if has_range else 0.76
    if has_period:
        score += 0.04
    if "2.02" in source_item or "7.01" in source_item:
        score += 0.06
    return min(score, 0.96)


def _extract_guidance_rows_from_record(record: dict[str, Any], *, source: str, run_id: str | None) -> list[dict[str, Any]]:
    text = None
    for column in GUIDANCE_TEXT_COLUMNS:
        text = _clean_guidance_text(record.get(column))
        if text:
            break
    if not text:
        return []
    sentences = re.split(r"(?<=[.!?])\s+", text)
    guidance_date = (
        _guidance_date_value(_guidance_record_value(record, "guidance_date"))
        or _guidance_date_value(_guidance_record_value(record, "filing_date"))
        or _guidance_date_value(_guidance_record_value(record, "report_date"))
        or _guidance_date_value(_guidance_record_value(record, "acceptance_datetime"))
    )
    available_at = (
        _guidance_ts_value(_guidance_record_value(record, "available_at"))
        or _guidance_ts_value(_guidance_record_value(record, "acceptance_datetime"))
        or (dt.datetime.combine(guidance_date, dt.time(23, 59, 59)) if guidance_date is not None else None)
    )
    security_id = _guidance_record_value(record, "security_id")
    if security_id is None:
        symbol = _guidance_record_value(record, "symbol")
        security_id = security_id_for_symbol(symbol_key(symbol)) if symbol is not None else None
    rows: list[dict[str, Any]] = []
    for sentence in sentences:
        if not GUIDANCE_SOURCE_CUE_RE.search(sentence):
            continue
        source_item = _guidance_source_item(record, sentence)
        fiscal_year, fiscal_period, period_end = _guidance_period_from_text(sentence, record)
        for measure_code, measure_re in GUIDANCE_MEASURE_PATTERNS:
            for match in measure_re.finditer(sentence):
                low, high, mid, units_scale, value_unit, currency = _guidance_values_after(sentence, match.end(), measure_code=measure_code)
                if low is None and high is None and mid is None:
                    continue
                has_range = low is not None and high is not None
                rows.append(
                    {
                        "security_id": security_id,
                        "measure_code": measure_code,
                        "fiscal_year": fiscal_year,
                        "fiscal_period": fiscal_period,
                        "period_end": period_end,
                        "low": low,
                        "high": high,
                        "mid": mid,
                        "guidance_type": "RANGE" if has_range else "POINT",
                        "basis": _guidance_basis(sentence),
                        "currency": currency,
                        "unit": value_unit,
                        "units_scale": units_scale,
                        "source_item": source_item,
                        "guidance_date": guidance_date,
                        "form": _guidance_record_value(record, "form") or "8-K",
                        "accession_number": _guidance_record_value(record, "accession_number"),
                        "as_of_date": guidance_date or period_end,
                        "available_at": available_at,
                        "extraction_confidence": _guidance_confidence(has_range=has_range, has_period=period_end is not None, source_item=source_item),
                        "evidence_text": sentence.strip(),
                        "source_url": _guidance_record_value(record, "source_url"),
                        "run_id": run_id,
                        "source": source,
                    }
                )
    return rows


def _period_end_from_fiscal_fields(fiscal_year: Any, fiscal_period: Any) -> dt.date | None:
    if pd.isna(fiscal_year) or pd.isna(fiscal_period):
        return None
    try:
        year = int(fiscal_year)
    except (TypeError, ValueError):
        return None
    period = str(fiscal_period).strip().upper()
    if period == "FY":
        return dt.date(year, 12, 31)
    if period in {"Q1", "Q2", "Q3", "Q4"}:
        return _quarter_end(year, int(period[1]))
    return None


def _guidance_type(row: pd.Series) -> str | None:
    explicit = row.get("guidance_type")
    if explicit is not None and not pd.isna(explicit) and str(explicit).strip():
        return str(explicit).strip().upper()
    has_low = row.get("low") is not None and not pd.isna(row.get("low"))
    has_high = row.get("high") is not None and not pd.isna(row.get("high"))
    has_mid = row.get("mid") is not None and not pd.isna(row.get("mid"))
    if has_low and has_high:
        return "RANGE"
    if has_mid:
        return "POINT"
    if has_low:
        return "OPEN_LOW"
    if has_high:
        return "OPEN_HIGH"
    return None


def _empty_estimate_guidance_frame() -> pd.DataFrame:
    return pd.DataFrame(columns=ESTIMATE_GUIDANCE_COLUMNS)


def normalize_estimate_guidance_rows(
    frame: pd.DataFrame,
    *,
    options: EstimateGuidanceOptions,
    source_file_sha256: str | None = None,
    source_file: Path | None = None,
) -> pd.DataFrame:
    if frame.empty:
        return _empty_estimate_guidance_frame()
    raw = _normalize_guidance_columns(frame.copy())
    extracted_rows: list[dict[str, Any]] = []
    has_text = pd.Series(False, index=raw.index)
    for column in GUIDANCE_TEXT_COLUMNS:
        if column in raw.columns:
            has_text = has_text | _string_series(raw, column).notna()
    for record in raw[has_text].to_dict("records"):
        extracted_rows.extend(_extract_guidance_rows_from_record(record, source=options.source, run_id=options.run_id))
    candidate_measure = raw.get("measure_code", pd.Series([pd.NA] * len(raw), index=raw.index))
    normalized_candidates = raw[candidate_measure.replace("", pd.NA).notna()].copy()
    raw = pd.concat([normalized_candidates, pd.DataFrame(extracted_rows)], ignore_index=True) if extracted_rows else normalized_candidates
    if raw.empty:
        return _empty_estimate_guidance_frame()

    now = now_utc_naive()
    measure_code = pd.Series([_canonical_measure(measure, None) for measure in _string_series(raw, "measure_code")], index=raw.index, dtype="string")
    symbol = _string_series(raw, "symbol").map(lambda value: symbol_key(value) if not pd.isna(value) and str(value).strip() else pd.NA).astype("string")
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
    fiscal_year = _integer_series(raw, "fiscal_year")
    fiscal_period = _string_series(raw, "fiscal_period").str.upper()
    period_end = _date_series(raw, "period_end")
    derived_period_end = pd.Series([_period_end_from_fiscal_fields(fy, fp) for fy, fp in zip(fiscal_year, fiscal_period)], index=raw.index, dtype="object")
    period_end = period_end.where(pd.notna(period_end), derived_period_end)
    fiscal_year = fiscal_year.where(fiscal_year.notna(), period_end.map(lambda value: pd.NA if pd.isna(value) else value.year).astype("Int64"))
    fiscal_period = fiscal_period.where(fiscal_period.notna(), period_end.map(lambda value: pd.NA if pd.isna(value) else _quarter_label(value)).astype("string"))
    guidance_date = _date_series(raw, "guidance_date")
    filing_date = _date_series(raw, "filing_date")
    report_date = _date_series(raw, "report_date")
    guidance_date = guidance_date.where(pd.notna(guidance_date), filing_date)
    guidance_date = guidance_date.where(pd.notna(guidance_date), report_date)
    guidance_date = guidance_date.where(pd.notna(guidance_date), period_end)
    as_of_date = _date_series(raw, "as_of_date", fallback=guidance_date)
    as_of_date = as_of_date.where(pd.notna(as_of_date), period_end)
    available_at = _timestamp_series(raw, "available_at")
    acceptance_at = _timestamp_series(raw, "acceptance_datetime")
    available_at = available_at.where(available_at.notna(), acceptance_at)
    fallback_available_at = _timestamp_from_dates_end_of_day(guidance_date, now)
    if source_file is None:
        fallback_available_at = pd.Series([now] * len(raw), index=raw.index, dtype="datetime64[ns]")
    available_at = available_at.where(available_at.notna(), fallback_available_at)
    low = _numeric_series(raw, "low")
    high = _numeric_series(raw, "high")
    mid = _numeric_series(raw, "mid")
    both_bounds = low.notna() & high.notna() & mid.isna()
    mid = mid.where(~both_bounds, (low + high) / 2.0)
    source = _source_series(raw, options.source)

    normalized = pd.DataFrame(index=raw.index)
    normalized["security_id"] = security_id
    normalized["measure_code"] = measure_code
    normalized["fiscal_year"] = fiscal_year
    normalized["fiscal_period"] = fiscal_period
    normalized["period_end"] = period_end
    normalized["low"] = low
    normalized["high"] = high
    normalized["mid"] = mid
    normalized["guidance_type"] = raw.apply(_guidance_type, axis=1)
    normalized["basis"] = _string_series(raw, "basis").str.upper()
    normalized["currency"] = _string_series(raw, "currency").str.upper()
    normalized["unit"] = _string_series(raw, "unit").str.upper()
    normalized["units_scale"] = _integer_series(raw, "units_scale").fillna(1)
    normalized["source_item"] = _string_series(raw, "source_item")
    normalized["guidance_date"] = guidance_date
    normalized["form"] = _string_series(raw, "form").str.upper()
    normalized["accession_number"] = _string_series(raw, "accession_number")
    normalized["as_of_date"] = as_of_date
    normalized["available_at"] = available_at
    normalized["extraction_confidence"] = _numeric_series(raw, "extraction_confidence")
    normalized["evidence_text"] = _string_series(raw, "evidence_text")
    normalized["source_file"] = str(source_file) if source_file else pd.NA
    normalized["source_file_sha256"] = source_file_sha256
    normalized["raw_payload_json"] = _raw_payloads(raw)
    normalized["run_id"] = _string_series(raw, "run_id").where(_string_series(raw, "run_id").notna(), options.run_id)
    normalized["source"] = source

    has_value = normalized[["low", "high", "mid"]].notna().any(axis=1)
    confidence_ok = normalized["extraction_confidence"].isna() | (normalized["extraction_confidence"] >= float(options.min_confidence))
    normalized = normalized[
        normalized["security_id"].notna()
        & normalized["measure_code"].notna()
        & normalized["period_end"].notna()
        & has_value
        & confidence_ok
    ].copy()
    if normalized.empty:
        return _empty_estimate_guidance_frame()
    normalized["est_guidance_id"] = [
        _hash_id(
            "EST-GUIDANCE",
            row["source"],
            row["security_id"],
            row["measure_code"],
            row["period_end"],
            row["guidance_date"],
            row["accession_number"],
            row["low"],
            row["high"],
            row["mid"],
            str(row["evidence_text"])[:160] if not pd.isna(row["evidence_text"]) else "",
        )
        for _, row in normalized.iterrows()
    ]
    return normalized[ESTIMATE_GUIDANCE_COLUMNS].drop_duplicates(subset=["est_guidance_id"])


def _write_estimate_guidance_frame(
    store: DuckDBStore,
    frame: pd.DataFrame,
    *,
    options: EstimateGuidanceOptions,
    source_file_sha256: str | None = None,
) -> int:
    with store.transaction():
        if source_file_sha256 and options.replace_source_file:
            store.con.execute(
                """
                DELETE FROM est_guidance
                WHERE source = ?
                  AND source_file_sha256 = ?
                """,
                [options.source, source_file_sha256],
            )
        if frame.empty:
            return 0
        insert_frame(store, frame, "est_guidance", "est_guidance_insert")
    return int(len(frame))


class EstimateGuidanceDataset(Dataset):
    """Management guidance loader.

    Default-empty unless `source_file` or both `fetch` and `parse` are supplied.

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
        if options.min_confidence < 0 or options.min_confidence > 1:
            raise ValueError("min_confidence must be in [0, 1]")

        if options.source_file is not None:
            source_file = Path(options.source_file)
            frame = _read_guidance_source_file(source_file)
            source_hash = file_sha256(source_file)
            guidance = normalize_estimate_guidance_rows(
                frame,
                options=options,
                source_file_sha256=source_hash,
                source_file=source_file,
            )
            record_source_file(
                store,
                dataset_id=self.dataset_id,
                source_url=str(source_file),
                cache_path=source_file,
                sha256=source_hash,
                status="loaded",
                metadata={"source": options.source, "rows": len(frame), "parsed_rows": len(guidance)},
            )
            rows_loaded = _write_estimate_guidance_frame(
                store,
                guidance,
                options=options,
                source_file_sha256=source_hash,
            )
            return DatasetLoadResult(
                dataset_id=self.dataset_id,
                rows_loaded=rows_loaded,
                source=options.source,
                details={
                    "source_file": str(source_file),
                    "source_file_sha256": source_hash,
                    "parsed_rows": len(guidance),
                    "min_confidence": options.min_confidence,
                },
            )

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

        guidance = normalize_estimate_guidance_rows(
            pd.DataFrame(parsed_rows),
            options=options,
        )
        rows_loaded = _write_estimate_guidance_frame(store, guidance, options=options)

        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows_loaded,
            source=self.source_name,
            details={"parsed_rows": len(guidance)},
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


@dataclass(frozen=True)
class EstimateRecommendationSummaryOptions:
    provider: Callable[[], Iterable[dict]] | None = None
    source_file: Path | None = None
    source: str = "est_recommendation_summary_injected_v1"
    provider_name: str = "INJECTED"
    vendor_security_id_type: str = "IBES_TICKER"
    source_vendor_table: str | None = None
    rating_scale: str = "IBES_1_STRONG_BUY_5_SELL"
    scale_direction: str = "LOWER_IS_BULLISH"
    replace_source_file: bool = True
    run_id: str | None = None


def _empty_estimate_recommendation_frame() -> pd.DataFrame:
    return pd.DataFrame(columns=ESTIMATE_RECOMMENDATION_COLUMNS)


def _empty_estimate_recommendation_summary_frame() -> pd.DataFrame:
    return pd.DataFrame(columns=ESTIMATE_RECOMMENDATION_SUMMARY_COLUMNS)


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


def _normalize_recommendation_summary_columns(frame: pd.DataFrame) -> pd.DataFrame:
    renamed: dict[str, str] = {}
    for column in frame.columns:
        normalized = snake_case(str(column)).lower()
        compact = normalized.replace("_", "")
        renamed[column] = RECOMMENDATION_SUMMARY_COLUMN_ALIASES.get(
            normalized,
            RECOMMENDATION_SUMMARY_COLUMN_ALIASES.get(compact, normalized),
        )
    return frame.rename(columns=renamed)


def _scale_direction_values(
    raw: pd.DataFrame,
    rating_scale: pd.Series,
    options: EstimateRecommendationSummaryOptions,
) -> pd.Series:
    explicit = _string_series(raw, "scale_direction").str.upper()
    values: list[str] = []
    for direction, scale, provider in zip(explicit, rating_scale, _provider_name_series(raw, options.provider_name)):
        if not pd.isna(direction) and str(direction).strip():
            cleaned = str(direction).strip().upper()
            values.append("HIGHER_IS_BULLISH" if "HIGHER" in cleaned else "LOWER_IS_BULLISH")
            continue
        scale_text = "" if pd.isna(scale) else str(scale).upper()
        provider_text = "" if pd.isna(provider) else str(provider).upper()
        if "BLOOMBERG" in scale_text or "BEST" in scale_text or "BLOOMBERG" in provider_text:
            values.append("HIGHER_IS_BULLISH")
        else:
            default = str(options.scale_direction or "LOWER_IS_BULLISH").strip().upper()
            values.append("HIGHER_IS_BULLISH" if "HIGHER" in default else "LOWER_IS_BULLISH")
    return pd.Series(values, index=raw.index, dtype="string")


def _canonical_recommendation_mean(values: pd.Series, scale_direction: pd.Series) -> pd.Series:
    canonical = values.astype("Float64")
    higher_is_bullish = scale_direction.astype("string").str.upper().eq("HIGHER_IS_BULLISH")
    canonical = canonical.where(~higher_is_bullish | canonical.isna(), 6.0 - canonical)
    return canonical


def _count_series(frame: pd.DataFrame, column: str) -> pd.Series:
    values = _integer_series(frame, column)
    return values.where(values.notna(), pd.NA)


def _sum_count_series(*series: pd.Series) -> pd.Series:
    if not series:
        return pd.Series(dtype="Int64")
    counts = pd.concat(series, axis=1)
    has_any = counts.notna().any(axis=1)
    summed = counts.fillna(0).sum(axis=1).round().astype("Int64")
    return summed.where(has_any, pd.NA)


def _infer_recommendation_summary_source_table(
    raw_table: pd.Series,
    *,
    options: EstimateRecommendationSummaryOptions,
    has_recommendation: pd.Series,
    has_price_target: pd.Series,
) -> pd.Series:
    if options.source_vendor_table:
        fallback = pd.Series([options.source_vendor_table] * len(raw_table), index=raw_table.index, dtype="string")
    else:
        inferred = [
            "RECOMMENDATION_PRICE_TARGET_SUMMARY"
            if rec and target
            else ("PTGSUM" if target else ("RECDSUM" if rec else "RECOMMENDATION_SUMMARY"))
            for rec, target in zip(has_recommendation, has_price_target)
        ]
        fallback = pd.Series(inferred, index=raw_table.index, dtype="string")
    return raw_table.where(raw_table.notna(), fallback).str.upper()


def normalize_estimate_recommendation_summary_rows(
    frame: pd.DataFrame,
    *,
    options: EstimateRecommendationSummaryOptions,
    source_file_sha256: str | None = None,
    source_file: Path | None = None,
) -> pd.DataFrame:
    if frame.empty:
        return _empty_estimate_recommendation_summary_frame()

    raw = _normalize_recommendation_summary_columns(frame.copy())
    metric_columns = {
        "mean_recommendation",
        "median_recommendation",
        "strong_buy_count",
        "buy_count",
        "hold_count",
        "underperform_count",
        "sell_count",
        "buy_equivalent_count",
        "sell_equivalent_count",
        "total_recommendations",
        "mean_price_target",
        "median_price_target",
        "high_price_target",
        "low_price_target",
        "price_target_count",
    }
    if not any(column in raw.columns for column in metric_columns):
        raise ValueError("Recommendation summary rows require recommendation counts/ratings or price-target stats")

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

    snapshot_date = _date_series(raw, "snapshot_date", fallback=_date_series(raw, "as_of_date"))
    as_of_date = _date_series(raw, "as_of_date", fallback=snapshot_date)
    available_at = _timestamp_series(raw, "available_at")
    available_date = _date_series(raw, "available_date")
    available_at = available_at.where(
        available_at.notna(),
        _timestamp_from_dates_end_of_day(available_date, now).where(pd.notna(available_date), pd.NaT),
    )
    available_at = available_at.where(
        available_at.notna(),
        _timestamp_from_dates_end_of_day(snapshot_date, now).where(pd.notna(snapshot_date), pd.NaT),
    )

    rating_scale = _string_series(raw, "rating_scale").where(
        _string_series(raw, "rating_scale").notna(),
        options.rating_scale,
    )
    scale_direction = _scale_direction_values(raw, rating_scale, options)
    mean_recommendation = _canonical_recommendation_mean(_numeric_series(raw, "mean_recommendation"), scale_direction)
    median_recommendation = _canonical_recommendation_mean(_numeric_series(raw, "median_recommendation"), scale_direction)

    strong_buy_count = _count_series(raw, "strong_buy_count")
    buy_count = _count_series(raw, "buy_count")
    hold_count = _count_series(raw, "hold_count")
    underperform_count = _count_series(raw, "underperform_count")
    sell_count = _count_series(raw, "sell_count")
    buy_equivalent_count = _count_series(raw, "buy_equivalent_count")
    sell_equivalent_count = _count_series(raw, "sell_equivalent_count")
    buy_equivalent_count = buy_equivalent_count.where(
        buy_equivalent_count.notna(),
        _sum_count_series(strong_buy_count, buy_count),
    )
    sell_equivalent_count = sell_equivalent_count.where(
        sell_equivalent_count.notna(),
        _sum_count_series(underperform_count, sell_count),
    )
    total_recommendations = _count_series(raw, "total_recommendations")
    total_recommendations = total_recommendations.where(
        total_recommendations.notna(),
        _sum_count_series(strong_buy_count, buy_count, hold_count, underperform_count, sell_count),
    )
    total_recommendations = total_recommendations.where(
        total_recommendations.notna(),
        _sum_count_series(buy_equivalent_count, hold_count, sell_equivalent_count),
    )

    mean_price_target = _numeric_series(raw, "mean_price_target")
    median_price_target = _numeric_series(raw, "median_price_target")
    high_price_target = _numeric_series(raw, "high_price_target")
    low_price_target = _numeric_series(raw, "low_price_target")
    price_target_count = _count_series(raw, "price_target_count")
    target_horizon_months = _integer_series(raw, "target_horizon_months")
    has_price_target = (
        mean_price_target.notna()
        | median_price_target.notna()
        | high_price_target.notna()
        | low_price_target.notna()
        | price_target_count.notna()
    )
    target_horizon_months = target_horizon_months.where(
        target_horizon_months.notna() | ~has_price_target,
        12,
    )
    has_recommendation = (
        mean_recommendation.notna()
        | median_recommendation.notna()
        | total_recommendations.notna()
        | buy_equivalent_count.notna()
        | hold_count.notna()
        | sell_equivalent_count.notna()
    )
    source_vendor_table = _infer_recommendation_summary_source_table(
        _string_series(raw, "source_vendor_table"),
        options=options,
        has_recommendation=has_recommendation,
        has_price_target=has_price_target,
    )

    normalized = pd.DataFrame(index=raw.index)
    normalized["security_id"] = security_id
    normalized["symbol"] = symbol
    normalized["vendor_security_id"] = vendor_security_id
    normalized["vendor_security_id_type"] = vendor_security_id_type
    normalized["cusip"] = _string_series(raw, "cusip").str.upper()
    normalized["provider"] = provider
    normalized["source_vendor_table"] = source_vendor_table
    normalized["snapshot_date"] = snapshot_date
    normalized["as_of_date"] = as_of_date
    normalized["available_at"] = available_at
    normalized["mean_recommendation"] = mean_recommendation
    normalized["median_recommendation"] = median_recommendation
    normalized["rating_scale"] = rating_scale
    normalized["scale_direction"] = scale_direction
    normalized["strong_buy_count"] = strong_buy_count
    normalized["buy_count"] = buy_count
    normalized["hold_count"] = hold_count
    normalized["underperform_count"] = underperform_count
    normalized["sell_count"] = sell_count
    normalized["buy_equivalent_count"] = buy_equivalent_count
    normalized["sell_equivalent_count"] = sell_equivalent_count
    normalized["total_recommendations"] = total_recommendations
    normalized["mean_price_target"] = mean_price_target
    normalized["median_price_target"] = median_price_target
    normalized["high_price_target"] = high_price_target
    normalized["low_price_target"] = low_price_target
    normalized["price_target_count"] = price_target_count
    normalized["target_currency"] = _string_series(raw, "target_currency").str.upper()
    normalized["target_horizon_months"] = target_horizon_months
    normalized["provider_scale_notes"] = _string_series(raw, "provider_scale_notes")
    normalized["source_file"] = str(source_file) if source_file else pd.NA
    normalized["source_file_sha256"] = source_file_sha256
    normalized["raw_payload_json"] = _raw_payloads(raw)
    normalized["run_id"] = _string_series(raw, "run_id").where(_string_series(raw, "run_id").notna(), options.run_id)
    normalized["source"] = source

    normalized = normalized[
        normalized["snapshot_date"].notna()
        & normalized["available_at"].notna()
        & (
            normalized["security_id"].notna()
            | normalized["symbol"].notna()
            | normalized["vendor_security_id"].notna()
            | normalized["cusip"].notna()
        )
        & (
            normalized["mean_recommendation"].notna()
            | normalized["median_recommendation"].notna()
            | normalized["total_recommendations"].notna()
            | normalized["mean_price_target"].notna()
            | normalized["median_price_target"].notna()
            | normalized["price_target_count"].notna()
        )
    ].copy()
    if normalized.empty:
        return _empty_estimate_recommendation_summary_frame()

    normalized["est_recommendation_summary_id"] = normalized.apply(
        lambda row: _hash_id(
            "EST-REC-SUMMARY",
            row.get("source"),
            row.get("provider"),
            row.get("security_id"),
            row.get("symbol"),
            row.get("vendor_security_id_type"),
            row.get("vendor_security_id"),
            row.get("source_vendor_table"),
            row.get("snapshot_date"),
            row.get("rating_scale"),
            row.get("source_file_sha256"),
        ),
        axis=1,
    )
    normalized = normalized.drop_duplicates(subset=["est_recommendation_summary_id"])
    return normalized[ESTIMATE_RECOMMENDATION_SUMMARY_COLUMNS]


def _write_estimate_recommendation_summary_frame(
    store: DuckDBStore,
    summary: pd.DataFrame,
    *,
    options: EstimateRecommendationSummaryOptions,
    source_file_sha256: str | None = None,
) -> int:
    if summary.empty:
        return 0
    with store.transaction():
        if source_file_sha256 and options.replace_source_file:
            store.con.execute(
                """
                DELETE FROM est_recommendation_summary
                WHERE source = ?
                  AND source_file_sha256 = ?
                """,
                [options.source, source_file_sha256],
            )
        insert_frame(store, summary, "est_recommendation_summary", "est_recommendation_summary_insert")
    return int(len(summary))


def load_estimate_recommendation_summary_rows(
    store: DuckDBStore,
    options: EstimateRecommendationSummaryOptions,
) -> int:
    store.initialize()
    if options.source_file is None:
        return 0
    source_file = Path(options.source_file)
    frame = pd.read_csv(source_file, dtype=str, keep_default_na=False)
    source_hash = file_sha256(source_file)
    summary = normalize_estimate_recommendation_summary_rows(
        frame,
        options=options,
        source_file_sha256=source_hash,
        source_file=source_file,
    )
    record_source_file(
        store,
        dataset_id="est_recommendation_summary",
        source_url=str(source_file),
        cache_path=source_file,
        sha256=source_hash,
        metadata={"provider": options.provider_name, "rows": int(len(frame))},
    )
    return _write_estimate_recommendation_summary_frame(
        store,
        summary,
        options=options,
        source_file_sha256=source_hash,
    )


class EstimateRecommendationSummaryDataset(Dataset):
    """Aggregate recommendation and price-target summary loader."""

    dataset_id = "est_recommendation_summary"
    source_name = "est_recommendation_summary_injectable"

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(
        self, store: DuckDBStore, options: EstimateRecommendationSummaryOptions
    ) -> DatasetLoadResult:
        if options.source_file is not None:
            rows_loaded = load_estimate_recommendation_summary_rows(store, options)
            if rows_loaded:
                quality_check(
                    store,
                    dataset_id=self.dataset_id,
                    table_name="est_recommendation_summary",
                    check_name="est_recommendation_summary_loaded",
                    status="passed",
                    observed_value=float(rows_loaded),
                    threshold_value=0.0,
                    details={"provider": options.provider_name, "source_file": str(options.source_file)},
                )
            return DatasetLoadResult(
                dataset_id=self.dataset_id,
                rows_loaded=rows_loaded,
                source=self.source_name,
                details={"provider": options.provider_name, "source_file": str(options.source_file)},
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

        frame = pd.DataFrame(rows)
        if "available_at" not in frame.columns:
            frame["available_at"] = now_utc_naive()
        else:
            frame["available_at"] = frame["available_at"].fillna(now_utc_naive())
        if "source" not in frame.columns:
            frame["source"] = self.source_name
        summary = normalize_estimate_recommendation_summary_rows(frame, options=options)
        rows_loaded = _write_estimate_recommendation_summary_frame(store, summary, options=options)

        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows_loaded,
            source=self.source_name,
            details={"provider": options.provider_name},
        )
