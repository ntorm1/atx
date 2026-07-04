from __future__ import annotations

import datetime as dt
import inspect
import threading
import time

import pytest


class TickClock:
    def __init__(self) -> None:
        self.current = dt.datetime(2026, 7, 4, 9, 30, 0)

    def __call__(self) -> dt.datetime:
        value = self.current
        self.current += dt.timedelta(seconds=1)
        return value


class FixtureBackfillDataset:
    dataset_id = "fixture_backfill"
    depends_on: tuple[str, ...] = ()
    calls: list[str] = []
    fail_once_for: set[str] = set()

    @classmethod
    def reset(cls) -> None:
        cls.calls = []
        cls.fail_once_for = set()

    def run(self, store, options):
        from db.dataset import DatasetLoadResult

        partition_key = str(options["partition_key"])
        window_lo = options["window_lo"]
        window_hi = options["window_hi"]
        type(self).calls.append(partition_key)

        current = window_lo
        rows = 0
        should_fail = partition_key in type(self).fail_once_for
        while current < window_hi:
            if should_fail and rows >= 2:
                type(self).fail_once_for.remove(partition_key)
                raise RuntimeError(f"planned partial failure for {partition_key}")
            store.con.execute(
                """
                INSERT OR REPLACE INTO fixture_backfill_rows (
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
            source="fixture",
            details={"watermark_after": window_hi.isoformat()},
        )


class FixtureSourceDataset:
    dataset_id = "fixture_source"
    depends_on: tuple[str, ...] = ()


class FixtureMaintenanceDataset(FixtureBackfillDataset):
    dataset_id = "fixture_maintenance"
    depends_on = (FixtureSourceDataset.dataset_id,)
    calls: list[str] = []
    fail_once_for: set[str] = set()


@pytest.fixture(autouse=True)
def _reset_fixture_dataset():
    FixtureBackfillDataset.reset()
    FixtureMaintenanceDataset.reset()
    yield
    FixtureBackfillDataset.reset()
    FixtureMaintenanceDataset.reset()


def _registry():
    return {
        FixtureBackfillDataset.dataset_id: (
            FixtureBackfillDataset,
            lambda params: dict(params),
        )
    }


def _maintenance_registry():
    return {
        FixtureSourceDataset.dataset_id: (
            FixtureSourceDataset,
            lambda params: dict(params),
        ),
        FixtureMaintenanceDataset.dataset_id: (
            FixtureMaintenanceDataset,
            lambda params: dict(params),
        ),
    }


def _prepare_fixture_rows(store) -> None:
    store.con.execute(
        """
        CREATE TABLE IF NOT EXISTS fixture_backfill_rows (
            dataset_id VARCHAR NOT NULL,
            as_of_date DATE NOT NULL,
            partition_key VARCHAR NOT NULL,
            backfill_run_id VARCHAR NOT NULL,
            PRIMARY KEY (dataset_id, as_of_date)
        )
        """
    )


def _upsert_dataset_watermark(
    store,
    dataset_id: str,
    watermark_name: str,
    watermark_value: str,
) -> None:
    store.con.execute(
        """
        DELETE FROM dataset_watermarks
        WHERE dataset_id = ? AND watermark_name = ?
        """,
        [dataset_id, watermark_name],
    )
    store.con.execute(
        """
        INSERT INTO dataset_watermarks (
            dataset_id, watermark_name, watermark_value, updated_at
        )
        VALUES (?, ?, ?, ?)
        """,
        [
            dataset_id,
            watermark_name,
            watermark_value,
            dt.datetime(2026, 7, 4, 10, 0, 0),
        ],
    )


def _partition_watermark_name(partition) -> str:
    return f"available_at:{partition.partition_key}"


def _seed_upstream_partition_watermarks(store, partitions) -> None:
    for partition in partitions:
        _upsert_dataset_watermark(
            store,
            FixtureSourceDataset.dataset_id,
            _partition_watermark_name(partition),
            partition.window_hi.isoformat(),
        )


def _completed_maintenance_fixture(tmp_store):
    from db.backfill import plan_backfill, run_backfill

    _prepare_fixture_rows(tmp_store)
    registry = _maintenance_registry()
    start = dt.date(2014, 1, 1)
    end = dt.date(2015, 1, 1)
    partitions = plan_backfill(
        FixtureMaintenanceDataset.dataset_id,
        start,
        end,
        "3mo",
        registry=registry,
    )
    run_backfill(
        tmp_store,
        FixtureMaintenanceDataset.dataset_id,
        start,
        end,
        "3mo",
        registry=registry,
        backfill_run_id="maintenance-baseline",
        include_dependencies=False,
        clock=TickClock(),
    )
    _seed_upstream_partition_watermarks(tmp_store, partitions)
    FixtureMaintenanceDataset.calls.clear()
    return registry, start, end, partitions


def test_plan_backfill_is_deterministic_for_half_open_quarterly_windows():
    from db.backfill import Partition, plan_backfill

    expected = [
        Partition(
            "fixture_backfill",
            dt.date(2014, 1, 1),
            dt.date(2014, 4, 1),
            "fixture_backfill:2014-01-01:2014-04-01",
        ),
        Partition(
            "fixture_backfill",
            dt.date(2014, 4, 1),
            dt.date(2014, 7, 1),
            "fixture_backfill:2014-04-01:2014-07-01",
        ),
        Partition(
            "fixture_backfill",
            dt.date(2014, 7, 1),
            dt.date(2014, 10, 1),
            "fixture_backfill:2014-07-01:2014-10-01",
        ),
        Partition(
            "fixture_backfill",
            dt.date(2014, 10, 1),
            dt.date(2015, 1, 1),
            "fixture_backfill:2014-10-01:2015-01-01",
        ),
    ]

    first = plan_backfill(
        "fixture_backfill",
        dt.date(2014, 1, 1),
        dt.date(2015, 1, 1),
        "3mo",
        registry=_registry(),
    )
    second = plan_backfill(
        "fixture_backfill",
        "2014-01-01",
        "2015-01-01",
        "1q",
        registry=_registry(),
    )

    assert first == expected
    assert second == expected


def test_run_backfill_full_run_records_succeeded_watermarks(tmp_store):
    from db.backfill import run_backfill

    _prepare_fixture_rows(tmp_store)
    start = dt.date(2014, 1, 1)
    end = dt.date(2020, 1, 1)

    result = run_backfill(
        tmp_store,
        "fixture_backfill",
        start,
        end,
        "2y",
        registry=_registry(),
        backfill_run_id="backfill-full",
        include_dependencies=False,
        clock=TickClock(),
    )

    assert result.status == "succeeded"
    assert result.partitions_planned == 3
    assert result.partitions_succeeded == 3
    assert result.partitions_skipped == 0
    assert result.rows_written == (end - start).days

    watermark_rows = tmp_store.con.execute(
        """
        SELECT status, count(*), sum(rows_written)
        FROM backfill_watermark
        WHERE dataset_id = 'fixture_backfill'
        GROUP BY status
        """
    ).fetchall()
    assert watermark_rows == [("succeeded", 3, (end - start).days)]

    fixture_count = tmp_store.con.execute(
        "SELECT count(*) FROM fixture_backfill_rows"
    ).fetchone()[0]
    assert fixture_count == (end - start).days

    header = tmp_store.con.execute(
        """
        SELECT dataset_id, start_date, end_date, chunk, status
        FROM backfill_run
        WHERE backfill_run_id = 'backfill-full'
        """
    ).fetchone()
    assert header == ("fixture_backfill", start, end, "2y", "succeeded")


def test_immediate_second_backfill_is_strict_noop(tmp_store):
    from db.backfill import run_backfill

    _prepare_fixture_rows(tmp_store)
    start = dt.date(2014, 1, 1)
    end = dt.date(2015, 1, 1)

    first = run_backfill(
        tmp_store,
        "fixture_backfill",
        start,
        end,
        "3mo",
        registry=_registry(),
        backfill_run_id="backfill-first",
        include_dependencies=False,
        clock=TickClock(),
    )
    calls_after_first = list(FixtureBackfillDataset.calls)
    watermarks_before = tmp_store.con.execute(
        """
        SELECT partition_key, status, rows_written, watermark_after, run_id, updated_at
        FROM backfill_watermark
        WHERE dataset_id = 'fixture_backfill'
        ORDER BY partition_key
        """
    ).fetchall()

    second = run_backfill(
        tmp_store,
        "fixture_backfill",
        start,
        end,
        "3mo",
        registry=_registry(),
        backfill_run_id="backfill-second",
        include_dependencies=False,
        clock=TickClock(),
    )
    watermarks_after = tmp_store.con.execute(
        """
        SELECT partition_key, status, rows_written, watermark_after, run_id, updated_at
        FROM backfill_watermark
        WHERE dataset_id = 'fixture_backfill'
        ORDER BY partition_key
        """
    ).fetchall()

    assert first.partitions_planned == 4
    assert second.status == "succeeded"
    assert second.partitions_planned == 4
    assert second.partitions_skipped == 4
    assert second.partitions_succeeded == 0
    assert second.rows_written == 0
    assert FixtureBackfillDataset.calls == calls_after_first
    assert watermarks_after == watermarks_before


def test_same_run_id_resumes_failed_and_running_partitions_without_duplicates(tmp_store):
    from db.backfill import plan_backfill, run_backfill

    _prepare_fixture_rows(tmp_store)
    start = dt.date(2014, 1, 1)
    end = dt.date(2014, 4, 1)
    backfill_run_id = "backfill-fails-mid-window"
    partitions = plan_backfill(
        "fixture_backfill",
        start,
        end,
        "1mo",
        registry=_registry(),
    )
    FixtureBackfillDataset.fail_once_for = {partitions[1].partition_key}

    with pytest.raises(RuntimeError, match="planned partial failure"):
        run_backfill(
            tmp_store,
            "fixture_backfill",
            start,
            end,
            "1mo",
            registry=_registry(),
            backfill_run_id=backfill_run_id,
            include_dependencies=False,
            clock=TickClock(),
        )

    failed_status = tmp_store.con.execute(
        """
        SELECT status
        FROM backfill_watermark
        WHERE dataset_id = ? AND partition_key = ?
        """,
        ["fixture_backfill", partitions[1].partition_key],
    ).fetchone()
    assert failed_status == ("failed",)

    failed_header = tmp_store.con.execute(
        """
        SELECT status, count(*)
        FROM backfill_run
        WHERE backfill_run_id = ?
        GROUP BY status
        """,
        [backfill_run_id],
    ).fetchone()
    assert failed_header == ("failed", 1)

    tmp_store.con.execute(
        """
        INSERT OR REPLACE INTO backfill_watermark (
            dataset_id, partition_key, window_lo, window_hi, status,
            rows_written, watermark_after, run_id, updated_at
        )
        VALUES (?, ?, ?, ?, 'running', 0, NULL, 'interrupted-run', ?)
        """,
        [
            "fixture_backfill",
            partitions[2].partition_key,
            partitions[2].window_lo,
            partitions[2].window_hi,
            dt.datetime(2026, 7, 4, 9, 45, 0),
        ],
    )

    result = run_backfill(
        tmp_store,
        "fixture_backfill",
        start,
        end,
        "1mo",
        registry=_registry(),
        backfill_run_id=backfill_run_id,
        include_dependencies=False,
        clock=TickClock(),
    )

    assert result.status == "succeeded"
    assert result.partitions_skipped == 1
    assert result.partitions_succeeded == 2
    assert result.rows_written == (
        (partitions[1].window_hi - partitions[1].window_lo).days
        + (partitions[2].window_hi - partitions[2].window_lo).days
    )
    assert FixtureBackfillDataset.calls == [
        partitions[0].partition_key,
        partitions[1].partition_key,
        partitions[1].partition_key,
        partitions[2].partition_key,
    ]

    final_statuses = tmp_store.con.execute(
        """
        SELECT partition_key, status
        FROM backfill_watermark
        WHERE dataset_id = 'fixture_backfill'
        ORDER BY window_lo
        """
    ).fetchall()
    assert final_statuses == [
        (partitions[0].partition_key, "succeeded"),
        (partitions[1].partition_key, "succeeded"),
        (partitions[2].partition_key, "succeeded"),
    ]

    fixture_count = tmp_store.con.execute(
        "SELECT count(*) FROM fixture_backfill_rows"
    ).fetchone()[0]
    duplicate_count = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM (
            SELECT dataset_id, as_of_date, count(*) AS n
            FROM fixture_backfill_rows
            GROUP BY dataset_id, as_of_date
            HAVING count(*) > 1
        )
        """
    ).fetchone()[0]
    assert fixture_count == (end - start).days
    assert duplicate_count == 0
    final_header = tmp_store.con.execute(
        """
        SELECT status, count(*)
        FROM backfill_run
        WHERE backfill_run_id = ?
        GROUP BY status
        """,
        [backfill_run_id],
    ).fetchone()
    assert final_header == ("succeeded", 1)


def test_backfill_dead_letters_poisoned_partition_and_clears_after_fixed_rerun(tmp_store):
    from db.backfill import plan_backfill, run_backfill
    from db.orchestrator import RetryPolicy

    _prepare_fixture_rows(tmp_store)
    start = dt.date(2014, 1, 1)
    end = dt.date(2014, 4, 1)
    partitions = plan_backfill(
        "fixture_backfill",
        start,
        end,
        "1mo",
        registry=_registry(),
    )
    poisoned = {partitions[1].partition_key}
    attempts: dict[str, int] = {}

    def executor(store, partition, options):
        attempts[partition.partition_key] = attempts.get(partition.partition_key, 0) + 1
        if partition.partition_key in poisoned:
            raise RuntimeError(f"poisoned fixture partition {partition.partition_key}")
        return FixtureBackfillDataset().run(store, options)

    first = run_backfill(
        tmp_store,
        "fixture_backfill",
        start,
        end,
        "1mo",
        registry=_registry(),
        backfill_run_id="backfill-poisoned",
        include_dependencies=False,
        executor=executor,
        retry_policy=RetryPolicy(max_retries=2, retry_delay_seconds=0),
        dead_letter=True,
        clock=TickClock(),
    )

    assert first.status == "partial"
    assert first.partitions_succeeded == 2
    assert first.partitions_failed == 1
    assert attempts[partitions[1].partition_key] == 3

    dead_letter = tmp_store.con.execute(
        """
        SELECT error, attempts
        FROM backfill_dead_letter
        WHERE dataset_id = ? AND partition_key = ? AND run_id = ?
        """,
        ["fixture_backfill", partitions[1].partition_key, "backfill-poisoned"],
    ).fetchone()
    assert dead_letter is not None
    assert "poisoned fixture partition" in dead_letter[0]
    assert dead_letter[1] == 3

    statuses = tmp_store.con.execute(
        """
        SELECT partition_key, status
        FROM backfill_watermark
        WHERE dataset_id = 'fixture_backfill'
        ORDER BY window_lo
        """
    ).fetchall()
    assert statuses == [
        (partitions[0].partition_key, "succeeded"),
        (partitions[1].partition_key, "failed"),
        (partitions[2].partition_key, "succeeded"),
    ]

    poisoned.clear()
    attempts.clear()
    fixed = run_backfill(
        tmp_store,
        "fixture_backfill",
        start,
        end,
        "1mo",
        registry=_registry(),
        backfill_run_id="backfill-poison-fixed",
        include_dependencies=False,
        executor=executor,
        retry_policy=RetryPolicy(max_retries=2, retry_delay_seconds=0),
        dead_letter=True,
        clock=TickClock(),
    )

    assert fixed.status == "succeeded"
    assert fixed.partitions_skipped == 2
    assert fixed.partitions_succeeded == 1
    assert attempts == {partitions[1].partition_key: 1}
    assert tmp_store.con.execute(
        """
        SELECT count(*)
        FROM backfill_dead_letter
        WHERE dataset_id = ? AND partition_key = ?
        """,
        ["fixture_backfill", partitions[1].partition_key],
    ).fetchone()[0] == 0
    assert tmp_store.con.execute(
        """
        SELECT status, run_id
        FROM backfill_watermark
        WHERE dataset_id = ? AND partition_key = ?
        """,
        ["fixture_backfill", partitions[1].partition_key],
    ).fetchone() == ("succeeded", "backfill-poison-fixed")


def test_backfill_max_parallel_caps_concurrent_partition_executor_calls(tmp_store):
    from db.backfill import run_backfill

    lock = threading.Lock()
    active = 0
    max_seen = 0

    def executor(_store, partition, _options):
        nonlocal active, max_seen
        with lock:
            active += 1
            max_seen = max(max_seen, active)
        time.sleep(0.03)
        with lock:
            active -= 1
        return {
            "rows_written": 1,
            "watermark_after": partition.window_hi.isoformat(),
        }

    result = run_backfill(
        tmp_store,
        "fixture_backfill",
        dt.date(2014, 1, 1),
        dt.date(2014, 7, 1),
        "1mo",
        registry=_registry(),
        backfill_run_id="backfill-parallel-cap",
        include_dependencies=False,
        executor=executor,
        max_parallel=2,
        clock=TickClock(),
    )

    assert result.status == "succeeded"
    assert result.partitions_succeeded == 6
    assert max_seen <= 2
    assert max_seen > 1


def test_maintenance_schedules_only_partition_with_advanced_upstream_watermark(tmp_store):
    from db.backfill import run_maintenance

    registry, start, end, partitions = _completed_maintenance_fixture(tmp_store)
    stale_partition = partitions[1]
    advanced_watermark = "2014-07-15T00:00:00"
    _upsert_dataset_watermark(
        tmp_store,
        FixtureSourceDataset.dataset_id,
        _partition_watermark_name(stale_partition),
        advanced_watermark,
    )
    watermarks_before = {
        row[0]: row
        for row in tmp_store.con.execute(
            """
            SELECT partition_key, status, rows_written, watermark_after, run_id, updated_at
            FROM backfill_watermark
            WHERE dataset_id = ?
            ORDER BY partition_key
            """,
            [FixtureMaintenanceDataset.dataset_id],
        ).fetchall()
    }

    result = run_maintenance(
        tmp_store,
        FixtureMaintenanceDataset.dataset_id,
        start,
        end,
        "3mo",
        registry=registry,
        backfill_run_id="maintenance-advance",
        clock=TickClock(),
    )

    assert result.status == "succeeded"
    assert result.partitions_planned == 1
    assert result.partitions_succeeded == 1
    assert result.partitions_skipped == 0
    assert result.rows_written == (stale_partition.window_hi - stale_partition.window_lo).days
    assert FixtureMaintenanceDataset.calls == [stale_partition.partition_key]

    watermarks_after = {
        row[0]: row
        for row in tmp_store.con.execute(
            """
            SELECT partition_key, status, rows_written, watermark_after, run_id, updated_at
            FROM backfill_watermark
            WHERE dataset_id = ?
            ORDER BY partition_key
            """,
            [FixtureMaintenanceDataset.dataset_id],
        ).fetchall()
    }
    for partition in partitions:
        if partition == stale_partition:
            continue
        assert watermarks_after[partition.partition_key] == watermarks_before[partition.partition_key]

    stale_after = watermarks_after[stale_partition.partition_key]
    assert stale_after[1] == "succeeded"
    assert stale_after[3] == advanced_watermark
    assert stale_after[4] == "maintenance-advance"

    rewritten_rows = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM fixture_backfill_rows
        WHERE dataset_id = ? AND backfill_run_id = 'maintenance-advance'
        """,
        [FixtureMaintenanceDataset.dataset_id],
    ).fetchone()[0]
    untouched_rows = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM fixture_backfill_rows
        WHERE dataset_id = ? AND backfill_run_id = 'maintenance-baseline'
        """,
        [FixtureMaintenanceDataset.dataset_id],
    ).fetchone()[0]
    assert rewritten_rows == (stale_partition.window_hi - stale_partition.window_lo).days
    assert untouched_rows == (end - start).days - rewritten_rows


def test_maintenance_with_only_coarse_current_watermark_is_immediate_noop(tmp_store):
    from db.backfill import run_maintenance

    registry, start, end, _partitions = _completed_maintenance_fixture(tmp_store)
    tmp_store.con.execute(
        """
        DELETE FROM dataset_watermarks
        WHERE dataset_id = ?
        """,
        [FixtureSourceDataset.dataset_id],
    )
    _upsert_dataset_watermark(
        tmp_store,
        FixtureSourceDataset.dataset_id,
        "max_available_at",
        end.isoformat(),
    )
    watermarks_before = tmp_store.con.execute(
        """
        SELECT partition_key, status, rows_written, watermark_after, run_id, updated_at
        FROM backfill_watermark
        WHERE dataset_id = ?
        ORDER BY partition_key
        """,
        [FixtureMaintenanceDataset.dataset_id],
    ).fetchall()

    result = run_maintenance(
        tmp_store,
        FixtureMaintenanceDataset.dataset_id,
        start,
        end,
        "3mo",
        registry=registry,
        backfill_run_id="maintenance-coarse-noop",
        clock=TickClock(),
    )

    watermarks_after = tmp_store.con.execute(
        """
        SELECT partition_key, status, rows_written, watermark_after, run_id, updated_at
        FROM backfill_watermark
        WHERE dataset_id = ?
        ORDER BY partition_key
        """,
        [FixtureMaintenanceDataset.dataset_id],
    ).fetchall()
    assert result.status == "succeeded"
    assert result.partitions_planned == 0
    assert result.partitions_succeeded == 0
    assert result.rows_written == 0
    assert FixtureMaintenanceDataset.calls == []
    assert watermarks_after == watermarks_before


def test_maintenance_caps_partial_trailing_coarse_watermark(tmp_store):
    from db.backfill import plan_backfill, run_maintenance

    _prepare_fixture_rows(tmp_store)
    registry = _maintenance_registry()
    start = dt.date(2014, 1, 1)
    end = dt.date(2014, 4, 1)
    partition = plan_backfill(
        FixtureMaintenanceDataset.dataset_id,
        start,
        end,
        "3mo",
        registry=registry,
    )[0]
    _upsert_dataset_watermark(
        tmp_store,
        FixtureSourceDataset.dataset_id,
        "max_available_at",
        "2014-02-15",
    )

    first = run_maintenance(
        tmp_store,
        FixtureMaintenanceDataset.dataset_id,
        start,
        end,
        "3mo",
        registry=registry,
        backfill_run_id="maintenance-partial-first",
        clock=TickClock(),
    )
    first_watermark = tmp_store.con.execute(
        """
        SELECT watermark_after
        FROM backfill_watermark
        WHERE dataset_id = ? AND partition_key = ?
        """,
        [FixtureMaintenanceDataset.dataset_id, partition.partition_key],
    ).fetchone()

    _upsert_dataset_watermark(
        tmp_store,
        FixtureSourceDataset.dataset_id,
        "max_available_at",
        "2014-03-01",
    )
    second = run_maintenance(
        tmp_store,
        FixtureMaintenanceDataset.dataset_id,
        start,
        end,
        "3mo",
        registry=registry,
        backfill_run_id="maintenance-partial-second",
        clock=TickClock(),
    )
    second_watermark = tmp_store.con.execute(
        """
        SELECT watermark_after
        FROM backfill_watermark
        WHERE dataset_id = ? AND partition_key = ?
        """,
        [FixtureMaintenanceDataset.dataset_id, partition.partition_key],
    ).fetchone()

    assert first.partitions_planned == 1
    assert first.partitions_succeeded == 1
    assert first_watermark == ("2014-02-15",)
    assert second.partitions_planned == 1
    assert second.partitions_succeeded == 1
    assert second_watermark == ("2014-03-01",)
    assert FixtureMaintenanceDataset.calls == [
        partition.partition_key,
        partition.partition_key,
    ]


def test_maintenance_treats_equivalent_date_timestamp_watermarks_as_current(tmp_store):
    from db.backfill import run_maintenance

    registry, start, end, partitions = _completed_maintenance_fixture(tmp_store)
    _upsert_dataset_watermark(
        tmp_store,
        FixtureSourceDataset.dataset_id,
        _partition_watermark_name(partitions[0]),
        "2014-04-01 00:00:00",
    )

    result = run_maintenance(
        tmp_store,
        FixtureMaintenanceDataset.dataset_id,
        start,
        end,
        "3mo",
        registry=registry,
        backfill_run_id="maintenance-equal-formats",
        clock=TickClock(),
    )

    assert result.status == "succeeded"
    assert result.partitions_planned == 0
    assert result.partitions_succeeded == 0
    assert result.rows_written == 0
    assert FixtureMaintenanceDataset.calls == []


def test_immediate_second_maintenance_rerun_without_advance_is_noop(tmp_store):
    from db.backfill import run_maintenance

    registry, start, end, partitions = _completed_maintenance_fixture(tmp_store)
    stale_partition = partitions[2]
    _upsert_dataset_watermark(
        tmp_store,
        FixtureSourceDataset.dataset_id,
        _partition_watermark_name(stale_partition),
        "2014-10-20T00:00:00",
    )
    first = run_maintenance(
        tmp_store,
        FixtureMaintenanceDataset.dataset_id,
        start,
        end,
        "3mo",
        registry=registry,
        backfill_run_id="maintenance-first",
        clock=TickClock(),
    )
    calls_after_first = list(FixtureMaintenanceDataset.calls)
    watermarks_after_first = tmp_store.con.execute(
        """
        SELECT partition_key, status, rows_written, watermark_after, run_id, updated_at
        FROM backfill_watermark
        WHERE dataset_id = ?
        ORDER BY partition_key
        """,
        [FixtureMaintenanceDataset.dataset_id],
    ).fetchall()

    second = run_maintenance(
        tmp_store,
        FixtureMaintenanceDataset.dataset_id,
        start,
        end,
        "3mo",
        registry=registry,
        backfill_run_id="maintenance-second",
        clock=TickClock(),
    )
    watermarks_after_second = tmp_store.con.execute(
        """
        SELECT partition_key, status, rows_written, watermark_after, run_id, updated_at
        FROM backfill_watermark
        WHERE dataset_id = ?
        ORDER BY partition_key
        """,
        [FixtureMaintenanceDataset.dataset_id],
    ).fetchall()

    assert first.partitions_planned == 1
    assert second.status == "succeeded"
    assert second.partitions_planned == 0
    assert second.partitions_succeeded == 0
    assert second.rows_written == 0
    assert FixtureMaintenanceDataset.calls == calls_after_first
    assert watermarks_after_second == watermarks_after_first


def test_maintenance_uses_existing_watermark_table_without_rewriting_completed_history(tmp_store):
    from db.backfill import run_maintenance

    registry, start, end, _partitions = _completed_maintenance_fixture(tmp_store)
    rows_before = tmp_store.con.execute(
        """
        SELECT partition_key, status, rows_written, watermark_after, run_id, updated_at
        FROM backfill_watermark
        WHERE dataset_id = ?
        ORDER BY partition_key
        """,
        [FixtureMaintenanceDataset.dataset_id],
    ).fetchall()
    fixture_rows_before = tmp_store.con.execute(
        """
        SELECT as_of_date, partition_key, backfill_run_id
        FROM fixture_backfill_rows
        WHERE dataset_id = ?
        ORDER BY as_of_date
        """,
        [FixtureMaintenanceDataset.dataset_id],
    ).fetchall()

    result = run_maintenance(
        tmp_store,
        FixtureMaintenanceDataset.dataset_id,
        start,
        end,
        "3mo",
        registry=registry,
        backfill_run_id="maintenance-shared-table",
        clock=TickClock(),
    )

    rows_after = tmp_store.con.execute(
        """
        SELECT partition_key, status, rows_written, watermark_after, run_id, updated_at
        FROM backfill_watermark
        WHERE dataset_id = ?
        ORDER BY partition_key
        """,
        [FixtureMaintenanceDataset.dataset_id],
    ).fetchall()
    fixture_rows_after = tmp_store.con.execute(
        """
        SELECT as_of_date, partition_key, backfill_run_id
        FROM fixture_backfill_rows
        WHERE dataset_id = ?
        ORDER BY as_of_date
        """,
        [FixtureMaintenanceDataset.dataset_id],
    ).fetchall()
    header = tmp_store.con.execute(
        """
        SELECT dataset_id, status
        FROM backfill_run
        WHERE backfill_run_id = 'maintenance-shared-table'
        """
    ).fetchone()

    assert result.partitions_planned == 0
    assert result.rows_written == 0
    assert FixtureMaintenanceDataset.calls == []
    assert rows_after == rows_before
    assert fixture_rows_after == fixture_rows_before
    assert header == (FixtureMaintenanceDataset.dataset_id, "succeeded")


def test_migration_0132_0133_backfill_catalog_and_indexes(tmp_store):
    from db import migrations

    versions = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT CAST(version AS INTEGER)
            FROM schema_migrations
            WHERE version ~ '^[0-9]+$'
            """
        ).fetchall()
    }
    assert {132, 133}.issubset(versions)

    tables = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT table_name
            FROM duckdb_tables()
            WHERE table_name IN (
                'backfill_run',
                'backfill_watermark',
                'backfill_dead_letter'
            )
            """
        ).fetchall()
    }
    assert tables == {"backfill_run", "backfill_watermark", "backfill_dead_letter"}

    catalogued = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT table_name
            FROM table_catalog
            WHERE table_name IN (
                'backfill_run',
                'backfill_watermark',
                'backfill_dead_letter'
            )
            """
        ).fetchall()
    }
    assert catalogued == {"backfill_run", "backfill_watermark", "backfill_dead_letter"}

    for table_name in catalogued:
        columns = {
            row[0]
            for row in tmp_store.con.execute(
                "SELECT column_name FROM duckdb_columns() WHERE table_name = ?",
                [table_name],
            ).fetchall()
        }
        fields = {
            row[0]
            for row in tmp_store.con.execute(
                "SELECT field_name FROM field_catalog WHERE table_name = ?",
                [table_name],
            ).fetchall()
        }
        assert fields == columns

    indexes = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT index_name
            FROM duckdb_indexes()
            WHERE table_name IN (
                'backfill_run',
                'backfill_watermark',
                'backfill_dead_letter'
            )
            """
        ).fetchall()
    }
    assert "idx_backfill_run_dataset_status" in indexes
    assert "idx_backfill_watermark_status" in indexes
    assert "idx_backfill_watermark_run" in indexes
    assert "idx_backfill_dead_letter_partition" in indexes

    schema_src = inspect.getsource(migrations._pf3_s1_backfill_schema_catalog)
    index_src = inspect.getsource(migrations._pf3_s1_backfill_indexes)
    assert "CREATE INDEX" not in schema_src
    assert "CREATE TABLE" not in index_src
