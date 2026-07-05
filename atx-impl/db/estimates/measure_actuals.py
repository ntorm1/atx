from __future__ import annotations

from ._columns import *
from ._common import *

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
                'GAAP'          AS basis,
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
                    period_end, value, unit, basis, form, accession_number,
                    announce_date, as_of_date, available_at, run_id, source
                )
                SELECT
                    security_id, measure_code, fiscal_year, fiscal_period,
                    period_end, value, unit, basis, form, accession_number,
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
