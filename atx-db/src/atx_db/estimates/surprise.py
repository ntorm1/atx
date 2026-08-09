from __future__ import annotations

from ._columns import *
from ._common import *

@dataclass(frozen=True)
class EstimateSurpriseOptions:
    measure_codes: tuple[str, ...] | None = None
    window: int = 8
    min_obs: int = 4
    model: str = "srw_drift"
    run_id: str | None = None


def _normalize_basis_tag(value: Any, *, default: str | None = None) -> str | None:
    if value is None or pd.isna(value) or not str(value).strip():
        return default
    normalized = str(value).strip().upper().replace("-", "_").replace(" ", "_")
    if normalized in {"NONGAAP", "NON_GAAP", "ADJUSTED"}:
        return "NON_GAAP"
    return normalized


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
                "actual_basis": _normalize_basis_tag(row.get("basis"), default="GAAP"),
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
            "actual_basis": _normalize_basis_tag(row.get("basis"), default="GAAP"),
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
                coalesce(nullif(basis, ''), 'GAAP') AS basis,
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
               period_end, actual, basis, available_at
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
                       available_at AS consensus_available_at,
                       basis AS consensus_basis
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
                    r["consensus_basis"] = None
                    r["basis_mismatch"] = False
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
        out_df["actual_basis"] = out_df["actual_basis"].map(lambda value: _normalize_basis_tag(value, default="GAAP"))

        # Enrich with consensus if available
        if has_consensus and not consensus_df.empty:
            out_df["_surprise_row_id"] = range(len(out_df))
            merged = out_df.merge(
                consensus_df,
                on=["security_id", "measure_code", "period_end"],
                how="left",
            )
            visible = (
                merged["consensus_available_at"].notna()
                & merged["available_at"].notna()
                & (merged["consensus_available_at"] <= merged["available_at"])
            )
            merged["_visible_consensus"] = visible
            merged["_actual_basis_norm"] = merged["actual_basis"].map(
                lambda value: _normalize_basis_tag(value, default="GAAP")
            )
            merged["_consensus_basis_norm"] = [
                _normalize_basis_tag(consensus_basis, default=actual_basis)
                for consensus_basis, actual_basis in zip(
                    merged["consensus_basis_y"],
                    merged["_actual_basis_norm"],
                )
            ]
            merged["_basis_match"] = (
                merged["_visible_consensus"]
                & merged["_actual_basis_norm"].notna()
                & merged["_consensus_basis_norm"].notna()
                & (merged["_actual_basis_norm"] == merged["_consensus_basis_norm"])
            )
            merged = merged.sort_values(
                by=[
                    "_surprise_row_id",
                    "_visible_consensus",
                    "_basis_match",
                    "consensus_available_at",
                ],
                ascending=[True, False, False, False],
                na_position="last",
            )
            best = merged.drop_duplicates(subset=["_surprise_row_id"], keep="first").copy()
            best["consensus_mean"] = best["consensus_mean_x"]
            best.loc[best["_visible_consensus"], "consensus_mean"] = best.loc[
                best["_visible_consensus"], "consensus_mean_y"
            ]
            best["actual_basis"] = best["_actual_basis_norm"]
            best["consensus_basis"] = None
            best.loc[best["_visible_consensus"], "consensus_basis"] = best.loc[
                best["_visible_consensus"], "_consensus_basis_norm"
            ]
            best["basis_mismatch"] = (
                best["_visible_consensus"]
                & best["actual_basis"].notna()
                & best["consensus_basis"].notna()
                & (best["actual_basis"] != best["consensus_basis"])
            )
            valid_pct = (
                best["_visible_consensus"]
                & ~best["basis_mismatch"]
                & best["consensus_mean"].notna()
                & (best["consensus_mean"] != 0)
            )
            best.loc[valid_pct, "surprise_pct"] = (
                (best.loc[valid_pct, "actual"] - best.loc[valid_pct, "consensus_mean"])
                / best.loc[valid_pct, "consensus_mean"].abs()
            )
            best.loc[best["basis_mismatch"], "surprise_pct"] = None
            cols = [
                "security_id", "measure_code", "fiscal_year", "fiscal_period",
                "period_end", "actual", "expected", "surprise", "sue",
                "consensus_mean", "surprise_pct", "actual_basis",
                "consensus_basis", "basis_mismatch", "model",
                "as_of_date", "available_at", "run_id", "source",
            ]
            out_df = best[cols].copy()
        else:
            out_df = out_df[[
                "security_id", "measure_code", "fiscal_year", "fiscal_period",
                "period_end", "actual", "expected", "surprise", "sue",
                "consensus_mean", "surprise_pct", "actual_basis",
                "consensus_basis", "basis_mismatch", "model",
                "as_of_date", "available_at", "run_id", "source",
            ]]

        store.con.register("_est_surprise_batch", out_df)
        try:
            store.con.execute(
                """
                INSERT OR REPLACE INTO est_surprise (
                    security_id, measure_code, fiscal_year, fiscal_period,
                    period_end, actual, expected, surprise, sue,
                    consensus_mean, surprise_pct, actual_basis,
                    consensus_basis, basis_mismatch, model,
                    as_of_date, available_at, run_id, source
                )
                SELECT
                    security_id, measure_code, fiscal_year, fiscal_period,
                    period_end, actual, expected, surprise, sue,
                    consensus_mean, surprise_pct, actual_basis,
                    consensus_basis, basis_mismatch, model,
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
