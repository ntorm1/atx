from __future__ import annotations

import json
from pathlib import Path

def test_storage_stats_and_compaction_create_backup(tmp_store, tmp_path):
    from atx_db.storage_admin import checkpoint_and_compact, record_storage_stats

    stats = record_storage_stats(tmp_store)

    assert stats.db_size_bytes > 0
    assert stats.table_count > 0
    assert tmp_store.con.execute(
        "SELECT count(*) FROM warehouse_storage_stats WHERE checked_at = ?",
        [stats.checked_at],
    ).fetchone()[0] == stats.table_count

    compacted = checkpoint_and_compact(tmp_store, backup_dir=tmp_path / "backups")

    assert compacted.checkpointed is True
    assert compacted.vacuumed is True
    assert compacted.backup.backup_path.exists()
    assert compacted.backup.byte_size > 0


def _build_lake_probe(db_path: Path) -> None:
    from atx_db.connection import DuckDBStore

    with DuckDBStore(db_path) as store:
        store.con.execute(
            """
            CREATE TABLE lake_probe (
                security_id VARCHAR NOT NULL,
                as_of_date DATE NOT NULL,
                value DOUBLE NOT NULL,
                available_at TIMESTAMP NOT NULL
            )
            """
        )
        store.con.execute(
            """
            INSERT INTO lake_probe VALUES
                ('A', DATE '2026-01-01', 1.0, TIMESTAMP '2026-01-02 00:00:00'),
                ('B', DATE '2026-01-01', 2.0, TIMESTAMP '2026-01-02 00:00:00'),
                ('C', DATE '2026-01-02', 3.0, TIMESTAMP '2026-01-03 00:00:00')
            """
        )
        store.con.execute(
            """
            INSERT OR REPLACE INTO lake_partition_specs (
                object_name, partition_columns_json, watermark_column,
                retention_runs, enabled, updated_at
            )
            VALUES ('lake_probe', '["as_of_date"]', 'available_at', 2, true, now())
            """
        )


def test_partitioned_incremental_lake_export_skips_unchanged_partitions(built_warehouse, tmp_path):
    from atx_db.connection import DuckDBStore
    from atx_db.lake import LakehouseExporter

    db_path = built_warehouse("lake_probe.duckdb")
    lake_root = tmp_path / "lake"
    _build_lake_probe(db_path)

    exporter = LakehouseExporter(db_path=db_path, lake_root=lake_root)
    first = exporter.export_objects(("lake_probe",), incremental=True)[0]
    first_manifest = json.loads(first.manifest_path.read_text(encoding="utf-8"))

    assert first_manifest["partition_columns"] == ["as_of_date"]
    assert first.rows == 3
    assert len(first_manifest["files"]) == 2
    assert all(Path(item["path"]).exists() for item in first_manifest["files"])

    second = exporter.export_objects(("lake_probe",), incremental=True)[0]
    second_manifest = json.loads(second.manifest_path.read_text(encoding="utf-8"))

    assert second.rows == 0
    assert len(second_manifest["files"]) == 0
    assert len(second_manifest["skipped_partitions"]) == 2

    with DuckDBStore(db_path) as store:
        store.con.execute(
            """
            INSERT INTO lake_probe VALUES
                ('D', DATE '2026-01-01', 4.0, TIMESTAMP '2026-01-04 00:00:00')
            """
        )

    third = exporter.export_objects(("lake_probe",), incremental=True)[0]
    third_manifest = json.loads(third.manifest_path.read_text(encoding="utf-8"))

    assert third.rows == 3
    assert len(third_manifest["files"]) == 1
    assert len(third_manifest["skipped_partitions"]) == 1
    assert third_manifest["files"][0]["partition_values"] == {"as_of_date": "2026-01-01"}


def test_undeclared_lake_object_keeps_single_file_layout(built_warehouse, tmp_path):
    from atx_db.connection import DuckDBStore
    from atx_db.lake import LakehouseExporter

    db_path = built_warehouse("lake_plain.duckdb")
    with DuckDBStore(db_path) as store:
        store.con.execute("CREATE TABLE lake_plain AS SELECT 1 AS id")

    result = LakehouseExporter(db_path=db_path, lake_root=tmp_path / "plain_lake").export_objects(("lake_plain",))[0]
    manifest = json.loads(result.manifest_path.read_text(encoding="utf-8"))

    assert result.output_path.name == "part-00000.parquet"
    assert manifest["partition_columns"] == []
    assert result.rows == 1


class RebuildProbeDataset:
    dataset_id = "rebuild_probe"
    depends_on: tuple[str, ...] = ()

    def run(self, store, options):
        from atx_db.dataset import DatasetLoadResult

        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=7,
            source="pytest",
            details={"options": dict(options)},
        )


def test_rebuild_wrapper_records_auditable_run(tmp_store):
    from atx_db.rebuild import run_warehouse_rebuild

    result = run_warehouse_rebuild(
        tmp_store,
        registry={"rebuild_probe": (RebuildProbeDataset, lambda params: dict(params))},
        since="2026-01-01",
        until="2026-01-31",
        rebuild_run_id="rebuild-fixture",
        orchestrator_run_id="rebuild-orchestrator-fixture",
        git_sha="abc123",
        gate=True,
    )

    assert result.status == "succeeded"
    assert result.dataset_counts == {"rebuild_probe": 7}
    row = tmp_store.con.execute(
        """
        SELECT orchestrator_run_id, git_sha, since_date, until_date, status, dataset_counts_json
        FROM warehouse_rebuild_runs
        WHERE rebuild_run_id = 'rebuild-fixture'
        """
    ).fetchone()
    assert (row[0], row[1], str(row[2]), str(row[3]), row[4]) == (
        "rebuild-orchestrator-fixture",
        "abc123",
        "2026-01-01",
        "2026-01-31",
        "succeeded",
    )
    assert json.loads(row[5]) == {"rebuild_probe": 7}
