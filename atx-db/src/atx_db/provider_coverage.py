"""Provider-facing schema coverage snapshots and institutional target SLOs."""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import uuid
from dataclasses import dataclass

from .api.catalog import DATASETS, RecordSchema
from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult

DATABENTO_METADATA_URL = (
    "https://databento.com/docs/api-reference-historical/basics/authentication?historical=http&live=http"
)
SEC_FSDS_URL = "https://www.sec.gov/file/financial-statement-data-sets"
SLO_VERSION = "1.0.0"


@dataclass(frozen=True)
class ProviderCoverageSlo:
    dataset_id: str
    schema_code: str
    expected_history_start: dt.date
    minimum_history_years: float
    minimum_security_count: int
    minimum_item_count: int | None
    maximum_freshness_lag_days: float
    citation: str
    description: str
    slo_version: str = SLO_VERSION


@dataclass(frozen=True)
class ProviderCoverageOptions:
    dataset_ids: tuple[str, ...] | None = None
    observed_at: dt.datetime | None = None
    run_id: str | None = None


@dataclass(frozen=True)
class ProviderCoverageSnapshot:
    coverage_snapshot_id: str
    dataset_id: str
    schema_code: str
    condition: str
    record_count: int
    security_count: int
    item_count: int | None
    basis_count: int | None
    start: dt.datetime | None
    end: dt.datetime | None
    failed_slos: tuple[dict[str, object], ...]
    observed_at: dt.datetime
    run_id: str


_FUNDAMENTAL_CITATION = f"{DATABENTO_METADATA_URL}; {SEC_FSDS_URL}"

DEFAULT_PROVIDER_COVERAGE_SLOS: tuple[ProviderCoverageSlo, ...] = (
    ProviderCoverageSlo(
        "ATX.US.FUNDAMENTALS",
        "reported",
        dt.date(2009, 1, 1),
        15.0,
        2_500,
        90,
        120.0,
        _FUNDAMENTAL_CITATION,
        "Institutional target for as-filed canonical SEC statement coverage.",
    ),
    ProviderCoverageSlo(
        "ATX.US.FUNDAMENTALS",
        "standardized",
        dt.date(2009, 1, 1),
        15.0,
        2_500,
        200,
        120.0,
        _FUNDAMENTAL_CITATION,
        "Institutional target for comparable standardized fundamentals.",
    ),
    ProviderCoverageSlo(
        "ATX.US.FUNDAMENTALS",
        "industry-standardized",
        dt.date(2009, 1, 1),
        15.0,
        2_500,
        200,
        120.0,
        _FUNDAMENTAL_CITATION,
        "Institutional target for dynamically routed industry statements.",
    ),
    ProviderCoverageSlo(
        "ATX.US.FUNDAMENTALS",
        "reconciliation",
        dt.date(2009, 1, 1),
        15.0,
        2_500,
        15,
        120.0,
        _FUNDAMENTAL_CITATION,
        "Institutional target for customer-visible accounting reconciliation.",
    ),
    ProviderCoverageSlo(
        "ATX.US.FUNDAMENTALS",
        "ratios",
        dt.date(2009, 1, 1),
        15.0,
        2_500,
        50,
        120.0,
        _FUNDAMENTAL_CITATION,
        "Institutional target for formula-governed ratios and scores.",
    ),
    ProviderCoverageSlo(
        "ATX.US.EQUITIES",
        "ohlcv-1d",
        dt.date(2010, 1, 1),
        10.0,
        5_000,
        None,
        7.0,
        DATABENTO_METADATA_URL,
        "Institutional target for survivorship-aware daily US equity observations.",
    ),
)


def _quote_identifier(value: str) -> str:
    if not value.replace("_", "").isalnum() or value[0].isdigit():
        raise ValueError(f"unsafe catalog identifier: {value!r}")
    return f'"{value}"'


def _relation_exists(store: DuckDBStore, relation: str) -> bool:
    row = store.con.execute(
        """
        SELECT count(*)
        FROM information_schema.tables
        WHERE table_schema='main' AND table_name=?
        """,
        [relation],
    ).fetchone()
    return row is not None and bool(row[0])


def _schema_stats(
    store: DuckDBStore,
    schema: RecordSchema,
) -> tuple[int, int, int | None, int | None, dt.datetime | None, dt.datetime | None,
           dt.datetime | None, dt.datetime | None]:
    table = _quote_identifier(schema.source_table)
    time_column = _quote_identifier(schema.time_column)
    item_expression = (
        f"count(DISTINCT {_quote_identifier(schema.item_column)})::BIGINT"
        if schema.item_column is not None
        else "NULL::BIGINT"
    )
    basis_expression = (
        f"count(DISTINCT {_quote_identifier(schema.basis_column)})::BIGINT"
        if schema.basis_column is not None
        else "NULL::BIGINT"
    )
    row = store.con.execute(
        f"""
        SELECT
            count(*)::BIGINT,
            count(DISTINCT security_id)::BIGINT,
            {item_expression},
            {basis_expression},
            min(CAST({time_column} AS TIMESTAMP)),
            max(CAST({time_column} AS TIMESTAMP)) + INTERVAL 1 DAY,
            min(coalesce(available_at,source_loaded_at)),
            max(coalesce(available_at,source_loaded_at))
        FROM {table}
        """
    ).fetchone()
    if row is None:
        raise RuntimeError(f"could not measure public schema {schema.dataset}/{schema.code}")
    return (
        int(row[0]),
        int(row[1]),
        None if row[2] is None else int(row[2]),
        None if row[3] is None else int(row[3]),
        row[4],
        row[5],
        row[6],
        row[7],
    )


def _active_slo(store: DuckDBStore, dataset_id: str, schema_code: str) -> ProviderCoverageSlo:
    row = store.con.execute(
        """
        SELECT
            dataset_id,schema_code,expected_history_start,minimum_history_years,
            minimum_security_count,minimum_item_count,maximum_freshness_lag_days,
            citation,description,slo_version
        FROM api_schema_coverage_slo
        WHERE dataset_id=? AND schema_code=? AND is_active
        ORDER BY valid_from DESC,slo_version DESC
        LIMIT 1
        """,
        [dataset_id, schema_code],
    ).fetchone()
    if row is None:
        raise RuntimeError(f"no active coverage SLO for {dataset_id}/{schema_code}")
    return ProviderCoverageSlo(
        dataset_id=str(row[0]),
        schema_code=str(row[1]),
        expected_history_start=row[2],
        minimum_history_years=float(row[3]),
        minimum_security_count=int(row[4]),
        minimum_item_count=None if row[5] is None else int(row[5]),
        maximum_freshness_lag_days=float(row[6]),
        citation=str(row[7]),
        description=str(row[8]),
        slo_version=str(row[9]),
    )


def _evaluate_slos(
    slo: ProviderCoverageSlo,
    *,
    record_count: int,
    security_count: int,
    item_count: int | None,
    start: dt.datetime | None,
    end: dt.datetime | None,
    freshness_lag_days: float | None,
) -> tuple[dict[str, object], ...]:
    failures: list[dict[str, object]] = []

    def fail(metric: str, observed: object, threshold: object, comparator: str) -> None:
        failures.append(
            {
                "metric": metric,
                "observed": observed,
                "threshold": threshold,
                "comparator": comparator,
            }
        )

    if record_count == 0:
        return ()
    if start is None or start.date() > slo.expected_history_start:
        fail("history_start", None if start is None else start.date(), slo.expected_history_start, "le")
    history_years = None if start is None or end is None else (end - start).days / 365.25
    if history_years is None or history_years < slo.minimum_history_years:
        fail("history_years", history_years, slo.minimum_history_years, "ge")
    if security_count < slo.minimum_security_count:
        fail("security_count", security_count, slo.minimum_security_count, "ge")
    if slo.minimum_item_count is not None and (
        item_count is None or item_count < slo.minimum_item_count
    ):
        fail("item_count", item_count, slo.minimum_item_count, "ge")
    if freshness_lag_days is None or freshness_lag_days > slo.maximum_freshness_lag_days:
        fail("freshness_lag_days", freshness_lag_days, slo.maximum_freshness_lag_days, "le")
    return tuple(failures)


def refresh_provider_coverage(
    store: DuckDBStore,
    options: ProviderCoverageOptions | None = None,
) -> tuple[ProviderCoverageSnapshot, ...]:
    """Append one measured availability/SLO snapshot per selected public schema."""

    options = options or ProviderCoverageOptions()
    observed_at = options.observed_at or dt.datetime.now(dt.UTC).replace(tzinfo=None)
    if observed_at.tzinfo is not None:
        observed_at = observed_at.astimezone(dt.UTC).replace(tzinfo=None)
    run_id = options.run_id or f"provider-coverage-{uuid.uuid4()}"
    selected = None if options.dataset_ids is None else set(options.dataset_ids)
    known_datasets = {dataset.code for dataset in DATASETS}
    if selected is not None:
        unknown = sorted(selected - known_datasets)
        if unknown:
            raise ValueError(f"unknown dataset IDs: {', '.join(unknown)}")

    snapshots: list[ProviderCoverageSnapshot] = []
    rows: list[tuple[object, ...]] = []
    for dataset in DATASETS:
        if selected is not None and dataset.code not in selected:
            continue
        for schema in dataset.schemas:
            slo = _active_slo(store, dataset.code, schema.code)
            relation_exists = _relation_exists(store, schema.source_table)
            if relation_exists:
                (
                    record_count,
                    security_count,
                    item_count,
                    basis_count,
                    start,
                    end,
                    first_available_at,
                    last_available_at,
                ) = _schema_stats(store, schema)
            else:
                record_count = security_count = 0
                item_count = basis_count = None
                start = end = first_available_at = last_available_at = None
            history_years = None if start is None or end is None else (end - start).days / 365.25
            freshness_lag_days = (
                None
                if last_available_at is None
                else max(0.0, (observed_at - last_available_at).total_seconds() / 86_400.0)
            )
            failed_slos = _evaluate_slos(
                slo,
                record_count=record_count,
                security_count=security_count,
                item_count=item_count,
                start=start,
                end=end,
                freshness_lag_days=freshness_lag_days,
            )
            condition = (
                "missing"
                if not relation_exists
                else "pending"
                if record_count == 0
                else "degraded"
                if failed_slos
                else "available"
            )
            natural = f"{dataset.code}|{schema.code}|{observed_at.isoformat()}"
            snapshot_id = hashlib.sha256(natural.encode()).hexdigest()
            snapshot = ProviderCoverageSnapshot(
                coverage_snapshot_id=snapshot_id,
                dataset_id=dataset.code,
                schema_code=schema.code,
                condition=condition,
                record_count=record_count,
                security_count=security_count,
                item_count=item_count,
                basis_count=basis_count,
                start=start,
                end=end,
                failed_slos=failed_slos,
                observed_at=observed_at,
                run_id=run_id,
            )
            snapshots.append(snapshot)
            rows.append(
                (
                    snapshot_id,
                    dataset.code,
                    schema.code,
                    schema.version,
                    schema.source_table,
                    schema.time_column,
                    observed_at,
                    start,
                    end,
                    first_available_at,
                    last_available_at,
                    record_count,
                    security_count,
                    item_count,
                    basis_count,
                    history_years,
                    freshness_lag_days,
                    condition,
                    json.dumps(failed_slos, default=str, sort_keys=True, separators=(",", ":")),
                    slo.slo_version,
                    run_id,
                )
            )

    with store.transaction():
        store.con.executemany(
            """
            INSERT OR REPLACE INTO api_schema_coverage_snapshot (
                coverage_snapshot_id,dataset_id,schema_code,schema_version,source_relation,
                time_column,observed_at,start_time,end_time,first_available_at,
                last_available_at,record_count,security_count,item_count,basis_count,
                history_years,freshness_lag_days,condition,failed_slos_json,slo_version,
                run_id,source_loaded_at
            ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,now())
            """,
            rows,
        )
    return tuple(snapshots)


class ProviderCoverageDataset(Dataset):
    dataset_id = "provider_schema_coverage"
    source_name = "provider_schema_coverage_v1"
    depends_on = (
        "fundamental_standardized",
        "fundamental_ratios",
        "tbltickerhistory_daily",
    )

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(
        self,
        store: DuckDBStore,
        options: ProviderCoverageOptions,
    ) -> DatasetLoadResult:
        snapshots = refresh_provider_coverage(store, options)
        conditions = {
            condition: sum(snapshot.condition == condition for snapshot in snapshots)
            for condition in ("available", "degraded", "pending", "missing")
        }
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=len(snapshots),
            source=self.source_name,
            details={
                "conditions": conditions,
                "schemas": [f"{row.dataset_id}/{row.schema_code}" for row in snapshots],
            },
            run_id=options.run_id,
        )
