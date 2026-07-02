from __future__ import annotations

import datetime as dt
import inspect
import json

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
