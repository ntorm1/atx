from __future__ import annotations

import datetime as dt
import json
from pathlib import Path


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
        from db.dataset import DatasetLoadResult

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


def _registry():
    return {
        CliBackfillDataset.dataset_id: (
            CliBackfillDataset,
            lambda params: dict(params),
        )
    }


def _run_cli(argv: list[str], capsys, *, registry=None) -> dict:
    from scripts import warehouse_backfill

    selected_registry = _registry() if registry is None else registry
    code = warehouse_backfill.main(argv, registry=selected_registry)
    captured = capsys.readouterr()
    assert code == 0
    return json.loads(captured.out)


def test_warehouse_backfill_cli_backfill_status_and_resume(tmp_path: Path, capsys) -> None:
    CliBackfillDataset.reset()
    db_path = tmp_path / "warehouse_backfill_cli.duckdb"
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
