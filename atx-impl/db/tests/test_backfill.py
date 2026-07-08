from __future__ import annotations

import datetime as dt
import inspect
import threading

import pandas as pd
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
    options_seen: list[dict] = []
    fail_once_for: set[str] = set()

    @classmethod
    def reset(cls) -> None:
        cls.calls = []
        cls.options_seen = []
        cls.fail_once_for = set()

    def run(self, store, options):
        from db.dataset import DatasetLoadResult

        partition_key = str(options["partition_key"])
        window_lo = options["window_lo"]
        window_hi = options["window_hi"]
        type(self).calls.append(partition_key)
        type(self).options_seen.append(dict(options))

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


class BarrierSourceDataset:
    dataset_id = "fixture_barrier_source"
    depends_on: tuple[str, ...] = ()


class BarrierDependentDataset:
    dataset_id = "fixture_barrier_dependent"
    depends_on = (BarrierSourceDataset.dataset_id,)


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


def _barrier_registry():
    return {
        BarrierSourceDataset.dataset_id: (
            BarrierSourceDataset,
            lambda params: dict(params),
        ),
        BarrierDependentDataset.dataset_id: (
            BarrierDependentDataset,
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


@pytest.mark.slow
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


def test_run_backfill_rejects_invalid_max_parallel_without_running_header(tmp_store):
    from db.backfill import run_backfill

    run_id = "backfill-invalid-max-parallel"

    with pytest.raises(ValueError, match="max_parallel must be positive"):
        run_backfill(
            tmp_store,
            "fixture_backfill",
            dt.date(2014, 1, 1),
            dt.date(2014, 3, 1),
            "1mo",
            registry=_registry(),
            backfill_run_id=run_id,
            include_dependencies=False,
            max_parallel=0,
            clock=TickClock(),
        )

    assert tmp_store.con.execute(
        """
        SELECT status
        FROM backfill_run
        WHERE backfill_run_id = ?
        """,
        [run_id],
    ).fetchall() == []
    assert tmp_store.con.execute(
        """
        SELECT count(*)
        FROM backfill_run
        WHERE backfill_run_id = ? AND status = 'running'
        """,
        [run_id],
    ).fetchone()[0] == 0


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

    rerun_status_rows = tmp_store.con.execute(
        """
        SELECT backfill_run_id, run_id, partition_key, status, rows_written, watermark_after, run_status
        FROM v_backfill_status
        WHERE dataset_id = 'fixture_backfill'
          AND backfill_run_id = 'backfill-second'
        ORDER BY partition_key
        """
    ).fetchall()
    watermark_by_partition = {row[0]: row for row in watermarks_before}
    assert len(rerun_status_rows) == 4
    assert {row[2] for row in rerun_status_rows} == set(watermark_by_partition)
    assert {row[0] for row in rerun_status_rows} == {"backfill-second"}
    assert {row[1] for row in rerun_status_rows} == {"backfill-first"}
    assert {row[3] for row in rerun_status_rows} == {"skipped"}
    assert {row[4] for row in rerun_status_rows} == {0}
    assert {row[6] for row in rerun_status_rows} == {"succeeded"}
    assert {
        (row[2], row[5])
        for row in rerun_status_rows
    } == {
        (partition_key, watermark_after)
        for partition_key, _status, _rows_written, watermark_after, _run_id, _updated_at
        in watermarks_before
    }


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


def test_backfill_status_view_reports_run_partition_and_dead_letter_state(tmp_store):
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
    poisoned = partitions[1].partition_key

    def executor(store, partition, options):
        if partition.partition_key == poisoned:
            raise RuntimeError(f"poisoned fixture partition {partition.partition_key}")
        return FixtureBackfillDataset().run(store, options)

    result = run_backfill(
        tmp_store,
        "fixture_backfill",
        start,
        end,
        "1mo",
        registry=_registry(),
        backfill_run_id="backfill-status-view",
        include_dependencies=False,
        executor=executor,
        retry_policy=RetryPolicy(max_retries=1, retry_delay_seconds=0),
        dead_letter=True,
        clock=TickClock(),
    )

    rows = tmp_store.con.execute(
        """
        SELECT
            backfill_run_id,
            run_id,
            root_dataset_id,
            partition_dataset_id,
            dataset_id,
            partition_key,
            window_lo,
            window_hi,
            status,
            rows_written,
            watermark_after,
            attempts,
            dead_letter_state,
            dead_letter_error,
            run_status,
            run_start_date,
            run_end_date,
            run_chunk
        FROM v_backfill_status
        WHERE dataset_id = ? AND backfill_run_id = ?
        ORDER BY window_lo
        """,
        ["fixture_backfill", "backfill-status-view"],
    ).fetchall()

    assert result.status == "partial"
    assert len(rows) == 3
    assert rows[0] == (
        "backfill-status-view",
        "backfill-status-view",
        "fixture_backfill",
        "fixture_backfill",
        "fixture_backfill",
        partitions[0].partition_key,
        partitions[0].window_lo,
        partitions[0].window_hi,
        "succeeded",
        (partitions[0].window_hi - partitions[0].window_lo).days,
        partitions[0].window_hi.isoformat(),
        0,
        "clear",
        None,
        "partial",
        start,
        end,
        "1mo",
    )
    assert rows[1][5:14] == (
        partitions[1].partition_key,
        partitions[1].window_lo,
        partitions[1].window_hi,
        "failed",
        0,
        None,
        2,
        "dead_lettered",
        rows[1][13],
    )
    assert "poisoned fixture partition" in rows[1][13]

    filtered = tmp_store.con.execute(
        """
        SELECT partition_key
        FROM v_backfill_status
        WHERE partition_dataset_id = ?
          AND backfill_run_id = ?
          AND status = 'failed'
        """,
        ["fixture_backfill", "backfill-status-view"],
    ).fetchall()
    assert filtered == [(poisoned,)]


def test_backfill_max_parallel_caps_concurrent_partition_executor_calls(tmp_store):
    from db.backfill import run_backfill

    overlap = threading.Barrier(2, timeout=5.0)
    lock = threading.Lock()
    active = 0
    max_seen = 0
    barrier_passes = 0

    def executor(_store, partition, _options):
        nonlocal active, max_seen, barrier_passes
        with lock:
            active += 1
            max_seen = max(max_seen, active)
        try:
            overlap.wait()
            with lock:
                barrier_passes += 1
            return {
                "rows_written": 1,
                "watermark_after": partition.window_hi.isoformat(),
            }
        finally:
            with lock:
                active -= 1

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
    assert max_seen == 2
    assert barrier_passes == 6


def test_backfill_with_dependencies_holds_dependent_partitions_until_upstream_finishes(tmp_store):
    from db.backfill import run_backfill

    lock = threading.Lock()
    source_started: set[str] = set()
    source_finished: set[str] = set()
    events: list[tuple[str, str, str]] = []
    violations: list[str] = []
    both_source_partitions_started = threading.Event()
    all_source_partitions_finished = threading.Event()
    release_slow_source = threading.Event()

    def executor(_store, partition, _options):
        dataset_id = partition.dataset_id
        with lock:
            events.append(("start", dataset_id, partition.partition_key))
            if dataset_id == BarrierSourceDataset.dataset_id:
                source_started.add(partition.partition_key)
                if len(source_started) == 2:
                    both_source_partitions_started.set()
            elif not all_source_partitions_finished.is_set():
                violations.append(partition.partition_key)
                release_slow_source.set()

        if dataset_id == BarrierSourceDataset.dataset_id:
            if partition.window_lo == dt.date(2014, 1, 1):
                assert both_source_partitions_started.wait(timeout=1.0)
            else:
                release_slow_source.wait(timeout=0.2)
            with lock:
                source_finished.add(partition.partition_key)
                events.append(("finish", dataset_id, partition.partition_key))
                if len(source_finished) == 2:
                    all_source_partitions_finished.set()
        else:
            with lock:
                events.append(("finish", dataset_id, partition.partition_key))

        return {
            "rows_written": 1,
            "watermark_after": partition.window_hi.isoformat(),
        }

    result = run_backfill(
        tmp_store,
        BarrierDependentDataset.dataset_id,
        dt.date(2014, 1, 1),
        dt.date(2014, 3, 1),
        "1mo",
        registry=_barrier_registry(),
        backfill_run_id="backfill-dependency-barrier",
        include_dependencies=True,
        executor=executor,
        max_parallel=2,
        clock=TickClock(),
    )

    assert result.status == "succeeded"
    assert result.dataset_order == (
        BarrierSourceDataset.dataset_id,
        BarrierDependentDataset.dataset_id,
    )
    assert result.partitions_succeeded == 4
    assert violations == []

    first_dependent_start = next(
        index
        for index, event in enumerate(events)
        if event[0] == "start" and event[1] == BarrierDependentDataset.dataset_id
    )
    last_source_finish = max(
        index
        for index, event in enumerate(events)
        if event[0] == "finish" and event[1] == BarrierSourceDataset.dataset_id
    )
    assert first_dependent_start > last_source_finish


def test_full_rebuild_partition_deletes_only_within_window(tmp_store):
    """S3-11: prove a per-partition ``full_rebuild`` backfill never table-wipes
    another partition's rows.

    ``backfill._partition_params`` sets ``full_rebuild=True`` together with
    per-partition ``start_date``/``end_date``/``window_lo``/``window_hi`` for
    every windowed dataset backfill (665-687). ``enterprise_value``'s delete
    scope (``_delete_enterprise_value_scope``) never looks at ``full_rebuild``
    at all -- it always scopes the delete by ``start_date``/``end_date`` when
    present. This test seeds real ``enterprise_value`` rows in two disjoint
    trade-date partitions (Jan and Feb 2020), then runs a ``full_rebuild``
    backfill of ONLY the Feb partition, and asserts:
      * the Jan partition's row survives byte-for-byte (proves the delete did
        not table-wipe the other partition), and
      * the Feb partition's row was actually deleted and recomputed (proves
        the assertion above isn't vacuously true because nothing ran).
    """
    from db.backfill import run_backfill
    from db.enterprise_value import EnterpriseValueOptions, refresh_enterprise_value
    from db.warehouse import insert_frame

    security_id = "SEC-EV-WINDOW"
    symbol = "EVW"
    jan_trade_date = dt.date(2020, 1, 2)
    feb_trade_date = dt.date(2020, 2, 15)

    def _market_cap_row(trade_date: dt.date) -> dict[str, object]:
        available_at = dt.datetime.combine(trade_date, dt.time(22, 0))
        return {
            "market_cap_id": f"mc-{security_id}-{trade_date}",
            "source": "derived_market_cap_v1",
            "price_source": "fixture_prices",
            "share_source": "fixture_shares",
            "security_id": security_id,
            "symbol": symbol,
            "trade_date": trade_date,
            "close": 10.0,
            "share_count": 100.0,
            "share_count_type_used": "shares_diluted_avg",
            "market_cap": 1000.0,
            "is_latest_revision": True,
            "as_of_date": trade_date,
            "available_at": available_at,
            "price_available_at": available_at,
            "share_available_at": available_at,
            "price_run_id": "price-run",
            "share_run_id": "share-run",
            "share_history_id": f"share-{security_id}",
            "input_codes_json": '{"market_cap": "fixture"}',
            "input_lineage_json": '{"market_cap": "fixture"}',
            "run_id": "market-run",
        }

    def _statement_row(metric: str, value: float, *, available_at: dt.datetime) -> dict[str, object]:
        period_end = dt.date(2019, 12, 31)
        return {
            "statement_point_id": f"stmt-{security_id}-{metric}",
            "fact_revision_id": f"fact-{security_id}-{metric}",
            "revision_group_id": f"rg-{security_id}-{metric}",
            "source": "fixture_statement",
            "security_id": security_id,
            "symbol": symbol,
            "cik": "0000000099",
            "statement_type": "balance_sheet",
            "statement_section": "valuation",
            "canonical_metric": metric,
            "canonical_label": metric.replace("_", " ").title(),
            "taxonomy": "us-gaap",
            "concept": metric,
            "unit": "USD",
            "unit_type": "monetary",
            "period_type": "instant",
            "normal_balance": "credit",
            "period_start": None,
            "period_end": period_end,
            "as_of_date": period_end,
            "available_at": available_at,
            "fiscal_year": period_end.year,
            "fiscal_period": "FY",
            "form": "10-K",
            "accession_number": f"acc-{security_id}-{period_end:%Y%m%d}",
            "revision_sequence": 1,
            "revision_count": 1,
            "is_latest_revision": True,
            "is_value_changed": False,
            "raw_value": value,
            "value": value,
            "previous_raw_value": None,
            "previous_value": None,
            "value_delta": None,
            "value_delta_percent": None,
            "run_id": "statement-run",
            "source_url": "fixture",
            "source_loaded_at": available_at,
        }

    insert_frame(
        tmp_store,
        pd.DataFrame([_market_cap_row(jan_trade_date), _market_cap_row(feb_trade_date)]),
        "market_cap",
        "s3_11_market_cap_seed",
    )
    insert_frame(
        tmp_store,
        pd.DataFrame(
            [
                _statement_row("total_debt", 200.0, available_at=dt.datetime(2020, 2, 1, 10)),
                _statement_row("pref_stock", 25.0, available_at=dt.datetime(2020, 2, 2, 10)),
                _statement_row("minority_int_bs", 10.0, available_at=dt.datetime(2020, 2, 3, 10)),
                _statement_row("cash_st_inv", 50.0, available_at=dt.datetime(2020, 2, 5, 10)),
            ]
        ),
        "fundamental_statement_points",
        "s3_11_statement_seed",
    )

    # Seed BOTH partitions' rows directly (unscoped refresh) before any backfill runs.
    assert refresh_enterprise_value(tmp_store, EnterpriseValueOptions(run_id="seed-run")) == 2

    before_rows = {
        row[0]: row
        for row in tmp_store.con.execute(
            """
            SELECT trade_date, run_id, enterprise_value, market_cap
            FROM enterprise_value
            ORDER BY trade_date
            """
        ).fetchall()
    }
    assert set(before_rows) == {jan_trade_date, feb_trade_date}
    assert before_rows[jan_trade_date][1] == "seed-run"
    assert before_rows[feb_trade_date][1] == "seed-run"

    # A single-partition full_rebuild backfill window that covers ONLY February.
    result = run_backfill(
        tmp_store,
        "enterprise_value",
        dt.date(2020, 2, 1),
        dt.date(2020, 3, 1),
        "1mo",
        include_dependencies=False,
        backfill_run_id="s3-11-feb-full-rebuild",
        clock=TickClock(),
    )

    assert result.status == "succeeded"
    assert result.partitions_planned == 1
    assert result.partitions_succeeded == 1
    partition_result = result.partition_results[0]
    assert partition_result.status == "succeeded"
    assert partition_result.rows_written == 1
    assert partition_result.partition.window_lo == dt.date(2020, 2, 1)
    assert partition_result.partition.window_hi == dt.date(2020, 3, 1)

    after_rows = {
        row[0]: row
        for row in tmp_store.con.execute(
            """
            SELECT trade_date, run_id, enterprise_value, market_cap
            FROM enterprise_value
            ORDER BY trade_date
            """
        ).fetchall()
    }
    assert set(after_rows) == {jan_trade_date, feb_trade_date}, (
        "a full_rebuild of the Feb partition must not table-wipe the Jan "
        "partition's row"
    )
    assert after_rows[jan_trade_date] == before_rows[jan_trade_date], (
        "the OTHER partition's row must survive byte-for-byte -- the "
        "full_rebuild delete must be scoped to the target partition's window"
    )
    assert after_rows[feb_trade_date][1] != "seed-run", (
        "the target partition's row must have been deleted and recomputed by "
        "the full_rebuild -- only the target window's rows were replaced"
    )


def test_run_maintenance_rejects_invalid_max_parallel_without_running_header(tmp_store):
    from db.backfill import run_maintenance

    run_id = "maintenance-invalid-max-parallel"

    with pytest.raises(ValueError, match="max_parallel must be positive"):
        run_maintenance(
            tmp_store,
            "fixture_backfill",
            dt.date(2014, 1, 1),
            dt.date(2014, 3, 1),
            "1mo",
            registry=_registry(),
            backfill_run_id=run_id,
            max_parallel=0,
            clock=TickClock(),
        )

    assert tmp_store.con.execute(
        """
        SELECT status
        FROM backfill_run
        WHERE backfill_run_id = ?
        """,
        [run_id],
    ).fetchall() == []
    assert tmp_store.con.execute(
        """
        SELECT count(*)
        FROM backfill_run
        WHERE backfill_run_id = ? AND status = 'running'
        """,
        [run_id],
    ).fetchone()[0] == 0


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
    assert first.rows_written == (dt.date(2014, 2, 15) - start).days
    assert first_watermark == ("2014-02-15",)
    assert second.partitions_planned == 1
    assert second.partitions_succeeded == 1
    assert second.rows_written == (dt.date(2014, 3, 1) - start).days
    assert second_watermark == ("2014-03-01",)
    assert FixtureMaintenanceDataset.calls == [
        partition.partition_key,
        partition.partition_key,
    ]
    assert [
        (
            options["partition_key"],
            options["backfill_partition_key"],
            options["start_date"],
            options["end_date"],
            options["window_lo"],
            options["window_hi"],
            options["window_hi_exclusive"],
            options["current_watermark"],
            options["maintenance_watermark"],
            options["full_rebuild"],
            options["maintenance"],
        )
        for options in FixtureMaintenanceDataset.options_seen
    ] == [
        (
            partition.partition_key,
            partition.partition_key,
            partition.window_lo,
            dt.date(2014, 2, 14),
            partition.window_lo,
            dt.date(2014, 2, 15),
            dt.date(2014, 2, 15),
            "2014-02-15",
            "2014-02-15",
            False,
            True,
        ),
        (
            partition.partition_key,
            partition.partition_key,
            partition.window_lo,
            dt.date(2014, 2, 28),
            partition.window_lo,
            dt.date(2014, 3, 1),
            dt.date(2014, 3, 1),
            "2014-03-01",
            "2014-03-01",
            False,
            True,
        ),
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


def test_migration_0132_0133_0134_backfill_catalog_indexes_and_status_view(tmp_store):
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
    assert {132, 133, 134}.issubset(versions)

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

    views = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT view_name
            FROM duckdb_views()
            WHERE view_name = 'v_backfill_status'
            """
        ).fetchall()
    }
    assert views == {"v_backfill_status"}
    assert tmp_store.con.execute(
        """
        SELECT count(*)
        FROM table_catalog
        WHERE table_name = 'v_backfill_status'
          AND entity = 'backfill_status'
        """
    ).fetchone()[0] == 1
    view_columns = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT column_name
            FROM information_schema.columns
            WHERE table_schema = 'main'
              AND table_name = 'v_backfill_status'
            """
        ).fetchall()
    }
    view_fields = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT field_name
            FROM field_catalog
            WHERE table_name = 'v_backfill_status'
            """
        ).fetchall()
    }
    assert view_fields == view_columns

    schema_src = inspect.getsource(migrations._pf3_s1_backfill_schema_catalog)
    index_src = inspect.getsource(migrations._pf3_s1_backfill_indexes)
    view_src = inspect.getsource(migrations._pf3_s1_backfill_status_view)
    assert "CREATE INDEX" not in schema_src
    assert "CREATE TABLE" not in index_src
    assert "CREATE TABLE" not in view_src
    assert "CREATE INDEX" not in view_src


def test_backfill_status_view_migration_handles_missing_manifest_tables(tmp_store):
    from db import migrations

    con = tmp_store.con
    con.execute("DROP VIEW IF EXISTS v_backfill_status")
    con.execute("DROP TABLE IF EXISTS etl_job_audit")
    con.execute("DROP TABLE IF EXISTS etl_job_steps")
    con.execute("DROP TABLE IF EXISTS etl_job_runs")
    con.execute("DELETE FROM backfill_watermark WHERE dataset_id = 'legacy_fixture'")
    con.execute("DELETE FROM backfill_run WHERE backfill_run_id = 'legacy-run'")
    con.execute(
        """
        INSERT INTO backfill_run (
            backfill_run_id, dataset_id, start_date, end_date, chunk, status, started_at
        )
        VALUES (
            'legacy-run',
            'legacy_fixture',
            DATE '2020-01-01',
            DATE '2020-02-01',
            '1mo',
            'succeeded',
            TIMESTAMP '2026-07-04 09:30:00'
        )
        """
    )
    con.execute(
        """
        INSERT INTO backfill_watermark (
            dataset_id,
            partition_key,
            window_lo,
            window_hi,
            status,
            rows_written,
            watermark_after,
            run_id,
            updated_at
        )
        VALUES (
            'legacy_fixture',
            'legacy_fixture:2020-01-01:2020-02-01',
            DATE '2020-01-01',
            DATE '2020-02-01',
            'succeeded',
            31,
            '2020-02-01',
            'legacy-run',
            TIMESTAMP '2026-07-04 09:31:00'
        )
        """
    )

    migrations._pf3_s1_backfill_status_view(con)

    manifest_tables = {
        row[0]
        for row in con.execute(
            """
            SELECT table_name
            FROM duckdb_tables()
            WHERE table_name IN ('etl_job_runs', 'etl_job_steps', 'etl_job_audit')
            """
        ).fetchall()
    }
    assert manifest_tables == {"etl_job_runs", "etl_job_steps", "etl_job_audit"}

    rows = con.execute(
        """
        SELECT
            backfill_run_id,
            run_id,
            root_dataset_id,
            partition_dataset_id,
            partition_key,
            status,
            rows_written,
            run_status
        FROM v_backfill_status
        WHERE backfill_run_id = 'legacy-run'
        ORDER BY partition_key
        """
    ).fetchall()
    assert rows == [
        (
            "legacy-run",
            "legacy-run",
            "legacy_fixture",
            "legacy_fixture",
            "legacy_fixture:2020-01-01:2020-02-01",
            "succeeded",
            31,
            "succeeded",
        )
    ]
