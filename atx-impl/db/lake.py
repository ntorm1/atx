from __future__ import annotations

import datetime as dt
import hashlib
import json
import re
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
    "equity_daily_bars",
    "corporate_actions",
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
    "fundamental_fact_revisions",
    "fundamental_statement_map",
    "fundamental_statement_points",
    "fundamental_ttm_points",
    "fundamental_periods",
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
    "thirteenf_managers",
    "thirteenf_manager_reports",
    "thirteenf_security_positions",
    "thirteenf_security_ownership",
    "v_thirteenf_positioning_by_security",
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
    "est_actual",
    "est_surprise",
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


class LakehouseExporter:
    def __init__(self, db_path: Path | str = DEFAULT_DB_PATH, lake_root: Path | str = DEFAULT_LAKE_ROOT) -> None:
        self.db_path = Path(db_path)
        self.lake_root = Path(lake_root)

    def export_objects(self, objects: tuple[str, ...] = DEFAULT_EXPORT_OBJECTS) -> list[LakeExportResult]:
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
                    json_dumps({"objects": list(objects), "format": "parquet", "compression": "zstd"}),
                ],
            )
            try:
                for object_name in objects:
                    quoted_object_name = _quote_object_name(object_name)
                    schema = _object_schema(store, object_name)
                    schema_hash = _schema_sha256(schema)
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
                        "schema": schema,
                    }
                    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
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
                            format,
                            compression,
                            exported_at
                        )
                        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                        """,
                        [
                            export_run_id,
                            object_name,
                            str(output_path.resolve()),
                            str(manifest_path.resolve()),
                            rows,
                            byte_count,
                            parquet_sha256,
                            schema_hash,
                            "parquet",
                            "zstd",
                            exported_at_naive,
                        ],
                    )
                    results.append(
                        LakeExportResult(
                            export_run_id=export_run_id,
                            object_name=object_name,
                            output_path=output_path,
                            manifest_path=manifest_path,
                            rows=rows,
                            byte_count=byte_count,
                            sha256=parquet_sha256,
                        )
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

            if not output.exists():
                problems.append(
                    _problem(
                        export_run_id=selected_export_run_id,
                        object_name=object_name,
                        problem="missing_parquet",
                        message="Parquet file is missing.",
                        path=str(output),
                    )
                )
                continue

            actual_byte_count = output.stat().st_size
            byte_count_checked += actual_byte_count
            if actual_byte_count != expected_byte_count:
                problems.append(
                    _problem(
                        export_run_id=selected_export_run_id,
                        object_name=object_name,
                        problem="byte_count_mismatch",
                        message="Parquet file byte count differs from audit row.",
                        path=str(output),
                        expected=expected_byte_count,
                        actual=actual_byte_count,
                    )
                )

            actual_sha256 = file_sha256(output)
            if actual_sha256 != expected_sha256:
                problems.append(
                    _problem(
                        export_run_id=selected_export_run_id,
                        object_name=object_name,
                        problem="sha256_mismatch",
                        message="Parquet file SHA-256 differs from audit row.",
                        path=str(output),
                        expected=expected_sha256,
                        actual=actual_sha256,
                    )
                )

            try:
                actual_rows = int(
                    store.con.execute("SELECT count(*) FROM read_parquet(?)", [str(output)]).fetchone()[0]
                )
            except Exception as exc:
                problems.append(
                    _problem(
                        export_run_id=selected_export_run_id,
                        object_name=object_name,
                        problem="parquet_read_failed",
                        message=str(exc),
                        path=str(output),
                    )
                )
            else:
                files_readable += 1
                rows_checked += actual_rows
                if actual_rows != expected_rows:
                    problems.append(
                        _problem(
                            export_run_id=selected_export_run_id,
                            object_name=object_name,
                            problem="row_count_mismatch",
                            message="Parquet row count differs from audit row.",
                            path=str(output),
                            expected=expected_rows,
                            actual=actual_rows,
                        )
                    )

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

    return LakeValidationSummary(
        export_run_id=selected_export_run_id,
        files_checked=len(audit_rows),
        files_readable=files_readable,
        rows_checked=rows_checked,
        byte_count_checked=byte_count_checked,
        problems=problems,
    )
