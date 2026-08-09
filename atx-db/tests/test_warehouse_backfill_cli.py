from __future__ import annotations

import datetime as dt
import json
from pathlib import Path

import pytest


class CliBackfillDataset:
    dataset_id = "cli_backfill"
    depends_on: tuple[str, ...] = ()
    calls: list[str] = []
    failing_partition: str | None = None

    @classmethod
    def reset(cls) -> None:
        cls.calls = []
        cls.failing_partition = None

    def run(self, store, options):
        from atx_db.dataset import DatasetLoadResult

        partition_key = str(options["partition_key"])
        type(self).calls.append(partition_key)
        if partition_key == type(self).failing_partition:
            raise RuntimeError(f"poisoned cli partition {partition_key}")

        store.con.execute(
            """
            CREATE TABLE IF NOT EXISTS cli_backfill_rows (
                dataset_id VARCHAR NOT NULL,
                as_of_date DATE NOT NULL,
                partition_key VARCHAR NOT NULL,
                backfill_run_id VARCHAR NOT NULL,
                PRIMARY KEY (dataset_id, as_of_date)
            )
            """
        )
        current = options["window_lo"]
        rows = 0
        while current < options["window_hi"]:
            store.con.execute(
                """
                INSERT OR REPLACE INTO cli_backfill_rows (
                    dataset_id, as_of_date, partition_key, backfill_run_id
                )
                VALUES (?, ?, ?, ?)
                """,
                [
                    self.dataset_id,
                    current,
                    partition_key,
                    options["backfill_run_id"],
                ],
            )
            rows += 1
            current += dt.timedelta(days=1)
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source="cli-fixture",
            details={"watermark_after": options["window_hi"].isoformat()},
        )


class CliBackfillDependencyDataset(CliBackfillDataset):
    dataset_id = "cli_backfill_dep"
    calls: list[str] = []
    failing_partition: str | None = None


class CliBackfillRootDataset(CliBackfillDataset):
    dataset_id = "cli_backfill_root"
    depends_on = ("cli_backfill_dep",)
    calls: list[str] = []
    failing_partition: str | None = None


def _registry():
    return {
        CliBackfillDataset.dataset_id: (
            CliBackfillDataset,
            lambda params: dict(params),
        )
    }


def _dependency_registry():
    return {
        cls.dataset_id: (
            cls,
            lambda params: dict(params),
        )
        for cls in (CliBackfillDependencyDataset, CliBackfillRootDataset)
    }


def _run_cli(argv: list[str], capsys, *, registry=None) -> dict:
    from scripts import warehouse_backfill

    selected_registry = _registry() if registry is None else registry
    code = warehouse_backfill.main(argv, registry=selected_registry)
    captured = capsys.readouterr()
    assert code == 0
    return json.loads(captured.out)


def test_warehouse_backfill_cli_backfill_status_and_resume(built_warehouse, capsys) -> None:
    CliBackfillDataset.reset()
    db_path = built_warehouse("warehouse_backfill_cli.duckdb")
    failed_key = "cli_backfill:2014-02-01:2014-03-01"
    CliBackfillDataset.failing_partition = failed_key

    first = _run_cli(
        [
            "backfill",
            "--dataset",
            "cli_backfill",
            "--start",
            "2014-01-01",
            "--end",
            "2014-03-01",
            "--chunk",
            "1mo",
            "--db-path",
            str(db_path),
        ],
        capsys,
    )

    assert first["status"] == "partial"
    assert first["partitions_succeeded"] == 1
    assert first["partitions_failed"] == 1
    run_id = first["backfill_run_id"]

    status = _run_cli(
        [
            "status",
            "--dataset",
            "cli_backfill",
            "--backfill-run-id",
            run_id,
            "--db-path",
            str(db_path),
        ],
        capsys,
    )
    assert status["count"] == 2
    failed_rows = [row for row in status["rows"] if row["partition_key"] == failed_key]
    assert len(failed_rows) == 1
    assert failed_rows[0]["status"] == "failed"
    assert failed_rows[0]["dead_letter_state"] == "dead_lettered"
    assert "poisoned cli partition" in failed_rows[0]["dead_letter_error"]

    CliBackfillDataset.failing_partition = None
    resumed = _run_cli(
        [
            "resume",
            "--backfill-run-id",
            run_id,
            "--db-path",
            str(db_path),
        ],
        capsys,
    )

    assert resumed["status"] == "succeeded"
    assert resumed["resumed_via"] == "orchestrator"
    assert "same backfill_run_id" in resumed["semantics"]
    assert resumed["partitions_skipped"] == 1
    assert resumed["partitions_succeeded"] == 1
    assert CliBackfillDataset.calls == [
        "cli_backfill:2014-01-01:2014-02-01",
        failed_key,
        failed_key,
    ]

    final_status = _run_cli(
        [
            "status",
            "--dataset",
            "cli_backfill",
            "--backfill-run-id",
            run_id,
            "--db-path",
            str(db_path),
        ],
        capsys,
    )
    assert final_status["count"] == 2
    assert {row["status"] for row in final_status["rows"]} == {"succeeded"}
    assert {row["dead_letter_state"] for row in final_status["rows"]} == {"clear"}


def test_warehouse_backfill_cli_status_reports_all_skipped_rerun(
    built_warehouse,
    capsys,
) -> None:
    CliBackfillDataset.reset()
    db_path = built_warehouse("warehouse_backfill_cli_skipped.duckdb")

    first = _run_cli(
        [
            "backfill",
            "--dataset",
            "cli_backfill",
            "--start",
            "2014-01-01",
            "--end",
            "2014-03-01",
            "--chunk",
            "1mo",
            "--db-path",
            str(db_path),
        ],
        capsys,
    )
    calls_after_first = list(CliBackfillDataset.calls)

    second = _run_cli(
        [
            "backfill",
            "--dataset",
            "cli_backfill",
            "--start",
            "2014-01-01",
            "--end",
            "2014-03-01",
            "--chunk",
            "1mo",
            "--db-path",
            str(db_path),
        ],
        capsys,
    )

    assert first["status"] == "succeeded"
    assert second["status"] == "succeeded"
    assert second["partitions_skipped"] == 2
    assert second["partitions_succeeded"] == 0
    assert second["rows_written"] == 0
    assert CliBackfillDataset.calls == calls_after_first

    status = _run_cli(
        [
            "status",
            "--dataset",
            "cli_backfill",
            "--backfill-run-id",
            second["backfill_run_id"],
            "--db-path",
            str(db_path),
        ],
        capsys,
    )

    assert status["count"] == 2
    assert {row["backfill_run_id"] for row in status["rows"]} == {second["backfill_run_id"]}
    assert {row["run_id"] for row in status["rows"]} == {first["backfill_run_id"]}
    assert {row["status"] for row in status["rows"]} == {"skipped"}
    assert {row["rows_written"] for row in status["rows"]} == {0}
    assert {row["run_status"] for row in status["rows"]} == {"succeeded"}


def test_warehouse_backfill_cli_status_reports_dependency_all_skipped_rerun(
    built_warehouse,
    capsys,
) -> None:
    CliBackfillDependencyDataset.reset()
    CliBackfillRootDataset.reset()
    db_path = built_warehouse("warehouse_backfill_cli_dependency_skipped.duckdb")
    registry = _dependency_registry()

    first = _run_cli(
        [
            "backfill",
            "--dataset",
            "cli_backfill_root",
            "--start",
            "2014-01-01",
            "--end",
            "2014-03-01",
            "--chunk",
            "1mo",
            "--db-path",
            str(db_path),
        ],
        capsys,
        registry=registry,
    )
    dependency_calls_after_first = list(CliBackfillDependencyDataset.calls)
    root_calls_after_first = list(CliBackfillRootDataset.calls)

    second = _run_cli(
        [
            "backfill",
            "--dataset",
            "cli_backfill_root",
            "--start",
            "2014-01-01",
            "--end",
            "2014-03-01",
            "--chunk",
            "1mo",
            "--db-path",
            str(db_path),
        ],
        capsys,
        registry=registry,
    )

    assert first["status"] == "succeeded"
    assert first["partitions_planned"] == 4
    assert second["status"] == "succeeded"
    assert second["partitions_planned"] == 4
    assert second["partitions_skipped"] == 4
    assert second["partitions_succeeded"] == 0
    assert second["rows_written"] == 0
    assert CliBackfillDependencyDataset.calls == dependency_calls_after_first
    assert CliBackfillRootDataset.calls == root_calls_after_first

    status = _run_cli(
        [
            "status",
            "--dataset",
            "cli_backfill_root",
            "--backfill-run-id",
            second["backfill_run_id"],
            "--db-path",
            str(db_path),
        ],
        capsys,
        registry=registry,
    )

    rows = status["rows"]
    identity_keys = {
        (row["partition_dataset_id"], row["partition_key"])
        for row in rows
    }
    assert status["count"] == second["partitions_planned"]
    assert status["count"] == second["partitions_skipped"]
    assert len(rows) == len(identity_keys)
    assert {row["root_dataset_id"] for row in rows} == {"cli_backfill_root"}
    assert {row["partition_dataset_id"] for row in rows} == {
        "cli_backfill_dep",
        "cli_backfill_root",
    }
    assert {
        dataset_id: sum(
            1 for row in rows if row["partition_dataset_id"] == dataset_id
        )
        for dataset_id in {"cli_backfill_dep", "cli_backfill_root"}
    } == {"cli_backfill_dep": 2, "cli_backfill_root": 2}
    assert {row["backfill_run_id"] for row in rows} == {second["backfill_run_id"]}
    assert {row["run_id"] for row in rows} == {first["backfill_run_id"]}
    assert {row["status"] for row in rows} == {"skipped"}
    assert {row["rows_written"] for row in rows} == {0}
    assert {row["run_status"] for row in rows} == {"succeeded"}


def test_warehouse_backfill_cli_direct_resume_missing_dataset_does_not_leave_running(
    built_warehouse,
    capsys,
) -> None:
    from atx_db import DuckDBStore
    from scripts import warehouse_backfill

    db_path = built_warehouse("warehouse_backfill_cli_direct_missing.duckdb")
    run_id = "direct-missing-dataset"
    with DuckDBStore(db_path) as store:
        store.con.execute(
            """
            INSERT INTO backfill_run (
                backfill_run_id,
                dataset_id,
                start_date,
                end_date,
                chunk,
                status,
                started_at,
                finished_at,
                error_message
            )
            VALUES (?, ?, ?, ?, ?, 'running', ?, NULL, NULL)
            """,
            [
                run_id,
                "missing_direct_dataset",
                dt.date(2014, 1, 1),
                dt.date(2014, 2, 1),
                "1mo",
                dt.datetime(2026, 7, 1, 12, 0, 0),
            ],
        )

    with pytest.raises(KeyError, match="Unknown dataset_id"):
        warehouse_backfill.main(
            [
                "resume",
                "--backfill-run-id",
                run_id,
                "--db-path",
                str(db_path),
            ],
            registry={},
        )
    captured = capsys.readouterr()
    assert captured.out == ""

    with DuckDBStore(db_path) as store:
        row = store.con.execute(
            """
            SELECT status, error_message
            FROM backfill_run
            WHERE backfill_run_id = ?
            """,
            [run_id],
        ).fetchone()
    assert row[0] == "failed"
    assert "Unknown dataset_id 'missing_direct_dataset'" in row[1]
