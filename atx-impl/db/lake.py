from __future__ import annotations

import datetime as dt
import hashlib
import json
import re
import shutil
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .connection import DEFAULT_DB_PATH, connect
from .warehouse import file_sha256, json_dumps, now_utc_naive


DEFAULT_LAKE_ROOT = Path(__file__).resolve().parent / "lake"
DEFAULT_EXPORT_OBJECTS = (
    "v_security_master_current",
    "nasdaq_symbol_directory",
    "nasdaq_listing_events",
    "listing_status_intervals",
    "delist_code_dim",
    "delisting_events",
    "delisting_return_observations",
    "equity_daily_bars",
    "corporate_actions",
    "corp_action_type_dim",
    "adjustment_factor_history",
    "daily_adjustment_factors",
    "shares_outstanding_history",
    "v_alpha_daily_panel",
    "fundamental_points",
    "xbrl_concept_catalog",
    "xbrl_taxonomy_packages",
    "xbrl_taxonomy_roles",
    "xbrl_taxonomy_relationships",
    "xbrl_dimension_edges",
    "xbrl_fact_frames",
    "xbrl_filing_contexts",
    "xbrl_filing_dimensions",
    "xbrl_filing_facts",
    "xbrl_validation_results",
    "fundamental_fact_revisions",
    "fundamental_statement_map",
    "fundamental_statement_points",
    "fundamental_ttm_points",
    "fundamental_periods",
    "fundamental_ratios",
    "fundamental_xbrl_metric",
    "short_interest_metrics",
    "macro_metrics",
    "equity_price_metrics",
    "thirteenf_position_metrics",
    "thirteenf_option_metrics",
    "thirteenf_concentration_metrics",
    "corporate_action_dividend_metrics",
    "corporate_action_split_metrics",
    "corporate_action_factor_reconciliation",
    "feature_build_manifests",
    "feature_definitions",
    "feature_set_catalog",
    "feature_dependency_edges",
    "feature_values",
    "alpha_expression_catalog",
    "alpha_signal_values",
    "alpha_backtest_manifests",
    "finra_short_interest",
    "finra_short_interest_backfill_manifests",
    "finra_short_volume",
    "short_volume_metrics",
    "offexchange_venue",
    "offexchange_volume",
    "offexchange_security_period",
    "offexchange_quality_report",
    "thirteenf_managers",
    "thirteenf_manager_reports",
    "thirteenf_security_positions",
    "thirteenf_security_ownership",
    "filer_13f_cik_alias",
    "v_thirteenf_positioning_by_security",
    "insider",
    "filing_form4",
    "insider_relationship",
    "insider_transaction",
    "insider_transaction_metrics",
    "security_listing_metrics",
    "insider_holding",
    "tradingplan_10b5_1",
    "blockholder_filing",
    "blockholder_reporting_person",
    "fund",
    "fund_class",
    "filing_nport",
    "fund_holding",
    "form144_intent",
    "form144_to_form4_link",
    "proxy_vote",
    "congressional_disclosure",
    "identifier_resolution_candidates",
    "identifier_resolution_decisions",
    "sec_submissions",
    "macro_series",
    "macro_observations",
    "universe_memberships",
    "provider_parity_matrix",
    "taxonomy",
    "taxonomy_node",
    "entity_classification",
    "taxonomy_mapping",
    "est_measure",
    "est_broker",
    "est_broker_alias",
    "est_analyst",
    "est_analyst_alias",
    "est_period_dim",
    "est_detail",
    "est_consensus",
    "est_actual",
    "est_surprise",
    "est_guidance",
    "est_recommendation",
    "est_recommendation_summary",
    "est_security_link",
    "v_factor_panel",
)


@dataclass(frozen=True)
class LakeExportResult:
    export_run_id: str
    object_name: str
    output_path: Path
    manifest_path: Path
    rows: int
    byte_count: int
    sha256: str


@dataclass(frozen=True)
class LakeValidationProblem:
    export_run_id: str
    object_name: str | None
    problem: str
    message: str
    path: str | None = None
    expected: Any | None = None
    actual: Any | None = None


@dataclass(frozen=True)
class LakeValidationSummary:
    export_run_id: str
    files_checked: int
    files_readable: int
    rows_checked: int
    byte_count_checked: int
    problems: list[LakeValidationProblem]


@dataclass(frozen=True)
class LakePartitionSpec:
    object_name: str
    partition_columns: tuple[str, ...]
    watermark_column: str | None
    retention_runs: int
    enabled: bool


@dataclass(frozen=True)
class LakeExportContract:
    object_name: str
    expected_schema_sha256: str


_OBJECT_NAME_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def _quote_object_name(object_name: str) -> str:
    if not _OBJECT_NAME_RE.fullmatch(object_name):
        raise ValueError(f"Unsupported export object name: {object_name!r}")
    return f'"{object_name}"'


def _schema_sha256(schema: list[dict[str, object]]) -> str:
    payload = json.dumps(schema, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def _object_schema(store, object_name: str) -> list[dict[str, object]]:
    rows = store.con.execute(
        """
        SELECT column_name, data_type, is_nullable, column_index
        FROM duckdb_columns()
        WHERE schema_name = 'main'
          AND table_name = ?
        ORDER BY column_index
        """,
        [object_name],
    ).fetchall()
    if not rows:
        raise ValueError(f"Export object not found in DuckDB catalog: {object_name}")
    return [
        {
            "ordinal": int(column_index),
            "name": str(column_name),
            "type": str(data_type),
            "nullable": bool(is_nullable),
        }
        for column_name, data_type, is_nullable, column_index in rows
    ]


def _table_exists(store, table_name: str) -> bool:
    row = store.con.execute(
        """
        SELECT count(*)
        FROM duckdb_tables()
        WHERE schema_name = 'main'
          AND table_name = ?
        """,
        [table_name],
    ).fetchone()
    return bool(row and row[0])


def _load_partition_specs(store, objects: tuple[str, ...]) -> dict[str, LakePartitionSpec]:
    if not objects or not _table_exists(store, "lake_partition_specs"):
        return {}
    placeholders = ", ".join("?" for _ in objects)
    rows = store.con.execute(
        f"""
        SELECT object_name, partition_columns_json, watermark_column, retention_runs, enabled
        FROM lake_partition_specs
        WHERE object_name IN ({placeholders})
        """,
        list(objects),
    ).fetchall()
    specs: dict[str, LakePartitionSpec] = {}
    for object_name, columns_json, watermark_column, retention_runs, enabled in rows:
        try:
            columns = tuple(str(value) for value in json.loads(str(columns_json)))
        except Exception:
            columns = ()
        specs[str(object_name)] = LakePartitionSpec(
            object_name=str(object_name),
            partition_columns=columns,
            watermark_column=None if watermark_column in (None, "") else str(watermark_column),
            retention_runs=max(0, int(retention_runs or 0)),
            enabled=bool(enabled),
        )
    return specs


def _load_export_contracts(store, objects: tuple[str, ...]) -> dict[str, LakeExportContract]:
    if not objects or not _table_exists(store, "lake_export_object_contract"):
        return {}
    placeholders = ", ".join("?" for _ in objects)
    rows = store.con.execute(
        f"""
        SELECT object_name, expected_schema_sha256
        FROM lake_export_object_contract
        WHERE object_name IN ({placeholders})
          AND enabled
        """,
        list(objects),
    ).fetchall()
    return {
        str(object_name): LakeExportContract(
            object_name=str(object_name),
            expected_schema_sha256=str(expected_schema_sha256),
        )
        for object_name, expected_schema_sha256 in rows
    }


def _assert_expected_schema(
    object_name: str,
    schema_sha256: str,
    contract: LakeExportContract | None,
) -> str | None:
    if contract is None:
        return None
    if schema_sha256 != contract.expected_schema_sha256:
        raise ValueError(
            f"Schema SHA-256 mismatch for {object_name}: "
            f"expected {contract.expected_schema_sha256}, got {schema_sha256}"
        )
    return contract.expected_schema_sha256


def _sql_literal(value: Any) -> str:
    if value is None:
        return "NULL"
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float)):
        return str(value)
    if isinstance(value, (dt.date, dt.datetime)):
        text = value.isoformat()
    else:
        text = str(value)
    return "'" + text.replace("'", "''") + "'"


def _partition_key(columns: tuple[str, ...], values: tuple[Any, ...]) -> str:
    return json.dumps(
        {column: None if value is None else str(value) for column, value in zip(columns, values, strict=True)},
        sort_keys=True,
        separators=(",", ":"),
    )


def _safe_partition_value(value: Any) -> str:
    if value is None:
        return "__HIVE_DEFAULT_PARTITION__"
    text = str(value)
    return re.sub(r"[^0-9A-Za-z_.=-]+", "_", text).strip("_") or "blank"


def _partition_dir(root: Path, columns: tuple[str, ...], values: tuple[Any, ...]) -> Path:
    path = root
    for column, value in zip(columns, values, strict=True):
        path = path / f"{column}={_safe_partition_value(value)}"
    return path


def _partition_predicate(columns: tuple[str, ...], values: tuple[Any, ...]) -> str:
    return " AND ".join(
        f"{_quote_object_name(column)} IS NOT DISTINCT FROM {_sql_literal(value)}"
        for column, value in zip(columns, values, strict=True)
    )


def _directory_sha256(files: list[dict[str, Any]]) -> str:
    digest = hashlib.sha256()
    for item in sorted(files, key=lambda value: str(value["path"])):
        digest.update(str(item["path"]).encode("utf-8"))
        digest.update(b"\0")
        digest.update(str(item["sha256"]).encode("ascii"))
        digest.update(b"\0")
    return digest.hexdigest()


def _safe_prune_run_dirs(lake_root: Path, keep_run_ids: set[str]) -> None:
    root = lake_root.resolve()
    for child in root.iterdir():
        if not child.is_dir():
            continue
        if child.name in keep_run_ids:
            continue
        try:
            uuid.UUID(child.name)
        except ValueError:
            continue
        resolved = child.resolve()
        if root not in resolved.parents:
            continue
        shutil.rmtree(resolved)


class LakehouseExporter:
    def __init__(self, db_path: Path | str = DEFAULT_DB_PATH, lake_root: Path | str = DEFAULT_LAKE_ROOT) -> None:
        self.db_path = Path(db_path)
        self.lake_root = Path(lake_root)

    def export_objects(
        self,
        objects: tuple[str, ...] = DEFAULT_EXPORT_OBJECTS,
        *,
        incremental: bool = False,
        retain_runs: int | None = None,
    ) -> list[LakeExportResult]:
        objects = tuple(objects)
        self.lake_root.mkdir(parents=True, exist_ok=True)
        results: list[LakeExportResult] = []
        export_run_id = str(uuid.uuid4())
        started_at = now_utc_naive()
        exported_at = dt.datetime.now(dt.timezone.utc).replace(microsecond=0)
        exported_at_naive = exported_at.replace(tzinfo=None)
        with connect(self.db_path, read_only=False) as store:
            store.initialize()
            store.con.execute(
                """
                INSERT INTO lake_export_runs (
                    export_run_id,
                    lake_root,
                    object_count,
                    total_rows,
                    total_byte_count,
                    started_at,
                    status,
                    params_json
                )
                VALUES (?, ?, ?, 0, 0, ?, 'running', ?)
                """,
                [
                    export_run_id,
                    str(self.lake_root.resolve()),
                    len(objects),
                    started_at,
                    json_dumps(
                        {
                            "objects": list(objects),
                            "format": "parquet",
                            "compression": "zstd",
                            "incremental": incremental,
                        }
                    ),
                ],
            )
            try:
                partition_specs = _load_partition_specs(store, objects)
                export_contracts = _load_export_contracts(store, objects)
                for object_name in objects:
                    spec = partition_specs.get(object_name)
                    contract = export_contracts.get(object_name)
                    if spec is not None and spec.enabled and spec.partition_columns:
                        result = self._export_partitioned_object(
                            store,
                            object_name=object_name,
                            spec=spec,
                            contract=contract,
                            export_run_id=export_run_id,
                            exported_at=exported_at,
                            exported_at_naive=exported_at_naive,
                            incremental=incremental,
                        )
                    else:
                        result = self._export_single_file_object(
                            store,
                            object_name=object_name,
                            contract=contract,
                            export_run_id=export_run_id,
                            exported_at=exported_at,
                            exported_at_naive=exported_at_naive,
                        )
                    results.append(result)
                keep = retain_runs
                if keep is None:
                    spec_retention = [
                        spec.retention_runs
                        for spec in partition_specs.values()
                        if spec.enabled and spec.partition_columns and spec.retention_runs > 0
                    ]
                    keep = max(spec_retention) if spec_retention else None
                if keep:
                    keep_rows = store.con.execute(
                        """
                        SELECT export_run_id
                        FROM lake_export_runs
                        WHERE status = 'succeeded'
                        ORDER BY coalesce(finished_at, started_at) DESC
                        LIMIT ?
                        """,
                        [int(keep)],
                    ).fetchall()
                    _safe_prune_run_dirs(
                        self.lake_root,
                        {export_run_id, *(str(row[0]) for row in keep_rows)},
                    )
                store.con.execute(
                    """
                    UPDATE lake_export_runs
                    SET status = 'succeeded',
                        finished_at = ?,
                        total_rows = ?,
                        total_byte_count = ?
                    WHERE export_run_id = ?
                    """,
                    [
                        now_utc_naive(),
                        sum(result.rows for result in results),
                        sum(result.byte_count for result in results),
                        export_run_id,
                    ],
                )
            except Exception as exc:
                store.con.execute(
                    """
                    UPDATE lake_export_runs
                    SET status = 'failed',
                        finished_at = ?,
                        error_message = ?,
                        total_rows = ?,
                        total_byte_count = ?
                    WHERE export_run_id = ?
                    """,
                    [
                        now_utc_naive(),
                        str(exc),
                        sum(result.rows for result in results),
                        sum(result.byte_count for result in results),
                        export_run_id,
                    ],
                )
                raise
        return results

    def _record_export_file(
        self,
        store,
        *,
        export_run_id: str,
        object_name: str,
        output_path: Path,
        manifest_path: Path,
        rows: int,
        byte_count: int,
        sha256: str,
        schema_sha256: str,
        expected_schema_sha256: str | None,
        exported_at_naive: dt.datetime,
    ) -> None:
        store.con.execute(
            """
            INSERT INTO lake_export_files (
                export_run_id,
                object_name,
                output_path,
                manifest_path,
                rows,
                byte_count,
                sha256,
                schema_sha256,
                expected_schema_sha256,
                format,
                compression,
                exported_at
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            [
                export_run_id,
                object_name,
                str(output_path.resolve()),
                str(manifest_path.resolve()),
                rows,
                byte_count,
                sha256,
                schema_sha256,
                expected_schema_sha256,
                "parquet",
                "zstd",
                exported_at_naive,
            ],
        )

    def _export_single_file_object(
        self,
        store,
        *,
        object_name: str,
        contract: LakeExportContract | None,
        export_run_id: str,
        exported_at: dt.datetime,
        exported_at_naive: dt.datetime,
    ) -> LakeExportResult:
        quoted_object_name = _quote_object_name(object_name)
        schema = _object_schema(store, object_name)
        schema_hash = _schema_sha256(schema)
        expected_schema_hash = _assert_expected_schema(object_name, schema_hash, contract)
        rows = int(store.con.execute(f"SELECT count(*) FROM {quoted_object_name}").fetchone()[0])
        output_dir = self.lake_root / object_name
        output_dir.mkdir(parents=True, exist_ok=True)
        output_path = output_dir / "part-00000.parquet"
        escaped = str(output_path).replace("'", "''")
        store.con.execute(
            f"""
            COPY (
                SELECT *
                FROM {quoted_object_name}
            )
            TO '{escaped}'
            (FORMAT PARQUET, COMPRESSION ZSTD)
            """
        )
        byte_count = output_path.stat().st_size
        parquet_sha256 = file_sha256(output_path)
        manifest_path = output_dir / "_manifest.json"
        manifest = {
            "manifest_version": 1,
            "export_run_id": export_run_id,
            "object_name": object_name,
            "rows": rows,
            "exported_at_utc": exported_at.isoformat(),
            "db_path": str(self.db_path.resolve()),
            "output_path": str(output_path.resolve()),
            "format": "parquet",
            "compression": "zstd",
            "partition_columns": [],
            "byte_count": byte_count,
            "sha256": parquet_sha256,
            "schema_sha256": schema_hash,
            "expected_schema_sha256": expected_schema_hash,
            "schema": schema,
        }
        manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        self._record_export_file(
            store,
            export_run_id=export_run_id,
            object_name=object_name,
            output_path=output_path,
            manifest_path=manifest_path,
            rows=rows,
            byte_count=byte_count,
            sha256=parquet_sha256,
            schema_sha256=schema_hash,
            expected_schema_sha256=expected_schema_hash,
            exported_at_naive=exported_at_naive,
        )
        return LakeExportResult(
            export_run_id=export_run_id,
            object_name=object_name,
            output_path=output_path,
            manifest_path=manifest_path,
            rows=rows,
            byte_count=byte_count,
            sha256=parquet_sha256,
        )

    def _export_partitioned_object(
        self,
        store,
        *,
        object_name: str,
        spec: LakePartitionSpec,
        contract: LakeExportContract | None,
        export_run_id: str,
        exported_at: dt.datetime,
        exported_at_naive: dt.datetime,
        incremental: bool,
    ) -> LakeExportResult:
        quoted_object_name = _quote_object_name(object_name)
        schema = _object_schema(store, object_name)
        schema_columns = {str(column["name"]) for column in schema}
        missing = [column for column in spec.partition_columns if column not in schema_columns]
        if missing:
            raise ValueError(f"Partition columns not found for {object_name}: {missing}")
        if spec.watermark_column and spec.watermark_column not in schema_columns:
            raise ValueError(f"Watermark column not found for {object_name}: {spec.watermark_column}")
        schema_hash = _schema_sha256(schema)
        expected_schema_hash = _assert_expected_schema(object_name, schema_hash, contract)
        partition_select = ", ".join(_quote_object_name(column) for column in spec.partition_columns)
        partition_rows = store.con.execute(
            f"""
            SELECT DISTINCT {partition_select}
            FROM {quoted_object_name}
            ORDER BY {partition_select}
            """
        ).fetchall()
        state_rows = store.con.execute(
            """
            SELECT partition_key, watermark_value
            FROM lake_partition_state
            WHERE object_name = ?
            """,
            [object_name],
        ).fetchall()
        prior_state = {str(key): None if value is None else str(value) for key, value in state_rows}
        output_dir = self.lake_root / export_run_id / object_name
        output_dir.mkdir(parents=True, exist_ok=True)
        files: list[dict[str, Any]] = []
        skipped: list[dict[str, Any]] = []
        total_rows = 0
        total_bytes = 0
        for row in partition_rows:
            values = tuple(row)
            key = _partition_key(spec.partition_columns, values)
            predicate = _partition_predicate(spec.partition_columns, values)
            watermark_value: str | None = None
            if spec.watermark_column:
                watermark_row = store.con.execute(
                    f"""
                    SELECT max({_quote_object_name(spec.watermark_column)})
                    FROM {quoted_object_name}
                    WHERE {predicate}
                    """
                ).fetchone()
                watermark_value = None if watermark_row is None or watermark_row[0] is None else str(watermark_row[0])
            if incremental and prior_state.get(key) == watermark_value:
                skipped.append({"partition_key": key, "watermark_value": watermark_value})
                continue
            partition_path = _partition_dir(output_dir, spec.partition_columns, values)
            partition_path.mkdir(parents=True, exist_ok=True)
            output_path = partition_path / "part-00000.parquet"
            escaped = str(output_path).replace("'", "''")
            store.con.execute(
                f"""
                COPY (
                    SELECT *
                    FROM {quoted_object_name}
                    WHERE {predicate}
                )
                TO '{escaped}'
                (FORMAT PARQUET, COMPRESSION ZSTD)
                """
            )
            row_count = int(
                store.con.execute(
                    f"SELECT count(*) FROM {quoted_object_name} WHERE {predicate}"
                ).fetchone()[0]
            )
            byte_count = output_path.stat().st_size
            sha256 = file_sha256(output_path)
            file_row = {
                "partition_key": key,
                "partition_values": {
                    column: None if value is None else str(value)
                    for column, value in zip(spec.partition_columns, values, strict=True)
                },
                "watermark_value": watermark_value,
                "path": str(output_path.resolve()),
                "rows": row_count,
                "byte_count": byte_count,
                "sha256": sha256,
            }
            files.append(file_row)
            total_rows += row_count
            total_bytes += byte_count
            store.con.execute(
                """
                INSERT OR REPLACE INTO lake_partition_state (
                    object_name, partition_key, watermark_value, export_run_id,
                    row_count, byte_count, exported_at
                )
                VALUES (?, ?, ?, ?, ?, ?, ?)
                """,
                [
                    object_name,
                    key,
                    watermark_value,
                    export_run_id,
                    row_count,
                    byte_count,
                    exported_at_naive,
                ],
            )
        parquet_sha256 = _directory_sha256(files)
        manifest_path = output_dir / "_manifest.json"
        manifest = {
            "manifest_version": 1,
            "export_run_id": export_run_id,
            "object_name": object_name,
            "rows": total_rows,
            "exported_at_utc": exported_at.isoformat(),
            "db_path": str(self.db_path.resolve()),
            "output_path": str(output_dir.resolve()),
            "format": "parquet",
            "compression": "zstd",
            "partition_columns": list(spec.partition_columns),
            "watermark_column": spec.watermark_column,
            "incremental": incremental,
            "skipped_partitions": skipped,
            "files": files,
            "byte_count": total_bytes,
            "sha256": parquet_sha256,
            "schema_sha256": schema_hash,
            "expected_schema_sha256": expected_schema_hash,
            "schema": schema,
        }
        manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        self._record_export_file(
            store,
            export_run_id=export_run_id,
            object_name=object_name,
            output_path=output_dir,
            manifest_path=manifest_path,
            rows=total_rows,
            byte_count=total_bytes,
            sha256=parquet_sha256,
            schema_sha256=schema_hash,
            expected_schema_sha256=expected_schema_hash,
            exported_at_naive=exported_at_naive,
        )
        return LakeExportResult(
            export_run_id=export_run_id,
            object_name=object_name,
            output_path=output_dir,
            manifest_path=manifest_path,
            rows=total_rows,
            byte_count=total_bytes,
            sha256=parquet_sha256,
        )


def _latest_succeeded_export_run_id(store) -> str:
    row = store.con.execute(
        """
        SELECT export_run_id
        FROM lake_export_runs
        WHERE status = 'succeeded'
        ORDER BY finished_at DESC, started_at DESC
        LIMIT 1
        """
    ).fetchone()
    if row is None:
        raise ValueError("No succeeded lake export run found")
    return str(row[0])


def _problem(
    *,
    export_run_id: str,
    object_name: str | None,
    problem: str,
    message: str,
    path: str | None = None,
    expected: Any | None = None,
    actual: Any | None = None,
) -> LakeValidationProblem:
    return LakeValidationProblem(
        export_run_id=export_run_id,
        object_name=object_name,
        problem=problem,
        message=message,
        path=path,
        expected=expected,
        actual=actual,
    )


def _validate_parquet_file(
    store,
    *,
    export_run_id: str,
    object_name: str,
    output: Path,
    expected_rows: int,
    expected_byte_count: int,
    expected_sha256: str,
    problems: list[LakeValidationProblem],
) -> tuple[int, int, int]:
    if not output.exists():
        problems.append(
            _problem(
                export_run_id=export_run_id,
                object_name=object_name,
                problem="missing_parquet",
                message="Parquet file is missing.",
                path=str(output),
            )
        )
        return 0, 0, 0

    byte_count = output.stat().st_size
    if byte_count != expected_byte_count:
        problems.append(
            _problem(
                export_run_id=export_run_id,
                object_name=object_name,
                problem="byte_count_mismatch",
                message="Parquet file byte count differs from audit row.",
                path=str(output),
                expected=expected_byte_count,
                actual=byte_count,
            )
        )

    actual_sha256 = file_sha256(output)
    if actual_sha256 != expected_sha256:
        problems.append(
            _problem(
                export_run_id=export_run_id,
                object_name=object_name,
                problem="sha256_mismatch",
                message="Parquet file SHA-256 differs from audit row.",
                path=str(output),
                expected=expected_sha256,
                actual=actual_sha256,
            )
        )

    try:
        rows = int(store.con.execute("SELECT count(*) FROM read_parquet(?)", [str(output)]).fetchone()[0])
    except Exception as exc:
        problems.append(
            _problem(
                export_run_id=export_run_id,
                object_name=object_name,
                problem="parquet_read_failed",
                message=str(exc),
                path=str(output),
            )
        )
        return 0, byte_count, 0

    if rows != expected_rows:
        problems.append(
            _problem(
                export_run_id=export_run_id,
                object_name=object_name,
                problem="row_count_mismatch",
                message="Parquet row count differs from audit row.",
                path=str(output),
                expected=expected_rows,
                actual=rows,
            )
        )
    return rows, byte_count, 1


def validate_lake_export(
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    export_run_id: str | None = None,
) -> LakeValidationSummary:
    """Validate audited Parquet files for a lake export run without mutating the warehouse."""

    problems: list[LakeValidationProblem] = []
    rows_checked = 0
    byte_count_checked = 0
    files_readable = 0
    with connect(db_path, read_only=True) as store:
        selected_export_run_id = export_run_id or _latest_succeeded_export_run_id(store)
        audit_rows = store.con.execute(
            """
            SELECT
                object_name,
                output_path,
                manifest_path,
                rows,
                byte_count,
                sha256,
                schema_sha256,
                expected_schema_sha256,
                format,
                compression
            FROM lake_export_files
            WHERE export_run_id = ?
            ORDER BY object_name
            """,
            [selected_export_run_id],
        ).fetchall()
        if not audit_rows:
            problems.append(
                _problem(
                    export_run_id=selected_export_run_id,
                    object_name=None,
                    problem="missing_export_files",
                    message="No lake_export_files rows exist for export run.",
                )
            )
            return LakeValidationSummary(
                export_run_id=selected_export_run_id,
                files_checked=0,
                files_readable=0,
                rows_checked=0,
                byte_count_checked=0,
                problems=problems,
            )

        for (
            object_name,
            output_path,
            manifest_path,
            expected_rows,
            expected_byte_count,
            expected_sha256,
            expected_schema_sha256,
            expected_contract_schema_sha256,
            expected_format,
            expected_compression,
        ) in audit_rows:
            output = Path(output_path)
            manifest_file = Path(manifest_path)
            object_name = str(object_name)
            expected_rows = int(expected_rows)
            expected_byte_count = int(expected_byte_count)
            expected_sha256 = str(expected_sha256)
            expected_schema_sha256 = str(expected_schema_sha256)
            expected_contract_schema_sha256 = (
                None if expected_contract_schema_sha256 in (None, "") else str(expected_contract_schema_sha256)
            )
            expected_format = str(expected_format)
            expected_compression = str(expected_compression)

            manifest: dict[str, Any] | None = None
            if not manifest_file.exists():
                problems.append(
                    _problem(
                        export_run_id=selected_export_run_id,
                        object_name=object_name,
                        problem="missing_manifest",
                        message="Manifest file is missing.",
                        path=str(manifest_file),
                    )
                )
            else:
                try:
                    manifest = json.loads(manifest_file.read_text(encoding="utf-8"))
                except Exception as exc:
                    problems.append(
                        _problem(
                            export_run_id=selected_export_run_id,
                            object_name=object_name,
                            problem="invalid_manifest_json",
                            message=str(exc),
                            path=str(manifest_file),
                        )
                    )

            if output.is_dir():
                if manifest is None:
                    continue
                file_items = manifest.get("files")
                if not isinstance(file_items, list):
                    problems.append(
                        _problem(
                            export_run_id=selected_export_run_id,
                            object_name=object_name,
                            problem="invalid_manifest_files",
                            message="Partitioned export manifest is missing a files list.",
                            path=str(manifest_file),
                        )
                    )
                    continue
                actual_file_rows = 0
                actual_file_bytes = 0
                actual_file_readable = 0
                actual_files: list[dict[str, Any]] = []
                for item in file_items:
                    if not isinstance(item, dict):
                        problems.append(
                            _problem(
                                export_run_id=selected_export_run_id,
                                object_name=object_name,
                                problem="invalid_manifest_file_entry",
                                message="Partitioned export manifest file entry is not an object.",
                                path=str(manifest_file),
                            )
                        )
                        continue
                    file_path = Path(str(item.get("path", "")))
                    item_rows = int(item.get("rows", 0))
                    item_bytes = int(item.get("byte_count", 0))
                    item_sha256 = str(item.get("sha256", ""))
                    rows, bytes_checked, readable = _validate_parquet_file(
                        store,
                        export_run_id=selected_export_run_id,
                        object_name=object_name,
                        output=file_path,
                        expected_rows=item_rows,
                        expected_byte_count=item_bytes,
                        expected_sha256=item_sha256,
                        problems=problems,
                    )
                    actual_file_rows += rows
                    actual_file_bytes += bytes_checked
                    actual_file_readable += readable
                    actual_files.append({"path": str(file_path.resolve()), "sha256": item_sha256})

                actual_sha256 = _directory_sha256(actual_files)
                if actual_sha256 != expected_sha256:
                    problems.append(
                        _problem(
                            export_run_id=selected_export_run_id,
                            object_name=object_name,
                            problem="sha256_mismatch",
                            message="Partitioned export directory SHA-256 differs from audit row.",
                            path=str(output),
                            expected=expected_sha256,
                            actual=actual_sha256,
                        )
                    )
                if actual_file_rows != expected_rows:
                    problems.append(
                        _problem(
                            export_run_id=selected_export_run_id,
                            object_name=object_name,
                            problem="row_count_mismatch",
                            message="Partitioned export row count differs from audit row.",
                            path=str(output),
                            expected=expected_rows,
                            actual=actual_file_rows,
                        )
                    )
                if actual_file_bytes != expected_byte_count:
                    problems.append(
                        _problem(
                            export_run_id=selected_export_run_id,
                            object_name=object_name,
                            problem="byte_count_mismatch",
                            message="Partitioned export byte count differs from audit row.",
                            path=str(output),
                            expected=expected_byte_count,
                            actual=actual_file_bytes,
                        )
                    )
                rows_checked += actual_file_rows
                byte_count_checked += actual_file_bytes
                files_readable += actual_file_readable
            else:
                actual_rows, actual_byte_count, readable = _validate_parquet_file(
                    store,
                    export_run_id=selected_export_run_id,
                    object_name=object_name,
                    output=output,
                    expected_rows=expected_rows,
                    expected_byte_count=expected_byte_count,
                    expected_sha256=expected_sha256,
                    problems=problems,
                )
                rows_checked += actual_rows
                byte_count_checked += actual_byte_count
                files_readable += readable

            if manifest is None:
                continue
            manifest_expectations = {
                "export_run_id": selected_export_run_id,
                "object_name": object_name,
                "rows": expected_rows,
                "byte_count": expected_byte_count,
                "sha256": expected_sha256,
                "schema_sha256": expected_schema_sha256,
                "format": expected_format,
                "compression": expected_compression,
            }
            if expected_contract_schema_sha256 is not None:
                manifest_expectations["expected_schema_sha256"] = expected_contract_schema_sha256
            for field, expected in manifest_expectations.items():
                actual = manifest.get(field)
                if actual != expected:
                    problems.append(
                        _problem(
                            export_run_id=selected_export_run_id,
                            object_name=object_name,
                            problem="manifest_field_mismatch",
                            message=f"Manifest field {field!r} differs from audit row.",
                            path=str(manifest_file),
                            expected=expected,
                            actual=actual,
                        )
                    )
            if (
                expected_contract_schema_sha256 is not None
                and expected_contract_schema_sha256 != expected_schema_sha256
            ):
                problems.append(
                    _problem(
                        export_run_id=selected_export_run_id,
                        object_name=object_name,
                        problem="schema_contract_mismatch",
                        message="Exported schema SHA-256 differs from the registered expected schema SHA-256.",
                        path=str(manifest_file),
                        expected=expected_contract_schema_sha256,
                        actual=expected_schema_sha256,
                    )
                )

    return LakeValidationSummary(
        export_run_id=selected_export_run_id,
        files_checked=len(audit_rows),
        files_readable=files_readable,
        rows_checked=rows_checked,
        byte_count_checked=byte_count_checked,
        problems=problems,
    )
