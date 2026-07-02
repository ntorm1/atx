from __future__ import annotations

import datetime as dt
import inspect
import json
from collections import Counter

import pytest


class StubA:
    dataset_id = "a"
    depends_on: tuple[str, ...] = ()


class StubB:
    dataset_id = "b"
    depends_on: tuple[str, ...] = ()


class StubC:
    dataset_id = "c"
    depends_on = ("b", "a")


class StubD:
    dataset_id = "d"
    depends_on = ("b",)


def _registry(*classes):
    return {cls.dataset_id: (cls, lambda params: params) for cls in classes}


class TickClock:
    def __init__(self) -> None:
        self.current = dt.datetime(2026, 7, 1, 12, 0, 0)

    def __call__(self) -> dt.datetime:
        value = self.current
        self.current += dt.timedelta(seconds=1)
        return value


class RecordingDataset:
    depends_on: tuple[str, ...] = ()
    calls: list[tuple[str, dict]] = []
    fail_remaining: dict[str, int] = {}
    watermark_values: dict[str, str] = {}

    @classmethod
    def reset(cls) -> None:
        cls.calls = []
        cls.fail_remaining = {}
        cls.watermark_values = {}

    def run(self, store, options):
        from db.dataset import DatasetLoadResult

        dataset_id = self.dataset_id
        RecordingDataset.calls.append((dataset_id, dict(options)))
        remaining = RecordingDataset.fail_remaining.get(dataset_id, 0)
        if remaining > 0:
            RecordingDataset.fail_remaining[dataset_id] = remaining - 1
            raise RuntimeError(f"planned failure for {dataset_id}")
        watermark = RecordingDataset.watermark_values.get(dataset_id)
        if watermark is not None:
            _upsert_watermark(store, dataset_id, "max_available_at", watermark)
        return DatasetLoadResult(
            dataset_id=dataset_id,
            rows_loaded=10,
            source=f"stub:{dataset_id}",
            details={},
        )


class ExecA(RecordingDataset):
    dataset_id = "a"


class ExecB(RecordingDataset):
    dataset_id = "b"
    depends_on = ("a",)


class ExecC(RecordingDataset):
    dataset_id = "c"
    depends_on = ("b",)


class ExecD(RecordingDataset):
    dataset_id = "d"


def _exec_registry(*classes):
    return {cls.dataset_id: (cls, lambda params: dict(params)) for cls in classes}


def _upsert_watermark(store, dataset_id: str, name: str, value: str) -> None:
    store.con.execute(
        """
        DELETE FROM dataset_watermarks
        WHERE dataset_id = ? AND watermark_name = ?
        """,
        [dataset_id, name],
    )
    store.con.execute(
        """
        INSERT INTO dataset_watermarks (
            dataset_id, watermark_name, watermark_value, updated_at
        )
        VALUES (?, ?, ?, ?)
        """,
        [dataset_id, name, value, dt.datetime(2026, 7, 1, 12, 0, 0)],
    )


@pytest.fixture(autouse=True)
def _reset_recording_dataset():
    RecordingDataset.reset()
    yield
    RecordingDataset.reset()


def test_build_dataset_dag_topo_sort_is_stable_by_dataset_id():
    from db.orchestrator import build_dataset_dag

    dag = build_dataset_dag(_registry(StubD, StubC, StubB, StubA))

    assert dag.order == ("a", "b", "c", "d")
    assert dag.dependencies_of("c") == ("a", "b")
    assert dag.children_of("b") == ("c", "d")


def test_build_dataset_dag_rejects_missing_dependency():
    from db.orchestrator import MissingDependencyError, build_dataset_dag

    class NeedsMissing:
        dataset_id = "needs_missing"
        depends_on = ("missing",)

    with pytest.raises(MissingDependencyError, match="missing"):
        build_dataset_dag(_registry(NeedsMissing))


def test_build_dataset_dag_cycle_error_names_path():
    from db.orchestrator import CycleError, build_dataset_dag

    class CycleA:
        dataset_id = "cycle_a"
        depends_on = ("cycle_c",)

    class CycleB:
        dataset_id = "cycle_b"
        depends_on = ("cycle_a",)

    class CycleC:
        dataset_id = "cycle_c"
        depends_on = ("cycle_b",)

    with pytest.raises(CycleError) as excinfo:
        build_dataset_dag(_registry(CycleA, CycleB, CycleC))

    assert excinfo.value.path == ("cycle_a", "cycle_c", "cycle_b", "cycle_a")
    assert "cycle_a -> cycle_c -> cycle_b -> cycle_a" in str(excinfo.value)


def test_build_dataset_dag_rejects_string_depends_on():
    from db.orchestrator import build_dataset_dag

    class BadDepends:
        dataset_id = "bad"
        depends_on = "a"

    with pytest.raises(TypeError, match="depends_on"):
        build_dataset_dag(_registry(BadDepends))


def test_create_run_manifest_writes_parent_steps_and_audit(tmp_store):
    from db.orchestrator import build_dataset_dag, create_run_manifest

    dag = build_dataset_dag(_registry(StubC, StubA, StubB))
    started_at = dt.datetime(2026, 7, 1, 12, 0, 0)

    manifest = create_run_manifest(
        tmp_store,
        dag,
        run_id="run-test-001",
        params={"full_rebuild": False, "symbols": ["AAPL"]},
        git_sha="abcdef1",
        actor="pytest",
        started_at=started_at,
    )

    assert manifest.run_id == "run-test-001"
    assert manifest.dataset_order == ("a", "b", "c")

    parent = tmp_store.con.execute(
        """
        SELECT job_run_id, run_id, run_kind, parent_run_id, job_name, dataset_id,
               status, started_at, attempt_count, max_retries,
               retry_delay_seconds, params_json, git_sha
        FROM etl_job_runs
        WHERE run_id = ?
        """,
        [manifest.run_id],
    ).fetchone()
    assert parent == (
        "run-test-001",
        "run-test-001",
        "orchestrator",
        None,
        "refresh_quant_warehouse",
        "__orchestrator__",
        "running",
        started_at,
        0,
        0,
        0.0,
        '{"full_rebuild": false, "symbols": ["AAPL"]}',
        "abcdef1",
    )

    steps = tmp_store.con.execute(
        """
        SELECT dataset_id, status, rows, started_at, finished_at,
               watermark_before, watermark_after, error
        FROM etl_job_steps
        WHERE run_id = ?
        ORDER BY dataset_id
        """,
        [manifest.run_id],
    ).fetchall()
    assert steps == [
        ("a", "pending", None, None, None, None, None, None),
        ("b", "pending", None, None, None, None, None, None),
        ("c", "pending", None, None, None, None, None, None),
    ]

    audit = tmp_store.con.execute(
        """
        SELECT actor, action, dataset_id, details_json
        FROM etl_job_audit
        WHERE run_id = ?
        """,
        [manifest.run_id],
    ).fetchall()
    assert len(audit) == 1
    actor, action, dataset_id, details_json = audit[0]
    assert (actor, action, dataset_id) == ("pytest", "run_start", None)
    details = json.loads(details_json)
    assert details == {"dataset_count": 3, "dataset_order": ["a", "b", "c"]}


def test_migration_0065_0066_catalog_idempotence_and_index_split(tmp_store):
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
    assert {65, 66}.issubset(versions)

    tables = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT table_name
            FROM duckdb_tables()
            WHERE schema_name = 'main'
              AND table_name IN ('etl_job_runs', 'etl_job_steps', 'etl_job_audit')
            """
        ).fetchall()
    }
    assert tables == {"etl_job_runs", "etl_job_steps", "etl_job_audit"}

    run_columns = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT column_name
            FROM information_schema.columns
            WHERE table_schema = 'main'
              AND table_name = 'etl_job_runs'
            """
        ).fetchall()
    }
    assert {"run_id", "run_kind", "parent_run_id", "git_sha"}.issubset(run_columns)

    expected_columns = {
        "etl_job_steps": {
            "run_id",
            "dataset_id",
            "status",
            "rows",
            "started_at",
            "finished_at",
            "watermark_before",
            "watermark_after",
            "error",
        },
        "etl_job_audit": {
            "audit_id",
            "run_id",
            "dataset_id",
            "actor",
            "ts",
            "action",
            "details_json",
        },
    }
    for table_name, expected in expected_columns.items():
        columns = {
            row[0]
            for row in tmp_store.con.execute(
                """
                SELECT column_name
                FROM information_schema.columns
                WHERE table_schema = 'main'
                  AND table_name = ?
                """,
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
        assert columns == expected
        assert fields == expected

    table_catalog_rows = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM table_catalog
        WHERE table_name IN ('etl_job_runs', 'etl_job_steps', 'etl_job_audit')
        """
    ).fetchone()[0]
    assert table_catalog_rows == 3

    index_names = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT index_name
            FROM duckdb_indexes()
            WHERE table_name IN ('etl_job_steps', 'etl_job_audit')
            """
        ).fetchall()
    }
    assert "idx_etl_job_steps_run_status" in index_names
    assert "idx_etl_job_audit_run_ts" in index_names

    schema_src = inspect.getsource(migrations._etl_job_orchestrator_manifest_schema)
    index_src = inspect.getsource(migrations._etl_job_orchestrator_manifest_indexes)
    assert "CREATE INDEX" not in schema_src
    assert "CREATE TABLE" not in index_src

    migrations._etl_job_orchestrator_manifest_schema(tmp_store.con)
    migrations._etl_job_orchestrator_manifest_indexes(tmp_store.con)

    table_catalog_rows_after = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM table_catalog
        WHERE table_name IN ('etl_job_runs', 'etl_job_steps', 'etl_job_audit')
        """
    ).fetchone()[0]
    assert table_catalog_rows_after == 3


def test_orchestrator_incremental_second_run_skips_all_current_nodes(tmp_store):
    from db.orchestrator import DatasetOrchestrator

    RecordingDataset.watermark_values = {
        "a": "2026-01-01T09:30:00",
        "b": "2026-01-01T10:00:00",
        "c": "2026-01-01T10:30:00",
    }
    orchestrator = DatasetOrchestrator(
        tmp_store,
        _exec_registry(ExecC, ExecA, ExecB),
        actor="pytest",
        clock=TickClock(),
        sleeper=lambda _delay: None,
    )

    first = orchestrator.run(run_id="incremental-run-1")
    second = orchestrator.run(run_id="incremental-run-2")

    assert first.status == "succeeded"
    assert second.status == "succeeded"
    assert [dataset_id for dataset_id, _options in RecordingDataset.calls] == [
        "a",
        "b",
        "c",
    ]

    steps = tmp_store.con.execute(
        """
        SELECT dataset_id, status, rows, watermark_before, watermark_after
        FROM etl_job_steps
        WHERE run_id = ?
        ORDER BY dataset_id
        """,
        [second.run_id],
    ).fetchall()
    assert [(dataset_id, status, rows) for dataset_id, status, rows, *_ in steps] == [
        ("a", "skipped", 0),
        ("b", "skipped", 0),
        ("c", "skipped", 0),
    ]
    assert all(watermark_before == watermark_after for *_, watermark_before, watermark_after in steps)

    audit_actions = [
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT action
            FROM etl_job_audit
            WHERE run_id = ?
            ORDER BY ts, action
            """,
            [second.run_id],
        ).fetchall()
    ]
    assert audit_actions.count("step_skip_incremental") == 3

    b_first_options = RecordingDataset.calls[1][1]
    assert b_first_options["incremental_since"] == "2026-01-01T09:30:00"
    assert b_first_options["since"] == "2026-01-01T09:30:00"
    assert b_first_options["start_date"] == "2026-01-01"


def test_orchestrator_upstream_watermark_change_runs_downstream_closure(tmp_store):
    from db.orchestrator import DatasetOrchestrator

    RecordingDataset.watermark_values = {
        "a": "2026-01-01T09:30:00",
        "b": "2026-01-01T10:00:00",
        "c": "2026-01-01T10:30:00",
        "d": "2026-01-01T11:00:00",
    }
    orchestrator = DatasetOrchestrator(
        tmp_store,
        _exec_registry(ExecA, ExecB, ExecC, ExecD),
        actor="pytest",
        clock=TickClock(),
        sleeper=lambda _delay: None,
    )
    orchestrator.run(run_id="closure-run-1")
    RecordingDataset.calls.clear()

    RecordingDataset.watermark_values["a"] = "2026-01-02T09:30:00"
    _upsert_watermark(tmp_store, "a", "max_available_at", "2026-01-02T09:30:00")

    orchestrator.run(run_id="closure-run-2")

    assert [dataset_id for dataset_id, _options in RecordingDataset.calls] == [
        "a",
        "b",
        "c",
    ]
    statuses = dict(
        tmp_store.con.execute(
            """
            SELECT dataset_id, status
            FROM etl_job_steps
            WHERE run_id = ?
            ORDER BY dataset_id
            """,
            ["closure-run-2"],
        ).fetchall()
    )
    assert statuses == {
        "a": "succeeded",
        "b": "succeeded",
        "c": "succeeded",
        "d": "skipped",
    }


def test_orchestrator_full_rebuild_runs_even_when_watermarks_match(tmp_store):
    from db.orchestrator import DatasetOrchestrator

    RecordingDataset.watermark_values = {
        "a": "2026-01-01T09:30:00",
        "b": "2026-01-01T10:00:00",
        "c": "2026-01-01T10:30:00",
    }
    orchestrator = DatasetOrchestrator(
        tmp_store,
        _exec_registry(ExecA, ExecB, ExecC),
        actor="pytest",
        clock=TickClock(),
        sleeper=lambda _delay: None,
    )
    orchestrator.run(run_id="full-rebuild-run-1")
    RecordingDataset.calls.clear()

    orchestrator.run(run_id="full-rebuild-run-2", full_rebuild=True)

    assert [dataset_id for dataset_id, _options in RecordingDataset.calls] == [
        "a",
        "b",
        "c",
    ]
    statuses = {
        row[0]: row[1]
        for row in tmp_store.con.execute(
            """
            SELECT dataset_id, status
            FROM etl_job_steps
            WHERE run_id = ?
            """,
            ["full-rebuild-run-2"],
        ).fetchall()
    }
    assert set(statuses.values()) == {"succeeded"}


def test_orchestrator_retries_with_exponential_backoff_and_audit(tmp_store):
    from db.orchestrator import DatasetOrchestrator, RetryPolicy

    RecordingDataset.watermark_values = {"a": "2026-01-01T09:30:00"}
    RecordingDataset.fail_remaining = {"a": 2}
    slept: list[float] = []
    orchestrator = DatasetOrchestrator(
        tmp_store,
        _exec_registry(ExecA),
        actor="pytest",
        clock=TickClock(),
        sleeper=slept.append,
        retry_policy_by_dataset={
            "a": RetryPolicy(max_retries=2, retry_delay_seconds=0.5)
        },
    )

    orchestrator.run(run_id="retry-run-1")

    assert [dataset_id for dataset_id, _options in RecordingDataset.calls] == [
        "a",
        "a",
        "a",
    ]
    assert slept == [0.5, 1.0]
    retry_rows = tmp_store.con.execute(
        """
        SELECT action, details_json
        FROM etl_job_audit
        WHERE run_id = ? AND dataset_id = ?
        ORDER BY ts
        """,
        ["retry-run-1", "a"],
    ).fetchall()
    assert [action for action, _details_json in retry_rows].count("step_retry") == 2
    retry_details = [
        json.loads(details_json)
        for action, details_json in retry_rows
        if action == "step_retry"
    ]
    assert [details["delay_seconds"] for details in retry_details] == [0.5, 1.0]


def test_orchestrator_resume_reruns_only_failed_and_pending_steps(tmp_store):
    from db.orchestrator import DatasetOrchestrator, OrchestratorRunError

    RecordingDataset.watermark_values = {
        "a": "2026-01-01T09:30:00",
        "b": "2026-01-01T10:00:00",
        "c": "2026-01-01T10:30:00",
    }
    RecordingDataset.fail_remaining = {"b": 1}
    orchestrator = DatasetOrchestrator(
        tmp_store,
        _exec_registry(ExecA, ExecB, ExecC),
        actor="pytest",
        clock=TickClock(),
        sleeper=lambda _delay: None,
    )

    with pytest.raises(OrchestratorRunError):
        orchestrator.run(run_id="resume-run-1")
    calls_before_resume = Counter(dataset_id for dataset_id, _options in RecordingDataset.calls)
    assert calls_before_resume == Counter({"a": 1, "b": 1})
    first_a_step = tmp_store.con.execute(
        """
        SELECT status, finished_at, watermark_after
        FROM etl_job_steps
        WHERE run_id = ? AND dataset_id = ?
        """,
        ["resume-run-1", "a"],
    ).fetchone()
    assert first_a_step[0] == "succeeded"

    orchestrator.resume("resume-run-1")

    calls_after_resume = Counter(dataset_id for dataset_id, _options in RecordingDataset.calls)
    assert calls_after_resume == Counter({"a": 1, "b": 2, "c": 1})
    second_a_step = tmp_store.con.execute(
        """
        SELECT status, finished_at, watermark_after
        FROM etl_job_steps
        WHERE run_id = ? AND dataset_id = ?
        """,
        ["resume-run-1", "a"],
    ).fetchone()
    assert second_a_step == first_a_step
    statuses = dict(
        tmp_store.con.execute(
            """
            SELECT dataset_id, status
            FROM etl_job_steps
            WHERE run_id = ?
            ORDER BY dataset_id
            """,
            ["resume-run-1"],
        ).fetchall()
    )
    assert statuses == {"a": "succeeded", "b": "succeeded", "c": "succeeded"}
    assert tmp_store.con.execute(
        """
        SELECT count(*)
        FROM etl_job_audit
        WHERE run_id = ? AND action = 'run_resume'
        """,
        ["resume-run-1"],
    ).fetchone()[0] == 1
