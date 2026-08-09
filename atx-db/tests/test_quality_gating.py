from __future__ import annotations

import datetime as dt
import json

import pytest


class TickClock:
    def __init__(self) -> None:
        self.current = dt.datetime(2026, 7, 4, 12, 0, 0)

    def __call__(self) -> dt.datetime:
        value = self.current
        self.current += dt.timedelta(seconds=1)
        return value


class GateProbeDataset:
    dataset_id = "schema_contract"
    depends_on: tuple[str, ...] = ()

    def run(self, store, options):
        from atx_db.dataset import DatasetLoadResult

        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=1,
            source="pytest",
            details={},
        )


class DownstreamDataset:
    dataset_id = "downstream_gate_probe"
    depends_on = ("schema_contract",)

    def run(self, store, options):
        from atx_db.dataset import DatasetLoadResult

        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=1,
            source="pytest",
            details={},
        )


def _registry(*classes):
    return {cls.dataset_id: (cls, lambda params: dict(params)) for cls in classes}


def _set_pit_gate(
    store,
    *,
    severity: str,
    threshold_value: float = 0.0,
    comparator: str = "eq",
) -> None:
    from atx_db.quality import PIT_COLUMN_PRESENCE_CHECK_NAME

    store.con.execute(
        """
        UPDATE quality_check_registry
        SET severity = ?,
            threshold_value = ?,
            comparator = ?,
            enabled = true,
            updated_at = ?
        WHERE check_name = ?
        """,
        [
            severity,
            threshold_value,
            comparator,
            dt.datetime(2026, 7, 4, 12, 0, 0),
            PIT_COLUMN_PRESENCE_CHECK_NAME,
        ],
    )


def _plant_non_exempt_pit_violation(store) -> None:
    store.con.execute(
        """
        CREATE TABLE quality_gate_pit_violation (
            violation_id VARCHAR PRIMARY KEY,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            run_id VARCHAR
        )
        """
    )
    store.con.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'quality_gate_pit_violation',
            'test',
            'schema_contract_gate_probe',
            'violation_id',
            'Test-only fact-like table intentionally missing is_latest_revision.',
            '["violation_id"]',
            'Carries strong PIT markers but omits one canonical PIT column so pit_column_presence can exercise gate failure behavior.',
            ?
        )
        """,
        [dt.datetime(2026, 7, 4, 12, 0, 0)],
    )


def _orchestrator(store):
    from atx_db.orchestrator import DatasetOrchestrator

    return DatasetOrchestrator(
        store,
        _registry(GateProbeDataset, DownstreamDataset),
        actor="pytest",
        clock=TickClock(),
        sleeper=lambda _delay: None,
    )


def test_critical_quality_gate_halts_run_and_leaves_downstream_pending(tmp_store):
    from atx_db.orchestrator import QualityGateError
    from atx_db.quality import PIT_COLUMN_PRESENCE_CHECK_NAME

    _plant_non_exempt_pit_violation(tmp_store)
    _set_pit_gate(tmp_store, severity="critical")

    with pytest.raises(QualityGateError):
        _orchestrator(tmp_store).run(run_id="gate-critical", gate=True)

    run_status = tmp_store.con.execute(
        "SELECT status FROM etl_job_runs WHERE run_id = ?",
        ["gate-critical"],
    ).fetchone()[0]
    assert run_status == "failed"

    statuses = dict(
        tmp_store.con.execute(
            """
            SELECT dataset_id, status
            FROM etl_job_steps
            WHERE run_id = ?
            ORDER BY dataset_id
            """,
            ["gate-critical"],
        ).fetchall()
    )
    assert statuses == {
        "schema_contract": "failed",
        "downstream_gate_probe": "pending",
    }

    audit = tmp_store.con.execute(
        """
        SELECT details_json
        FROM etl_job_audit
        WHERE run_id = ?
          AND dataset_id = 'schema_contract'
          AND action = 'step_quality_gate_halt'
        """,
        ["gate-critical"],
    ).fetchone()
    assert audit is not None
    details = json.loads(audit[0])
    assert details["decision"] == "halt"
    assert details["worst_severity"] == "critical"
    assert any(row["check_name"] == PIT_COLUMN_PRESENCE_CHECK_NAME for row in details["failed_checks"])

    recorded = tmp_store.con.execute(
        """
        SELECT status, severity
        FROM data_quality_checks
        WHERE check_name = ?
        ORDER BY checked_at DESC
        LIMIT 1
        """,
        [PIT_COLUMN_PRESENCE_CHECK_NAME],
    ).fetchone()
    assert recorded == ("failed", "critical")


def test_error_quality_gate_degrades_run_to_partial(tmp_store):
    _plant_non_exempt_pit_violation(tmp_store)
    _set_pit_gate(tmp_store, severity="error")

    result = _orchestrator(tmp_store).run(run_id="gate-error", gate=True)

    assert result.status == "partial"
    run_status = tmp_store.con.execute(
        "SELECT status FROM etl_job_runs WHERE run_id = ?",
        ["gate-error"],
    ).fetchone()[0]
    assert run_status == "partial"
    statuses = dict(
        tmp_store.con.execute(
            "SELECT dataset_id, status FROM etl_job_steps WHERE run_id = ?",
            ["gate-error"],
        ).fetchall()
    )
    assert set(statuses.values()) == {"succeeded"}
    assert tmp_store.con.execute(
        """
        SELECT count(*)
        FROM etl_job_audit
        WHERE run_id = ?
          AND action = 'step_quality_gate_degrade'
        """,
        ["gate-error"],
    ).fetchone()[0] == 1


def test_warning_quality_gate_records_but_succeeds(tmp_store):
    _set_pit_gate(tmp_store, severity="warning")

    result = _orchestrator(tmp_store).run(run_id="gate-warning", gate=True)

    assert result.status == "succeeded"
    recorded = tmp_store.con.execute(
        """
        SELECT severity
        FROM data_quality_checks
        WHERE check_name = 'pit_column_presence'
        ORDER BY checked_at DESC
        LIMIT 1
        """
    ).fetchone()[0]
    assert recorded == "warning"


def test_gate_false_preserves_ungated_orchestrator_walk(tmp_store):
    _set_pit_gate(tmp_store, severity="critical")

    result = _orchestrator(tmp_store).run(run_id="gate-disabled", gate=False)

    assert result.status == "succeeded"
    assert tmp_store.con.execute(
        """
        SELECT count(*)
        FROM etl_job_audit
        WHERE run_id = ?
          AND action LIKE 'step_quality_gate%'
        """,
        ["gate-disabled"],
    ).fetchone()[0] == 0


def test_registry_threshold_override_changes_gate_without_python_edit(tmp_store):
    _set_pit_gate(tmp_store, severity="critical", threshold_value=999.0, comparator="le")

    result = _orchestrator(tmp_store).run(run_id="gate-threshold-override", gate=True)

    assert result.status == "succeeded"
    row = tmp_store.con.execute(
        """
        SELECT status, threshold_value, severity
        FROM data_quality_checks
        WHERE check_name = 'pit_column_presence'
        ORDER BY checked_at DESC
        LIMIT 1
        """
    ).fetchone()
    assert row == ("passed", 999.0, "critical")
